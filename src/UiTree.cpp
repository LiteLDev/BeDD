// The `ui` op (config "ui", issue #21): the walk (UiJson.cpp) runs on the client thread, where a
// ClientInstance::update hook runs the jobs the connection threads queue (they wait with a
// timeout). This file holds that job queue and the node fields every reply uses; UiView.cpp builds
// the replies, UiActions.cpp acts on refs.
#include "UiTree.h"

#include "BeDD.h"

#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

#include "ll/api/memory/Hook.h"

#include "mc/client/game/ClientInstance.h"

namespace bedd {

namespace ui {

namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------------------------
// Client-thread jobs
// ---------------------------------------------------------------------------------------------
struct Job {
    std::function<void()> fn;
    bool                  done = false, ran = false, cancelled = false; // guarded by gJobsMutex
};

std::mutex                        gJobsMutex;
std::condition_variable           gJobsCv;
std::vector<std::shared_ptr<Job>> gJobs;           // guarded by gJobsMutex
bool                              gRunning = false; // the frame hook is installed (gJobsMutex)
std::atomic<bool>                 gHooked{false};

std::string fnvHex(std::initializer_list<std::string const*> parts) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (auto const* p : parts) {
        for (unsigned char c : *p) h = (h ^ c) * 0x100000001b3ull;
        h = (h ^ 0x1f) * 0x100000001b3ull; // field separator
    }
    char buf[20];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

json rectJson(Rect const& r) {
    return {
        {"x", std::lround(r.x)},
        {"y", std::lround(r.y)},
        {"w", std::lround(r.w)},
        {"h", std::lround(r.h)}
    };
}

double round4(double v) { return std::round(v * 10000.0) / 10000.0; }

} // namespace

json actionsOf(Node const& n, POINT client) {
    json a = json::array();
    if (!n.visible || (n.kind != "jsonui" && n.kind != "dom")) return a;
    // A plain panel that takes input is clickable too (a custom control), unless it is one of the
    // window-sized input catchers every screen has.
    bool panel = n.interactive && n.bounds.w * n.bounds.h < 0.5 * client.x * client.y;
    if (n.role == "button" || n.role == "toggle" || n.role == "item" || n.role == "slider" || n.role == "text_input" || panel)
        a.push_back("click");
    if (n.role == "text_input") a.push_back("setText");
    if (n.role == "scroll") a.push_back("scroll");
    return a;
}

json fail(char const* code, std::string const& msg) {
    return {
        {"ok",    false},
        {"code",  code },
        {"error", msg  }
    };
}

std::string surfaceId(uint64_t scene) {
    char buf[32];
    snprintf(buf, sizeof(buf), "scene-%llx", static_cast<unsigned long long>(scene));
    return buf;
}

std::string nodeHash(Node const& n) {
    return fnvHex({&n.role, &n.name, &n.text, &n.path, &n.type, &n.kind, &n.url, &n.route});
}

json nodeJson(Node const& n, POINT client) {
    double cw = client.x > 0 ? client.x : 1, ch = client.y > 0 ? client.y : 1;
    json   state{
        {"visible",    n.visible                                   },
        {"enabled",    n.enabled                                   },
        {"hovered",    n.hovered                                   },
        {"focused",    n.focused                                   },
        {"selected",   n.selected                                  },
        {"pressed",    n.pressed                                   },
        {"editable",   n.role == "text_input"                      },
        {"scrollable", n.role == "scroll"                          },
        {"checked",    n.checked < 0 ? json(nullptr) : json(n.checked == 1)},
    };
    json source{
        {"kind", n.kind}
    };
    if (n.kind != "oreui") {
        source["path"] = n.path;
        source["type"] = n.type; // dom: the tag
        if (n.collectionIndex >= 0) {
            source["collectionIndex"] = n.collectionIndex;
            source["collectionName"]  = n.collectionName;
        }
    }
    if (n.kind != "jsonui") {
        source["url"]   = n.url;
        source["route"] = n.route;
    }
    json j{
        {"role",       n.role                                                                                   },
        {"name",       n.name                                                                                   },
        {"text",       n.text                                                                                   },
        {"value",      n.value                                                                                  },
        {"state",      std::move(state)                                                                         },
        {"bounds",     rectJson(n.bounds)                                                                       },
        {"boundsNorm",
         {{"x", round4(n.bounds.x / cw)}, {"y", round4(n.bounds.y / ch)}, {"w", round4(n.bounds.w / cw)}, {"h", round4(n.bounds.h / ch)}}},
        {"actions",    actionsOf(n, client)                                                                     },
        {"surface",    surfaceId(n.sceneId)                                                                       },
        {"source",     std::move(source)                                                                        },
        {"hash",       nodeHash(n)                                                                              },
    };
    if (!n.debug.is_null()) j["debug"] = n.debug;
    return j;
}

bool runOnClientThread(std::function<void()> fn, std::chrono::milliseconds timeout) {
    auto job = std::make_shared<Job>();
    job->fn  = std::move(fn);
    std::unique_lock lock(gJobsMutex);
    if (!gRunning) return false;
    gJobs.push_back(job);
    bool done = gJobsCv.wait_for(lock, timeout, [&] { return job->done; });
    if (!done) job->cancelled = true; // it captured what it writes by value: it may still run, harmlessly
    return done && job->ran;
}

namespace {

// The client thread, after each ClientInstance::update: run what the pipe threads queued.
void drainJobs() {
    std::vector<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard lock(gJobsMutex);
        if (gJobs.empty()) return;
        jobs.swap(gJobs);
    }
    for (auto& job : jobs) {
        bool run;
        {
            std::lock_guard lock(gJobsMutex);
            run = !job->cancelled;
        }
        bool ran = false;
        if (run) {
            try {
                job->fn();
                ran = true;
            } catch (std::exception const& e) {
                log().warn("ui: a client-thread job failed: {}", e.what());
            }
        }
        {
            std::lock_guard lock(gJobsMutex);
            job->done = true;
            job->ran  = ran;
        }
    }
    gJobsCv.notify_all();
}

} // namespace

} // namespace ui

// Outside the anonymous namespace: the macro's static members would be unused there.
LL_TYPE_INSTANCE_HOOK(UiFrameHook, ll::memory::HookPriority::Normal, ClientInstance, &ClientInstance::$update, bool, bool isInitFinished, bool allowGameUpdate) {
    bool r = origin(isInitFinished, allowGameUpdate);
    if (isInitFinished) ui::drainJobs();
    return r;
}

void startUi() {
    if (!options().ui || ui::gHooked) return;
    {
        HookPause pause;
        ui::gHooked = UiFrameHook::hook(false) == 0;
    }
    if (!ui::gHooked) {
        log().error("ClientInstance::update could not be hooked: the ui op answers not_ready");
        return;
    }
    std::lock_guard lock(ui::gJobsMutex);
    ui::gRunning = true;
}

void stopUi() {
    {
        std::lock_guard lock(ui::gJobsMutex);
        ui::gRunning = false;
        for (auto& job : ui::gJobs) job->done = true; // never ran: the waiter answers not_ready
        ui::gJobs.clear();
    }
    ui::gJobsCv.notify_all();
    if (ui::gHooked) {
        HookPause pause;
        UiFrameHook::unhook(false);
        ui::gHooked = false;
    }
}

nlohmann::json uiOp(nlohmann::json const& req) {
    if (!options().ui)
        return {
            {"ok",    false                                     },
            {"code",  "disabled"                                },
            {"key",   "ui"                                      },
            {"error", "ui is off (config.json \"ui\": true)"}
        };
    if (req.contains("action")) return ui::actionOp(req);
    return ui::viewOp(req);
}

} // namespace bedd
