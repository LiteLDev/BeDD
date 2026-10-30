// Tap capture from the client's own network stack (config "tap").
//
// The client's NetworkSystem (ll::service::getNetworkSystem(true)) opens one outgoing connection,
// to the server it plays on: that NetworkConnection is the link the tap follows. Its mType tells a
// remote server (RakNet/NetherNet) from a local one (singleplayer, or a LAN world this client
// hosts, which LoopbackPacketSender serves without serializing).
//
// * NetworkSystem::$onNewOutgoingConnection: the link comes up (LINK_UP marker; gLinkType).
//   $onConnectionClosed / $onAllConnectionsClosed (the server's side) and closeConnection /
//   disconnect (the client leaving): it goes down (LINK_DOWN, once).
// * NetworkConnection::receivePacket (after it, HasData): one decompressed, decrypted game packet
//   of the remote link, varint header + body: a CLIENTBOUND frame.
// * NetworkSystem::_sendInternal: a packet the client sends on that link, already serialized
//   (header + body): a SERVERBOUND_ECHO frame. login, the handshakes and request_network_settings
//   are left out.
//
// The hooks only copy the bytes and hand them to tap::emit (Tap.cpp queues them for the pipe).
#include "BeDD.h"
#include "Tap.h"
#include "TapSink.h"

#include <mutex>
#include <optional>

#include <magic_enum/magic_enum.hpp>

#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/network/NetworkConnection.h"
#include "mc/network/NetworkIdentifier.h"
#include "mc/network/NetworkPeer.h"
#include "mc/network/NetworkSystem.h"

namespace bedd::tap {

namespace {

using json = nlohmann::json;

std::atomic<NetworkSystem*>      gClientSystem{nullptr}; // the client's NetworkSystem, once it connected
std::atomic<NetworkConnection*>  gServerConn{nullptr};   // its connection to the server (compared, never read)
std::mutex                       gLinkMutex;
std::optional<NetworkIdentifier> gServerId; // id of gServerConn
std::string                      gAddress;  // "ip:port" of a remote server

std::atomic<uint64_t> gIn{}, gOut{}, gTraced{};

constexpr uint64_t kTraceRecords = 50;

bool isClientSystem(NetworkSystem* s) {
    auto client = ll::service::getNetworkSystem(true);
    return client && s == client.as_ptr();
}

void capture(Kind kind, std::string const& bytes) {
    (kind == Kind::Clientbound ? gIn : gOut).fetch_add(1, std::memory_order_relaxed);
    // Trace mode: the first packets of the link in order, to compare with a BDS capture (bedboo's
    // join_flat: network_settings, ..., play_status, resource_packs_info, ..., start_game).
    if (options().trace) {
        uint64_t n = gTraced.fetch_add(1, std::memory_order_relaxed);
        if (n < kTraceRecords) {
            log().info(
                "tap trace #{} {} id={} len={}",
                n + 1,
                kind == Kind::Clientbound ? "in" : "out",
                packetIdOf(bytes),
                bytes.size()
            );
        }
    }
    // The copy for the frame only when someone takes it (a reader, or a sink chained before ours).
    if (frameWanted()) emit(kind, 0, bytes);
}

std::string linkTypeName(int type) { return type == static_cast<int>(Link::Local) ? "local" : "remote"; }

std::string reasonText(Connection::DisconnectFailReason reason, std::string const& message) {
    std::string text{magic_enum::enum_name(reason)};
    if (text.empty()) text = std::to_string(static_cast<int>(reason));
    if (!message.empty()) text += ": " + message;
    return text;
}

void linkDown(std::string reason);

void linkUp(NetworkSystem* system, NetworkIdentifier const& id) {
    NetworkConnection* conn = system->getConnectionFromId(id);
    if (!conn) return;
    if (gServerConn.load()) linkDown("replaced by a new connection");
    Link type = conn->mType == NetworkConnection::Type::Local ? Link::Local : Link::Remote;
    json marker{
        {"m",    "LINK_UP"                           },
        {"type", linkTypeName(static_cast<int>(type))},
    };
    {
        std::lock_guard lock(gLinkMutex);
        gServerId = id;
        gAddress.clear();
        if (type == Link::Remote) {
            try {
                gAddress = id.getIPAndPort();
            } catch (...) {}
            if (!gAddress.empty()) marker["address"] = gAddress;
        }
        gLinkType = static_cast<int>(type);
    }
    gClientSystem = system;
    gTraced       = 0; // trace the first packets of every link
    gServerConn   = conn;
    log().info(
        "tap: link up ({}{}{})",
        linkTypeName(static_cast<int>(type)),
        marker.contains("address") ? " " : "",
        marker.value("address", "")
    );
    emit(Kind::Marker, 0, marker.dump());
}

// Once per link: the game may report its end more than once (closeConnection, then the callbacks).
void linkDown(std::string reason) {
    if (!gServerConn.exchange(nullptr)) return;
    {
        std::lock_guard lock(gLinkMutex);
        gServerId.reset();
        gAddress.clear();
        gLinkType = static_cast<int>(Link::None);
    }
    log().info("tap: link down ({})", reason);
    emit(
        Kind::Marker,
        0,
        json{
            {"m",      "LINK_DOWN"      },
            {"reason", std::move(reason)}
    }.dump()
    );
}

bool isServerId(NetworkIdentifier const& id) {
    std::lock_guard lock(gLinkMutex);
    return gServerId && *gServerId == id;
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    TapOutgoingConnectionHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::$onNewOutgoingConnection,
    bool,
    NetworkIdentifier const&       id,
    std::shared_ptr<NetworkPeer>&& peer
) {
    bool ok = origin(id, std::move(peer));
    if (ok && isClientSystem(this)) linkUp(this, id);
    return ok;
}

LL_TYPE_INSTANCE_HOOK(
    TapConnectionClosedHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::$onConnectionClosed,
    void,
    NetworkIdentifier const&               id,
    Connection::DisconnectFailReason const discoReason,
    std::string const&                     messageFromServer,
    std::string const&                     messageBodyOverride,
    bool                                   skipDisconnectMessage,
    Json::Value const&                     sessionSummary
) {
    if (this == gClientSystem.load() && isServerId(id)) linkDown(reasonText(discoReason, messageFromServer));
    origin(id, discoReason, messageFromServer, messageBodyOverride, skipDisconnectMessage, sessionSummary);
}

LL_TYPE_INSTANCE_HOOK(
    TapAllConnectionsClosedHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::$onAllConnectionsClosed,
    void,
    Connection::DisconnectFailReason discoReason,
    bool                             skipDisconnectMessage
) {
    if (this == gClientSystem.load()) linkDown(reasonText(discoReason, {}));
    origin(discoReason, skipDisconnectMessage);
}

// The client leaving the server itself (the callbacks above report the server's side).
LL_TYPE_INSTANCE_HOOK(
    TapCloseConnectionHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::closeConnection,
    void,
    NetworkIdentifier const&         id,
    Connection::DisconnectFailReason discoReason,
    std::string const&               messageFromServer
) {
    if (this == gClientSystem.load() && isServerId(id)) linkDown(reasonText(discoReason, messageFromServer));
    origin(id, discoReason, messageFromServer);
}

// Leaving the world from the pause screen (Save & Quit) drops every connection this way.
LL_TYPE_INSTANCE_HOOK(
    TapDisconnectHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::disconnect,
    void
) {
    if (this == gClientSystem.load()) linkDown("disconnect");
    origin();
}

LL_TYPE_INSTANCE_HOOK(
    TapReceivePacketHook,
    ll::memory::HookPriority::Normal,
    NetworkConnection,
    &NetworkConnection::receivePacket,
    NetworkPeer::DataStatus,
    std::string&                                                  receiveBuffer,
    std::shared_ptr<std::chrono::steady_clock::time_point> const& timepointPtr
) {
    auto status = origin(receiveBuffer, timepointPtr);
    if (status == NetworkPeer::DataStatus::HasData && this == gServerConn.load(std::memory_order_acquire)
        && gLinkType.load(std::memory_order_relaxed) == static_cast<int>(Link::Remote)) {
        capture(Kind::Clientbound, receiveBuffer);
    }
    return status;
}

LL_TYPE_INSTANCE_HOOK(
    TapSendInternalHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    &NetworkSystem::_sendInternal,
    void,
    NetworkIdentifier const& id,
    Packet const&            packet,
    std::string const&       data
) {
    if (this == gClientSystem.load(std::memory_order_acquire)
        && gLinkType.load(std::memory_order_relaxed) == static_cast<int>(Link::Remote) && isServerId(id)) {
        switch (packetIdOf(data)) {
        case 1:   // login
        case 3:   // server_to_client_handshake
        case 4:   // client_to_server_handshake
        case 193: // request_network_settings
            break;
        default:
            capture(Kind::ServerboundEcho, data);
        }
    }
    origin(id, packet, data);
}

namespace {

struct TapHook {
    char const* name;
    int (*hook)(bool);
    bool (*unhook)(bool);
    std::atomic<bool> installed{false}; // read by the pipe thread ("state", "tap")
};

TapHook gHooks[]{
    {"NetworkSystem::onNewOutgoingConnection", &TapOutgoingConnectionHook::hook,   &TapOutgoingConnectionHook::unhook  },
    {"NetworkSystem::onConnectionClosed",      &TapConnectionClosedHook::hook,     &TapConnectionClosedHook::unhook    },
    {"NetworkSystem::onAllConnectionsClosed",  &TapAllConnectionsClosedHook::hook, &TapAllConnectionsClosedHook::unhook},
    {"NetworkSystem::closeConnection",         &TapCloseConnectionHook::hook,      &TapCloseConnectionHook::unhook     },
    {"NetworkSystem::disconnect",              &TapDisconnectHook::hook,           &TapDisconnectHook::unhook          },
    {"NetworkConnection::receivePacket",       &TapReceivePacketHook::hook,        &TapReceivePacketHook::unhook       },
    {"NetworkSystem::_sendInternal",           &TapSendInternalHook::hook,         &TapSendInternalHook::unhook        },
};

} // namespace

bool installNetTap() {
    {
        HookPause pause;
        for (auto& h : gHooks) {
            if (!h.installed) h.installed = h.hook(false) == 0;
        }
    }
    bool all = true;
    for (auto const& h : gHooks) {
        if (!h.installed) {
            log().error("tap: {} could not be hooked", h.name);
            all = false;
        }
    }
    return all;
}

void uninstallNetTap() {
    {
        HookPause pause;
        for (auto& h : gHooks) {
            if (h.installed) h.unhook(false);
            h.installed = false;
        }
    }
    gServerConn   = nullptr;
    gClientSystem = nullptr;
    std::lock_guard lock(gLinkMutex);
    gServerId.reset();
    gAddress.clear();
    gLinkType = static_cast<int>(Link::None);
}

json netTapHooks() {
    json j = json::object();
    for (auto const& h : gHooks) j[h.name] = h.installed.load();
    return j;
}

json netTapLink() {
    std::lock_guard lock(gLinkMutex); // type and address of the same link
    int             type = gLinkType.load();
    json            j{
        {"state", type == static_cast<int>(Link::None) ? "down" : "up"},
    };
    if (type != static_cast<int>(Link::None)) j["type"] = linkTypeName(type);
    if (!gAddress.empty()) j["address"] = gAddress;
    return j;
}

uint64_t netTapIn() { return gIn.load(); }

uint64_t netTapOut() { return gOut.load(); }

} // namespace bedd::tap
