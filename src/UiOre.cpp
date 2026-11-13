// The `oreui` op (proof of concept for the `ui` mode, issue #21): read the DOM of the Ore UI view.
//
// Ore UI screens are HTML pages in one Coherent Gameface (cohtml 2.2.x) view. cohtml.WindowsDesktop.dll
// exports no View, System or DOM/INodeProxy API: those are pure-virtual interfaces whose vtable
// layout is not published for this version, so calling View::ExecuteScript through a guessed slot
// is not an option. Its script engine is a stock V8 9.4 in v8.dll, though, and v8.dll exports the
// whole V8 embedder API. So the op hooks v8::Object::CallAsFunction and v8::Function::Call, through
// which cohtml calls JavaScript callbacks on its script thread (animation frames, timers, events:
// some 300 calls a second while an Ore UI screen shows, none while only JSON UI screens do): on the
// next such call while a request is pending, the hook compiles and runs a read-only script in that
// call's context (the view's page) and copies the JSON string it returns. The script reads the
// DOM only: tag, id, class, data-testid, role/aria-*, own text, value, state, bounds, children.
//
// The hooks are installed on the first request and stay (unhooking could free a trampoline under a
// thread that is inside it); while nothing is pending they cost one atomic load per call. A request
// waits at most timeoutMs for the script thread; a crash inside V8 is caught (SEH) and turns the op
// off for the rest of the session, since V8's handle scopes may then be unbalanced.
#include "UiOre.h"

#include "BeDD.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ll/api/memory/Hook.h"

namespace bedd {

namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------------------------
// V8 9.4 embedder API, called through its exports with the MSVC x64 ABI spelled out. A Local<T> or
// MaybeLocal<T> is one pointer (the handle); it is passed in a register, but returned through a
// hidden pointer (it has user-provided constructors): after `this` for members, first otherwise.
// A member function receives the handle itself as `this`, as `local->Method()` does.
// ---------------------------------------------------------------------------------------------
using GetIsolateFn  = void* (*)(void* context);
using GetQueueFn    = void* (*)(void* context);
using ScopeCtorFn   = void* (*)(void* self, void* isolate);
using ScopeDtorFn   = void (*)(void* self);
using HasCaughtFn   = bool (*)(void const* self);
using ExceptionFn   = void** (*)(void const* self, void** ret);
using NewFromUtf8Fn = void** (*)(void** ret, void* isolate, char const* data, int type, int length);
using CompileFn     = void** (*)(void** ret, void* context, void* source, void* origin);
using RunFn         = void** (*)(void* script, void** ret, void* context);
using ToStringFn    = void** (*)(void const* value, void** ret, void* context);
using Utf8LengthFn  = int (*)(void const* string, void* isolate);
using WriteUtf8Fn   = int (*)(void const* string, void* isolate, char* buffer, int capacity, int* chars, int options);
using CallFn        = void** (*)(void* function, void** ret, void* context, void* recv, int argc, void** argv);
using VersionFn     = char const* (*)();
using SuppressFn    = void* (*)(void* self, void* isolate, void* queue);

constexpr int kNewStringNormal        = 0;   // v8::NewStringType::kNormal
constexpr int kWriteNoNullTermination = 2;   // v8::String::NO_NULL_TERMINATION
constexpr int kWriteReplaceInvalid    = 8;   // v8::String::REPLACE_INVALID_UTF8
constexpr int kHandleScopeBytes       = 64;  // sizeof(v8::HandleScope) = 24
constexpr int kTryCatchBytes          = 128; // sizeof(v8::TryCatch) = 48 on 9.4
constexpr int kSuppressBytes          = 64;  // sizeof(v8::Isolate::SuppressMicrotaskExecutionScope) = 24

struct V8Api {
    GetIsolateFn  getIsolate            = nullptr;
    GetQueueFn    microtaskQueue        = nullptr;
    ScopeCtorFn   handleScope           = nullptr;
    ScopeDtorFn   handleScopeEnd        = nullptr;
    ScopeCtorFn   tryCatch              = nullptr;
    ScopeDtorFn   tryCatchEnd           = nullptr;
    SuppressFn    suppressMicrotasks    = nullptr;
    ScopeDtorFn   suppressMicrotasksEnd = nullptr;
    HasCaughtFn   hasCaught             = nullptr;
    ExceptionFn   exception             = nullptr;
    NewFromUtf8Fn newFromUtf8           = nullptr;
    CompileFn     compile               = nullptr;
    RunFn         run                   = nullptr;
    ToStringFn    toString              = nullptr;
    Utf8LengthFn  utf8Length            = nullptr;
    WriteUtf8Fn   writeUtf8             = nullptr;
    // The hook targets: cohtml calls JS callbacks (timers, animation frames, events) through them.
    void* call           = nullptr; // v8::Function::Call
    void* callAsFunction = nullptr; // v8::Object::CallAsFunction
};

struct Export {
    char const* name;
    void**      slot;
};

V8Api gApi;

// The exports the op needs, mangled as v8.dll exports them.
Export const kExports[] = {
    {"?GetIsolate@Context@v8@@QEAAPEAVIsolate@2@XZ",                                                                    reinterpret_cast<void**>(&gApi.getIsolate)           },
    {"??0HandleScope@v8@@QEAA@PEAVIsolate@1@@Z",                                                                        reinterpret_cast<void**>(&gApi.handleScope)          },
    {"?GetMicrotaskQueue@Context@v8@@QEAAPEAVMicrotaskQueue@2@XZ",                                                      reinterpret_cast<void**>(&gApi.microtaskQueue)       },
    {"??1HandleScope@v8@@QEAA@XZ",                                                                                      reinterpret_cast<void**>(&gApi.handleScopeEnd)       },
    {"??0TryCatch@v8@@QEAA@PEAVIsolate@1@@Z",                                                                           reinterpret_cast<void**>(&gApi.tryCatch)             },
    {"??1TryCatch@v8@@QEAA@XZ",                                                                                         reinterpret_cast<void**>(&gApi.tryCatchEnd)          },
    {"??0SuppressMicrotaskExecutionScope@Isolate@v8@@QEAA@PEAV12@PEAVMicrotaskQueue@2@@Z",
     reinterpret_cast<void**>(&gApi.suppressMicrotasks)                                                                                                                      },
    {"??1SuppressMicrotaskExecutionScope@Isolate@v8@@QEAA@XZ",                                                          reinterpret_cast<void**>(&gApi.suppressMicrotasksEnd)},
    {"?HasCaught@TryCatch@v8@@QEBA_NXZ",                                                                                reinterpret_cast<void**>(&gApi.hasCaught)            },
    {"?Exception@TryCatch@v8@@QEBA?AV?$Local@VValue@v8@@@2@XZ",                                                         reinterpret_cast<void**>(&gApi.exception)            },
    {"?NewFromUtf8@String@v8@@SA?AV?$MaybeLocal@VString@v8@@@2@PEAVIsolate@2@PEBDW4NewStringType@2@H@Z",
     reinterpret_cast<void**>(&gApi.newFromUtf8)                                                                                                                             },
    {"?Compile@Script@v8@@SA?AV?$MaybeLocal@VScript@v8@@@2@V?$Local@VContext@v8@@@2@V?$Local@VString@v8@@@2@"
     "PEAVScriptOrigin@2@@Z",                                                                                      reinterpret_cast<void**>(&gApi.compile)              },
    {"?Run@Script@v8@@QEAA?AV?$MaybeLocal@VValue@v8@@@2@V?$Local@VContext@v8@@@2@@Z",
     reinterpret_cast<void**>(&gApi.run)                                                                                                                                     },
    {"?ToString@Value@v8@@QEBA?AV?$MaybeLocal@VString@v8@@@2@V?$Local@VContext@v8@@@2@@Z",
     reinterpret_cast<void**>(&gApi.toString)                                                                                                                                },
    {"?Utf8Length@String@v8@@QEBAHPEAVIsolate@2@@Z",                                                                    reinterpret_cast<void**>(&gApi.utf8Length)           },
    {"?WriteUtf8@String@v8@@QEBAHPEAVIsolate@2@PEADHPEAHH@Z",                                                           reinterpret_cast<void**>(&gApi.writeUtf8)            },
    {"?Call@Function@v8@@QEAA?AV?$MaybeLocal@VValue@v8@@@2@V?$Local@VContext@v8@@@2@V?$Local@VValue@v8@@@2@HQEAV52@@Z",
     reinterpret_cast<void**>(&gApi.call)                                                                                                                                    },
    {"?CallAsFunction@Object@v8@@QEAA?AV?$MaybeLocal@VValue@v8@@@2@V?$Local@VContext@v8@@@2@V?$Local@VValue@v8@@@2@"
     "HQEAV52@@Z",                                                                                                 reinterpret_cast<void**>(&gApi.callAsFunction)       },
};

constexpr char kGamefaceVersion[] = "?GetLibraryVersion@Library@cohtml@@SAPEBDXZ";

// Read-only DOM walk, run in the view's context. %MAX% / %MAXT% are replaced with the node and text
// caps. Every DOM access is guarded: Gameface implements a subset of the DOM.
constexpr char kWalker[] = R"JS((function () {
  var MAX = %MAX%, MAXT = %MAXT%;
  var nodes = [], truncated = false, active = null;
  try { active = document.activeElement; } catch (x) {}
  function clip(s) { s = String(s).replace(/\s+/g, ' ').trim(); return s.length > MAXT ? s.slice(0, MAXT) : s; }
  function attr(e, n) { try { var v = e.getAttribute(n); return v == null ? null : String(v); } catch (x) { return null; } }
  function ownText(e) {
    var s = '';
    try { var c = e.childNodes; for (var i = 0; c && i < c.length; i++) if (c[i].nodeType === 3) s += c[i].nodeValue; } catch (x) {}
    return clip(s);
  }
  function walk(e, depth) {
        if (nodes.length >= MAX || depth > 256) { truncated = true; return -1; }  // a deep branch only skips itself
    var n = { tag: String(e.tagName || e.nodeName || '').toLowerCase() }, i = nodes.length, v;
    nodes.push(n);
    try { if ((v = e.id)) n.id = String(v); } catch (x) {}
    if ((v = attr(e, 'class'))) n['class'] = clip(v);
    if ((v = attr(e, 'data-testid'))) n.testId = v;
    if ((v = attr(e, 'role'))) n.role = v;
    v = attr(e, 'aria-label') || attr(e, 'title') || attr(e, 'alt') || attr(e, 'placeholder') || attr(e, 'name');
    if (v) n.name = clip(v);
    if ((v = ownText(e))) n.text = v;
    try { if ('value' in e && e.value != null && e.value !== '') n.value = clip(e.value); } catch (x) {}
    var st = {};
    try { if (e.disabled || attr(e, 'aria-disabled') === 'true') st.disabled = true; } catch (x) {}
    try { if (e.checked || attr(e, 'aria-checked') === 'true') st.checked = true; } catch (x) {}
    if (attr(e, 'aria-selected') === 'true') st.selected = true;
    if ((v = attr(e, 'aria-expanded'))) st.expanded = v === 'true';
    if (attr(e, 'aria-hidden') === 'true') st.hidden = true;
    if (e === active) st.focused = true;
    try { var cs = getComputedStyle(e); if (cs && (cs.display === 'none' || cs.visibility === 'hidden')) st.invisible = true; } catch (x) {}
    n.state = st;
    try {
      var r = e.getBoundingClientRect();
      n.bounds = { x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width), h: Math.round(r.height) };
    } catch (x) {}
    var kids = [], ch = e.children;
    for (var j = 0; ch && j < ch.length; j++) {
      var k = walk(ch[j], depth + 1);
            if (k >= 0) kids.push(k); else if (nodes.length >= MAX) break;
    }
    n.children = kids;
    return i;
  }
  var root = document.documentElement || document.body;
  if (root) walk(root, 0);
  return JSON.stringify({ href: String(location.href), width: innerWidth, height: innerHeight,
    title: String(document.title || ''), nodes: nodes, truncated: truncated });
})())JS";

constexpr char kProbe[] = "String(location.href).slice(0, 1024)";

constexpr size_t kResultCap  = 15u << 20; // under the transport's 16 MiB message cap, with room for the reply around it
constexpr size_t kProbeCap   = 4096;
constexpr int    kMaxTimeout = 30000;

// Script outcome (runScript).
enum Rc : int { kOk = 0, kThrew = 1, kTooLarge = 2, kFailed = 3, kCrashed = 4 };

// The request, written by the pipe thread before kPending, read by the script thread that claims
// it (kPending -> kRunning); the result is written by that thread before kDone.
enum State : int { kIdle, kPending, kRunning, kDone };
std::atomic<int>         gState{kIdle};
std::atomic<uint64_t>    gCalls{0};      // hooked script calls seen while pending
std::atomic<bool>        gCancel{false}; // the request timed out: script threads stop picking it up
std::atomic<bool>        gBroken{false}; // a V8 call crashed: the op is off for this session
std::string              gScript;        // the walker, caps filled in
std::string              gUrlFilter;     // substring of location.href the view must have ("" = any)
std::vector<char>        gBuf;           // result bytes (kResultCap)
int                      gLen = 0;       // bytes of gBuf in use
int                      gRc  = kFailed;
std::mutex               gSeenMutex;
std::vector<std::string> gSeen; // location.href of the contexts probed (guarded)

std::mutex              gWaitMutex;
std::condition_variable gCv;
std::mutex              gOpMutex; // one request at a time

CallFn            o_call           = nullptr;
CallFn            o_callAsFunction = nullptr;
std::atomic<bool> gHooked{false};
thread_local bool tInHook = false;

// Value -> UTF-8 into buf. No objects with destructors (runs under SEH).
int toUtf8(void const* value, void* context, void* isolate, char* buf, int cap, int* len) {
    void* str = nullptr;
    gApi.toString(value, &str, context);
    if (!str) return kFailed;
    int n = gApi.utf8Length(str, isolate);
    *len  = n;
    if (n > cap) return kTooLarge;
    gApi.writeUtf8(str, isolate, buf, cap, nullptr, kWriteNoNullTermination | kWriteReplaceInvalid);
    return kOk;
}

// Compile and run `src` in `context`; its value (or the exception thrown) as UTF-8 in buf. The
// handle scope and try/catch live on this stack frame, as V8 requires.
int runScriptInner(void* context, char const* src, int srcLen, char* buf, int cap, int* len) {
    void* isolate = gApi.getIsolate(context);
    if (!isolate) return kFailed;
    alignas(16) unsigned char scope[kHandleScopeBytes];
    alignas(16) unsigned char trycatch[kTryCatchBytes];
    alignas(16) unsigned char noMicrotasks[kSuppressBytes];
    gApi.handleScope(scope, isolate);
    // The page's pending promise callbacks must not run when our script returns (at call depth 0
    // under the auto policy): they belong to the page's own turn, after the callback cohtml calls.
    // cohtml may give each context its own queue (Context::New takes one): suppress that one.
    gApi.suppressMicrotasks(noMicrotasks, isolate, gApi.microtaskQueue(context));
    gApi.tryCatch(trycatch, isolate);
    int   rc     = kFailed;
    void* source = nullptr;
    gApi.newFromUtf8(&source, isolate, src, kNewStringNormal, srcLen);
    if (source) {
        void* script = nullptr;
        gApi.compile(&script, context, source, nullptr);
        if (script) {
            void* value = nullptr;
            gApi.run(script, &value, context);
            if (value) rc = toUtf8(value, context, isolate, buf, cap, len);
        }
    }
    if (rc == kFailed && gApi.hasCaught(trycatch)) {
        void* ex = nullptr;
        gApi.exception(trycatch, &ex);
        rc = ex && toUtf8(ex, context, isolate, buf, cap, len) == kOk ? kThrew : kFailed;
    }
    gApi.tryCatchEnd(trycatch);
    gApi.suppressMicrotasksEnd(noMicrotasks);
    gApi.handleScopeEnd(scope);
    return rc;
}

int runScript(void* context, char const* src, int srcLen, char* buf, int cap, int* len) {
    __try {
        return runScriptInner(context, src, srcLen, buf, cap, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kCrashed;
    }
}

// On the script thread, with the request claimed: true once it is answered (result or error),
// false if this context is not the view asked for (the request goes back to pending).
bool serve(void* context) {
    char probe[kProbeCap];
    int  len = 0;
    int  rc  = runScript(context, kProbe, static_cast<int>(sizeof(kProbe) - 1), probe, sizeof(probe), &len);
    if (rc == kCrashed) {
        gBroken = true;
        gRc     = rc;
        return true;
    }
    std::string href = rc == kOk ? std::string(probe, static_cast<size_t>(len)) : std::string("<probe failed>");
    {
        std::lock_guard lock(gSeenMutex);
        if (gSeen.size() < 16 && std::ranges::find(gSeen, href) == gSeen.end()) gSeen.push_back(href);
    }
    if (rc != kOk || (!gUrlFilter.empty() && href.find(gUrlFilter) == std::string::npos)) return false;
    gLen = 0;
    gRc  = runScript(
        context,
        gScript.data(),
        static_cast<int>(gScript.size()),
        gBuf.data(),
        static_cast<int>(gBuf.size()),
        &gLen
    );
    if (gRc == kCrashed) gBroken = true;
    return true;
}

// Both hooks (Function::Call, Object::CallAsFunction): serve a pending request in this call's context.
void onScriptCall(void* context) {
    if (gState.load(std::memory_order_acquire) == kPending && !tInHook && context && !gBroken && !gCancel) {
        ++gCalls;
        int expected = kPending;
        if (gState.compare_exchange_strong(expected, kRunning, std::memory_order_acq_rel)) {
            tInHook   = true;
            bool done = serve(context);
            tInHook   = false;
            {
                std::lock_guard lock(gWaitMutex);
                gState.store(done ? kDone : kPending, std::memory_order_release);
            }
            gCv.notify_all();
        }
    }
}

void** d_call(void* function, void** ret, void* context, void* recv, int argc, void** argv) {
    onScriptCall(context);
    return o_call(function, ret, context, recv, argc, argv);
}

void** d_callAsFunction(void* object, void** ret, void* context, void* recv, int argc, void** argv) {
    onScriptCall(context);
    return o_callAsFunction(object, ret, context, recv, argc, argv);
}

json unsupported(std::string error, json findings) {
    return {
        {"ok",       false              },
        {"code",     "unsupported"      },
        {"error",    std::move(error)   },
        {"findings", std::move(findings)},
    };
}

// Resolve the V8 exports (once); the names of those missing go to `missing`.
bool resolveApi(HMODULE v8, json& missing) {
    bool ok = true;
    for (auto const& e : kExports) {
        if (!*e.slot) *e.slot = reinterpret_cast<void*>(GetProcAddress(v8, e.name));
        if (!*e.slot) {
            missing.push_back(e.name);
            ok = false;
        }
    }
    return ok;
}

// Both or neither: a half-installed pair is undone.
bool installHooks() {
    if (gHooked) return true;
    int  rcCall = -1, rcObject = -1;
    bool ok = false;
    {
        HookPause pause;
        rcCall = ll::memory::hook(
            gApi.call,
            reinterpret_cast<void*>(&d_call),
            reinterpret_cast<ll::memory::FuncPtr*>(&o_call),
            ll::memory::HookPriority::Normal,
            false
        );
        rcObject = ll::memory::hook(
            gApi.callAsFunction,
            reinterpret_cast<void*>(&d_callAsFunction),
            reinterpret_cast<ll::memory::FuncPtr*>(&o_callAsFunction),
            ll::memory::HookPriority::Normal,
            false
        );
        ok = rcCall == 0 && o_call && rcObject == 0 && o_callAsFunction;
        if (!ok) {
            // Nothing is pending yet, so a thread inside a detour only passes through to the original.
            if (rcCall == 0) ll::memory::unhook(gApi.call, reinterpret_cast<void*>(&d_call), false);
            if (rcObject == 0)
                ll::memory::unhook(gApi.callAsFunction, reinterpret_cast<void*>(&d_callAsFunction), false);
        }
    }
    if (!ok) {
        log().error("oreui: V8 call hooks failed (Function::Call {}, Object::CallAsFunction {})", rcCall, rcObject);
        return false;
    }
    gHooked = true;
    log().info("oreui: V8 call hooks installed (Ore UI DOM reads)");
    return true;
}

std::string replaceAll(std::string s, std::string const& from, std::string const& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

} // namespace

json oreUiOp(json const& req) {
    std::lock_guard serial(gOpMutex);
    json            findings{
        {"method",
         "a read-only script run in the Ore UI view's V8 context from hooks on v8::Function::Call and "
         "v8::Object::CallAsFunction"},
    };
    if (HMODULE cohtml = GetModuleHandleW(L"cohtml.WindowsDesktop.dll")) {
        if (auto version = reinterpret_cast<VersionFn>(GetProcAddress(cohtml, kGamefaceVersion)))
            findings["gameface"] = version();
    } else {
        findings["gameface"] = nullptr;
    }
    if (gBroken)
        return unsupported(
            "a V8 call crashed earlier in this session; the oreui op is off until the game restarts",
            findings
        );
    HMODULE v8 = GetModuleHandleW(L"v8.dll");
    if (!v8) return unsupported("v8.dll is not loaded (Gameface has not started its script engine yet?)", findings);
    json missing = json::array();
    if (!resolveApi(v8, missing)) {
        findings["missingExports"] = missing;
        return unsupported("v8.dll lacks exports the oreui op needs", findings);
    }
    if (!installHooks()) return unsupported("the V8 call hooks could not be installed", findings);

    int maxNodes  = std::clamp(req.value("maxNodes", 2000), 1, 50000);
    int maxText   = std::clamp(req.value("maxText", 200), 8, 4096);
    int timeoutMs = std::clamp(req.value("timeoutMs", 3000), 100, kMaxTimeout);
    if (gBuf.size() != kResultCap) gBuf.resize(kResultCap);
    gScript    = replaceAll(replaceAll(kWalker, "%MAX%", std::to_string(maxNodes)), "%MAXT%", std::to_string(maxText));
    gUrlFilter = req.value("url", "");
    gCalls     = 0;
    gCancel    = false;
    gRc        = kFailed;
    gLen       = 0;
    {
        std::lock_guard lock(gSeenMutex);
        gSeen.clear();
    }
    gState.store(kPending, std::memory_order_release);

    std::unique_lock lock(gWaitMutex);
    bool done = gCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return gState.load() == kDone; });
    if (!done) {
        // Withdraw the request. A script thread may hold it (probing a context, or running the walker):
        // wait until it puts it back (pending, withdrawn now) or answers it.
        gCancel       = true;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMaxTimeout);
        for (;;) {
            int s = gState.load();
            if (s == kDone) {
                done = true;
                break;
            }
            if (s == kIdle || (s == kPending && gState.compare_exchange_strong(s, kIdle))) break;
            if (s == kRunning && !gCv.wait_until(lock, deadline, [] { return gState.load() != kRunning; })) {
                gBroken = true; // still running: never touch the request buffers again
                return unsupported(
                    "the DOM script did not finish; the oreui op is off until the game restarts",
                    findings
                );
            }
        }
    }
    lock.unlock();
    findings["jsCalls"] = gCalls.load();
    {
        std::lock_guard seen(gSeenMutex);
        findings["contextsSeen"] = gSeen;
    }
    if (!done) {
        return {
            {"ok",       false                                                                               },
            {"code",     "not_ready"                                                                         },
            {"error",
             gUrlFilter.empty()
                 ? "no Ore UI view ran JavaScript within the timeout (is the top screen a JSON UI one?)"
                 : "no Ore UI view whose URL contains '" + gUrlFilter + "' ran JavaScript within the timeout"},
            {"findings", findings                                                                            },
        };
    }
    gState.store(kIdle);
    std::string text =
        gRc == kTooLarge
            ? std::string()
            : std::string(gBuf.data(), static_cast<size_t>(std::clamp(gLen, 0, static_cast<int>(gBuf.size()))));
    switch (gRc) {
    case kOk:
        break;
    case kThrew:
        return unsupported("the DOM script threw: " + text, findings);
    case kTooLarge:
        return unsupported("the DOM snapshot is " + std::to_string(gLen) + " bytes; lower maxNodes", findings);
    case kCrashed:
        return unsupported("a V8 call crashed; the oreui op is off until the game restarts", findings);
    default:
        return unsupported("the DOM script returned nothing", findings);
    }
    json snap = json::parse(text, nullptr, false);
    if (snap.is_discarded() || !snap.is_object())
        return unsupported("the DOM script returned no JSON object", findings);
    return {
        {"ok", true},
        {"url", snap.value("href", "")},
        {"title", snap.value("title", "")},
        {"width", snap.value("width", 0)},
        {"height", snap.value("height", 0)},
        {"nodes", std::move(snap["nodes"])},
        {"truncated", snap.value("truncated", false)},
        {"findings", findings},
    };
}

} // namespace bedd
