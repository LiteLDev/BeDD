// HookPause: stops every other thread of the process while a batch of hooks is (un)installed, but
// never while one of them is inside the process heap.
//
// The hooking code (PreLoader's pl_hook) allocates from the process heap while the other threads
// are stopped. A thread stopped in the middle of RtlAllocateHeap or RtlFreeHeap can leave the heap's
// low-fragmentation front end busy, and the hooking thread then spins in it for good: the game
// stalled in enable(), its endpoint never came up, most often with several games starting at once
// (issue #14). Locking the heap while the threads stop (what this did before) only covers the back
// end: the front end does not take that lock. So every stopped thread's stack is walked, and while
// one of them is in a heap function all are resumed and stopped again a moment later.
//
// Nothing here may allocate, take a lock another thread may hold, or log between the first
// SuspendThread and the last ResumeThread: the stacks are walked with the image's own unwind data
// (.pdata) instead of RtlLookupFunctionEntry, which may take the loader's or a function table's lock.
#include "BeDD.h"

#include <tlhelp32.h>

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "ll/api/memory/Hook.h"

namespace bedd {

namespace {

using Clock = std::chrono::steady_clock;

// How long to wait for a moment when no thread is in the heap; after that the threads stay stopped
// wherever they are (as before) and a warning is logged.
constexpr auto kSafeDeadline = std::chrono::seconds(5);
// Heap functions sit at the top of a stack: frames deeper than this are not looked at.
constexpr int kMaxFrames = 48;

// One pause at a time: two threads pausing each other would never resume.
std::recursive_mutex gPauseMutex;

struct HeapFunctions {
    uintptr_t                ntdll = 0;
    std::array<uint32_t, 4>  begin{}; // primary RUNTIME_FUNCTION BeginAddress (RVA) of each
    size_t                   count = 0;
};

// The image a code address belongs to (nullptr for JIT code, the stack or nothing at all).
uintptr_t imageOf(uintptr_t pc) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void const*>(pc), &mbi, sizeof(mbi))) return 0;
    if (mbi.Type != MEM_IMAGE || mbi.State != MEM_COMMIT) return 0;
    return reinterpret_cast<uintptr_t>(mbi.AllocationBase);
}

// The image's RUNTIME_FUNCTION for pc (binary search in .pdata), or nullptr for a leaf function.
PRUNTIME_FUNCTION entryFor(uintptr_t base, uintptr_t pc) {
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(base + static_cast<uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    auto const& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir.VirtualAddress || dir.Size < sizeof(RUNTIME_FUNCTION)) return nullptr;
    auto*    table = reinterpret_cast<PRUNTIME_FUNCTION>(base + dir.VirtualAddress);
    size_t   lo = 0, hi = dir.Size / sizeof(RUNTIME_FUNCTION);
    auto     rva = static_cast<uint32_t>(pc - base);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < table[mid].BeginAddress) hi = mid;
        else if (rva >= table[mid].EndAddress) lo = mid + 1;
        else return &table[mid];
    }
    return nullptr;
}

// The function a (possibly split, PGO cold) chunk belongs to: follows indirect and chained entries.
uint32_t primaryBegin(uintptr_t base, PRUNTIME_FUNCTION f) {
    for (int guard = 0; guard < 32; ++guard) {
        if (f->UnwindData & 1) { // indirect: points to another RUNTIME_FUNCTION
            f = reinterpret_cast<PRUNTIME_FUNCTION>(base + (f->UnwindData & ~1u));
            continue;
        }
        auto const* info  = reinterpret_cast<uint8_t const*>(base + f->UnwindData);
        uint8_t     flags = static_cast<uint8_t>(info[0] >> 3);
        if (!(flags & UNW_FLAG_CHAININFO)) return f->BeginAddress;
        uint8_t codes = info[2];
        f = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(info) + 4 + 2 * ((codes + 1u) & ~1u));
    }
    return f->BeginAddress;
}

HeapFunctions const& heapFunctions() {
    static HeapFunctions const fns = [] {
        HeapFunctions h;
        HMODULE       ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return h;
        h.ntdll = reinterpret_cast<uintptr_t>(ntdll);
        for (char const* name : {"RtlAllocateHeap", "RtlFreeHeap", "RtlReAllocateHeap", "RtlSizeHeap"}) {
            auto pc = reinterpret_cast<uintptr_t>(GetProcAddress(ntdll, name));
            if (!pc) continue;
            // An export may be a forwarder stub or jump: only a real function with unwind data counts.
            if (auto* f = entryFor(h.ntdll, pc)) h.begin[h.count++] = primaryBegin(h.ntdll, f);
        }
        return h;
    }();
    return fns;
}

bool isHeapFunction(HeapFunctions const& h, uintptr_t base, PRUNTIME_FUNCTION f) {
    if (base != h.ntdll) return false;
    uint32_t begin = primaryBegin(base, f);
    for (size_t i = 0; i < h.count; ++i)
        if (h.begin[i] == begin) return true;
    return false;
}

// Whether the (stopped) thread is inside one of the heap functions.
bool inHeap(HeapFunctions const& h, HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx)) return false; // also waits until the thread has really stopped
    for (int frame = 0; frame < kMaxFrames && ctx.Rip; ++frame) {
        uintptr_t base = imageOf(ctx.Rip);
        if (!base) return false; // JIT code or garbage: no unwind data to follow
        PRUNTIME_FUNCTION f = entryFor(base, ctx.Rip);
        if (!f) { // leaf function: the return address is on top of the stack
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void const*>(ctx.Rsp), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
            ctx.Rip  = *reinterpret_cast<DWORD64 const*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        if (isHeapFunction(h, base, f)) return true;
        void*   handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &handlerData, &establisher, nullptr);
    }
    return false;
}

} // namespace

HookPause::HookPause() {
    if (!ll::memory::shouldHookSuspendThreads()) return;
    HeapFunctions const& heap = heapFunctions(); // resolved before any thread stops
    mLock = std::unique_lock(gPauseMutex);
    DWORD const self = GetCurrentThreadId();
    DWORD const pid  = GetCurrentProcessId();
    auto const  deadline = Clock::now() + kSafeDeadline;
    std::vector<DWORD> ids;
    for (;;) {
        // Everything that allocates happens here, before the first thread stops.
        ids.clear();
        if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) ids.push_back(te.th32ThreadID);
            CloseHandle(snap);
        }
        mThreads.clear();
        mThreads.reserve(ids.size());
        for (DWORD id : ids) {
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
            if (!t) continue;
            if (SuspendThread(t) == static_cast<DWORD>(-1)) {
                CloseHandle(t);
                continue;
            }
            mThreads.push_back(t);
        }
        bool safe = true;
        for (HANDLE t : mThreads) {
            if (inHeap(heap, t)) {
                safe = false;
                break;
            }
        }
        if (safe) return;
        if (Clock::now() >= deadline) {
            mUnsafe = true;
            return;
        }
        resume();
        ++mRetries;
        Sleep(1);
    }
}

void HookPause::resume() {
    for (HANDLE t : mThreads) {
        ResumeThread(t);
        CloseHandle(t);
    }
    mThreads.clear(); // keeps the capacity: frees nothing
}

void HookPause::end() {
    if (!mLock.owns_lock()) return;
    resume();
    mLock.unlock();
    if (mUnsafe)
        log().warn("hook pause: a thread was still in the heap after {} retries; paused it there anyway", mRetries);
    else if (mRetries >= 50)
        log().info("hook pause: waited {} times for every thread to leave the heap", mRetries);
}

HookPause::~HookPause() { end(); }

} // namespace bedd
