// Loopback tap (config "tap" and "tapLoopback"): singleplayer worlds and LAN worlds this client hosts.
//
// The client then talks to the server in its own process through the LocalConnector, and the
// packets the remote tap (NetworkConnection::receivePacket / NetworkSystem::_sendInternal) sees on a
// server link never show up. LoopbackPacketSender is where both sides hand their packets over:
//
// * The server's sender (its mNetwork is the ServerNetworkSystem): $sendToClient (both overloads),
//   $sendToClients, $sendBroadcast (both overloads) and $sendTo are published as CLIENTBOUND, but
//   only what goes to the local player: the NetworkIdentifier must be a connection of type Local on
//   the server, so a LAN guest's traffic (a Remote connection) is never published.
// * The client's sender (its mNetwork is the ClientNetworkSystem): $sendToServer, $send and $sendTo
//   are published as SERVERBOUND_ECHO, except login, the handshakes and request_network_settings.
//
// Nothing is on the wire here, so every packet is re-serialized as NetworkSystem::send does it: the
// varint header (id | senderSub << 10 | targetSub << 12), then Packet::write with the sending
// NetworkSystem's ReflectionCtx. Frames carry the flags Reserialized and Local.
//
// Only while the client's link is Local (tap::gLinkType). The tap core sets it; NetworkSystem
// $onNewOutgoingConnection / $onConnectionClosed hooks here set it the same way (from the client's
// connection type), so the loopback tap also works on its own. A sender method may call another
// one: a thread_local guard publishes a packet once, at the outermost hook that published it.
//
// Without a tap pipe (tap::gEmit null) frames are only counted (state.stats.tapLoopback). Trace
// mode also writes the first frames, framed as on the tap pipe, to <mod>/data/tap-loopback.bin.
#include "BeDD.h"
#include "TapSink.h"

#include <cstdio>
#include <cstring>
#include <share.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/entity/components/UserEntityIdentifierComponent.h"
#include "mc/network/ClientNetworkSystem.h"
#include "mc/network/LoopbackPacketSender.h"
#include "mc/network/NetworkConnection.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/NetworkIdentifierWithSubId.h"
#include "mc/network/NetworkPeer.h"
#include "mc/network/NetworkSystem.h"
#include "mc/network/Packet.h"
#include "mc/network/ServerNetworkSystem.h"

namespace bedd {

namespace {

using tap::Kind;
using tap::Link;

constexpr size_t kTraceLogFrames  = 50;
constexpr size_t kTraceDumpFrames = 4000;
constexpr size_t kTraceDumpBytes  = 64ull << 20;

std::atomic<bool>     gInstalled{false};
std::atomic<int>      gHooks{};
std::atomic<uint64_t> gClientbound{}, gEcho{}, gBytes{}, gUnpublished{}, gGuest{};
// The local player got start_game since the link came up: broadcasts reach it from then on.
std::atomic<bool> gJoined{false};

// The client's server link (onNewOutgoingConnection), to match onConnectionClosed.
std::mutex                       gLinkMutex;
std::optional<NetworkIdentifier> gServerId;

// Trace mode only.
std::mutex   gTraceMutex;
size_t       gTraceLogged = 0, gTraceDumped = 0, gTraceDumpBytes = 0;
std::FILE*   gTraceFile = nullptr;

// The packet this thread's outermost sender hook published, while the original method runs.
thread_local Packet const* tPublished = nullptr;

NetworkSystem& networkOf(LoopbackPacketSender const& s) {
    return std::visit([](auto const& ref) -> NetworkSystem& { return ref.get(); }, static_cast<ClientOrServerNetworkSystemRef::BaseT const&>(s.mNetwork.get()));
}

bool isServerSide(LoopbackPacketSender const& s) {
    return std::holds_alternative<ClientOrServerNetworkSystemRef::ServerRefT>(static_cast<ClientOrServerNetworkSystemRef::BaseT const&>(s.mNetwork.get()));
}

bool isClientSystem(NetworkSystem const* ns) {
    auto client = ll::service::getNetworkSystem(true);
    return client && client.as_ptr() == ns;
}


void setLink(Link link) {
    if (tap::gLinkType.exchange(static_cast<int>(link), std::memory_order_acq_rel) != static_cast<int>(link)) gJoined = false;
}

/// The client is in a world it hosts: its NetworkSystem has a connection of type Local.
bool clientHasLocalConnection() {
    auto client = ll::service::getNetworkSystem(true);
    if (!client) return false;
    std::lock_guard lock(client->mConnectionsMutex.get());
    for (auto const& c : client->mConnections.get()) {
        if (c && c->mType == NetworkConnection::Type::Local) return true;
    }
    return false;
}

/// While no link is known yet (the server already sends while the client's connect call has not
/// returned, or the link came up before the hooks were installed), a local connection of the
/// client counts. Only the link hooks write gLinkType, as the tap core's do.
bool localLink() {
    int type = tap::gLinkType.load(std::memory_order_acquire);
    if (type == static_cast<int>(Link::Local)) return true;
    return type == static_cast<int>(Link::None) && clientHasLocalConnection();
}

/// On the server: id is the local player's connection (not a LAN guest's).
bool isLocalPeer(NetworkSystem& server, NetworkIdentifier const& id) {
    auto* conn = server.getConnectionFromId(id);
    return conn && conn->mType == NetworkConnection::Type::Local;
}

void appendVarUInt(std::string& out, uint32_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

std::string reserialize(NetworkSystem& ns, Packet const& packet, SubClientId targetSub) {
    std::string bytes;
    bytes.reserve(64);
    auto id = static_cast<uint32_t>(packet.getId()) & 0x3ff;
    appendVarUInt(bytes, id | (static_cast<uint32_t>(packet.mSenderSubId) & 3) << 10 | (static_cast<uint32_t>(targetSub) & 3) << 12);
    BinaryStream stream(bytes);
    packet.write(stream, *ns.mReflectionCtx.get());
    return bytes;
}

void traceFrame(Kind kind, uint8_t flags, std::string const& bytes) {
    std::lock_guard lock(gTraceMutex);
    if (gTraceLogged < kTraceLogFrames) {
        ++gTraceLogged;
        log().info("trace tap loopback: {} id={} len={}", kind == Kind::Clientbound ? "in" : "out", tap::packetIdOf(bytes), bytes.size());
    }
    if (gTraceDumped >= kTraceDumpFrames || gTraceDumpBytes >= kTraceDumpBytes) return;
    if (!gTraceFile && gTraceDumped == 0) {
        auto dir = ll::mod::NativeMod::current()->getDataDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto path = dir / "tap-loopback.bin";
        gTraceFile = _wfsopen(path.c_str(), L"wb", _SH_DENYWR); // readable while the game runs
        if (!gTraceFile) {
            gTraceDumped = kTraceDumpFrames; // no dump; do not retry for every packet
            log().warn("trace tap loopback: cannot write {}", path.string());
            return;
        }
        log().info("trace tap loopback: writing the first {} frames to {}", kTraceDumpFrames, path.string());
    }
    if (!gTraceFile) return;
    // The tap pipe's framing: u32 len | u8 kind | u8 flags | u16 0 | u64 seq | u64 tUs | payload (seq/tUs 0).
    std::string head(24, '\0');
    auto len = static_cast<uint32_t>(bytes.size() + 20);
    std::memcpy(head.data(), &len, 4);
    head[4] = static_cast<char>(kind);
    head[5] = static_cast<char>(flags);
    std::fwrite(head.data(), 1, head.size(), gTraceFile);
    std::fwrite(bytes.data(), 1, bytes.size(), gTraceFile);
    ++gTraceDumped;
    gTraceDumpBytes += bytes.size() + 24;
    if (gTraceDumped >= kTraceDumpFrames || gTraceDumpBytes >= kTraceDumpBytes) {
        std::fclose(gTraceFile);
        gTraceFile = nullptr;
        log().info("trace tap loopback: wrote {} frames", gTraceDumped);
    }
}

bool traceWants() {
    if (!options().trace) return false;
    std::lock_guard lock(gTraceMutex);
    return gTraceLogged < kTraceLogFrames || (gTraceDumped < kTraceDumpFrames && gTraceDumpBytes < kTraceDumpBytes);
}

/// Publish one packet (or count it when no tap pipe takes frames).
void publish(Kind kind, NetworkSystem& ns, Packet const& packet, SubClientId targetSub) {
    auto id = static_cast<uint32_t>(packet.getId()) & 0x3ff;
    if (kind == Kind::Clientbound) {
        gClientbound.fetch_add(1, std::memory_order_relaxed);
        if (id == 11) gJoined = true; // start_game
    } else {
        gEcho.fetch_add(1, std::memory_order_relaxed);
    }
    bool emit  = tap::gEmit.load(std::memory_order_acquire) != nullptr;
    bool trace = traceWants();
    if (!emit) gUnpublished.fetch_add(1, std::memory_order_relaxed);
    if (!emit && !trace) return;
    auto    bytes = reserialize(ns, packet, targetSub);
    uint8_t flags = tap::Reserialized | tap::Local;
    gBytes.fetch_add(bytes.size(), std::memory_order_relaxed);
    if (trace) traceFrame(kind, flags, bytes);
    if (emit) tap::emit(kind, flags, std::move(bytes));
}

/// Runs a sender method; the packet it published is not published again by the hooks it calls
/// (other packets sent meanwhile on this thread still are).
template <class F>
void guarded(Packet const& packet, bool published, F&& original) {
    if (!published) {
        original();
        return;
    }
    struct Restore {
        Packet const* previous;
        ~Restore() { tPublished = previous; }
    } restore{tPublished};
    tPublished = &packet;
    original();
}

/// Not yet published by an outer sender hook on this thread.
bool fresh(Packet const& packet) { return tPublished != &packet; }

// Clientbound: whether the server's packet to `id` reaches the local player. Counts LAN guests.
bool toLocalPlayer(LoopbackPacketSender& s, NetworkIdentifier const& id, Packet const& packet) {
    if (!fresh(packet) || !isServerSide(s) || !localLink()) return false;
    if (isLocalPeer(networkOf(s), id)) return true;
    gGuest.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void clientbound(LoopbackPacketSender& s, Packet const& packet, SubClientId targetSub) {
    publish(Kind::Clientbound, networkOf(s), packet, targetSub);
}

// Serverbound echo from the client's own sender. Never login (1), the handshakes (3, 4) or
// request_network_settings (193).
bool echoes(LoopbackPacketSender& s, Packet const& packet) {
    if (!fresh(packet) || isServerSide(s)) return false;
    auto id = static_cast<uint32_t>(packet.getId()) & 0x3ff;
    if (id == 1) gJoined = false; // login: a new world is being joined, whoever tracks the link
    if (!localLink()) return false;
    return id != 1 && id != 3 && id != 4 && id != 193;
}

void echo(LoopbackPacketSender& s, Packet const& packet) {
    publish(Kind::ServerboundEcho, networkOf(s), packet, SubClientId::PrimaryClient);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Client link type
// ---------------------------------------------------------------------------------------------
LL_TYPE_INSTANCE_HOOK(
    TapLoopbackOutgoingHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::$onNewOutgoingConnection,
    bool,
    NetworkIdentifier const&       id,
    std::shared_ptr<NetworkPeer>&& peer
) {
    NetworkPeer* raw = peer.get();
    bool         ok  = origin(id, std::move(peer));
    if (ok && isClientSystem(this)) {
        auto* conn  = getConnectionFromId(id);
        bool  local = (conn && conn->mType == NetworkConnection::Type::Local) || (raw && raw->isLocal());
        {
            std::lock_guard lock(gLinkMutex);
            gServerId.emplace(id);
        }
        setLink(local ? Link::Local : Link::Remote);
        log().info("tap loopback: the client's link is {}", local ? "local" : "remote");
    }
    return ok;
}

LL_TYPE_INSTANCE_HOOK(
    TapLoopbackClosedHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::$onConnectionClosed,
    void,
    NetworkIdentifier const&                 id,
    ::Connection::DisconnectFailReason const discoReason,
    std::string const&                       messageFromServer,
    std::string const&                       messageBodyOverride,
    bool                                     skipDisconnectMessage,
    Json::Value const&                       sessionSummary
) {
    if (isClientSystem(this)) {
        std::lock_guard lock(gLinkMutex);
        if (gServerId && *gServerId == id) {
            gServerId.reset();
            setLink(Link::None);
        }
    }
    origin(id, discoReason, messageFromServer, messageBodyOverride, skipDisconnectMessage, sessionSummary);
}

// ---------------------------------------------------------------------------------------------
// Clientbound (the server's sender)
// ---------------------------------------------------------------------------------------------
LL_TYPE_INSTANCE_HOOK(
    TapLoopbackSendToClientHook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    static_cast<void (LoopbackPacketSender::*)(NetworkIdentifier const&, Packet const&, SubClientId)>(&LoopbackPacketSender::$sendToClient),
    void,
    NetworkIdentifier const& id,
    Packet const&            packet,
    SubClientId              recipientSubId
) {
    bool local = toLocalPlayer(*this, id, packet);
    if (local) clientbound(*this, packet, recipientSubId);
    guarded(packet, local, [&] { origin(id, packet, recipientSubId); });
}

LL_TYPE_INSTANCE_HOOK(
    TapLoopbackSendToUserHook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    static_cast<void (LoopbackPacketSender::*)(UserEntityIdentifierComponent const*, Packet const&)>(&LoopbackPacketSender::$sendToClient),
    void,
    UserEntityIdentifierComponent const* userIdentifier,
    Packet const&                        packet
) {
    bool local = userIdentifier && toLocalPlayer(*this, userIdentifier->mNetworkId.get(), packet);
    if (local) clientbound(*this, packet, userIdentifier->mClientSubId);
    guarded(packet, local, [&] { origin(userIdentifier, packet); });
}

LL_TYPE_INSTANCE_HOOK(
    TapLoopbackSendToClientsHook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    &LoopbackPacketSender::$sendToClients,
    void,
    std::vector<NetworkIdentifierWithSubId> const& ids,
    Packet const&                                  packet
) {
    NetworkIdentifierWithSubId const* local = nullptr;
    for (auto const& target : ids) {
        if (toLocalPlayer(*this, target.id, packet) && !local) local = &target;
    }
    if (local) clientbound(*this, packet, local->subClientId);
    guarded(packet, local != nullptr, [&] { origin(ids, packet); });
}

LL_TYPE_INSTANCE_HOOK(
    TapLoopbackBroadcastHook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    static_cast<void (LoopbackPacketSender::*)(Packet const&)>(&LoopbackPacketSender::$sendBroadcast),
    void,
    Packet const& packet
) {
    // To every player in the world: the local player too, once it got start_game.
    bool local = fresh(packet) && isServerSide(*this) && gJoined && localLink();
    if (local) clientbound(*this, packet, SubClientId::PrimaryClient);
    guarded(packet, local, [&] { origin(packet); });
}

LL_TYPE_INSTANCE_HOOK(
    TapLoopbackBroadcastExceptHook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    static_cast<void (LoopbackPacketSender::*)(NetworkIdentifier const&, SubClientId, Packet const&)>(&LoopbackPacketSender::$sendBroadcast),
    void,
    NetworkIdentifier const& exceptId,
    SubClientId              exceptSubid,
    Packet const&            packet
) {
    bool local = fresh(packet) && isServerSide(*this) && gJoined && localLink()
              && !(exceptSubid == SubClientId::PrimaryClient && isLocalPeer(networkOf(*this), exceptId));
    if (local) clientbound(*this, packet, SubClientId::PrimaryClient);
    guarded(packet, local, [&] { origin(exceptId, exceptSubid, packet); });
}

// sendTo: to a client on the server's sender, to the server on the client's.
LL_TYPE_INSTANCE_HOOK(
    TapLoopbackSendToHook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    &LoopbackPacketSender::$sendTo,
    void,
    NetworkIdentifier const& id,
    SubClientId              subid,
    Packet&                  packet
) {
    bool published = false;
    if (toLocalPlayer(*this, id, packet)) {
        clientbound(*this, packet, subid);
        published = true;
    } else if (echoes(*this, packet)) {
        echo(*this, packet);
        published = true;
    }
    guarded(packet, published, [&] { origin(id, subid, packet); });
}

// ---------------------------------------------------------------------------------------------
// Serverbound echo (the client's sender)
// ---------------------------------------------------------------------------------------------
LL_TYPE_INSTANCE_HOOK(
    TapLoopbackSendToServerHook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    &LoopbackPacketSender::$sendToServer,
    void,
    Packet& packet
) {
    bool published = echoes(*this, packet);
    if (published) echo(*this, packet);
    guarded(packet, published, [&] { origin(packet); });
}

LL_TYPE_INSTANCE_HOOK(
    TapLoopbackSendHook,
    ll::memory::HookPriority::Normal,
    LoopbackPacketSender,
    &LoopbackPacketSender::$send,
    void,
    Packet& packet
) {
    bool published = echoes(*this, packet);
    if (published) echo(*this, packet);
    guarded(packet, published, [&] { origin(packet); });
}

namespace {

struct LoopbackHook {
    char const* name;
    int (*hook)(bool);
    bool (*unhook)(bool);
    bool installed = false;
};

std::vector<LoopbackHook>& loopbackHooks() {
    static std::vector<LoopbackHook> hooks{
        {"NetworkSystem::onNewOutgoingConnection",       &TapLoopbackOutgoingHook::hook,        &TapLoopbackOutgoingHook::unhook       },
        {"NetworkSystem::onConnectionClosed",            &TapLoopbackClosedHook::hook,          &TapLoopbackClosedHook::unhook         },
        {"LoopbackPacketSender::sendToClient(id)",       &TapLoopbackSendToClientHook::hook,    &TapLoopbackSendToClientHook::unhook   },
        {"LoopbackPacketSender::sendToClient(user)",     &TapLoopbackSendToUserHook::hook,      &TapLoopbackSendToUserHook::unhook     },
        {"LoopbackPacketSender::sendToClients",          &TapLoopbackSendToClientsHook::hook,   &TapLoopbackSendToClientsHook::unhook  },
        {"LoopbackPacketSender::sendBroadcast",          &TapLoopbackBroadcastHook::hook,       &TapLoopbackBroadcastHook::unhook      },
        {"LoopbackPacketSender::sendBroadcast(except)",  &TapLoopbackBroadcastExceptHook::hook, &TapLoopbackBroadcastExceptHook::unhook},
        {"LoopbackPacketSender::sendTo",                 &TapLoopbackSendToHook::hook,          &TapLoopbackSendToHook::unhook         },
        {"LoopbackPacketSender::sendToServer",           &TapLoopbackSendToServerHook::hook,    &TapLoopbackSendToServerHook::unhook   },
        {"LoopbackPacketSender::send",                   &TapLoopbackSendHook::hook,            &TapLoopbackSendHook::unhook           },
    };
    return hooks;
}

} // namespace

void installTapLoopback() {
    if (gInstalled.exchange(true)) return;
    std::vector<char const*> failures;
    int                      n = 0;
    {
        HookPause pause;
        for (auto& h : loopbackHooks()) {
            h.installed = h.hook(false) == 0;
            if (h.installed) ++n;
            else failures.push_back(h.name);
        }
    }
    gHooks = n;
    for (auto const* f : failures) log().error("tap loopback: {} could not be hooked", f);
    log().info("tap loopback: {}/{} hooks installed", n, loopbackHooks().size());
}

void uninstallTapLoopback() {
    if (!gInstalled.exchange(false)) return;
    {
        HookPause pause;
        for (auto& h : loopbackHooks()) {
            if (h.installed) h.unhook(false);
            h.installed = false;
        }
    }
    gHooks  = 0;
    gJoined = false;
    {
        std::lock_guard lock(gLinkMutex);
        gServerId.reset();
    }
    std::lock_guard lock(gTraceMutex);
    if (gTraceFile) std::fclose(gTraceFile);
    gTraceFile   = nullptr;
    gTraceLogged = gTraceDumped = gTraceDumpBytes = 0;
}

TapLoopbackStats tapLoopbackStats() {
    return {
        gInstalled.load(),
        gHooks.load(),
        tap::gLinkType.load(),
        gClientbound.load(),
        gEcho.load(),
        gBytes.load(),
        gUnpublished.load(),
        gGuest.load(),
    };
}

} // namespace bedd
