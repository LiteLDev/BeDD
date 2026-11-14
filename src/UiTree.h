#pragma once

// The `ui` op (config "ui", issue #21): the game's UI as a tree of controls, and semantic actions on
// them that go through the same virtual mouse, keys and text as the `mouse`, `key` and `text` ops.
//   * UiJson.cpp reads the scene stack on the client thread: JSON UI scenes (UIScene -> ScreenView ->
//     VisualTree -> UIControl) control by control, an Ore UI scene as one node (UiView.cpp adds
//     its page's elements below it).
//   * UiTree.cpp runs that read on the client thread for a connection's thread.
//   * UiView.cpp turns it into the reply (a line per control, or JSON) and keeps the refs: one per
//     control while its screen lives; an Ore UI page's DOM (UiOre.cpp) is mapped to the same roles.
//   * UiActions.cpp resolves a ref against the live tree and drives the input ops.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace bedd::ui {

/// A rectangle in client pixels (the space of `mouse` x/y).
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] bool contains(double px, double py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

/// One control (JSON UI) or one opaque Ore UI scene, as read on the client thread.
struct Node {
    std::string role = "group"; // button text text_input toggle slider scroll image item group
    std::string name;           // the control's name in its JSON UI definition
    std::string text;           // resolved display text (its own, or its first label for a button/toggle)
    nlohmann::json value;       // text_input: its text; slider: 0..1; toggle: checked
    // state
    bool visible = true; // effective: the control and all its ancestors
    bool enabled = true;
    bool hovered = false, focused = false, selected = false, pressed = false;
    int  checked = -1; // -1 = not a toggle
    // A control that takes pointer input (ScreenView's input controls): what can cover another one.
    bool interactive = false;
    Rect bounds;
    // tree (indices into Tree::nodes; a scene's nodes are contiguous, in pre-order = paint order)
    int parent     = -1;
    int subtreeEnd = 0; // one past the last descendant
    int depth      = 0;
    std::vector<int> children;
    // source
    std::string kind = "jsonui"; // jsonui | oreui (an Ore UI screen) | dom (an element of its page)
    std::string path;            // /root/child/child[2] (JSON UI)
    std::string type;            // panel label image button toggle edit_box slider scroll_view (JSON UI)
    int         collectionIndex = -1;
    std::string collectionName;
    std::string url, route; // Ore UI
    uint64_t    sceneId = 0; // the scene object's address: unique while it lives
    nlohmann::json debug; // components (request "debug": true)
};

struct Screen {
    uint64_t    id = 0; // the scene object's address
    std::string kind; // jsonui | oreui | unknown
    std::string name, rawName, route;
    bool        absorbsInput = false;
    bool        modal        = false;
    bool        ready        = true; // JSON UI: its visual tree exists
    std::string domError;            // Ore UI: why its page was not read
    int         root         = -1;   // index into Tree::nodes (-1: nothing read)
    int         end          = 0;    // one past its last node
    nlohmann::json debug;
};

struct Tree {
    bool                ok = false; // false: not_ready (no client instance / scene stack)
    std::string         error;
    std::vector<Screen> screens; // bottom to top
    int                 active = -1;
    std::vector<Node>   nodes;
    bool                truncated = false; // the walk hit its control cap
    POINT               client{};
};

struct WalkOptions {
    std::string scope = "active"; // active | all | <screen name> | scene-<id>
    bool        includeInvisible = false;
    bool        debug            = false;
    size_t      maxControls      = 20000; // per walk, a guard against runaway trees
    uint64_t    fromScene        = 0;     // non-zero: this scene and the ones above it (overrides scope)
};

/// On the client thread: read the scene stack (UiJson.cpp). Never throws; faults become ok=false.
[[nodiscard]] Tree walk(WalkOptions const& opt);

/// Runs fn on the client thread (the next ClientInstance::update) and waits for it up to timeout.
/// false: timed out, or the UI unit is not running (fn may still run later: capture by value).
bool runOnClientThread(std::function<void()> fn, std::chrono::milliseconds timeout);

/// The semantic hash of a node (role, name, text, source): what makes a ref stale.
[[nodiscard]] std::string nodeHash(Node const& n);

/// A control as a reply last named it under its ref: enough to find it again in the live tree.
struct StoredNode {
    std::string ref, role, name, text, path, hash, kind;
    uint64_t    sceneId = 0; // the scene object's address: unique while it lives
};

// UiView.cpp
/// Reads the screens opt names (on the client thread) and, with dom, the page of the topmost Ore UI
/// one; drops the refs of screens that left the stack. nullptr: error is the reply.
[[nodiscard]] std::shared_ptr<Tree> readTree(WalkOptions const& opt, int timeoutMs, bool dom, nlohmann::json& error);
/// What a reply calls a screen: its name; an Ore UI screen its route (its name is the page's URL).
[[nodiscard]] std::string screenName(Screen const& s);
/// The ref of a control (made on first use; the same while its screen lives).
[[nodiscard]] std::string refOf(Node const& n);
/// The control a ref names, as last reported. false: no such ref (or its screen closed).
[[nodiscard]] bool refEntry(std::string const& ref, StoredNode& out);
/// One line of the view: "- role \"text\" = value flags [ref]".
[[nodiscard]] std::string nodeLine(Node const& n, POINT client, bool withRef);
/// The view of the screens req names (scope, find, maxNodes, ...), with "screen": the one in front;
/// front (if given): that screen's id.
[[nodiscard]] nlohmann::json screenView(nlohmann::json const& req, uint64_t* front = nullptr);
/// The `ui` op without an action.
[[nodiscard]] nlohmann::json viewOp(nlohmann::json const& req);

/// What a node takes: click, setText, scroll (empty: nothing).
[[nodiscard]] nlohmann::json actionsOf(Node const& n, POINT client);

/// "scene-<hex>": the id of a scene in replies (`surface`, `screens[].id`, `active`).
[[nodiscard]] std::string surfaceId(uint64_t scene);

/// {ok:false, code, error} replies.
[[nodiscard]] nlohmann::json fail(char const* code, std::string const& msg);

/// The JSON of a node (without its ref and children) as the `ui` op reports it.
[[nodiscard]] nlohmann::json nodeJson(Node const& n, POINT client);

// UiActions.cpp
[[nodiscard]] nlohmann::json actionOp(nlohmann::json const& req);

} // namespace bedd::ui

namespace bedd {
/// Frame hook (config "ui").
void startUi();
void stopUi();
/// The `ui` op: the view without "action", else an action on a ref (UiActions.cpp).
[[nodiscard]] nlohmann::json uiOp(nlohmann::json const& req);
/// A `mouse` op with "ref": the pointer goes to that control, then the plain op runs.
[[nodiscard]] nlohmann::json uiMouseOp(nlohmann::json const& req);
} // namespace bedd
