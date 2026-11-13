// The tap (config "tap"): what the client receives and sends, streamed to one subscriber (bedboo's
// transport 'bedd'). A WebSocket client sends {op:"tap", action:"subscribe"}; after the reply its
// connection receives one binary message per tap frame (docs/reference/tap.md: the framing and
// markers). The connection still takes requests; a second subscriber gets code:"busy".
//
// The capture hooks (NetTap.cpp, and any other tap publisher) call tap::emit, which lands in
// onEmit below: it hands the frame on to the publisher installed before (the join journal chains
// the same way) and queues a copy while a reader is attached. One writer thread drains the queue
// into the connection, so the game's threads never wait for the reader. A reader more than 32 MiB
// behind gets DROPPED and is disconnected; it reconnects and gets a full replay.
#include "Tap.h"
#include "BeDD.h"
#include "Discovery.h"
#include "TapSink.h"
#include "WsServer.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "ll/api/Versions.h"

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;
using tap::Frame;
using tap::Kind;

constexpr size_t   kLagCap     = 32u << 20; // queued bytes before a slow reader is dropped
constexpr size_t   kHeaderSize = 24;        // u32 len | u8 kind | u8 flags | u16 0 | u64 seq | u64 tUs
constexpr uint32_t kMaxLen     = (16u << 20) + 20;

std::jthread      gWriter;
std::atomic<bool> gStopping{false};
std::atomic<bool> gStarted{false};
std::atomic<bool> gAttached{false}; // a reader is connected (frames are queued for it)
bool              gStillChained = false; // stopTap could not hand gEmit back (another sink chained onto ours)

std::atomic<tap::EmitFn> gPrev{nullptr}; // the publisher installed before us (forwarded to)

std::mutex              gQueueMutex;
std::condition_variable gQueueCv;
std::deque<Frame>       gQueue;
size_t                  gQueuedBytes    = 0;
bool                    gOverflow       = false;
uint64_t                gOverflowFrames = 0;

std::atomic<uint64_t> gDropped{}, gSent{}, gReaders{};

// The subscriber: handed over by the `tap` op once its reply is sent, then served by the writer.
std::mutex                      gSubMutex;
std::condition_variable         gSubCv;
std::shared_ptr<ws::Connection> gPending;            // guarded by gSubMutex
bool                            gSubscribed = false; // pending or being served, guarded by gSubMutex

// What the last reader was replayed (the journal's live stats are in "state".stats.journal).
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

bool writeFrame(ws::Connection& conn, Frame const& f) {
    if (f.bytes.size() + 20 > kMaxLen) {
        // Larger than the protocol allows: the reader would miss it unnoticed, so it is dropped
        // as a lagging one (it reconnects and gets a replay).
        gDropped.fetch_add(1, std::memory_order_relaxed);
        log().warn("tap: a {} byte frame exceeds the frame limit; disconnecting the reader", f.bytes.size());
        writeFrame(
            conn,
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
    // One binary WebSocket message per frame, header included.
    if (!conn.send(ws::kBinary, {std::string_view(header, kHeaderSize), f.bytes})) return false;
    gSent.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// gReplay's sink (a plain function pointer): collects the journal on the writer thread.
std::vector<Frame> gReplayFrames;

void collectReplay(Frame&& f) { gReplayFrames.push_back(std::move(f)); }

void serveReader(ws::Connection& conn) {
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
    // The journal (TapJournal.cpp) knows whether it kept everything since start_game (tapJournalMiB).
    bool const complete = replay != nullptr && tapJournalStats().complete;
    uint64_t lastReplayed = 0, replayBytes = 0;
    for (auto& f : gReplayFrames) {
        f.flags      |= tap::Replay;
        lastReplayed  = std::max(lastReplayed, f.seq);
        replayBytes  += f.bytes.size();
    }
    json journal{
        {"frames",   gReplayFrames.size()},
        {"bytes",    replayBytes         },
        {"complete", complete            },
    };
    {
        std::lock_guard lock(gJournalMutex);
        gLastJournal = journal;
    }

    json hello{
        {"m",        "HELLO"                                  },
        {"tap",      1                                        },
        {"mod",      modVersion()                             },
        {"game",     "1.26.60.05"                             },
        {"protocol", ll::getNetworkProtocolVersion()          },
        {"pid",      GetCurrentProcessId()                    },
        {"link",     tap::netTapLink()                        },
        {"journal",  journal                                  },
    };
    bool ok = writeFrame(conn, marker(hello))
           && writeFrame(
                  conn,
                  marker({
                      {"m",      "REPLAY_BEGIN"      },
                      {"frames", gReplayFrames.size()}
    })
           );
    for (size_t i = 0; ok && i < gReplayFrames.size(); ++i) ok = writeFrame(conn, gReplayFrames[i]);
    gReplayFrames.clear();
    gReplayFrames.shrink_to_fit();
    ok = ok
      && writeFrame(
             conn,
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
                conn,
                marker({
                    {"m",     "DROPPED"},
                    {"count", dropped  }
            })
            );
            ok = false;
            break;
        }
        if (batch.empty()) {
            if (conn.closed()) break;
            continue;
        }
        for (auto& f : batch) {
            // Already part of the replay.
            if (f.kind == Kind::Clientbound && f.seq <= lastReplayed) continue;
            if (!(ok = writeFrame(conn, f))) break;
        }
        batch.clear();
    }

    {
        std::lock_guard lock(gQueueMutex);
        gAttached = false;
        gQueue.clear();
        gQueuedBytes = 0;
    }
    // Dropped (or a frame too large): the reader reconnects and gets a full replay. 1013 = try again later.
    if (!ok) conn.close(1013, "tap reader dropped");
}

void serve(std::stop_token) {
    while (!gStopping) {
        std::shared_ptr<ws::Connection> conn;
        {
            std::unique_lock lock(gSubMutex);
            gSubCv.wait(lock, [] { return gStopping || gPending; });
            if (gStopping) break;
            conn = std::move(gPending);
        }
        log().info("tap: client {} subscribed", conn->id());
        serveReader(*conn);
        log().info("tap: client {} left the tap", conn->id());
        std::lock_guard lock(gSubMutex);
        gSubscribed = false;
    }
    std::lock_guard lock(gSubMutex);
    gPending.reset();
    gSubscribed = false;
}

json fail(char const* code, std::string const& error) {
    return {
        {"ok",    false},
        {"code",  code },
        {"error", error},
    };
}

} // namespace

void startTap() {
    if (!options().tap || gStarted.exchange(true)) return;
    gStopping = false;
    // Chain onto whatever publisher sink is installed already (the journal forwards the same way).
    // Still chained from a previous start (someone chained onto us since): gPrev is still right,
    // and taking gEmit again would make the two sinks forward to each other.
    if (!gStillChained) gPrev = tap::gEmit.exchange(&onEmit, std::memory_order_acq_rel);
    gStillChained = false;
    gWriter       = std::jthread(serve);
    tap::installNetTap();
    log().info("tap: on (a client subscribes with {op:\"tap\", action:\"subscribe\"})");
}

void stopTap() {
    if (!gStarted.exchange(false)) return;
    tap::uninstallNetTap();
    // Hand gEmit back only if nobody chained onto us since (then they keep forwarding to us, and
    // onEmit keeps forwarding to gPrev; frames just stop being queued).
    tap::EmitFn self = &onEmit;
    gStillChained    = !tap::gEmit.compare_exchange_strong(self, gPrev.load(), std::memory_order_acq_rel);
    {
        std::lock_guard lock(gSubMutex);
        gStopping = true;
    }
    gQueueCv.notify_all();
    gSubCv.notify_all();
    // The writer leaves at its next wait; a send to the subscriber fails once the WebSocket server
    // has stopped (disable() stops it first).
    gWriter.join();
}

std::string tapInfo() {
    json j{
        {"enabled",  gStarted.load()               },
        {"subscribed", [] {
             std::lock_guard lock(gSubMutex);
             return gSubscribed;
         }()},
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
    if (tap::gReplay.load() != nullptr) {
        auto s       = tapJournalStats();
        j["journal"] = {
            {"frames",   s.frames  },
            {"bytes",    s.bytes   },
            {"complete", s.complete},
        };
    } else {
        j["journal"] = {
            {"frames",   0    },
            {"bytes",    0    },
            {"complete", false},
        };
    }
    {
        std::lock_guard lock(gJournalMutex);
        j["lastReplay"] = gLastJournal;
    }
    return j.dump();
}

std::string tapHooks() { return tap::netTapHooks().dump(); }

json tapOp(json const& req) {
    if (!options().tap)
        return {
            {"ok",    false                                   },
            {"code",  "disabled"                              },
            {"key",   "tap"                                   },
            {"error", "the tap is off (config.json \"tap\")"},
        };
    std::string action = req.value("action", "");
    if (action.empty() || action == "status") {
        json r{
            {"ok", true}
        };
        r.update(json::parse(tapInfo()));
        return r;
    }
    if (action != "subscribe") return fail("bad_request", "unknown tap action '" + action + "' (subscribe)");
    auto conn = currentConnection();
    if (!conn) return fail("bad_request", "subscribe needs a client connection");
    {
        std::lock_guard lock(gSubMutex);
        if (gStopping) return fail("disabled", "BeDD is unloading");
        if (gSubscribed) return fail("busy", "another client is subscribed to the tap (one at a time)");
        gSubscribed = true;
    }
    // The frames start after this reply (HELLO first).
    afterReply([conn] {
        {
            std::lock_guard lock(gSubMutex);
            gPending = conn;
        }
        gSubCv.notify_all();
    });
    return {
        {"ok", true}
    };
}

// Any other sink in the chain (before ours, or chained onto ours since) takes every frame.
bool tap::frameWanted() {
    return gAttached.load(std::memory_order_relaxed) || gPrev.load(std::memory_order_relaxed)
        || tap::gEmit.load(std::memory_order_relaxed) != &onEmit;
}

} // namespace bedd
