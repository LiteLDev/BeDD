// The client tick cadence: how often the client ticks its level now, so that "the client is in a
// world" and the default timeouts of the ops that wait for a tick follow the game speed
// (Minecraft::setSimTimeScale) instead of assuming 20 ticks a second.
//
// One ClientLevelTickEvent listener stamps every tick and keeps a smoothed interval between them;
// the pipe threads only read atomics. A gap longer than kMaxGapUs (a paused singleplayer game,
// loading another world) is no cadence and is not measured; a shorter hitch counts as at most
// 4 intervals, so one slow frame barely moves it while a slower game speed is followed within a few
// ticks. At 20 TPS and faster, and for the jitter around it, the interval reads as the nominal 50 ms,
// so nothing changes at normal speed.
#include "BeDD.h"

#include <algorithm>
#include <atomic>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"

namespace bedd {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kNominalUs = 50'000;    // 20 TPS
constexpr int64_t kSlowUs    = 75'000;    // below this the interval reads as nominal (jitter, faster speeds)
constexpr int64_t kMaxGapUs  = 3'000'000; // longer gaps are pauses, not a cadence (0.05x = 1 s a tick)
constexpr int64_t kStaleTicks = 20;       // ticks missed before the client counts as not ticking

ll::event::ListenerPtr gListener;
std::atomic<int64_t>   gLastUs{0};              // steady clock of the last tick, 0 = none yet
std::atomic<int64_t>   gIntervalUs{kNominalUs}; // smoothed interval (written on the client thread only)
std::atomic<uint64_t>  gTicks{};

int64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

void onTick() {
    int64_t now  = nowUs();
    int64_t last = gLastUs.exchange(now);
    ++gTicks;
    if (last == 0) return;
    int64_t d = now - last;
    if (d <= 0 || d > kMaxGapUs) return;
    int64_t ema = gIntervalUs.load();
    d           = std::min(d, 4 * ema);
    gIntervalUs.store(ema + (d - ema) / 4);
}

} // namespace

void startTickCadence() {
    if (gListener) return;
    gListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>(
        [](ll::event::ClientLevelTickEvent&) { onTick(); }
    );
    if (!gListener) log().error("tick cadence: the client level tick listener could not be added");
}

void stopTickCadence() {
    if (gListener) ll::event::EventBus::getInstance().removeListener(gListener);
    gListener = nullptr;
}

uint64_t clientTicks() { return gTicks.load(); }

std::chrono::microseconds clientTickInterval() {
    int64_t us = gIntervalUs.load();
    return std::chrono::microseconds(us < kSlowUs ? kNominalUs : us);
}

bool clientTickStale(std::chrono::milliseconds minimum) {
    int64_t last = gLastUs.load();
    if (last == 0) return true;
    auto limit = std::max<std::chrono::microseconds>(minimum, clientTickInterval() * kStaleTicks);
    return nowUs() - last > limit.count();
}

std::chrono::milliseconds tickScaled(std::chrono::milliseconds base, std::chrono::milliseconds cap) {
    auto interval = clientTickInterval().count();
    if (interval <= kNominalUs) return base;
    return std::min(cap, std::chrono::milliseconds(base.count() * interval / kNominalUs));
}

} // namespace bedd
