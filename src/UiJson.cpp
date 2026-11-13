// The `ui` op's game side (issue #21): the client's scene stack read on the client thread.
//
// JSON UI scenes are UIScene -> ScreenView -> VisualTree -> UIControl trees. LeviLamina's headers
// declare these classes field by field but offer almost no getters, so the walk reads fields:
// name, positions, size and the visibility/enabled/hover/selected bits of each UIControl, and the
// fields of the components it can identify. The game has no RTTI, and only a handful of component
// functions are exported, so a component's type is learned from its vtable pointer:
//   * TextComponent: its vtable's `render` slot is TextComponent::$render (exported): exact.
//   * text edit boxes, sliders, scroll views: ScreenView keeps lists of those controls; among their
//     components, the type is the one whose control-pointing weak_ptr fields (label, slider box,
//     scroll content, ...) point at descendants of the control.
//   * buttons and toggles (ScreenView's input controls): ButtonComponent starts with 4 weak_ptrs to
//     its default/hover/pressed/locked child controls, ToggleComponent with 8 (checked, unchecked,
//     their hover and locked variants).
// What was learned is kept per vtable for the process (a vtable is a type). A scene is a UIScene or
// an OreUI::Scene when one of its vtable slots is that class's exported implementation.
// Ore UI scenes (cohtml pages) are one opaque node with the page URL and route.
//
// Every read is under SEH: an unexpected layout makes the walk fail (code "unsupported"), it never
// takes the game down. Nothing from the game outlives the walk: the result is plain data.
#include "UiTree.h"

#include "BeDD.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/memory/Memory.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/controls/ButtonComponent.h"
#include "mc/client/gui/controls/SliderComponent.h"
#include "mc/client/gui/controls/ScrollViewComponent.h"
#include "mc/client/gui/controls/TextComponent.h"
#include "mc/client/gui/controls/TextEditComponent.h"
#include "mc/client/gui/controls/ToggleComponent.h"
#include "mc/client/gui/controls/UIComponent.h"
#include "mc/client/gui/controls/UIControl.h"
#include "mc/client/gui/controls/VisualTree.h"
#include "mc/client/gui/oreui/Scene.h"
#include "mc/client/gui/oreui/views/View.h"
#include "mc/client/gui/screens/AbstractScene.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/gui/screens/UIScene.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"

namespace bedd::ui {

namespace {

using json = nlohmann::json;

enum class Kind : uint8_t { Unknown, Text, Button, Toggle, TextEdit, Slider, ScrollView };

char const* kindName(Kind k) {
    switch (k) {
    case Kind::Text: return "text";
    case Kind::Button: return "button";
    case Kind::Toggle: return "toggle";
    case Kind::TextEdit: return "text_edit";
    case Kind::Slider: return "slider";
    case Kind::ScrollView: return "scroll_view";
    default: return "unknown";
    }
}

// vtable pointer -> component type, learned (client thread only).
std::unordered_map<void const*, Kind> gKinds;

struct Probe {
    unsigned    slot;
    void const* impl;
};

struct Probes {
    std::optional<unsigned> textRender; // TextComponent::render's slot
    void const*             textRenderImpl = nullptr;
    std::vector<Probe>      uiScene, oreScene;
};

uintptr_t gameBase() {
    static uintptr_t const base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base;
}

bool inGameImage(void const* p) {
    static uintptr_t const size = [] {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(gameBase());
        return static_cast<uintptr_t>(reinterpret_cast<IMAGE_NT_HEADERS const*>(gameBase() + dos->e_lfanew)->OptionalHeader.SizeOfImage);
    }();
    auto a = reinterpret_cast<uintptr_t>(p);
    return a >= gameBase() && a < gameBase() + size;
}

// The address of an exported game function is that of a jump stub in the mod (the import of
// LeviLamina's link step): follow the jumps until the game's own code.
void const* gameCode(void const* p) {
    for (int i = 0; i < 4 && p && !inGameImage(p); ++i) {
        auto b = static_cast<uint8_t const*>(p);
        if (b[0] == 0xFF && b[1] == 0x25) p = *reinterpret_cast<void const* const*>(b + 6 + *reinterpret_cast<int32_t const*>(b + 2));
        else if (b[0] == 0x48 && b[1] == 0xFF && b[2] == 0x25) p = *reinterpret_cast<void const* const*>(b + 7 + *reinterpret_cast<int32_t const*>(b + 3));
        else if (b[0] == 0xE9) p = b + 5 + *reinterpret_cast<int32_t const*>(b + 1);
        else break;
    }
    return p;
}

template <class V, class I>
void addProbe(std::vector<Probe>& to, V virt, I impl) {
    if (auto slot = ll::memory::getVtableIndex(virt)) to.push_back({*slot, gameCode(ll::memory::toFuncPtr(impl))});
}

Probes const& probes() {
    static Probes const p = [] {
        Probes r;
        r.textRender     = ll::memory::getVtableIndex(&TextComponent::render);
        r.textRenderImpl = gameCode(ll::memory::toFuncPtr(&TextComponent::$render));
        addProbe(r.uiScene, &AbstractScene::render, &UIScene::$render);
        addProbe(r.uiScene, &AbstractScene::onFocusLost, &UIScene::$onFocusLost);
        addProbe(r.uiScene, &AbstractScene::handleTextChar, &UIScene::$handleTextChar);
        addProbe(r.uiScene, &AbstractScene::renderGameBehind, &UIScene::$renderGameBehind);
        addProbe(r.uiScene, &AbstractScene::closeOnPlayerHurt, &UIScene::$closeOnPlayerHurt);
        addProbe(r.uiScene, &AbstractScene::onScreenExit, &UIScene::$onScreenExit);
        addProbe(r.oreScene, &AbstractScene::render, &OreUI::Scene::$render);
        addProbe(r.oreScene, &AbstractScene::onScreenExit, &OreUI::Scene::$onScreenExit);
        return r;
    }();
    return p;
}

void const* vptrOf(void const* obj) { return *static_cast<void const* const*>(obj); }

bool vtableHas(void const* vptr, std::vector<Probe> const& ps) {
    auto slots = static_cast<void const* const*>(vptr);
    return std::any_of(ps.begin(), ps.end(), [&](Probe const& p) { return slots[p.slot] == p.impl; });
}

// The raw pointer of a std::weak_ptr field ({T* ptr, control block*} in the MSVC STL).
void const* weakRaw(void const* field) { return *static_cast<void const* const*>(field); }

// Whether the bytes at field look like an MSVC std::string (16-byte buffer, size, capacity).
bool looksLikeString(void const* field) {
    auto words = static_cast<uint64_t const*>(field);
    uint64_t size = words[2], cap = words[3];
    return cap >= 15 && cap < (1ull << 24) && size <= cap;
}

std::string hex(uintptr_t v) {
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// One control of the scene being walked.
struct Ctl {
    UIControl* c = nullptr;
    int        parent = -1;
    int        depth  = 0;
    int        end    = 0; // one past its last descendant (index into the walk)
    bool       visible = true;
    std::string path;
};

struct SceneWalk {
    WalkOptions const&                      opt;
    std::vector<Ctl>                        ctls;
    std::unordered_map<void const*, int>    index; // control -> index in ctls
    std::unordered_set<void const*>         input, textEdit, slider, scroll;
    bool                                    truncated = false;

    // How many of the weak_ptr fields at `offsets` of component `comp` point at a descendant of control i.
    int descendantRefs(void const* comp, std::initializer_list<size_t> offsets, int i, int* lastMatch = nullptr) const {
        int n = 0, k = 0;
        for (size_t off : offsets) {
            void const* p = weakRaw(static_cast<char const*>(comp) + off);
            if (auto it = index.find(p); p && it != index.end() && it->second > i && it->second < ctls[static_cast<size_t>(i)].end) {
                ++n;
                if (lastMatch) *lastMatch = k;
            }
            ++k;
        }
        return n;
    }
};

#define BEDD_OFF(T, f) offsetof(T, f)

// The type of component `comp` of control i, learned once per vtable.
Kind kindOf(SceneWalk const& w, UIComponent const* comp, int i) {
    void const* vt = vptrOf(comp);
    if (auto it = gKinds.find(vt); it != gKinds.end()) return it->second;
    auto const& pr = probes();
    if (pr.textRender && static_cast<void const* const*>(vt)[*pr.textRender] == pr.textRenderImpl) return gKinds[vt] = Kind::Text;
    UIControl const* c = w.ctls[static_cast<size_t>(i)].c;
    if (w.textEdit.contains(c)) {
        int n = w.descendantRefs(
            comp,
            {BEDD_OFF(TextEditComponent, mLabelControl),
             BEDD_OFF(TextEditComponent, mPlaceHolderControl),
             BEDD_OFF(TextEditComponent, mVirtualKeyboardBufferControl)},
            i
        );
        if (n >= 1) return gKinds[vt] = Kind::TextEdit;
    }
    if (w.slider.contains(c)) {
        int n = w.descendantRefs(
            comp,
            {BEDD_OFF(SliderComponent, mSliderBoxControl),
             BEDD_OFF(SliderComponent, mBackground),
             BEDD_OFF(SliderComponent, mProgress),
             BEDD_OFF(SliderComponent, mDefaultControl),
             BEDD_OFF(SliderComponent, mHoverControl)},
            i
        );
        if (n >= 2) return gKinds[vt] = Kind::Slider;
    }
    if (w.scroll.contains(c)) {
        int n = w.descendantRefs(
            comp,
            {BEDD_OFF(ScrollViewComponent, mScrollViewPort),
             BEDD_OFF(ScrollViewComponent, mScrollContent),
             BEDD_OFF(ScrollViewComponent, mScrollTrack),
             BEDD_OFF(ScrollViewComponent, mScrollBox)},
            i
        );
        if (n >= 2) return gKinds[vt] = Kind::ScrollView;
        return Kind::Unknown; // a scroll view's weak_ptrs look like a toggle's: never guess below
    }
    if (w.input.contains(c)) {
        int lastToggle = -1;
        int toggle     = w.descendantRefs(
            comp,
            {BEDD_OFF(ToggleComponent, mCheckedStateControl),
             BEDD_OFF(ToggleComponent, mUncheckedStateControl),
             BEDD_OFF(ToggleComponent, mCheckedHoverStateControl),
             BEDD_OFF(ToggleComponent, mUncheckedHoverStateControl),
             BEDD_OFF(ToggleComponent, mCheckedLockedStateControl),
             BEDD_OFF(ToggleComponent, mCheckedLockedHoverStateControl),
             BEDD_OFF(ToggleComponent, mUncheckedLockedStateControl),
             BEDD_OFF(ToggleComponent, mUncheckedLockedHoverStateControl)},
            i,
            &lastToggle
        );
        // Past the weak_ptrs: a byte of 5 flag bits, then four strings (the grid collection name, the
        // narration texts, the description). Other components with control pointers there do not.
        if (toggle >= 3 && lastToggle >= 4) {
            auto const* tg    = static_cast<ToggleComponent const*>(comp);
            auto        flags = *(reinterpret_cast<uint8_t const*>(&tg->mUncheckedLockedHoverStateControl) + sizeof(std::weak_ptr<UIControl>));
            if (flags < 32 && looksLikeString(&tg->mGridCollectionName) && looksLikeString(&tg->mTTSValueOn)
                && looksLikeString(&tg->mTTSValueOff) && looksLikeString(&tg->mDescriptiveText))
                return gKinds[vt] = Kind::Toggle;
        }
        int button = w.descendantRefs(
            comp,
            {BEDD_OFF(ButtonComponent, mDefaultStateControl),
             BEDD_OFF(ButtonComponent, mHoverStateControl),
             BEDD_OFF(ButtonComponent, mPressedStateControl),
             BEDD_OFF(ButtonComponent, mLockedStateControl)},
            i
        );
        auto const* b = static_cast<ButtonComponent const*>(comp);
        auto        pressed = *reinterpret_cast<uint8_t const*>(&b->mPressed);
        auto        consume = *reinterpret_cast<uint8_t const*>(&b->mConsumeEvents);
        if (button >= 2 && toggle == button && pressed <= 1 && consume <= 1) return gKinds[vt] = Kind::Button;
    }
    return Kind::Unknown;
}

// The text a TextComponent shows: bound and localized when the game resolved it.
// mLabel is the definition's text (a localization key, or a '#binding'), mCachedText the bound text
// and mLOCCachedText that text localized, as drawn. The cached texts are only filled once the label
// was laid out: a label that was never drawn (a button's hover state, say) has none.
std::string textOf(TextComponent const* t) {
    if (!t->mLOCCachedText.get().empty()) return t->mLOCCachedText.get();
    return t->mCachedText.get();
}

char const* typeName(Kind k) {
    switch (k) {
    case Kind::Text: return "label";
    case Kind::Button: return "button";
    case Kind::Toggle: return "toggle";
    case Kind::TextEdit: return "edit_box";
    case Kind::Slider: return "slider";
    case Kind::ScrollView: return "scroll_view";
    default: return "panel";
    }
}


bool inScope(WalkOptions const& opt, Screen const& s, bool active, bool aboveFrom) {
    if (opt.fromScene) return aboveFrom;
    if (opt.scope.empty() || opt.scope == "active") return active;
    if (opt.scope == "all") return true;
    return opt.scope == s.name || opt.scope == s.rawName || opt.scope == surfaceId(s.id);
}

// Walk one JSON UI scene into t.nodes.
void walkUiScene(Tree& t, Screen& s, UIScene& scene, WalkOptions const& opt) {
    ScreenView* view = scene.mScreenView.get();
    VisualTree* tree = view ? view->mVisualTree.get() : nullptr;
    UIControl*  root = tree ? tree->mRootControl.get() : nullptr;
    if (!root || !tree->mRootControlWasInitialized) {
        s.ready = false;
        return;
    }
    SceneWalk w{opt};
    auto      fill = [](std::unordered_set<void const*>& set, std::vector<std::shared_ptr<UIControl>> const& v) {
        for (auto const& c : v)
            if (c) set.insert(c.get());
    };
    fill(w.input, view->mInputControls.get());
    fill(w.textEdit, view->mTextEditBoxControls.get());
    fill(w.slider, view->mSliderControls.get());
    fill(w.scroll, view->mScrollViewControls.get());
    void const* selected = weakRaw(&view->mSelectedControl);

    // Pass 1: the controls in pre-order (= paint order), invisible subtrees skipped unless asked.
    struct Frame {
        UIControl* c;
        int        parent;
        int        depth;
        bool       visible;
        std::string path;
    };
    std::vector<Frame> stack{{root, -1, 0, true, "/" + root->mName.get()}};
    while (!stack.empty()) {
        Frame f = std::move(stack.back());
        stack.pop_back();
        // Close the subtrees this control is not part of.
        if (w.ctls.size() >= opt.maxControls) {
            w.truncated = true;
            break;
        }
        // mIsVisibleInTree is what the game draws by: a hidden modal keeps mVisible but leaves the tree.
        bool visible = f.visible && f.c->mVisible != ::ui::VisibilityFlag::None && f.c->mIsVisibleInTree;
        // Invisible controls are walked too: a button is recognised by its (hidden) state children.
        // The reply leaves them out unless includeInvisible.
        int i = static_cast<int>(w.ctls.size());
        w.ctls.push_back({f.c, f.parent, f.depth, 0, visible, std::move(f.path)});
        w.index[f.c] = i;
        auto const& kids = f.c->mChildren.get();
        // Disambiguate siblings that share a name: name[k], k counting from 0 among them.
        std::unordered_map<std::string, int> count, seen;
        for (auto const& k : kids)
            if (k) ++count[k->mName.get()];
        std::vector<Frame> next;
        next.reserve(kids.size());
        for (auto const& k : kids) {
            if (!k) continue;
            std::string const& n    = k->mName.get();
            std::string        path = w.ctls[static_cast<size_t>(i)].path + "/" + n;
            int                nth  = seen[n]++;
            if (count[n] > 1) path += "[" + std::to_string(nth) + "]";
            next.push_back({k.get(), i, f.depth + 1, visible, std::move(path)});
        }
        for (auto it = next.rbegin(); it != next.rend(); ++it) stack.push_back(std::move(*it));
    }
    // Subtree ends: a control's subtree runs until the next control at its depth or above.
    for (int i = static_cast<int>(w.ctls.size()) - 1; i >= 0; --i) {
        int end = i + 1;
        while (end < static_cast<int>(w.ctls.size()) && w.ctls[static_cast<size_t>(end)].depth > w.ctls[static_cast<size_t>(i)].depth)
            end = w.ctls[static_cast<size_t>(end)].end;
        w.ctls[static_cast<size_t>(i)].end = end;
    }

    // UI units -> client pixels.
    glm::vec2 viewSize = view->mSize.get();
    double    sx = viewSize.x > 0 ? t.client.x / static_cast<double>(viewSize.x) : 1.0;
    double    sy = viewSize.y > 0 ? t.client.y / static_cast<double>(viewSize.y) : 1.0;

    // Pass 2: the nodes.
    int base = static_cast<int>(t.nodes.size());
    s.root   = base;
    for (size_t i = 0; i < w.ctls.size(); ++i) {
        Ctl const& ctl = w.ctls[i];
        UIControl* c   = ctl.c;
        Node       n;
        n.sceneId    = s.id;
        n.name       = c->mName.get();
        n.path       = ctl.path;
        n.depth      = ctl.depth;
        n.parent     = ctl.parent < 0 ? -1 : base + ctl.parent;
        n.subtreeEnd = base + ctl.end;
        n.visible    = ctl.visible;
        n.enabled    = c->mEnabled && c->mAllAncestorsEnabled;
        n.hovered    = c->mHover;
        n.selected   = c->mSelected;
        n.focused    = c == selected;
        n.interactive = w.input.contains(c);
        if (c->mCachedPositionDirty) c->_setCachedPosition();
        glm::vec2 pos  = c->mCachedPosition.get();
        glm::vec2 size = c->mSize.get();
        n.bounds       = {pos.x * sx, pos.y * sy, size.x * sx, size.y * sy};

        TextComponent const*     text   = nullptr;
        ButtonComponent const*   button = nullptr;
        ToggleComponent const*   toggle = nullptr;
        TextEditComponent const* edit   = nullptr;
        SliderComponent const*   slide  = nullptr;
        bool                     scroll = w.scroll.contains(c);
        Kind                     main   = Kind::Unknown;
        if (opt.debug)
            n.debug = {
                {"inUse",      hex(c->mComponentsInUse)                },
                {"visible",    static_cast<int>(c->mVisible)           },
                {"inTree",     static_cast<bool>(c->mIsVisibleInTree)  },
                {"clipped",    static_cast<bool>(c->mIsClipped)        },
                {"alpha",      c->mAlpha                               },
                {"layer",      c->mLayer                               },
                {"zOrder",     c->mZOrder                              },
                {"components", json::array()                           },
            };
        for (auto const& up : c->mComponents.get()) {
            UIComponent const* comp = up.get();
            if (!comp) continue;
            Kind k = kindOf(w, comp, static_cast<int>(i));
            switch (k) {
            case Kind::Text: text = static_cast<TextComponent const*>(comp); break;
            case Kind::Button: button = static_cast<ButtonComponent const*>(comp); break;
            case Kind::Toggle: toggle = static_cast<ToggleComponent const*>(comp); break;
            case Kind::TextEdit: edit = static_cast<TextEditComponent const*>(comp); break;
            case Kind::Slider: slide = static_cast<SliderComponent const*>(comp); break;
            case Kind::ScrollView: scroll = true; break;
            default: break;
            }
            if (opt.debug) {
                json d{{"vtable", hex(reinterpret_cast<uintptr_t>(vptrOf(comp)) - gameBase())}, {"kind", kindName(k)}};
                if (k == Kind::Text) {
                    auto const* tc = static_cast<TextComponent const*>(comp);
                    d["label"]     = tc->mLabel.get();
                    d["cached"]    = tc->mCachedText.get();
                    d["loc"]       = tc->mLOCCachedText.get();
                }
                n.debug["components"].push_back(std::move(d));
            }
        }
        if (text) n.text = textOf(text);
        if (edit || w.textEdit.contains(c)) {
            n.role = "text_input";
            main   = Kind::TextEdit;
            if (edit) {
                n.focused = n.focused || edit->mFocused;
                n.hovered = n.hovered || edit->mHover;
                // Its text is the label control's.
                if (auto it = w.index.find(weakRaw(&edit->mLabelControl)); it != w.index.end()) {
                    for (auto const& up : w.ctls[static_cast<size_t>(it->second)].c->mComponents.get()) {
                        if (up && kindOf(w, up.get(), it->second) == Kind::Text) {
                            n.value = textOf(static_cast<TextComponent const*>(up.get()));
                            break;
                        }
                    }
                }
            }
            if (n.value.is_null()) n.value = "";
        } else if (slide || w.slider.contains(c)) {
            n.role = "slider";
            main   = Kind::Slider;
            if (slide) n.value = slide->mPercentage;
        } else if (toggle) {
            n.role    = "toggle";
            main      = Kind::Toggle;
            n.checked = toggle->mChecked ? 1 : 0;
            n.hovered = n.hovered || toggle->mHover;
            n.value   = toggle->mChecked != 0;
        } else if (button) {
            n.role    = "button";
            main      = Kind::Button;
            n.pressed = button->mPressed;
        } else if (scroll) {
            n.role = "scroll";
            main   = Kind::ScrollView;
        } else if (text) {
            n.role = "text";
            main   = Kind::Text;
        }
        n.type = typeName(main);
        if (n.role != "text" && (n.role != "group" || n.interactive)) {
            // The callback's index argument does not arrive intact through function_ref: take the
            // innermost collection's name from it and ask for the index by name.
            c->findAllCollectionIndices([&](std::string const& name, int) {
                if (n.collectionName.empty()) n.collectionName = name;
            });
            if (!n.collectionName.empty()) n.collectionIndex = c->findCollectionIndex(n.collectionName);
        }
        // Takes pointer input without a known button component, in a collection: a slot, a grid cell.
        if (n.role == "group" && n.interactive && n.collectionIndex >= 0) n.role = "item";
        t.nodes.push_back(std::move(n));
    }
    s.end = static_cast<int>(t.nodes.size());
    for (int i = base; i < s.end; ++i)
        if (t.nodes[static_cast<size_t>(i)].parent >= 0) t.nodes[static_cast<size_t>(t.nodes[static_cast<size_t>(i)].parent)].children.push_back(i);
    // A button, toggle, item or slider reads as its first visible label.
    for (int i = base; i < s.end; ++i) {
        Node& n = t.nodes[static_cast<size_t>(i)];
        if (!n.text.empty() || n.role == "group" || n.role == "text" || n.role == "text_input" || n.role == "scroll") continue;
        for (int j = i + 1; j < n.subtreeEnd; ++j) {
            Node const& d = t.nodes[static_cast<size_t>(j)];
            if (d.role == "text" && d.visible && !d.text.empty()) {
                n.text = d.text;
                break;
            }
        }
    }
    if (w.truncated) t.truncated = true;
}

void walkOreScene(Tree& t, Screen& s, OreUI::Scene& scene) {
    Node n;
    n.kind    = "oreui";
    n.sceneId = s.id;
    n.name    = s.name;
    n.route   = scene.mCurrentRoute.get();
    if (OreUI::IView* iv = scene.mView.get().get()) n.url = static_cast<OreUI::View*>(iv)->mUrl.get();
    n.bounds      = {0, 0, static_cast<double>(t.client.x), static_cast<double>(t.client.y)};
    n.interactive = s.absorbsInput;
    n.path        = "/" + s.name;
    n.subtreeEnd  = static_cast<int>(t.nodes.size()) + 1;
    s.route       = n.route;
    s.root        = static_cast<int>(t.nodes.size());
    t.nodes.push_back(std::move(n));
    s.end = static_cast<int>(t.nodes.size());
}

struct WalkCtx {
    WalkOptions const* opt;
    Tree*              tree;
};

void walkInner(WalkCtx* ctx) {
    Tree&              t   = *ctx->tree;
    WalkOptions const& opt = *ctx->opt;
    auto               ci  = ll::service::getClientInstance();
    if (!ci) {
        t.error = "no client instance yet";
        return;
    }
    RECT r{};
    if (HWND h = gameWindow()) GetClientRect(h, &r);
    t.client = {r.right, r.bottom};
    auto         stackPtr = ci->getCurrentSceneStack();
    ISceneStack* stack    = stackPtr.get().get();
    if (!stack) {
        t.error = "no scene stack yet";
        return;
    }
    AbstractScene* top = stack->getTopScene();
    std::vector<AbstractScene*> scenes;
    for (auto const& e : stack->getScreenStackView())
        if (AbstractScene* a = e.mScene.get()) scenes.push_back(a);
    if (scenes.empty()) {
        t.error = "the scene stack is empty";
        return;
    }
    auto const& pr = probes();
    for (size_t i = 0; i < scenes.size(); ++i) {
        AbstractScene* a = scenes[i];
        Screen         s;
        s.id           = reinterpret_cast<uintptr_t>(a);
        s.name         = a->getScreenName();
        s.rawName      = a->getRawScreenName();
        s.absorbsInput = a->absorbsInput();
        s.modal        = a->isModal();
        void const* vt = vptrOf(a);
        s.kind         = vtableHas(vt, pr.uiScene) ? "jsonui" : vtableHas(vt, pr.oreScene) ? "oreui" : "unknown";
        if (opt.debug) {
            json d{{"vtable", hex(reinterpret_cast<uintptr_t>(vt) - gameBase())}, {"probes", json::array()}};
            for (auto const* ps : {&pr.uiScene, &pr.oreScene})
                for (auto const& p : *ps)
                    d["probes"].push_back({p.slot, hex(reinterpret_cast<uintptr_t>(p.impl) - gameBase()),
                                           hex(reinterpret_cast<uintptr_t>(static_cast<void const* const*>(vt)[p.slot]) - gameBase())});
            s.debug = std::move(d);
        }
        if (a == top) t.active = static_cast<int>(i);
        t.screens.push_back(std::move(s));
    }
    if (t.active < 0) t.active = static_cast<int>(scenes.size()) - 1;
    int from = -1; // index of opt.fromScene once reached
    for (size_t i = 0; i < scenes.size(); ++i) {
        Screen& s = t.screens[i];
        if (s.id == opt.fromScene) from = static_cast<int>(i);
        if (!inScope(opt, s, static_cast<int>(i) == t.active, from >= 0)) continue;
        if (s.kind == "jsonui") walkUiScene(t, s, *static_cast<UIScene*>(scenes[i]), opt);
        else if (s.kind == "oreui") walkOreScene(t, s, *static_cast<OreUI::Scene*>(scenes[i]));
    }
    t.ok = true;
}

// No C++ object with a destructor may live in a function with __try: the guard is its own function.
bool guardedWalk(WalkCtx* ctx) {
    __try {
        walkInner(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

Tree walk(WalkOptions const& opt) {
    // The function-local statics are initialized here, outside the guard: a fault inside a static's
    // initialization would leave its guard taken for good.
    probes();
    inGameImage(nullptr);
    // On a fault the half-built tree is abandoned (leaked) rather than destroyed: it may be torn.
    auto*   t = new Tree;
    WalkCtx ctx{&opt, t};
    if (!guardedWalk(&ctx)) {
        log().warn("ui: reading the UI faulted (unexpected layout); answering unsupported");
        Tree bad;
        bad.error = "unsupported";
        return bad;
    }
    Tree r = std::move(*t);
    delete t;
    return r;
}

} // namespace bedd::ui
