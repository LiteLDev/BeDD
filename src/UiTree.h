#pragma once

// The `ui` op (config "ui", issue #21): the game's UI as a tree of controls, and semantic actions on
// them that go through the same virtual mouse, keys and text as the `mouse`, `key` and `text` ops.
//   * UiJson.cpp reads the scene stack on the client thread: JSON UI scenes (UIScene -> ScreenView ->
//     VisualTree -> UIControl) control by control, Ore UI scenes as one opaque node.
//   * UiTree.cpp runs that read on the client thread for a control-pipe thread, turns the result into
//     the reply (refs e1.., hashes, bounds) and keeps each client's last snapshot.
//   * UiActions.cpp resolves a ref of that snapshot against the live tree and drives the input ops.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <functional>
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
    std::string kind = "jsonui"; // jsonui | oreui
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

/// Fields of a stored snapshot node, enough to find it again in the live tree.
struct StoredNode {
    std::string ref, role, name, text, path, hash, kind;
    uint64_t    sceneId = 0; // the scene object's address: unique while it lives
};

/// The last snapshot of the current control-pipe client: nullptr-equivalent (empty id) if none.
struct StoredSnapshot {
    std::string             id;
    std::vector<StoredNode> nodes;
};
[[nodiscard]] bool lookupRef(std::string const& snapshot, std::string const& ref, StoredNode& out, nlohmann::json& error);

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
/// Frame hook and per-client snapshot cleanup (config "ui").
void startUi();
void stopUi();
/// The `ui` op: a snapshot without "action", else a semantic action on a ref (UiActions.cpp).
[[nodiscard]] nlohmann::json uiOp(nlohmann::json const& req);
/// A `mouse` op with "ref" and "snapshot": the pointer goes to that control, then the plain op runs.
[[nodiscard]] nlohmann::json uiMouseOp(nlohmann::json const& req);
} // namespace bedd
