// Semantic UI actions (issue #21): click, setText and scroll on a ref of the connection's last `ui`
// snapshot, and the `mouse` op aimed at a ref. A ref is resolved against the live tree (read again
// on the client thread): the control must still be there with the same hash (else stale_ref), be
// visible and on screen, and its target point must not be covered by an input control painted
// after it or by a screen above that takes input (else occluded; "force": true skips that check).
// Then the action runs as plain `mouse`, `key` and `text` requests through handleControlLine: the
// same virtual pointer, GameInput buttons and window messages as a script sending them itself, with
// the same per-connection ownership of held keys and buttons.
#include "UiTree.h"

#include "BeDD.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

namespace bedd::ui {

namespace {

using json = nlohmann::json;

// The live tree of scene `scene` and the screens above it (what can cover it).
std::shared_ptr<Tree> liveTree(json const& req, uint64_t scene, json& error) {
    WalkOptions opt;
    opt.fromScene        = scene;
    opt.includeInvisible = true;
    auto tree            = std::make_shared<Tree>();
    auto timeout         = std::chrono::milliseconds(req.value("timeoutMs", 3000));
    if (!runOnClientThread([tree, opt] { *tree = walk(opt); }, timeout)) {
        error = fail("not_ready", "the client thread did not read the UI within the timeout (still loading?)");
        return nullptr;
    }
    if (!tree->ok) {
        error = tree->error == "unsupported" ? fail("unsupported", "the UI of this game version could not be read") : fail("not_ready", tree->error);
        return nullptr;
    }
    return tree;
}

int findLive(Tree const& t, StoredNode const& s) {
    for (size_t i = 0; i < t.nodes.size(); ++i) {
        Node const& n = t.nodes[i];
        if (n.sceneId == s.sceneId && n.kind == s.kind && n.path == s.path) return static_cast<int>(i);
    }
    return -1;
}

int screenOf(Tree const& t, int idx) {
    for (size_t s = 0; s < t.screens.size(); ++s)
        if (t.screens[s].root >= 0 && idx >= t.screens[s].root && idx < t.screens[s].end) return static_cast<int>(s);
    return -1;
}

// Nodes of the live tree that look like the one a stale ref named.
json candidates(Tree const& t, StoredNode const& s) {
    json c = json::array();
    for (auto const& n : t.nodes) {
        if (c.size() >= 5) break;
        if (!n.visible || n.role != s.role) continue;
        bool same = (!s.name.empty() && n.name == s.name) || (!s.text.empty() && n.text == s.text);
        if (!same) continue;
        c.push_back({
            {"role",    n.role                              },
            {"name",    n.name                              },
            {"text",    n.text                              },
            {"surface", surfaceId(n.sceneId)},
            {"path",    n.path                              },
        });
    }
    return c;
}

// Where on the control to point: anchor (center, top_left, top, top_right, left, right,
// bottom_left, bottom, bottom_right) inside its bounds, plus offsetX/offsetY pixels.
POINT pointOf(Node const& n, json const& req) {
    std::string a  = req.value("anchor", std::string("center"));
    double      fx = 0.5, fy = 0.5;
    if (a.find("left") != std::string::npos) fx = 0;
    if (a.find("right") != std::string::npos) fx = 1;
    if (a.find("top") != std::string::npos) fy = 0;
    if (a.find("bottom") != std::string::npos) fy = 1;
    double x = n.bounds.x + std::clamp(fx * n.bounds.w, 0.0, std::max(0.0, n.bounds.w - 1));
    double y = n.bounds.y + std::clamp(fy * n.bounds.h, 0.0, std::max(0.0, n.bounds.h - 1));
    return {static_cast<LONG>(std::floor(x)) + req.value("offsetX", 0), static_cast<LONG>(std::floor(y)) + req.value("offsetY", 0)};
}

// The node or screen that would take a click at p instead of node idx (-1 = none).
json occluderOf(Tree const& t, int idx, POINT p) {
    int si = screenOf(t, idx);
    if (si < 0) return nullptr;
    Screen const& s  = t.screens[static_cast<size_t>(si)];
    double        px = p.x + 0.5, py = p.y + 0.5;
    auto          describe = [&](Node const& n) {
        return json{
            {"role",    n.role                              },
            {"name",    n.name                              },
            {"text",    n.text                              },
            {"surface", surfaceId(n.sceneId)},
            {"path",    n.path                              },
            {"bounds",  {{"x", std::lround(n.bounds.x)}, {"y", std::lround(n.bounds.y)}, {"w", std::lround(n.bounds.w)}, {"h", std::lround(n.bounds.h)}}},
        };
    };
    // A control that would take the click: one with a known input component, at the point. Screens
    // also hold large invisible input catchers (a full-window "take progress" button in the
    // inventory, say) that let clicks through: anything covering half the window or more is not one.
    double half  = 0.5 * t.client.x * t.client.y;
    auto   takes = [&](Node const& n) {
        bool control = n.role == "button" || n.role == "toggle" || n.role == "text_input" || n.role == "slider" || n.role == "item";
        return n.visible && n.interactive && control && n.bounds.w > 0 && n.bounds.h > 0 && n.bounds.w * n.bounds.h < half
            && n.bounds.contains(px, py);
    };
    // Painted after it (later in pre-order, outside its subtree) in its own screen.
    for (int j = t.nodes[static_cast<size_t>(idx)].subtreeEnd; j < s.end; ++j)
        if (takes(t.nodes[static_cast<size_t>(j)])) return describe(t.nodes[static_cast<size_t>(j)]);
    // A screen above that takes input.
    for (size_t u = static_cast<size_t>(si) + 1; u < t.screens.size(); ++u) {
        Screen const& up = t.screens[u];
        if (!up.absorbsInput) continue;
        if (up.kind != "jsonui") return {{"screen", up.name}, {"surface", surfaceId(up.id)}};
        for (int j = up.root; j >= 0 && j < up.end; ++j)
            if (takes(t.nodes[static_cast<size_t>(j)])) return describe(t.nodes[static_cast<size_t>(j)]);
    }
    return nullptr;
}

struct Target {
    std::shared_ptr<Tree> tree;
    int                   idx = -1;
    POINT                 point{};
    StoredNode            stored;
};

// The live node behind req's snapshot + ref, and the point to aim at; or the error reply.
json resolve(json const& req, bool checkOcclusion, Target& out) {
    if (!req.contains("snapshot") || !req["snapshot"].is_string() || !req.contains("ref") || !req["ref"].is_string())
        return fail("bad_request", "a ui action needs \"snapshot\" and \"ref\" (strings) from the last {op:\"ui\"}");
    json err;
    if (!lookupRef(req["snapshot"].get<std::string>(), req["ref"].get<std::string>(), out.stored, err)) return err;
    out.tree = liveTree(req, out.stored.sceneId, err);
    if (!out.tree) return err;
    Tree const& t = *out.tree;
    out.idx       = findLive(t, out.stored);
    if (out.idx < 0 || nodeHash(t.nodes[static_cast<size_t>(out.idx)]) != out.stored.hash) {
        json e = fail(
            "stale_ref",
            "'" + out.stored.ref + "' (" + out.stored.role + " '" + (out.stored.text.empty() ? out.stored.name : out.stored.text)
                + "') " + (out.idx < 0 ? "is gone" : "has changed") + ": take a new snapshot with {op:\"ui\"}"
        );
        // Its screen is gone: look for it on the screen now in front.
        std::shared_ptr<Tree> front;
        if (out.idx < 0 && t.nodes.empty()) {
            WalkOptions opt;
            front = std::make_shared<Tree>();
            if (!runOnClientThread([front, opt] { *front = walk(opt); }, std::chrono::milliseconds(req.value("timeoutMs", 3000))))
                front.reset();
        }
        e["candidates"] = candidates(front && front->ok ? *front : t, out.stored);
        return e;
    }
    Node const& n = t.nodes[static_cast<size_t>(out.idx)];
    if (n.kind != "jsonui") return fail("unsupported", "'" + out.stored.ref + "' is an Ore UI page: its controls cannot be targeted yet");
    out.point = pointOf(n, req);
    if (!n.visible || n.bounds.w <= 0 || n.bounds.h <= 0)
        return fail("occluded", "'" + out.stored.ref + "' is not visible");
    if (out.point.x < 0 || out.point.y < 0 || out.point.x >= t.client.x || out.point.y >= t.client.y) {
        json e    = fail("occluded", "'" + out.stored.ref + "' is outside the window (scroll it into view first)");
        e["point"] = {{"x", out.point.x}, {"y", out.point.y}};
        return e;
    }
    if (checkOcclusion && !req.value("force", false)) {
        if (json by = occluderOf(t, out.idx, out.point); !by.is_null()) {
            json e        = fail("occluded", "'" + out.stored.ref + "' is covered at its target point (\"force\": true clicks anyway)");
            e["occludedBy"] = std::move(by);
            e["point"]      = {{"x", out.point.x}, {"y", out.point.y}};
            return e;
        }
    }
    return nullptr;
}

// One plain request through the control protocol (same thread: same client, input lock held).
json sub(json req) {
    json r = json::parse(handleControlLine(req.dump()), nullptr, false);
    if (r.is_discarded() || !r.is_object()) return fail("internal", "bad reply from " + req.value("op", std::string()));
    return r;
}

// What changed on screen, hover and press aside: the active screen, and every visible node's text,
// state and place (a scrolled list moves its items).
std::string digest(Tree const& t) {
    std::string d;
    if (t.active >= 0) d += std::to_string(t.screens[static_cast<size_t>(t.active)].id);
    for (auto const& n : t.nodes) {
        d += nodeHash(n);
        d += n.visible ? 'v' : '-';
        d += n.enabled ? 'e' : '-';
        d += n.selected ? 's' : '-';
        d += static_cast<char>('0' + n.checked + 1);
        d += n.value.dump();
        if (n.visible) d += std::to_string(std::lround(n.bounds.x)) + "," + std::to_string(std::lround(n.bounds.y));
    }
    return d;
}

void copyTiming(json const& from, json& to) {
    for (char const* k : {"holdMs", "settleMs", "afterMs", "timeoutMs", "button", "delayMs"})
        if (from.contains(k)) to[k] = from[k];
}

// The reply of an action: the target as it is now, and whether the UI changed.
json after(json const& req, Target const& tg, json r) {
    // A label shows typed text, and a screen opens or closes, a frame or two after the input.
    std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(req.value("observeMs", 150), 0, 5000)));
    json err;
    auto now   = liveTree(req, tg.stored.sceneId, err);
    r["ok"]    = true;
    r["ref"]   = tg.stored.ref;
    r["point"] = {{"x", tg.point.x}, {"y", tg.point.y}};
    if (!now) {
        r["changed"] = nullptr;
        return r;
    }
    r["changed"] = digest(*now) != digest(*tg.tree);
    if (now->active >= 0) r["screen"] = now->screens[static_cast<size_t>(now->active)].name;
    int i = findLive(*now, tg.stored);
    r["target"] = i >= 0 ? nodeJson(now->nodes[static_cast<size_t>(i)], now->client) : json(nullptr);
    return r;
}

} // namespace

json actionOp(json const& req) {
    std::string action = req.value("action", std::string());
    if (action != "click" && action != "setText" && action != "scroll")
        return fail("bad_request", "unknown ui action '" + action + "' (click, setText, scroll)");
    if (action == "setText" && (!req.contains("text") || !req["text"].is_string())) return fail("bad_request", "setText needs \"text\"");
    Target tg;
    if (json e = resolve(req, true, tg); !e.is_null()) return e;
    json r = json::object();
    if (action == "click" || action == "setText") {
        json m{
            {"op",     "mouse"   },
            {"action", "click"   },
            {"x",      tg.point.x},
            {"y",      tg.point.y}
        };
        copyTiming(req, m);
        m.erase("delayMs");
        if (json c = sub(m); !c.value("ok", false)) return c;
    }
    if (action == "setText") {
        std::vector<json> steps;
        if (req.value("clear", true)) {
            // Select all, then delete it.
            steps.push_back({{"op", "key"}, {"key", "CTRL"}, {"action", "down"}});
            steps.push_back({{"op", "key"}, {"key", "A"}, {"action", "press"}});
            steps.push_back({{"op", "key"}, {"key", "CTRL"}, {"action", "up"}});
            steps.push_back({{"op", "key"}, {"key", "BACK"}, {"action", "press"}});
        }
        json text{{"op", "text"}, {"text", req["text"]}};
        if (req.contains("delayMs")) text["delayMs"] = req["delayMs"];
        steps.push_back(std::move(text));
        if (req.value("submit", false)) steps.push_back({{"op", "key"}, {"key", "ENTER"}, {"action", "press"}});
        bool ctrl = false; // held by a step above: let go of it whatever fails
        for (auto& s : steps) {
            if (req.contains("timeoutMs")) s["timeoutMs"] = req["timeoutMs"];
            json c = sub(s);
            if (s.value("key", "") == "CTRL") ctrl = s.value("action", "") == "down" && c.value("ok", false);
            if (!c.value("ok", false)) {
                if (ctrl) sub({{"op", "key"}, {"key", "CTRL"}, {"action", "up"}});
                return c;
            }
        }
    }
    if (action == "scroll") {
        // Positive notches scroll down (the content moves up), like a wheel turned towards you.
        int  notches = req.value("notches", 1);
        json m{
            {"op",      "mouse"   },
            {"action",  "wheel"   },
            {"x",       tg.point.x},
            {"y",       tg.point.y},
            {"notches", -notches  }
        };
        copyTiming(req, m);
        m.erase("button");
        m.erase("delayMs");
        if (json c = sub(m); !c.value("ok", false)) return c;
    }
    r["action"] = action;
    return after(req, tg, std::move(r));
}

} // namespace bedd::ui

namespace bedd {

nlohmann::json uiMouseOp(nlohmann::json const& req) {
    if (!options().ui)
        return {
            {"ok",    false                                                  },
            {"code",  "disabled"                                             },
            {"key",   "ui"                                                   },
            {"error", "mouse with a ref is off (config.json \"ui\": true)"}
        };
    std::string action = req.value("action", std::string("move"));
    ui::Target  tg;
    // Pressing on a covered control would press what covers it; moving there is a plain hover.
    if (auto e = ui::resolve(req, action == "click" || action == "down", tg); !e.is_null()) return e;
    nlohmann::json m = req;
    for (char const* k : {"id", "ref", "snapshot", "anchor", "offsetX", "offsetY", "rx", "ry", "force"}) m.erase(k);
    m["x"]           = tg.point.x;
    m["y"]           = tg.point.y;
    nlohmann::json r = ui::sub(m);
    if (!r.value("ok", false)) return r;
    return ui::after(req, tg, std::move(r));
}

} // namespace bedd
