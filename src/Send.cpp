// `send` op (allowSend): the local client sends one serverbound packet that a caller serialized.
//
// bedboo serializes text and command_request with bedrock-protocol; the bytes are the game's own
// wire format (varint header + body). The packet is rebuilt on the client thread
// (MinecraftPackets::createPacket + readNoHeader) and sent through the local player
// (LocalPlayer::sendNetworkPacket), so it takes the client's own path to the server: the remote
// connection, or the loopback sender in singleplayer and in a LAN world the client hosts.
//
// Only ids in sendAllow (default text and command_request) are sent. Packets the client itself
// owns (login, inventory_transaction, player_auth_input, item_stack_request) are always refused:
// forging them would fight the client's own prediction and bookkeeping.
#include "BeDD.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>

#include <nlohmann/json.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/service/Bedrock.h"

#include "mc/common/SubClientId.h"
#include "mc/deps/cereal/ReflectionCtx.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/NetworkSystem.h"
#include "mc/network/Packet.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

// Never sent, whatever sendAllow says: login, inventory_transaction, player_auth_input, item_stack_request.
constexpr std::array<int, 4> kAlwaysRefused{1, 30, 144, 147};

// The client ticks its level ~20 times a second while in a world; longer without a tick = not in one.
constexpr auto kTickStale = 1s;

// One packet handed to the client thread; the pipe thread waits for `done`.
struct Job {
    int                id = 0;
    std::string        body;
    std::promise<json> done;
    // Pending -> Claimed (the client thread sends it) or Abandoned (the pipe thread timed out first).
    enum State : int { Pending, Claimed, Abandoned };
    std::atomic<int> state{Pending};
};

std::mutex                        gMutex;
std::deque<std::shared_ptr<Job>>  gJobs;
ll::event::ListenerPtr            gListener;
std::atomic<int64_t>              gLastTickMs{0};
std::atomic<uint64_t>             gSent{}, gRefused{}, gFailed{};
std::string                       gLastError; // guarded by gMutex

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void noteError(std::string const& e) {
    std::lock_guard lock(gMutex);
    gLastError = e;
}

json fail(std::string code, std::string const& error) {
    if (code == "refused") ++gRefused;
    else ++gFailed;
    noteError(error);
    return {
        {"ok",    false         },
        {"code",  std::move(code)},
        {"error", error         }
    };
}

bool alwaysRefused(int id) { return std::ranges::find(kAlwaysRefused, id) != kAlwaysRefused.end(); }

json allowList() {
    json a = json::array();
    for (int id : options().sendAllow)
        if (!alwaysRefused(id)) a.push_back(id);
    return a;
}

bool allowed(int id) {
    if (alwaysRefused(id)) return false;
    auto const& allow = options().sendAllow;
    return std::ranges::find(allow, id) != allow.end();
}

// Standard base64 (padding optional); nullopt on any other character.
std::optional<std::string> base64Decode(std::string_view in) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);
    if (in.size() % 4 == 1) return std::nullopt; // a lost character
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int      bits = 0;
    for (char c : in) {
        int v = value(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    if (acc & ((1u << bits) - 1)) return std::nullopt; // leftover bits: truncated or not canonical
    return out;
}

// Varint packet header: id | senderSubId << 10 | targetSubId << 12. Returns the header length (0 = bad).
size_t readHeader(std::string_view b, uint32_t& header) {
    header = 0;
    for (size_t i = 0; i < b.size() && i < 5; ++i) {
        auto c = static_cast<uint8_t>(b[i]);
        header |= static_cast<uint32_t>(c & 0x7f) << (7 * i);
        if (!(c & 0x80)) return i + 1;
    }
    return 0;
}

// On the client thread: rebuild the packet and send it through the local player.
json sendNow(Level& level, Job const& job) {
    Player* player = level.getPrimaryLocalPlayer();
    if (!player || !player->isLocalPlayer()) return fail("not_in_level", "no local player");
    std::shared_ptr<Packet> packet = MinecraftPackets::createPacket(static_cast<MinecraftPacketIds>(job.id));
    if (!packet) return fail("bad_args", "the game has no packet with id " + std::to_string(job.id));
    ReadOnlyBinaryStream stream(job.body, false);
    // Cerealized packets (text, command_request) need the network system's reflection context,
    // as the game's own receive path does; Packet::read without it is the fallback.
    Bedrock::Result<void> read;
    if (auto ns = ll::service::getNetworkSystem(true)) {
        cereal::ReflectionCtx const& ctx = *ns->mReflectionCtx;
        read = packet->readNoHeader(stream, ctx, SubClientId::PrimaryClient);
    } else {
        read = packet->read(stream);
    }
    if (!read) return fail("bad_args", std::string(packet->getName()) + " did not read: " + read.error().code().message());
    if (stream.mHasOverflowed) return fail("bad_args", std::string(packet->getName()) + " is truncated");
    // Bytes the game did not read mean the caller's layout differs from the game's: what was read is
    // not what the caller meant, so nothing is sent.
    if (stream.mReadPointer != job.body.size())
        return fail(
            "bad_args",
            std::string(packet->getName()) + " has " + std::to_string(job.body.size() - stream.mReadPointer)
                + " bytes the game does not read (protocol mismatch?)"
        );
    player->sendNetworkPacket(*packet);
    ++gSent;
    return {
        {"ok",       true                                     },
        {"packetId", job.id                                   },
        {"name",     std::string(packet->getName())           },
    };
}

void onTick(ll::event::ClientLevelTickEvent& ev) {
    gLastTickMs = nowMs();
    std::deque<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard lock(gMutex);
        if (gJobs.empty()) return;
        jobs.swap(gJobs);
    }
    for (auto& job : jobs) {
        int expected = Job::Pending;
        if (!job->state.compare_exchange_strong(expected, Job::Claimed)) continue; // abandoned
        json r;
        try {
            r = sendNow(ev.level(), *job);
        } catch (std::exception const& e) {
            r = fail("bad_args", std::string("send failed: ") + e.what());
        }
        job->done.set_value(std::move(r));
    }
}

} // namespace

void installSend() {
    if (gListener) return;
    gListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>(onTick);
    if (!gListener) log().error("send: the client level tick listener could not be added; the op will time out");
}

void uninstallSend() {
    if (!gListener) return;
    ll::event::EventBus::getInstance().removeListener(gListener);
    gListener = nullptr;
    std::lock_guard lock(gMutex);
    for (auto& job : gJobs) job->done.set_value({{"ok", false}, {"code", "disabled"}, {"error", "BeDD is unloading"}});
    gJobs.clear();
}

json sendOp(json const& req) {
    if (!options().allowSend)
        return {
            {"ok",    false                                                },
            {"code",  "disabled"                                           },
            {"key",   "allowSend"                                          },
            {"error", "send is off (set \"allowSend\": true in config.json)"}
        };
    if (!req.contains("buf")) return {{"ok", true}, {"allow", allowList()}};
    if (!req["buf"].is_string()) return fail("bad_args", "'buf' must be a base64 string");
    auto bytes = base64Decode(req["buf"].get<std::string>());
    if (!bytes) return fail("bad_args", "'buf' is not base64");
    uint32_t header = 0;
    size_t   n      = readHeader(*bytes, header);
    if (!n) return fail("bad_args", "'buf' has no packet header");
    int id = static_cast<int>(header & 0x3ff);
    if (!allowed(id)) return fail("refused", "packet id " + std::to_string(id) + " is not allowed (sendAllow)");
    auto ticking = [] { return nowMs() - gLastTickMs.load() <= std::chrono::milliseconds(kTickStale).count(); };
    if (!ticking()) return fail("not_in_level", "the client is not in a world (or it is paused)");
    auto const& t = req.contains("timeoutMs") ? req["timeoutMs"] : json(2000);
    if (!t.is_number_integer()) return fail("bad_args", "'timeoutMs' must be an integer");
    // Bounded: the pipe thread waits, and every other op waits with it.
    auto timeout = std::chrono::milliseconds(std::clamp<int64_t>(t.get<int64_t>(), 0, 10000));

    auto job  = std::make_shared<Job>();
    job->id   = id;
    job->body = bytes->substr(n);
    auto done = job->done.get_future();
    {
        std::lock_guard lock(gMutex);
        std::erase_if(gJobs, [](auto const& j) { return j->state.load() == Job::Abandoned; });
        gJobs.push_back(job);
    }
    if (done.wait_for(timeout) != std::future_status::ready) {
        int expected = Job::Pending;
        if (job->state.compare_exchange_strong(expected, Job::Abandoned)) {
            if (!ticking()) return fail("not_in_level", "the client left the world (or paused it); nothing was sent");
            return fail("timeout", "the client thread did not send it in time; nothing was sent");
        }
        // The client thread claimed it meanwhile: it is being sent, and its reply stands.
    }
    return done.get();
}

json sendStats() {
    std::lock_guard lock(gMutex);
    return {
        {"sent",      gSent.load()   },
        {"refused",   gRefused.load()},
        {"failed",    gFailed.load() },
        {"lastError", gLastError     },
    };
}

} // namespace bedd
