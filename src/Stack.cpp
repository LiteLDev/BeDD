// `stack` op (allowStack): item stack requests (inventory clicks, crafting, chest moves) that a
// caller serialized, submitted by the local client.
//
// bedboo builds item_stack_request packets with bedrock-protocol and predicts their result in its
// own inventory model. In bedd mode the real client owns the connection, so the request goes out
// through the client: the packet is rebuilt on the client thread (MinecraftPackets::createPacket +
// readNoHeader) and sent through the local player (LocalPlayer::sendNetworkPacket).
//
// Request ids. The server answers by request id, and the client numbers its own requests -1, -3,
// -5, ... (TypedClientNetId<ItemStackRequestIdTag>). Each request gets a fresh odd id from a range
// far below that sequence (kFirstId), so it never collides with a request of the client's UI; a
// slot whose stack id names a caller request (an earlier pending one, or the request itself) is
// renamed with it. The reply maps the caller's ids to the client's, and bedboo matches
// item_stack_response (seen on the tap) through that map. The client's own counter is not used:
// its sNextRawId has no symbol, and ItemStackRequestId::clientInit (virtual, no symbol either)
// crashed the game when called through the header's vtable layout.
//
// Why not ItemStackNetManagerClient. The client's manager (begin request, add action, end request,
// send batch, and the predictive containers) has no callable symbol in this game version (every
// member is MCNAPI in LeviLamina's headers), and its predictions are made by the screen
// controllers while they move items in the client's containers, not from request actions. So the
// request is sent as is ("via":"raw") and the client's view is brought back to the server's
// afterwards: when the response to one of these requests arrives (hooked
// ClientNetworkHandler::handle(ItemStackResponsePacket)), the client asks the server for its
// inventory (see resync()), as it does itself when it finds its inventory out of step.
#include "BeDD.h"

#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include <nlohmann/json.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/Listener.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/common/SubClientId.h"
#include "mc/deps/cereal/ReflectionCtx.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/network/MinecraftPacketIds.h"
#include "mc/network/MinecraftPackets.h"
#include "mc/network/NetworkSystem.h"
#include "mc/network/Packet.h"
#include "mc/network/packet/ItemStackRequestPacket.h"
#include "mc/network/packet/ItemStackResponseContainerInfo.h"
#include "mc/network/packet/ItemStackResponseSlotInfo.h"
#include "mc/network/packet/ItemStackResponseInfo.h"
#include "mc/network/packet/ItemStackResponsePacket.h"
#include "mc/world/Container.h"
#include "mc/world/PlayerUIContainer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/containers/managers/models/ContainerManagerModel.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/Level.h"

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

constexpr int kItemStackRequest = 147;
// The client ticks its level ~20 times a second while in a world; longer without a tick = not in one.
constexpr auto kTickStale = 1s;
// Caller ids remembered for renaming stack ids (a request names at most a few pending ones).
constexpr size_t kIdMapCap = 256;
// Request ids given out here: odd and negative like the client's own (-1, -3, -5, ...), but far
// below them, so they never collide with a request of the client's UI.
constexpr int kFirstId = -0x40000001;

// One packet handed to the client thread; the pipe thread waits for `done`.
struct Job {
    bool               view = false; // the client's view of the inventory instead of a send
    std::string        body;
    std::promise<json> done;
    // Pending -> Claimed (the client thread sends it) or Abandoned (the pipe thread timed out first).
    enum State : int { Pending, Claimed, Abandoned };
    std::atomic<int> state{Pending};
};

std::mutex                       gMutex;
std::deque<std::shared_ptr<Job>> gJobs;
ll::event::ListenerPtr           gListener;
bool                             gHooked = false;
std::atomic<int64_t>             gLastTickMs{0};
std::atomic<uint64_t>            gSent{}, gAnswered{}, gRejected{}, gFailed{}, gResyncs{};

// Client thread only (the client level tick and the client's packet handlers both run there).
std::unordered_map<int, int> gIdMap; // caller request id -> client request id
std::deque<int>              gIdOrder;
std::unordered_set<int>      gAwaiting; // client ids sent here whose response has not arrived
std::deque<int>              gAwaitingOrder;
std::atomic<bool>            gResyncDue{false};

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

json fail(std::string code, std::string const& error) {
    ++gFailed;
    return {
        {"ok",    false          },
        {"code",  std::move(code)},
        {"error", error          }
    };
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
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc  = 0;
    int      bits = 0;
    for (char c : in) {
        int v = value(c);
        if (v < 0) return std::nullopt;
        acc   = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// Varint packet header: id | senderSubId << 10 | targetSubId << 12. Returns the header length (0 = bad).
size_t readHeader(std::string_view b, uint32_t& header) {
    header = 0;
    for (size_t i = 0; i < b.size() && i < 5; ++i) {
        auto c  = static_cast<uint8_t>(b[i]);
        header |= static_cast<uint32_t>(c & 0x7f) << (7 * i);
        if (!(c & 0x80)) return i + 1;
    }
    return 0;
}

void remember(int callerId, int clientId) {
    if (gIdMap.insert_or_assign(callerId, clientId).second) gIdOrder.push_back(callerId);
    while (gIdOrder.size() > kIdMapCap) {
        gIdMap.erase(gIdOrder.front());
        gIdOrder.pop_front();
    }
}

// A slot's stack id that names a caller request (pending, or the current one) names the client's.
void renameSlot(ItemStackRequestCereal::SlotInfoData& slot) {
    auto& v = slot.mNetIdVariant->mVariant.get();
    if (v.index() != 1) return; // a server stack id (or a legacy request id) stays
    auto& id = std::get<ItemStackRequestId>(v);
    if (auto it = gIdMap.find(id.mRawId); it != gIdMap.end()) id.mRawId = it->second;
}

template <class A>
concept HasSource = requires(A a) { a.mSource; };
template <class A>
concept HasDestination = requires(A a) { a.mDestination; };

// The next request id of ours (see kFirstId).
int nextClientId(ItemStackRequestId& id) {
    static int sNext = kFirstId;
    id.mRawId = sNext;
    sNext       = sNext > std::numeric_limits<int>::min() + 1 ? sNext - 2 : kFirstId;
    return id.mRawId;
}

// On the client thread: rebuild the packet, give its requests client ids and send it.
json sendNow(Level& level, Job const& job) {
    Player* player = level.getPrimaryLocalPlayer();
    if (!player || !player->isLocalPlayer()) return fail("not_in_level", "no local player");
    std::shared_ptr<Packet> base = MinecraftPackets::createPacket(static_cast<MinecraftPacketIds>(kItemStackRequest));
    if (!base) return fail("bad_args", "the game has no item_stack_request packet");
    ReadOnlyBinaryStream stream(job.body, false);
    // Cerealized packet: it needs the network system's reflection context, as the game's own
    // receive path does.
    auto ns = ll::service::getNetworkSystem(true);
    if (!ns) return fail("not_in_level", "no network system");
    cereal::ReflectionCtx const& ctx  = *ns->mReflectionCtx;
    Bedrock::Result<void>        read = base->readNoHeader(stream, ctx, SubClientId::PrimaryClient);
    if (!read) return fail("bad_args", "item_stack_request did not read: " + read.error().code().message());
    if (stream.mHasOverflowed) return fail("bad_args", "item_stack_request is truncated");
    // Bytes left over mean the caller's layout differs from the game's: the parse is not trusted.
    if (stream.mReadPointer != job.body.size())
        return fail(
            "bad_args",
            "item_stack_request has " + std::to_string(job.body.size() - stream.mReadPointer)
                + " bytes the game does not read (another protocol version?)"
        );
    auto& packet   = static_cast<ItemStackRequestPacket&>(*base);
    auto& requests = packet.mRequests.get();
    if (requests.empty()) return fail("bad_args", "item_stack_request has no request");

    json ids = json::array();
    json map = json::object();
    for (auto& request : requests) {
        auto& id       = request.mClientRequestId.get();
        int   callerId = id.mRawId;
        int   clientId = nextClientId(id);
        remember(callerId, clientId);
        for (auto& action : request.mActions.get()) {
            std::visit(
                [](auto& a) {
                    using A = std::remove_cvref_t<decltype(a)>;
                    if constexpr (HasSource<A>) renameSlot(a.mSource.get());
                    if constexpr (HasDestination<A>) renameSlot(a.mDestination.get());
                },
                action
            );
        }
        // Forget the oldest when answers never came (a disconnect): at most kIdMapCap are tracked.
        if (gAwaiting.insert(clientId).second) gAwaitingOrder.push_back(clientId);
        while (gAwaitingOrder.size() > kIdMapCap) {
            gAwaiting.erase(gAwaitingOrder.front());
            gAwaitingOrder.pop_front();
        }
        ids.push_back(clientId);
        map[std::to_string(callerId)] = clientId;
    }
    player->sendNetworkPacket(packet);
    gSent += requests.size();
    if (options().trace) log().info("stack: sent {} request(s) {}", requests.size(), map.dump());
    return {
        {"ok",         true                                 },
        {"via",        "raw"                                },
        {"requestIds", std::move(ids)                       },
        {"map",        std::move(map)                       },
    };
}

json itemJson(ItemStack const& item, int slot) {
    json  j{{"slot", slot}, {"item", item.getRawNameId()}, {"count", item.mCount}};
    auto& v = item.mNetIdVariant->mVariant.get();
    if (v.index() == 0) j["stackId"] = std::get<ItemStackNetId>(v).mRawId;
    else if (v.index() == 1) j["stackId"] = std::get<ItemStackRequestId>(v).mRawId; // predicted by a request
    return j;
}

json itemsJson(Container const& c, int begin, int end) {
    json a = json::array();
    for (int i = begin; i < end && i < c.getContainerSize(); ++i)
        if (auto const& item = c.getItem(i); !item.isNull() && item.mCount > 0) a.push_back(itemJson(item, i));
    return a;
}

// On the client thread: what the client shows (for checking that it agrees with the server).
json viewNow(Level& level) {
    Player* player = level.getPrimaryLocalPlayer();
    if (!player || !player->isLocalPlayer()) return fail("not_in_level", "no local player");
    json r{
        {"ok",        true                                              },
        // hotbar 0-8 and main inventory 9-35, as item_stack_request numbers them
        {"inventory", itemsJson(player->getInventory(), 0, 36)          },
        // the player UI container: 0 cursor, 28-31 the 2x2 crafting grid, 50 the created output
        {"ui",        itemsJson(player->mPlayerUIContainer.get(), 0, 64)},
    };
    if (auto const& model = player->mContainerManager.get()) {
        json items  = json::array();
        auto copies = model->getItemCopies();
        for (int i = 0; i < static_cast<int>(copies.size()); ++i)
            if (!copies[i].isNull() && copies[i].mCount > 0) items.push_back(itemJson(copies[i], i));
        r["open"] = {
            {"containerId", static_cast<int>(model->getContainerId())  },
            {"type",        static_cast<int>(model->getContainerType())},
            {"items",       std::move(items)                           },
        };
    }
    return r;
}

// inventory_transaction {legacy_request_id 0 (and no legacy slots), type inventory_mismatch, no actions},
// the bytes bedrock-protocol writes for it and the game reads back in full: what the client
// sends itself when it finds its inventory out of step. The server answers with the player's whole
// inventory (inventory_content), which the client applies like any other.
constexpr char kInventoryMismatch[] = {0x00, 0x00, 0x01, 0x00};

void resync(Level& level) {
    Player* player = level.getPrimaryLocalPlayer();
    if (!player || !player->isLocalPlayer()) return;
    std::shared_ptr<Packet> packet = MinecraftPackets::createPacket(MinecraftPacketIds::InventoryTransaction);
    auto                    ns     = ll::service::getNetworkSystem(true);
    if (!packet || !ns) return;
    ReadOnlyBinaryStream stream(std::string_view(kInventoryMismatch, sizeof(kInventoryMismatch)), false);
    if (!packet->readNoHeader(stream, *ns->mReflectionCtx, SubClientId::PrimaryClient) || stream.mHasOverflowed
        || stream.mReadPointer != sizeof(kInventoryMismatch)) {
        log().error("stack: the inventory resync packet did not read");
        return;
    }
    player->sendNetworkPacket(*packet);
    ++gResyncs;
    if (options().trace) log().info("stack: inventory resync requested");
}

void onTick(ll::event::ClientLevelTickEvent& ev) {
    gLastTickMs = nowMs();
    if (gResyncDue.exchange(false)) resync(ev.level());
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
            r = job->view ? viewNow(ev.level()) : sendNow(ev.level(), *job);
        } catch (std::exception const& e) {
            r = fail("bad_args", std::string("stack failed: ") + e.what());
        }
        job->done.set_value(std::move(r));
    }
}

// The response to requests sent here: the client handles it as usual (it predicted nothing for
// them, so it changes nothing), and a request the server carried out makes the client ask for its
// inventory on the next tick (one resync for every response packet that has any).
LL_TYPE_INSTANCE_HOOK(
    ItemStackResponseHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    static_cast<void (ClientNetworkHandler::*)(NetworkIdentifier const&, ItemStackResponsePacket const&)>(
        &ClientNetworkHandler::$handle
    ),
    void,
    NetworkIdentifier const&       source,
    ItemStackResponsePacket const& packet
) {
    origin(source, packet);
    for (auto const& response : packet.mResponses.get()) {
        if (!gAwaiting.erase(response.mClientRequestId->mRawId)) continue;
        ++gAnswered;
        if (response.mResult == ItemStackNetResult::Success) gResyncDue = true;
        else ++gRejected;
        if (options().trace)
            log().info(
                "stack: response {} result {}",
                response.mClientRequestId->mRawId,
                static_cast<int>(response.mResult)
            );
    }
}

} // namespace

void installStack() {
    if (gListener) return;
    gListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>(onTick);
    if (!gListener) log().error("stack: the client level tick listener could not be added; the op will time out");
    HookPause pause;
    gHooked = ItemStackResponseHook::hook(false) == 0;
    pause.end();
    if (!gHooked) log().error("stack: ClientNetworkHandler::handle(ItemStackResponsePacket) could not be hooked");
}

void uninstallStack() {
    if (gHooked) {
        HookPause pause;
        ItemStackResponseHook::unhook(false);
        gHooked = false;
    }
    if (!gListener) return;
    ll::event::EventBus::getInstance().removeListener(gListener);
    gListener = nullptr;
    std::lock_guard lock(gMutex);
    for (auto& job : gJobs) job->done.set_value({{"ok", false}, {"code", "disabled"}, {"error", "BeDD is unloading"}});
    gJobs.clear();
}

json stackOp(json const& req) {
    if (!options().allowStack)
        return {
            {"ok",      false                                                  },
            {"code",    "disabled"                                             },
            {"key",     "allowStack"                                           },
            {"enabled", false                                                  },
            {"error",   "stack is off (set \"allowStack\": true in config.json)"}
        };
    if (!req.contains("buf") && !req.contains("view"))
        return {
            {"ok",      true },
            {"enabled", true },
            {"via",     "raw"},
            {"stats",
             {{"sent", gSent.load()},
              {"answered", gAnswered.load()},
              {"rejected", gRejected.load()},
              {"failed", gFailed.load()},
              {"resyncs", gResyncs.load()}}},
        };
    auto timeout = req.contains("timeoutMs") && req["timeoutMs"].is_number_integer() ? req["timeoutMs"].get<int>() : 2000;
    if (timeout <= 0 || timeout > 60000) return fail("bad_args", "'timeoutMs' must be 1..60000");
    auto job  = std::make_shared<Job>();
    job->view = req.contains("view") && req["view"].is_boolean() && req["view"].get<bool>();
    if (!job->view) {
        if (!req.contains("buf") || !req["buf"].is_string()) return fail("bad_args", "'buf' must be a base64 string");
        auto bytes = base64Decode(req["buf"].get<std::string>());
        if (!bytes) return fail("bad_args", "'buf' is not base64");
        uint32_t header = 0;
        size_t   n      = readHeader(*bytes, header);
        if (!n) return fail("bad_args", "'buf' has no packet header");
        if (int id = static_cast<int>(header & 0x3ff); id != kItemStackRequest)
            return fail("bad_args", "packet id " + std::to_string(id) + " is not item_stack_request (147)");
        job->body = bytes->substr(n);
    }
    if (nowMs() - gLastTickMs.load() > std::chrono::milliseconds(kTickStale).count())
        return fail("not_in_level", "the client is not in a world");
    auto done = job->done.get_future();
    {
        std::lock_guard lock(gMutex);
        gJobs.push_back(job);
    }
    if (done.wait_for(std::chrono::milliseconds(timeout)) != std::future_status::ready) {
        int expected = Job::Pending;
        if (job->state.compare_exchange_strong(expected, Job::Abandoned))
            return fail("timeout", "the client thread did not send it in time");
        // The client thread claimed it meanwhile: it is being sent, and its reply stands.
    }
    return done.get();
}

} // namespace bedd
