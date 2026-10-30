#pragma once
// Shared by every tap publisher. Keep this file byte-identical across units.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace bedd::tap {

enum class Kind : uint8_t { Clientbound = 1, ServerboundEcho = 2, Marker = 4 };
enum Flag : uint8_t { Replay = 1, Reserialized = 2, Local = 4, Suppressed = 8 };
enum class Link : int { None = 0, Remote = 1, Local = 2 };

struct Frame {
    Kind        kind{};
    uint8_t     flags{};
    uint64_t    seq{};
    uint64_t    tUs{};
    std::string bytes; // varint header + body, or UTF-8 JSON for markers
};

using EmitFn   = void (*)(Frame&&);
using ReplayFn = void (*)(EmitFn sink);

inline std::atomic<EmitFn>   gEmit{nullptr};   // set by the pipe (BD1); BD2 chains onto it
inline std::atomic<ReplayFn> gReplay{nullptr}; // set by the journal (BD2)
inline std::atomic<int>      gLinkType{0};     // Link
inline std::atomic<uint64_t> gSeq{0};

inline uint32_t packetIdOf(std::string_view b) {
    uint32_t v = 0;
    for (int i = 0, shift = 0; i < static_cast<int>(b.size()) && i < 5; ++i, shift += 7) {
        auto c = static_cast<uint8_t>(b[i]);
        v |= static_cast<uint32_t>(c & 0x7f) << shift;
        if (!(c & 0x80)) break;
    }
    return v & 0x3ff;
}

inline void emit(Kind kind, uint8_t flags, std::string bytes) {
    auto fn = gEmit.load(std::memory_order_acquire);
    if (!fn) return;
    Frame f{kind, flags, ++gSeq,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()),
            std::move(bytes)};
    fn(std::move(f));
}

} // namespace bedd::tap
