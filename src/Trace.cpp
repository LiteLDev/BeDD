// Trace mode: log what reaches the game's input layer (LeviLamina's input events fire in
// HIDControllerGameCoreDesktop::onKeyDown/onKeyUp and MouseDevice::feed), marked synthetic or real.
#include "Headless.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"

namespace headless {

namespace {
ll::event::ListenerPtr gKeyListener;
ll::event::ListenerPtr gMouseListener;

char const* origin() { return inSyntheticDispatch() ? "synthetic" : "real"; }
} // namespace

void startInputTrace() {
    if (!options().trace) return;
    auto& bus     = ll::event::EventBus::getInstance();
    gKeyListener  = bus.emplaceListener<ll::event::KeyInputEvent>([](ll::event::KeyInputEvent& ev) {
        log().info("trace input: key {} {} ({})", ev.keyCode(), ev.isDown() ? "down" : "up", origin());
    });
    gMouseListener = bus.emplaceListener<ll::event::MouseInputEvent>([](ll::event::MouseInputEvent& ev) {
        // Plain moves are frequent; log buttons, wheel and relative moves only.
        if (ev.actionButtonId() == 0) return;
        log().info(
            "trace input: mouse action={} data={} pos=({},{}) delta=({},{}) ({})",
            static_cast<int>(ev.actionButtonId()),
            static_cast<int>(ev.buttonData()),
            ev.x(),
            ev.y(),
            ev.dx(),
            ev.dy(),
            origin()
        );
    });
}

void stopInputTrace() {
    auto& bus = ll::event::EventBus::getInstance();
    if (gKeyListener) bus.removeListener(gKeyListener);
    if (gMouseListener) bus.removeListener(gMouseListener);
    gKeyListener   = nullptr;
    gMouseListener = nullptr;
}

} // namespace headless
