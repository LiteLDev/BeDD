// `slot`, `action` and `button` control-pipe ops (config `gameHooks`): hotbar slot, the game's own
// button actions (drop, inventory, perspective, pause) and held mouse buttons counted in game ticks.
//
// The pipe thread never touches the game: it queues a command and waits (timeoutMs, default 1000)
// until the next ClientLevelTickEvent, on the client thread, has applied it. Slot and actions call
// the ClientInputCallbacks::handle* statics the game's own key bindings call, so the client sends
// mob_equipment, drops and opens screens itself. handleSlotSelectButtonPress leaves the slot alone
// for a keyboard player (1.26.51), so `slot` then presses the hotbar key '1'..'9' instead (the
// default key map) and replies once the player holds the slot. Buttons drive the virtual GameInput mouse
// (GameInputHooks.cpp) like the `raw` op, but the tick listener releases them: after `holdTicks`
// ticks, or (`releaseOn:'blockChange'`) once the block the client's hit result pointed at when the
// button went down is gone or no longer targeted. So a hold never blocks the pipe.
#include "BeDD.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInputCallbacks.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/HitResult.h"

namespace bedd {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

// attack, use, pick: the game's default mouse bindings.
struct ButtonDef {
    char const* name;
    int         vk;
    int         gi;
};
constexpr std::array<ButtonDef, 3> kButtons{
    {{"attack", VK_LBUTTON, kGiLeft}, {"use", VK_RBUTTON, kGiRight}, {"pick", VK_MBUTTON, kGiMiddle}}
};

constexpr std::array<char const*, 5> kActions{"drop", "dropAll", "inventory", "perspective", "pause"};

struct Held {
    bool        down        = false;
    int64_t     releaseTick = -1; // -1 = no tick limit
    bool        onBlockChange = false;
    BlockPos    pos{};
    Block const* block = nullptr;
};

// One queued op, applied by the tick listener.
struct Command {
    std::string op;
    json        req;
    bool        done      = false;
    bool        cancelled = false; // the pipe gave up waiting
    json        reply;
    int         waitSlot  = -1; // slot: replied once the player holds it (hotbar key fallback)
    int64_t     waitUntil = 0;
};

std::mutex                            gMutex; // everything below
std::condition_variable               gApplied;
std::deque<std::shared_ptr<Command>>  gQueue;
std::vector<std::shared_ptr<Command>> gWaiting;    // slot commands waiting for the hotbar key
int                                   gKeyUp = -1; // hotbar key to release on the next tick
int64_t                               gKeyDownTick = 0;
std::array<Held, kButtons.size()>     gHeld{};
int64_t                               gTick = 0;
int                                   gSlot = -1;
std::chrono::steady_clock::time_point gLastTick{};

ll::event::ListenerPtr gTickListener;

json fail(std::string_view code, std::string error) {
    return {
        {"ok",    false         },
        {"code",  code          },
        {"error", std::move(error)}
    };
}

void setButton(size_t b, bool down) {
    vstate::setKey(kButtons[b].vk, down);
    gameInputMouse(0, 0, 0, kButtons[b].gi, down);
}

void releaseHeld(size_t b) {
    if (gHeld[b].down) setButton(b, false);
    gHeld[b] = {};
}

json heldNames() {
    json held = json::array();
    for (size_t b = 0; b < kButtons.size(); ++b)
        if (gHeld[b].down) held.push_back(kButtons[b].name);
    return held;
}

size_t buttonIndex(std::string const& name) {
    for (size_t b = 0; b < kButtons.size(); ++b)
        if (name == kButtons[b].name) return b;
    return kButtons.size();
}

// The hotbar key of the default key map ('1'..'9'), as a window message like the `key` op's.
void postKey(int vk, bool down) {
    UINT   sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | (static_cast<LPARAM>(sc & 0xFF) << 16);
    if (!down) lp |= (1LL << 30) | (1LL << 31);
    post(Event{.msg = static_cast<UINT>(down ? WM_KEYDOWN : WM_KEYUP), .wp = static_cast<WPARAM>(vk), .lp = lp, .vk = vk, .vkDown = down});
}

bool inLevel() { return gTick > 0 && std::chrono::steady_clock::now() - gLastTick < 1s; }

// True when the client's hit result no longer points at the block `h` was pressed on, or that block changed.
bool targetChanged(Held const& h, LocalPlayer& player) {
    HitResult const& hit = player.getLevel().getHitResult();
    if (hit.mType != HitResultType::Tile || hit.mBlock.x != h.pos.x || hit.mBlock.y != h.pos.y || hit.mBlock.z != h.pos.z)
        return true;
    Block const& now = player.getDimensionBlockSourceConst().getBlock(h.pos);
    return &now != h.block || now.isAir();
}

// Client thread, under gMutex. A null reply = the command waits (gWaiting).
json apply(Command& c, ClientInstance& client, LocalPlayer& player) {
    // Input goes to the world only, like a hand on the keyboard: not while a screen is open.
    bool inWorld = client.getMouseGrabbed() && !client.isShowingMenu();
    json r{
        {"ok",   true },
        {"tick", gTick}
    };
    if (c.op == "slot") {
        if (!inWorld) return fail("busy", "a screen is open (" + client.getTopScreenName() + ")");
        int slot = c.req["slot"].get<int>();
        ClientInputCallbacks::handleSlotSelectButtonPress(client, slot);
        gSlot = player.getSelectedItemSlot();
        if (gSlot != slot) {
            // The callback does nothing for a keyboard player (1.26.51): press the hotbar key instead.
            if (gKeyUp > 0) postKey(gKeyUp, false);
            gKeyUp       = '1' + slot;
            gKeyDownTick = gTick;
            postKey(gKeyUp, true);
            c.waitSlot  = slot;
            c.waitUntil = gTick + 10;
            return nullptr;
        }
        r["slot"] = gSlot;
        r["via"]  = "callback";
    } else if (c.op == "action") {
        std::string name = c.req["name"].get<std::string>();
        if (name != "pause" && !inWorld) return fail("busy", "a screen is open (" + client.getTopScreenName() + ")");
        if (name == "drop") ClientInputCallbacks::handleDropButtonPress(client);
        else if (name == "dropAll") ClientInputCallbacks::handleDropAllButtonPress(client);
        else if (name == "inventory") ClientInputCallbacks::handleInventoryButtonPress(client, CraftingType::Inventory);
        else if (name == "perspective") ClientInputCallbacks::handleTogglePerspectiveButtonPress(client);
        else if (name == "pause") ClientInputCallbacks::handlePauseButtonPress(client);
        r["name"] = name;
    } else { // button
        size_t      b      = buttonIndex(c.req["button"].get<std::string>());
        std::string action = c.req["action"].get<std::string>(); // down or click (up never queues)
        Held&       h      = gHeld[b];
        if (!inWorld) return fail("busy", "a screen is open (" + client.getTopScreenName() + ")");
        Held next;
        next.down = true;
        bool onBlock = c.req.value("releaseOn", "") == "blockChange";
        int  ticks   = c.req.value("holdTicks", action == "click" && !onBlock ? 2 : 0);
        if (ticks > 0) next.releaseTick = gTick + ticks;
        if (onBlock) {
            HitResult const& hit = player.getLevel().getHitResult();
            if (hit.mType != HitResultType::Tile) return fail("refused", "releaseOn blockChange: no block is targeted");
            next.onBlockChange = true;
            next.pos           = hit.mBlock;
            next.block         = &player.getDimensionBlockSourceConst().getBlock(hit.mBlock);
            r["block"]         = {
                {"x", hit.mBlock.x},
                {"y", hit.mBlock.y},
                {"z", hit.mBlock.z}
            };
        }
        // (the `release` op may have let go of it since the last tick)
        if (!h.down || !vstate::keyDown(kButtons[b].vk)) setButton(b, true);
        h = next;
        r["button"] = kButtons[b].name;
        r["held"]   = heldNames();
    }
    return r;
}

void onTick() {
    std::lock_guard lock(gMutex);
    ++gTick;
    gLastTick = std::chrono::steady_clock::now();
    auto         client = ll::service::getClientInstance();
    LocalPlayer* player = client ? client->getLocalPlayer() : nullptr;
    if (player) gSlot = player->getSelectedItemSlot();
    if (gKeyUp > 0 && gTick > gKeyDownTick) {
        postKey(gKeyUp, false);
        gKeyUp = -1;
    }

    bool notify = false;
    std::erase_if(gWaiting, [&](std::shared_ptr<Command> const& c) {
        if (gSlot != c->waitSlot && gTick < c->waitUntil && !c->cancelled) return false;
        c->reply = gSlot == c->waitSlot ? json{{"ok", true}, {"tick", gTick}, {"slot", gSlot}, {"via", "keys"}}
                                        : fail("timeout", "the hotbar key did not select the slot");
        c->done  = true;
        notify   = true;
        return true;
    });
    for (auto& c : gQueue) {
        if (c->cancelled) continue;
        try {
            c->reply = player ? apply(*c, *client, *player) : fail("not_in_level", "no local player");
        } catch (std::exception const& e) {
            c->reply = fail("bad_args", e.what());
        }
        if (c->reply.is_null()) {
            gWaiting.push_back(c);
            continue;
        }
        c->done = true;
        notify  = true;
    }
    gQueue.clear();
    if (notify) gApplied.notify_all();

    for (size_t b = 0; b < kButtons.size(); ++b) {
        Held& h = gHeld[b];
        if (!h.down) continue;
        if (!vstate::keyDown(kButtons[b].vk)) { // released by the `release` op or background off
            h = {};
            continue;
        }
        bool expired = h.releaseTick >= 0 && gTick >= h.releaseTick;
        if (expired || (h.onBlockChange && (!player || targetChanged(h, *player)))) {
            if (options().trace) log().info("button {} released at tick {}", kButtons[b].name, gTick);
            releaseHeld(b);
        }
    }
}

json disabled(std::string const& op) {
    return fail("disabled", "op '" + op + "' is off: set \"gameHooks\": true in config.json");
}

// Queue for the tick listener and wait for its reply.
json run(std::string const& op, json const& req) {
    auto timeout = std::chrono::milliseconds(std::clamp(req.value("timeoutMs", 1000), 50, 60000));
    auto c       = std::make_shared<Command>(Command{op, req});
    std::unique_lock lock(gMutex);
    if (!inLevel()) return fail("not_in_level", "not in a world");
    gQueue.push_back(c);
    if (!gApplied.wait_for(lock, timeout, [&] { return c->done; })) {
        c->cancelled = true;
        return fail("timeout", "the game did not tick within the timeout");
    }
    return c->reply;
}

} // namespace

void installGameActions() {
    if (!options().gameHooks || gTickListener) return;
    gTickListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>(
        [](ll::event::ClientLevelTickEvent&) { onTick(); }
    );
    log().info("game actions (slot, action, button): {}", gTickListener ? "on" : "listener failed");
}

void uninstallGameActions() {
    if (gTickListener) ll::event::EventBus::getInstance().removeListener(gTickListener);
    gTickListener = nullptr;
    releaseGameButtons();
}

void releaseGameButtons() {
    std::lock_guard lock(gMutex);
    for (size_t b = 0; b < kButtons.size(); ++b) releaseHeld(b);
    if (gKeyUp > 0) postKey(gKeyUp, false);
    gKeyUp = -1;
}

nlohmann::json gameActionOp(std::string const& op, nlohmann::json const& req) {
    if (!options().gameHooks || !gTickListener) return disabled(op);
    if (req.contains("timeoutMs") && !req["timeoutMs"].is_number_integer()) return fail("bad_args", "timeoutMs must be an integer");
    bool query = !req.contains(op == "slot" ? "slot" : op == "action" ? "name" : "button");
    if (op == "slot") {
        if (query) {
            std::lock_guard lock(gMutex);
            return {
                {"ok",   true                                     },
                {"slot", inLevel() && gSlot >= 0 ? json(gSlot) : json()}
            };
        }
        if (!req["slot"].is_number_integer() || req["slot"].get<int>() < 0 || req["slot"].get<int>() > 8)
            return fail("bad_args", "slot must be 0..8");
    } else if (op == "action") {
        if (query) return {{"ok", true}, {"names", kActions}};
        auto const& n = req["name"];
        if (!n.is_string() || std::ranges::find(kActions, n.get<std::string>()) == kActions.end())
            return fail("bad_args", "name must be one of drop, dropAll, inventory, perspective, pause");
    } else { // button
        if (query) {
            std::lock_guard lock(gMutex);
            return {{"ok", true}, {"held", heldNames()}};
        }
        auto const& b      = req["button"];
        std::string action = req.value("action", "click");
        if (!b.is_string() || buttonIndex(b.get<std::string>()) == kButtons.size())
            return fail("bad_args", "button must be attack, use or pick");
        if (action != "down" && action != "up" && action != "click")
            return fail("bad_args", "action must be down, up or click");
        if (req.contains("holdTicks") && (!req["holdTicks"].is_number_integer() || req["holdTicks"].get<int>() < 1))
            return fail("bad_args", "holdTicks must be a positive integer");
        if (req.contains("releaseOn") && req["releaseOn"] != "blockChange")
            return fail("bad_args", "releaseOn must be 'blockChange'");
        if (action == "up") { // also while the game does not tick (e.g. a paused singleplayer world)
            std::lock_guard lock(gMutex);
            releaseHeld(buttonIndex(b.get<std::string>()));
            return {{"ok", true}, {"button", b}, {"held", heldNames()}};
        }
        // The game reads the virtual mouse only in background mode (GameInputHooks.cpp).
        if (!backgroundMode().load())
            return fail("disabled", "button drives the virtual GameInput mouse, which needs background mode (\"background\": true)");
        json full      = req;
        full["action"] = action;
        return run(op, full);
    }
    return run(op, req);
}

} // namespace bedd
