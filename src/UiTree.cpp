// The `ui` op (config "ui"): a snapshot of the game's UI as a tree of nodes with refs (issue #21).
// The walk itself (UiJson.cpp) runs on the client thread: a ClientInstance::update hook runs the
// jobs the control-pipe threads queue, which wait for them with a timeout. This file turns the walk
// into the reply: nodes pruned (compact), capped (maxNodes), numbered e1.. in paint order, hashed;
// and it keeps the last snapshot of each control-pipe client so actions can name refs of it.
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

// ---------------------------------------------------------------------------------------------
// Snapshots: the last one of each control-pipe client.
// ---------------------------------------------------------------------------------------------
std::mutex                         gSnapMutex;
std::map<uint64_t, StoredSnapshot> gSnaps; // guarded by gSnapMutex
std::atomic<uint64_t>              gNextSnapshot{0};
std::once_flag                     gGoneOnce;

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

json actionsOf(Node const& n, POINT client) {
    json a = json::array();
    if (!n.visible || n.kind != "jsonui") return a;
    // A plain panel that takes input is clickable too (a custom control), unless it is one of the
    // window-sized input catchers every screen has.
    bool panel = n.interactive && n.bounds.w * n.bounds.h < 0.5 * client.x * client.y;
    if (n.role == "button" || n.role == "toggle" || n.role == "item" || n.role == "slider" || n.role == "text_input" || panel)
        a.push_back("click");
    if (n.role == "text_input") a.push_back("setText");
    if (n.role == "scroll") a.push_back("scroll");
    return a;
}


} // namespace

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
    if (n.kind == "jsonui") {
        source["path"] = n.path;
        source["type"] = n.type;
        if (n.collectionIndex >= 0) {
            source["collectionIndex"] = n.collectionIndex;
            source["collectionName"]  = n.collectionName;
        }
    } else {
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

bool lookupRef(std::string const& snapshot, std::string const& ref, StoredNode& out, json& error) {
    std::lock_guard lock(gSnapMutex);
    auto            it = gSnaps.find(currentClient());
    if (it == gSnaps.end() || it->second.id != snapshot) {
        std::string last = it == gSnaps.end() ? "none" : it->second.id;
        error            = fail("stale_snapshot", "snapshot '" + snapshot + "' is not this connection's last ui snapshot (" + last + "): take a new one with {op:\"ui\"}");
        error["snapshot"] = last == "none" ? json(nullptr) : json(last);
        return false;
    }
    for (auto const& n : it->second.nodes) {
        if (n.ref == ref) {
            out = n;
            return true;
        }
    }
    error = fail("stale_ref", "no ref '" + ref + "' in snapshot " + snapshot + ": take a new one with {op:\"ui\"}");
    error["candidates"] = json::array();
    return false;
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

json readOp(json const& req) {
    WalkOptions opt;
    opt.scope            = req.value("scope", std::string("active"));
    opt.includeInvisible = req.value("includeInvisible", false);
    opt.debug            = req.value("debug", false);
    int  depth           = req.value("depth", -1);
    int  maxNodes        = std::max(1, req.value("maxNodes", options().uiMaxNodes));
    bool compact         = req.value("compact", true);
    auto timeout         = std::chrono::milliseconds(req.value("timeoutMs", 3000));

    auto tree = std::make_shared<Tree>();
    if (!runOnClientThread([tree, opt] { *tree = walk(opt); }, timeout))
        return fail("not_ready", "the client thread did not read the UI within the timeout (still loading?)");
    if (!tree->ok) {
        if (tree->error == "unsupported") return fail("unsupported", "the UI of this game version could not be read");
        return fail("not_ready", tree->error);
    }
    Tree& t = *tree;

    // Which nodes to report: within depth; with compact, only what can be seen, read or used, and
    // the groups that hold several of those (a chain of single-child layout panels collapses into
    // the nearest reported ancestor). Every reported node keeps its full source path.
    size_t            n = t.nodes.size();
    std::vector<char> keep(n, 0);
    std::vector<int>  through(n, 0); // reported nodes a parent sees through this one
    for (size_t k = n; k-- > 0;) {
        Node const& nd = t.nodes[k];
        if (depth >= 0 && nd.depth > depth) continue;
        if (!nd.visible && !opt.includeInvisible) continue;
        int inner = 0;
        for (int c : nd.children) inner += keep[static_cast<size_t>(c)] ? 1 : through[static_cast<size_t>(c)];
        // Hidden button states and unused labels are laid out at zero size: nothing to see or click.
        bool shown = nd.bounds.w > 0 && nd.bounds.h > 0;
        bool plain = nd.role == "group" || (nd.role == "text" && nd.text.empty());
        keep[k]    = !compact || nd.parent < 0 || inner >= 2 || (shown && (!plain || nd.interactive));
        through[k] = inner;
    }
    std::vector<std::string> refs(n);
    size_t                   count     = 0;
    bool                     truncated = t.truncated;
    for (size_t k = 0; k < n; ++k) {
        if (!keep[k]) continue;
        if (count >= static_cast<size_t>(maxNodes)) {
            truncated = true;
            break;
        }
        refs[k] = "e" + std::to_string(++count);
    }
    // The nearest reported ancestor of each reported node.
    std::vector<int> up(n, -1);
    for (size_t k = 0; k < n; ++k) {
        int p = t.nodes[k].parent;
        while (p >= 0 && refs[static_cast<size_t>(p)].empty()) p = t.nodes[static_cast<size_t>(p)].parent;
        up[k] = p;
    }

    StoredSnapshot snap;
    snap.id    = "ui-" + std::to_string(++gNextSnapshot);
    json nodes = json::object();
    json roots = json::array();
    for (size_t k = 0; k < n; ++k) {
        if (refs[k].empty()) continue;
        Node const& nd = t.nodes[k];
        json        j  = nodeJson(nd, t.client);
        j["ref"]       = refs[k];
        j["children"]  = json::array();
        j["parent"]    = up[k] >= 0 ? json(refs[static_cast<size_t>(up[k])]) : json(nullptr);
        if (up[k] >= 0) nodes[refs[static_cast<size_t>(up[k])]]["children"].push_back(refs[k]);
        else roots.push_back(refs[k]);
        snap.nodes.push_back({refs[k], nd.role, nd.name, nd.text, nd.path, j["hash"].get<std::string>(), nd.kind, nd.sceneId});
        nodes[refs[k]] = std::move(j);
    }
    json screens = json::array();
    for (auto const& s : t.screens) {
        json js{
            {"id",          surfaceId(s.id)},
            {"kind",        s.kind       },
            {"screen",      s.name       },
            {"interactive", s.absorbsInput},
            {"modal",       s.modal      },
        };
        if (s.rawName != s.name) js["rawScreen"] = s.rawName;
        if (!s.route.empty()) js["route"] = s.route;
        if (!s.ready) js["ready"] = false;
        if (!s.debug.is_null()) js["debug"] = s.debug;
        screens.push_back(std::move(js));
    }
    json r{
        {"ok",        true                                                                        },
        {"snapshot",  snap.id                                                                     },
        {"tick",      worldPose().tick                                                            },
        {"screens",   std::move(screens)                                                          },
        {"active",    t.active >= 0 ? json(surfaceId(t.screens[static_cast<size_t>(t.active)].id)) : json(nullptr)},
        {"roots",     std::move(roots)                                                            },
        {"nodes",     std::move(nodes)                                                            },
        {"truncated", truncated                                                                   },
        {"client",    {{"w", t.client.x}, {"h", t.client.y}}                                      },
    };
    {
        std::lock_guard lock(gSnapMutex);
        gSnaps[currentClient()] = std::move(snap);
    }
    return r;
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
    std::call_once(ui::gGoneOnce, [] {
        onClientGone([](uint64_t client) {
            std::lock_guard lock(ui::gSnapMutex);
            ui::gSnaps.erase(client);
        });
    });
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
    return ui::readOp(req);
}

} // namespace bedd
