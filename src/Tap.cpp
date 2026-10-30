// Tap pipe (config "tap"): \\.\pipe\<pipeName>-tap streams what the client receives and sends
// to one reader (bedboo's transport 'bedd'), binary and one way. See CONTRIBUTING.md (Tap
// protocol) for the framing and markers.
//
// The capture hooks (NetTap.cpp, and any other tap publisher) call tap::emit, which lands in
// onEmit below: it hands the frame on to the publisher installed before (the join journal chains
// the same way) and queues a copy while a reader is attached. One writer thread drains the queue
// into the pipe, so the game's threads never wait for the reader. A reader more than 32 MiB behind
// gets DROPPED and is disconnected; it reconnects and gets a full replay.
#include "Tap.h"
#include "BeDD.h"
#include "TapSink.h"

#include <sddl.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "ll/api/mod/NativeMod.h"

#include "mc/common/SharedConstants.h"

#pragma comment(lib, "advapi32.lib")

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;
using tap::Frame;
using tap::Kind;

constexpr size_t   kLagCap     = 32u << 20; // queued bytes before a slow reader is dropped
constexpr size_t   kHeaderSize = 24;        // u32 len | u8 kind | u8 flags | u16 0 | u64 seq | u64 tUs
constexpr uint32_t kMaxLen     = (16u << 20) + 20;

std::wstring      gTapPath; // \\.\pipe\<pipeName>-tap
std::string       gTapName;
std::jthread      gWriter;
std::atomic<bool> gStopping{false};
std::atomic<bool> gStarted{false};
std::atomic<bool> gAttached{false}; // a reader is connected (frames are queued for it)
std::atomic<bool> gWriterDone{false};
bool              gStillChained = false; // stopTap could not hand gEmit back (another sink chained onto ours)

std::atomic<tap::EmitFn> gPrev{nullptr}; // the publisher installed before us (forwarded to)

std::mutex              gQueueMutex;
std::condition_variable gQueueCv;
std::deque<Frame>       gQueue;
size_t                  gQueuedBytes    = 0;
bool                    gOverflow       = false;
uint64_t                gOverflowFrames = 0;

std::atomic<uint64_t> gDropped{}, gSent{}, gReaders{};

// What the last reader was replayed (the journal's own stats live in BD2's "state").
std::mutex gJournalMutex;
json       gLastJournal{
    {"frames",   0    },
    {"bytes",    0    },
    {"complete", false},
};

void enqueue(Frame&& f) {
    if (!gAttached.load()) return;
    {
        std::lock_guard lock(gQueueMutex);
        if (!gAttached.load()) return;
        if (gOverflow) {
            ++gOverflowFrames;
            return;
        }
        gQueuedBytes += kHeaderSize + f.bytes.size();
        if (gQueuedBytes > kLagCap) {
            // The reader is too slow: drop what it has not read yet; the writer says DROPPED and hangs up.
            gOverflow       = true;
            gOverflowFrames = gQueue.size() + 1;
            gQueue.clear();
            gQueuedBytes = 0;
        } else {
            gQueue.push_back(std::move(f));
        }
    }
    gQueueCv.notify_one();
}

// gEmit. The frame goes on to the previous publisher first, then into our queue: a frame is then
// either in the journal snapshot a new reader gets, or in its live queue (or both; live frames
// the replay already covered are skipped by seq).
void onEmit(Frame&& f) {
    tap::EmitFn prev = gPrev.load(std::memory_order_acquire);
    if (!prev) {
        enqueue(std::move(f));
        return;
    }
    Frame copy = f;
    prev(std::move(f));
    enqueue(std::move(copy));
}

Frame marker(json const& j) {
    return Frame{
        Kind::Marker,
        0,
        ++tap::gSeq,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count()
        ),
        j.dump(),
    };
}

bool writeAll(HANDLE pipe, char const* data, size_t size) {
    while (size > 0) {
        DWORD n = 0;
        if (!WriteFile(pipe, data, static_cast<DWORD>(std::min<size_t>(size, 1u << 20)), &n, nullptr) || n == 0)
            return false;
        data += n;
        size -= n;
    }
    return true;
}

bool writeFrame(HANDLE pipe, Frame const& f) {
    if (f.bytes.size() + 20 > kMaxLen) {
        // Larger than the protocol allows: the reader would miss it unnoticed, so it is dropped
        // as a lagging one (it reconnects and gets a replay).
        gDropped.fetch_add(1, std::memory_order_relaxed);
        log().warn("tap: a {} byte frame exceeds the frame limit; disconnecting the reader", f.bytes.size());
        writeFrame(
            pipe,
            marker({
                {"m",     "DROPPED"},
                {"count", 1        }
        })
        );
        return false;
    }
    char     header[kHeaderSize]{};
    uint32_t len = static_cast<uint32_t>(f.bytes.size() + 20);
    header[4]    = static_cast<char>(f.kind);
    header[5]    = static_cast<char>(f.flags);
    std::memcpy(header, &len, 4); // x64: little-endian
    std::memcpy(header + 8, &f.seq, 8);
    std::memcpy(header + 16, &f.tUs, 8);
    if (!writeAll(pipe, header, kHeaderSize) || !writeAll(pipe, f.bytes.data(), f.bytes.size())) return false;
    gSent.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// gReplay's sink (a plain function pointer): collects the journal on the writer thread.
std::vector<Frame> gReplayFrames;

void collectReplay(Frame&& f) { gReplayFrames.push_back(std::move(f)); }

std::string modVersion() {
    auto self = ll::mod::NativeMod::current();
    if (!self) return {};
    auto const& v = self->getManifest().version;
    return v ? v->to_string() : std::string();
}

// The reader went away (PeekNamedPipe fails once it closed its end).
bool readerGone(HANDLE pipe) { return !PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr); }

void serveReader(HANDLE pipe) {
    {
        std::lock_guard lock(gQueueMutex);
        gQueue.clear();
        gQueuedBytes    = 0;
        gOverflow       = false;
        gOverflowFrames = 0;
        gAttached       = true; // from here on, live frames queue up (until REPLAY_END they wait)
    }
    gReaders.fetch_add(1);

    gReplayFrames.clear();
    tap::ReplayFn replay = tap::gReplay.load(std::memory_order_acquire);
    if (replay) replay(&collectReplay);
    uint64_t lastReplayed = 0, replayBytes = 0;
    for (auto& f : gReplayFrames) {
        f.flags      |= tap::Replay;
        lastReplayed  = std::max(lastReplayed, f.seq);
        replayBytes  += f.bytes.size();
    }
    json journal{
        {"frames",   gReplayFrames.size()},
        {"bytes",    replayBytes         },
        {"complete", replay != nullptr   },
    };
    {
        std::lock_guard lock(gJournalMutex);
        gLastJournal = journal;
    }

    json hello{
        {"m",        "HELLO"                                  },
        {"tap",      1                                        },
        {"mod",      modVersion()                             },
        {"game",     "1.26.51.01"                             },
        {"protocol", SharedConstants::NetworkProtocolVersion()},
        {"pid",      GetCurrentProcessId()                    },
        {"link",     tap::netTapLink()                        },
        {"journal",  journal                                  },
    };
    bool ok = writeFrame(pipe, marker(hello))
           && writeFrame(
                  pipe,
                  marker({
                      {"m",      "REPLAY_BEGIN"      },
                      {"frames", gReplayFrames.size()}
    })
           );
    for (size_t i = 0; ok && i < gReplayFrames.size(); ++i) ok = writeFrame(pipe, gReplayFrames[i]);
    gReplayFrames.clear();
    gReplayFrames.shrink_to_fit();
    ok = ok
      && writeFrame(
             pipe,
             marker({
                 {"m",        "REPLAY_END"       },
                 {"complete", journal["complete"]}
    })
      );

    std::deque<Frame> batch;
    while (ok && !gStopping) {
        uint64_t dropped = 0;
        {
            std::unique_lock lock(gQueueMutex);
            gQueueCv.wait_for(lock, 1s, [] { return gStopping || gOverflow || !gQueue.empty(); });
            if (gOverflow) {
                dropped = gOverflowFrames;
            } else {
                batch.swap(gQueue);
                gQueuedBytes = 0;
            }
        }
        if (dropped) {
            gDropped.fetch_add(dropped, std::memory_order_relaxed);
            log().warn(
                "tap: reader more than {} MiB behind; {} frames dropped, disconnecting it",
                kLagCap >> 20,
                dropped
            );
            writeFrame(
                pipe,
                marker({
                    {"m",     "DROPPED"},
                    {"count", dropped  }
            })
            );
            break;
        }
        if (batch.empty()) {
            if (readerGone(pipe)) break;
            continue;
        }
        for (auto& f : batch) {
            // Already part of the replay.
            if (f.kind == Kind::Clientbound && f.seq <= lastReplayed) continue;
            if (!(ok = writeFrame(pipe, f))) break;
        }
        batch.clear();
    }

    std::lock_guard lock(gQueueMutex);
    gAttached = false;
    gQueue.clear();
    gQueuedBytes = 0;
}

// Only this user may open the pipe (the tap carries the whole game session).
struct CurrentUserOnly {
    SECURITY_ATTRIBUTES  sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;

    CurrentUserOnly() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<char> buf(size);
        LPWSTR            sid = nullptr;
        if (size && GetTokenInformation(token, TokenUser, buf.data(), size, &size)
            && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
            std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")(A;;GA;;;SY)";
            LocalFree(sid);
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
                sa.lpSecurityDescriptor = sd;
        }
        CloseHandle(token);
    }
    ~CurrentUserOnly() {
        if (sd) LocalFree(sd);
    }
    CurrentUserOnly(CurrentUserOnly const&)            = delete;
    CurrentUserOnly& operator=(CurrentUserOnly const&) = delete;
};

void serve(std::stop_token) {
    CurrentUserOnly security;
    if (!security.sd)
        log().error("tap: could not build the current-user DACL ({}); using the default one", GetLastError());
    while (!gStopping) {
        HANDLE pipe = CreateNamedPipeW(
            gTapPath.c_str(),
            PIPE_ACCESS_DUPLEX, // duplex only so PeekNamedPipe notices a reader that left; nothing is read
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            1 << 20,
            4096,
            0,
            security.sd ? &security.sa : nullptr
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            log().error("tap: CreateNamedPipe failed ({}); retrying", GetLastError());
            std::this_thread::sleep_for(1s);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && !gStopping) serveReader(pipe);
        // No DisconnectNamedPipe: a closed server end still lets the reader drain what was written (DROPPED).
        CloseHandle(pipe);
    }
    gWriterDone = true;
}

} // namespace

void startTap() {
    if (!options().tap || gStarted.exchange(true)) return;
    std::string const& name = options().pipeName;
    gTapName                = name + "-tap";
    gTapPath                = L"\\\\.\\pipe\\" + std::wstring(gTapName.begin(), gTapName.end());
    gStopping               = false;
    gWriterDone             = false;
    // Chain onto whatever publisher sink is installed already (the journal forwards the same way).
    // Still chained from a previous start (someone chained onto us since): gPrev is still right,
    // and taking gEmit again would make the two sinks forward to each other.
    if (!gStillChained) gPrev = tap::gEmit.exchange(&onEmit, std::memory_order_acq_rel);
    gStillChained = false;
    gWriter       = std::jthread(serve);
    tap::installNetTap();
    log().info("tap pipe: \\\\.\\pipe\\{}", gTapName);
}

void stopTap() {
    if (!gStarted.exchange(false)) return;
    tap::uninstallNetTap();
    // Hand gEmit back only if nobody chained onto us since (then they keep forwarding to us, and
    // onEmit keeps forwarding to gPrev; frames just stop being queued).
    tap::EmitFn self = &onEmit;
    gStillChained    = !tap::gEmit.compare_exchange_strong(self, gPrev.load(), std::memory_order_acq_rel);
    gStopping        = true;
    gQueueCv.notify_all();
    // Until the writer is out: unblock ConnectNamedPipe with a throwaway connection, and cancel a
    // WriteFile to a reader that stopped reading. Repeated, as the writer may be between calls.
    for (int i = 0; !gWriterDone && i < 300; ++i) {
        HANDLE h = CreateFileW(gTapPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CancelSynchronousIo(gWriter.native_handle());
        if (!gWriterDone) std::this_thread::sleep_for(10ms);
    }
    gWriter.join();
}

std::string tapInfo() {
    json j{
        {"enabled",  gStarted.load()               },
        {"pipe",     "\\\\.\\pipe\\" + gTapName    },
        {"attached", gAttached.load()              },
        {"link",     tap::netTapLink()             },
        {"counters",
         {
             {"in", tap::netTapIn()},
             {"out", tap::netTapOut()},
             {"dropped", gDropped.load()},
             {"sent", gSent.load()},
             {"readers", gReaders.load()},
         }                                         },
        {"hooks",    tap::netTapHooks()            },
        {"replay",   tap::gReplay.load() != nullptr},
    };
    {
        std::lock_guard lock(gJournalMutex);
        j["journal"] = gLastJournal;
    }
    return j.dump();
}

std::string tapHooks() { return tap::netTapHooks().dump(); }

// Any other sink in the chain (before ours, or chained onto ours since) takes every frame.
bool tap::frameWanted() {
    return gAttached.load(std::memory_order_relaxed) || gPrev.load(std::memory_order_relaxed)
        || tap::gEmit.load(std::memory_order_relaxed) != &onEmit;
}

} // namespace bedd
