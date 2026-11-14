// What the `ui` op answers (issue #21), built for an agent that reads it: the screen in front as one
// line per control, small enough to read every time, with ways to look closer when it needs to.
//   * Refs: every control a reply names gets a ref (e1, e2, ...) that stays its own while its screen
//     lives, across reads and connections. A ref is never moved to another control: an action on a
//     ref whose control is gone or changed answers stale_ref.
//   * The view: JSON UI and Ore UI screens alike (the DOM of an Ore UI page is read and mapped to the
//     same roles), pruned to what can be seen, read or used; a label is folded into its button, a run
//     of alike siblings (inventory slots) into one line; capped at maxNodes lines.
//   * Closer: find (lines whose text or name contains a string), ref (the subtree of one control,
//     nothing folded), detail (every field of one control).
//   * format "json": the whole tree as objects (role, name, text, value, state, bounds, source,
//     hash, parent/children), for programs.
#include "UiOre.h"
#include "UiTree.h"

#include "BeDD.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace bedd::ui {

namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------------------------
// Refs: one per control (scene + kind + path), for as long as its scene is on the stack.
// ---------------------------------------------------------------------------------------------
std::mutex                                   gRefMutex;
std::unordered_map<std::string, std::string> gRefByKey; // identity -> ref
std::unordered_map<std::string, StoredNode>  gRefs;     // ref -> the control as last reported
uint64_t                                     gNextRef = 0;
std::atomic<uint64_t>                        gNextSnapshot{0};

std::string keyOf(Node const& n) { return std::to_string(n.sceneId) + "|" + n.kind + "|" + n.path; }

std::string refOfLocked(Node const& n) {
    auto [it, fresh] = gRefByKey.try_emplace(keyOf(n));
    if (fresh) it->second = "e" + std::to_string(++gNextRef);
    gRefs[it->second] = StoredNode{it->second, n.role, n.name, n.text, n.path, nodeHash(n), n.kind, n.sceneId};
    return it->second;
}

// Refs of scenes that left the stack are dropped (a scene's address may be reused by a new one).
void pruneRefs(std::vector<Screen> const& screens) {
    std::set<uint64_t> live;
    for (auto const& s : screens) live.insert(s.id);
    std::lock_guard lock(gRefMutex);
    for (auto it = gRefs.begin(); it != gRefs.end();) {
        if (live.count(it->second.sceneId)) {
            ++it;
            continue;
        }
        gRefByKey.erase(std::to_string(it->second.sceneId) + "|" + it->second.kind + "|" + it->second.path);
        it = gRefs.erase(it);
    }
}

// ---------------------------------------------------------------------------------------------
// Ore UI: the page's DOM (UiOre.cpp) as nodes below the screen's node.
// ---------------------------------------------------------------------------------------------
std::string domRole(json const& d) {
    std::string tag = d.value("tag", ""), role = d.value("role", ""), type = d.value("type", "");
    if (tag == "input") {
        if (type == "checkbox" || type == "radio") return "toggle";
        if (type == "range") return "slider";
        if (type == "button" || type == "submit" || type == "reset") return "button";
        return "text_input";
    }
    if (tag == "textarea" || role == "textbox" || role == "searchbox" || role == "combobox") return "text_input";
    if (role == "checkbox" || role == "switch" || role == "radio" || role == "menuitemcheckbox" || role == "menuitemradio") return "toggle";
    if (role == "slider") return "slider";
    if (tag == "button" || tag == "a" || tag == "select" || d.value("act", 0) != 0) return "button";
    if (d.contains("text")) return "text";
    return "group";
}

bool isControlRole(std::string const& r) {
    return r == "button" || r == "toggle" || r == "item" || r == "slider" || r == "text_input";
}

void spliceDom(Tree& t, size_t si, json const& dom) {
    Screen const& s = t.screens[si];
    if (s.root < 0 || !dom.contains("nodes") || !dom["nodes"].is_array() || dom["nodes"].empty()) return;
    json const&  d     = dom["nodes"];
    size_t const count = d.size();
    size_t const base  = t.nodes.size();
    Node const   top   = t.nodes[static_cast<size_t>(s.root)];
    t.nodes.resize(base + count);
    // The walker lists nodes in pre-order: a parent comes before its children.
    std::vector<int> parent(count, -1);
    for (size_t i = 0; i < count; ++i)
        if (d[i].contains("children"))
            for (auto const& c : d[i]["children"])
                if (c.is_number_integer() && c.get<size_t>() < count) parent[c.get<size_t>()] = static_cast<int>(i);
    std::vector<int> pos(count, 0); // index among its parent's children
    for (size_t i = 0; i < count; ++i)
        if (d[i].contains("children")) {
            int k = 0;
            for (auto const& c : d[i]["children"])
                if (c.is_number_integer() && c.get<size_t>() < count) pos[c.get<size_t>()] = k++;
        }
    for (size_t i = 0; i < count; ++i) {
        json const& e  = d[i];
        Node&       n  = t.nodes[base + i];
        int         p  = parent[i];
        Node const& up = p >= 0 ? t.nodes[base + static_cast<size_t>(p)] : top;
        json const  st = e.value("state", json::object());
        n.kind         = "dom";
        n.type         = e.value("tag", "");
        n.sceneId      = s.id;
        n.url          = top.url;
        n.route        = top.route;
        n.role         = domRole(e);
        n.text         = e.value("text", "");
        n.name         = e.value("name", e.value("testId", ""));
        if (e.contains("value")) n.value = e["value"];
        n.path    = (p >= 0 ? up.path : top.path) + "/" + n.type + "[" + std::to_string(pos[i]) + "]";
        n.visible = up.visible && !st.value("invisible", false) && !st.value("hidden", false);
        n.enabled = !st.value("disabled", false);
        n.focused = st.value("focused", false);
        n.selected = st.value("selected", false);
        if (n.role == "toggle") n.checked = st.value("checked", false) ? 1 : 0;
        n.interactive = e.value("act", 0) != 0;
        if (e.contains("bounds")) {
            json const& b = e["bounds"];
            n.bounds      = {b.value("x", 0.0), b.value("y", 0.0), b.value("w", 0.0), b.value("h", 0.0)};
        }
        n.parent = p >= 0 ? static_cast<int>(base) + p : s.root;
        n.depth  = up.depth + 1;
        t.nodes[static_cast<size_t>(n.parent)].children.push_back(static_cast<int>(base + i));
    }
    // Subtree ends (pre-order: a subtree is contiguous).
    std::vector<int> size(count, 1);
    for (size_t i = count; i-- > 0;)
        if (parent[i] >= 0) size[static_cast<size_t>(parent[i])] += size[i];
    for (size_t i = 0; i < count; ++i) t.nodes[base + i].subtreeEnd = static_cast<int>(base + i) + size[i];
    // A control without text of its own reads as the text inside it (not inside a nested control),
    // and those texts are then not reported again.
    for (size_t i = 0; i < count; ++i) {
        Node& n = t.nodes[base + i];
        if (!isControlRole(n.role) || !n.text.empty()) continue;
        std::string label;
        for (int j = static_cast<int>(base + i) + 1; j < n.subtreeEnd; ++j) {
            Node& c = t.nodes[static_cast<size_t>(j)];
            if (isControlRole(c.role)) {
                j = c.subtreeEnd - 1;
                continue;
            }
            if (c.role != "text" || c.text.empty() || !c.visible) continue;
            if (label.size() < 100) label += (label.empty() ? "" : " ") + c.text;
            c.text.clear();
        }
        n.text = label.size() > 120 ? label.substr(0, 120) : label;
    }
    // A control only by its hover styling (act 2), without text, beside a control with text: a layer
    // of that control (outline, shadow), not one of its own.
    for (size_t i = 0; i < count; ++i) {
        Node& n = t.nodes[base + i];
        if (d[i].value("act", 0) != 2 || !n.text.empty() || n.parent < 0) continue;
        for (int sib : t.nodes[static_cast<size_t>(n.parent)].children) {
            Node const& o = t.nodes[static_cast<size_t>(sib)];
            if (sib == static_cast<int>(base + i) || !isControlRole(o.role) || o.text.empty()) continue;
            n.role        = "group";
            n.interactive = false;
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------------------------
int effectiveTimeout(json const& req) { return std::clamp(req.value("timeoutMs", 3000), 100, 30000); }

} // namespace

std::string screenName(Screen const& s) {
    if (s.kind != "oreui" || s.route.empty()) return s.name;
    return s.route.substr(0, s.route.find('?'));
}

std::shared_ptr<Tree> readTree(WalkOptions const& opt, int timeoutMs, bool dom, json& error) {
    auto tree = std::make_shared<Tree>();
    if (!runOnClientThread([tree, opt] { *tree = walk(opt); }, std::chrono::milliseconds(timeoutMs))) {
        error = fail("not_ready", "the client thread did not read the UI within the timeout (still loading?)");
        return nullptr;
    }
    if (!tree->ok) {
        error = tree->error == "unsupported" ? fail("unsupported", "the UI of this game version could not be read")
                                             : fail("not_ready", tree->error);
        return nullptr;
    }
    Tree& t = *tree;
    if (dom) {
        // The one Ore UI view shows the topmost Ore UI screen read.
        int ore = -1;
        for (size_t i = 0; i < t.screens.size(); ++i)
            if (t.screens[i].kind == "oreui" && t.screens[i].root >= 0) ore = static_cast<int>(i);
        if (ore >= 0) {
            json d = oreUiOp({{"timeoutMs", timeoutMs}, {"maxNodes", 4000}, {"maxText", 120}});
            if (d.value("ok", false)) spliceDom(t, static_cast<size_t>(ore), d);
            else t.screens[static_cast<size_t>(ore)].domError = d.value("error", std::string("not read"));
        }
    }
    pruneRefs(t.screens);
    return tree;
}

bool refEntry(std::string const& ref, StoredNode& out) {
    std::lock_guard lock(gRefMutex);
    auto            it = gRefs.find(ref);
    if (it == gRefs.end()) return false;
    out = it->second;
    return true;
}

std::string refOf(Node const& n) {
    std::lock_guard lock(gRefMutex);
    return refOfLocked(n);
}

std::string nodeLine(Node const& n, POINT client, bool withRef) {
    std::string s = "- " + n.role;
    if (!n.text.empty()) s += " " + json(n.text).dump(-1, ' ', false, json::error_handler_t::replace);
    else if (!n.name.empty()) s += " " + n.name;
    if (n.role == "toggle") s += n.checked == 1 ? " checked" : " unchecked";
    else if (!n.value.is_null() && !(n.value.is_string() && n.value.get<std::string>().empty()))
        s += " = " + n.value.dump(-1, ' ', false, json::error_handler_t::replace);
    if (!n.enabled) s += " disabled";
    if (n.focused) s += " focused";
    if (n.selected) s += " selected";
    if (withRef && !actionsOf(n, client).empty()) s += " [" + refOf(n) + "]";
    return s;
}

namespace {

// Which nodes the view reports: what can be seen, read or used, and (compact) the groups that hold
// several of those; a text that repeats the control it is in is left out.
std::vector<char> reported(Tree const& t, bool compact, bool includeInvisible, int depth) {
    size_t                   n = t.nodes.size();
    std::vector<std::string> ctlText(n); // the text of the nearest control at or above each node
    for (size_t k = 0; k < n; ++k) {
        Node const& nd = t.nodes[k];
        if (isControlRole(nd.role)) ctlText[k] = nd.text;
        else if (nd.parent >= 0) ctlText[k] = ctlText[static_cast<size_t>(nd.parent)];
    }
    std::vector<char> keep(n, 0);
    std::vector<int>  through(n, 0);
    for (size_t k = n; k-- > 0;) {
        Node const& nd = t.nodes[k];
        if (depth >= 0 && nd.depth > depth) continue;
        if (!nd.visible && !includeInvisible) continue;
        int inner = 0;
        for (int c : nd.children) inner += keep[static_cast<size_t>(c)] ? 1 : through[static_cast<size_t>(c)];
        bool shown = nd.bounds.w > 0 && nd.bounds.h > 0;
        bool dup   = nd.role == "text" && nd.parent >= 0 && !nd.text.empty() && ctlText[static_cast<size_t>(nd.parent)] == nd.text;
        bool plain = nd.role == "group" || (nd.role == "text" && nd.text.empty()) || dup;
        keep[k]    = !compact || nd.parent < 0 || inner >= 2 || (shown && (!plain || nd.interactive));
        through[k] = inner;
    }
    return keep;
}

// A name that says nothing about the control ("button" in a group named "take_screenshot_btn").
bool genericName(Node const& n) {
    static char const* const kGeneric[] = {"", "button", "default", "panel", "image", "label", "content", "control"};
    for (char const* g : kGeneric)
        if (n.name == g) return true;
    return n.name == n.role;
}

struct Renderer {
    Tree const&              t;
    std::vector<char>        keep;
    std::vector<char>        line;  // the node gets a line
    std::vector<std::string> out;
    bool                     fold = true;

    Renderer(Tree const& tree, std::vector<char> k) : t(tree), keep(std::move(k)), line(tree.nodes.size(), 0) {
        // Controls and texts get a line. A group gets one only when it takes a click itself and holds
        // no line of its own (else it is just the frame of the control inside it); so does an Ore UI
        // control without text (a focusable wrapper around the real one).
        size_t           n = t.nodes.size();
        std::vector<char> below(n, 0);
        for (size_t i = n; i-- > 0;) {
            Node const& nd   = t.nodes[i];
            bool        base = keep[i] && nd.parent >= 0 && (nd.role != "group" || !actionsOf(nd, t.client).empty());
            bool frame       = nd.role == "group" || (nd.kind == "dom" && nd.text.empty());
            line[i]          = base && !(frame && below[i]);
            if (nd.parent >= 0 && (base || below[i])) below[static_cast<size_t>(nd.parent)] = 1;
        }
    }
    struct Unit {
        int         node;
        std::string alias; // the name of the group it is in, for a control whose own name says nothing
    };
    // The nodes that print at one level: children with a line, and the ones with a line below the
    // children without.
    void units(int k, std::string const& alias, std::vector<Unit>& u) const {
        for (int c : t.nodes[static_cast<size_t>(k)].children) {
            Node const& n = t.nodes[static_cast<size_t>(c)];
            if (line[static_cast<size_t>(c)]) u.push_back({c, alias});
            else units(c, n.role == "group" && n.kind == "jsonui" && !genericName(n) ? n.name : alias, u);
        }
    }
    std::string text(Unit const& u, bool withRef) const {
        Node const& n = t.nodes[static_cast<size_t>(u.node)];
        if (n.text.empty() && genericName(n) && !u.alias.empty()) {
            Node named = n;
            named.name = u.alias;
            std::string s = nodeLine(named, t.client, false);
            if (withRef && !actionsOf(n, t.client).empty()) s += " [" + refOf(n) + "]";
            return s;
        }
        return nodeLine(n, t.client, withRef);
    }
    void children(int k, int indent, std::string const& alias = {}) {
        std::vector<Unit> u;
        units(k, alias, u);
        std::string pad(static_cast<size_t>(indent) * 2, ' ');
        for (size_t i = 0; i < u.size();) {
            size_t j = i + 1;
            if (fold) {
                std::string like = text(u[i], false);
                while (j < u.size() && text(u[j], false) == like) ++j;
            }
            if (j - i >= 6) { // a run of alike lines: one line; the ref of the node they are in lists them
                out.push_back(pad + text(u[i], false) + " x" + std::to_string(j - i) + " [in " + refOf(t.nodes[static_cast<size_t>(k)]) + "]");
                i = j;
                continue;
            }
            for (; i < j; ++i) {
                out.push_back(pad + text(u[i], true));
                children(u[i].node, indent + 1);
            }
        }
    }
};


std::string screenHeader(Tree const& t, size_t si) {
    Screen const& s = t.screens[si];
    std::string   h = "screen " + screenName(s);
    if (s.kind == "oreui") h += " (Ore UI)";
    if (!s.ready) h += " (not ready: read again)";
    if (!s.domError.empty()) h += " (page not read: " + s.domError + ")";
    return h;
}

// The lines, capped: past max, the first 3/4 and the last 1/4 stay (a dialog's buttons are last).
json linesReply(std::vector<std::string> lines, int max, std::string const& more) {
    bool truncated = static_cast<int>(lines.size()) > max;
    if (truncated) {
        int  tail = max / 4, head = max - tail;
        auto cut  = static_cast<int>(lines.size()) - head - tail;
        std::vector<std::string> kept(lines.begin(), lines.begin() + head);
        kept.push_back("... " + std::to_string(cut) + " lines cut: " + more);
        kept.insert(kept.end(), lines.end() - tail, lines.end());
        lines = std::move(kept);
    }
    std::string view;
    for (auto const& l : lines) view += l + "\n";
    return {
        {"ok",        true     },
        {"view",      view     },
        {"truncated", truncated}
    };
}

int maxLines(json const& req) { return std::clamp(req.value("maxNodes", 80), 1, 100000); }

WalkOptions walkOptions(json const& req) {
    WalkOptions opt;
    opt.scope            = req.value("scope", std::string("active"));
    opt.includeInvisible = req.value("includeInvisible", false);
    opt.debug            = req.value("debug", false);
    return opt;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// {op:"ui"} and {op:"ui", find}: the screens in scope.
json viewOf(Tree const& t, json const& req) {
    bool        includeInvisible = req.value("includeInvisible", false);
    std::string find             = lower(req.value("find", std::string()));
    Renderer    r{t, reported(t, req.value("compact", true), includeInvisible, req.value("depth", -1))};
    std::vector<std::string> under;
    for (size_t si = t.screens.size(); si-- > 0;) {
        Screen const& s = t.screens[si];
        if (s.root < 0) {
            under.push_back(screenName(s));
            continue;
        }
        r.out.push_back(screenHeader(t, si));
        if (find.empty()) {
            r.children(s.root, 0);
            if (s.kind == "oreui" && t.nodes[static_cast<size_t>(s.root)].children.empty() && s.domError.empty())
                r.out.push_back("(nothing read on this page)");
            continue;
        }
        int found = 0;
        for (int k = s.root; k < static_cast<int>(t.nodes.size()); ++k) {
            Node const& n = t.nodes[static_cast<size_t>(k)];
            if (n.sceneId != s.id || k == s.root || (!n.visible && !includeInvisible)) continue;
            if (n.role == "group" && actionsOf(n, t.client).empty()) continue;
            std::string value = n.value.is_string() ? n.value.get<std::string>() : std::string();
            if (lower(n.text).find(find) == std::string::npos && lower(n.name).find(find) == std::string::npos
                && lower(value).find(find) == std::string::npos)
                continue;
            r.out.push_back(nodeLine(n, t.client, true));
            ++found;
        }
        if (!found) r.out.push_back("(no match)");
    }
    if (!under.empty() && req.value("scope", std::string("active")) == "active") {
        std::string u = "under:";
        for (size_t i = 0; i < under.size(); ++i) u += (i ? ", " : " ") + under[i];
        r.out.push_back(u);
    }
    return linesReply(std::move(r.out), maxLines(req), "look closer with {\"op\":\"ui\",\"find\":\"<text>\"} or {\"op\":\"ui\",\"ref\":\"<ref>\"}, or raise maxNodes");
}

int findLiveNode(Tree const& t, StoredNode const& s) {
    for (size_t i = 0; i < t.nodes.size(); ++i) {
        Node const& n = t.nodes[i];
        if (n.sceneId == s.sceneId && n.kind == s.kind && n.path == s.path) return static_cast<int>(i);
    }
    return -1;
}

// {op:"ui", ref} (no action): one control and everything below it, nothing folded; with detail,
// every field of it.
json refView(json const& req) {
    std::string ref = req["ref"].get<std::string>();
    StoredNode  s;
    if (!refEntry(ref, s))
        return fail("stale_ref", "no ref '" + ref + "' (its screen closed, or no reply named it): read again with {\"op\":\"ui\"}");
    WalkOptions opt      = walkOptions(req);
    opt.scope            = surfaceId(s.sceneId);
    opt.includeInvisible = true;
    json err;
    auto tree = readTree(opt, effectiveTimeout(req), s.kind == "dom", err);
    if (!tree) return err;
    Tree const& t   = *tree;
    int         idx = findLiveNode(t, s);
    if (idx < 0) return fail("stale_ref", "'" + ref + "' is gone: read again with {\"op\":\"ui\"}");
    Node const& n = t.nodes[static_cast<size_t>(idx)];
    if (req.value("detail", false)) {
        json j     = nodeJson(n, t.client);
        j["ref"]   = ref;
        json above = json::array();
        for (int p = n.parent; p >= 0; p = t.nodes[static_cast<size_t>(p)].parent) {
            Node const& a = t.nodes[static_cast<size_t>(p)];
            above.push_back(a.text.empty() ? a.name : a.text);
        }
        j["ancestors"] = std::move(above);
        return {
            {"ok",   true},
            {"node", std::move(j)}
        };
    }
    Renderer r{t, reported(t, true, req.value("includeInvisible", false), -1)};
    r.fold = false;
    r.out.push_back(nodeLine(n, t.client, true) + (actionsOf(n, t.client).empty() ? " [" + refOf(n) + "]" : ""));
    r.children(idx, 1);
    return linesReply(std::move(r.out), maxLines(req), "raise maxNodes, or look for one control with {\"op\":\"ui\",\"find\":\"<text>\"}");
}

// format "json": the tree as objects, for programs.
json jsonView(Tree const& t, json const& req) {
    int               maxNodes = std::max(1, req.value("maxNodes", options().uiMaxNodes));
    std::vector<char> keep     = reported(t, req.value("compact", true), req.value("includeInvisible", false), req.value("depth", -1));
    size_t            n        = t.nodes.size();
    std::vector<std::string> refs(n);
    size_t                   count     = 0;
    bool                     truncated = t.truncated;
    {
        std::lock_guard lock(gRefMutex);
        for (size_t k = 0; k < n; ++k) {
            if (!keep[k]) continue;
            if (count >= static_cast<size_t>(maxNodes)) {
                truncated = true;
                break;
            }
            refs[k] = refOfLocked(t.nodes[k]);
            ++count;
        }
    }
    std::vector<int> up(n, -1);
    for (size_t k = 0; k < n; ++k) {
        int p = t.nodes[k].parent;
        while (p >= 0 && refs[static_cast<size_t>(p)].empty()) p = t.nodes[static_cast<size_t>(p)].parent;
        up[k] = p;
    }
    json nodes = json::object(), roots = json::array();
    for (size_t k = 0; k < n; ++k) {
        if (refs[k].empty()) continue;
        json j        = nodeJson(t.nodes[k], t.client);
        j["ref"]      = refs[k];
        j["children"] = json::array();
        j["parent"]   = up[k] >= 0 ? json(refs[static_cast<size_t>(up[k])]) : json(nullptr);
        if (up[k] >= 0) nodes[refs[static_cast<size_t>(up[k])]]["children"].push_back(refs[k]);
        else roots.push_back(refs[k]);
        nodes[refs[k]] = std::move(j);
    }
    json screens = json::array();
    for (auto const& s : t.screens) {
        json js{
            {"id",          surfaceId(s.id)},
            {"kind",        s.kind         },
            {"screen",      s.name         },
            {"interactive", s.absorbsInput },
            {"modal",       s.modal        },
        };
        if (s.rawName != s.name) js["rawScreen"] = s.rawName;
        if (!s.route.empty()) js["route"] = s.route;
        if (!s.ready) js["ready"] = false;
        if (!s.domError.empty()) js["pageError"] = s.domError;
        if (!s.debug.is_null()) js["debug"] = s.debug;
        screens.push_back(std::move(js));
    }
    return {
        {"ok",        true                                                                                             },
        {"snapshot",  "ui-" + std::to_string(++gNextSnapshot)                                                          },
        {"tick",      worldPose().tick                                                                                 },
        {"screens",   std::move(screens)                                                                               },
        {"active",    t.active >= 0 ? json(surfaceId(t.screens[static_cast<size_t>(t.active)].id)) : json(nullptr)},
        {"roots",     std::move(roots)                                                                                 },
        {"nodes",     std::move(nodes)                                                                                 },
        {"truncated", truncated                                                                                        },
        {"client",    {{"w", t.client.x}, {"h", t.client.y}}                                                          },
    };
}

} // namespace

json screenView(json const& req, uint64_t* front) {
    WalkOptions opt = walkOptions(req);
    json        err;
    auto        tree = readTree(opt, effectiveTimeout(req), true, err);
    if (!tree) return err;
    Tree const& t = *tree;
    json        r = viewOf(t, req);
    if (t.active >= 0) {
        r["screen"] = screenName(t.screens[static_cast<size_t>(t.active)]);
        if (front) *front = t.screens[static_cast<size_t>(t.active)].id;
    }
    return r;
}

json viewOp(json const& req) {
    for (char const* k : {"maxNodes", "depth", "timeoutMs"})
        if (req.contains(k) && !req[k].is_number_integer()) return fail("bad_args", std::string(k) + " must be an integer");
    std::string format = req.value("format", std::string("text"));
    if (format != "text" && format != "json") return fail("bad_args", "format must be 'text' or 'json'");
    if (req.contains("find") && !req["find"].is_string()) return fail("bad_args", "find must be a string");
    if (req.contains("ref")) {
        if (!req["ref"].is_string()) return fail("bad_args", "ref must be a string");
        return refView(req);
    }
    if (format == "json") {
        json err;
        auto tree = readTree(walkOptions(req), effectiveTimeout(req), true, err);
        if (!tree) return err;
        return jsonView(*tree, req);
    }
    return screenView(req);
}

} // namespace bedd::ui
