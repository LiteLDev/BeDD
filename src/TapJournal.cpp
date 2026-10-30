// Join journal of the tap (tap): a compacted copy of every clientbound frame since start_game,
// replayed to each new tap client so it can attach in the middle of a session.
//
// * Chained onto bedd::tap::gEmit (TapSink.h): journalEmit records the frame and then forwards it
//   to the sink that was installed before it (the tap pipe, when that came first). The pipe chains
//   the same way, so the install order does not matter; with no pipe the journal is the only sink.
// * Only CLIENTBOUND frames are kept. The journal resets on a new link (LINK_UP marker, or a change
//   of gLinkType to a link) and on start_game (11), which it then keeps as its first frame.
// * Compaction reads the packet header and leading fields, never a full decode: a frame with a key
//   replaces the previous frame with that key (last write wins). Packets that carry only what
//   changed (set_entity_data, update_attributes, move_entity_delta) are keyed by the entity *and*
//   the set of fields they carry, so the last value of every field survives. A subchunk packet
//   holds many sub-chunks around one origin: it is dropped once newer frames carry every one of
//   them (keyed per sub-chunk, not per packet). remove_entity (14)
//   deletes the entity's add_* frame and every frame of its runtime id. Sounds, particles, animate,
//   text and network_stack_latency are not kept at all.
// * Past tapJournalMiB the journal stops growing and reports complete:false until the next reset.
// * gReplay streams the frames in their original seq order with the replay flag set. The journal
//   has its own mutex: network threads parse and copy outside it and only insert under it; a
//   replay copies pointers under it and the bytes outside it.
// * A frame recorded right before a client attaches can reach that client twice (in the replay and
//   live): the tap pipe drops buffered live frames whose seq it already replayed.
#include "BeDD.h"
#include "TapSink.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bedd {

namespace {

namespace tap = bedd::tap;

// Packet ids (bedrock protocol 1.26.51).
enum : uint32_t {
    kText                        = 9,
    kSetTime                     = 10,
    kStartGame                   = 11,
    kAddPlayer                   = 12,
    kAddEntity                   = 13,
    kRemoveEntity                = 14,
    kAddItemEntity               = 15,
    kTakeItemEntity              = 17,
    kMoveEntity                  = 18,
    kMovePlayer                  = 19,
    kUpdateBlock                 = 21,
    kLevelEvent                  = 25,
    kEntityEvent                 = 27,
    kMobEffect                   = 28,
    kUpdateAttributes            = 29,
    kMobEquipment                = 31,
    kMobArmorEquipment           = 32,
    kSetEntityData               = 39,
    kSetEntityMotion             = 40,
    kSetHealth                   = 42,
    kAnimate                     = 44,
    kInventoryContent            = 49,
    kInventorySlot               = 50,
    kLevelChunk                  = 58,
    kPlaySound                   = 86,
    kStopSound                   = 87,
    kMoveEntityDelta             = 111,
    kNetworkStackLatency         = 115,
    kSpawnParticleEffect         = 118,
    kLevelSoundEventV2           = 120,
    kNetworkChunkPublisherUpdate = 121,
    kLevelSoundEvent             = 123,
    kLevelEventGeneric           = 124,
    kSubChunk                    = 174,
    kUpdateAbilities             = 187,
    kCurrentStructureFeature     = 314,
};

bool transient(uint32_t id) {
    switch (id) {
    case kText:
    case kAnimate:
    case kPlaySound:
    case kStopSound:
    case kNetworkStackLatency:
    case kSpawnParticleEffect:
    case kLevelSoundEventV2:
    case kLevelSoundEvent:
    case kLevelEventGeneric: return true;
    default: return false;
    }
}

// The leading fields of a packet body.
struct Reader {
    std::string_view b;
    size_t           i  = 0;
    bool             ok = true;

    uint64_t uvarint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            if (i >= b.size()) break;
            auto c = static_cast<uint8_t>(b[i++]);
            v |= static_cast<uint64_t>(c & 0x7f) << shift;
            if (!(c & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    int64_t zigzag() {
        uint64_t v = uvarint();
        return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    }
    int64_t le(size_t n) { // li32 / li64
        if (i + n > b.size()) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (size_t k = 0; k < n; ++k) v |= static_cast<uint64_t>(static_cast<uint8_t>(b[i + k])) << (8 * k);
        i += n;
        return n == 4 ? static_cast<int64_t>(static_cast<int32_t>(v)) : static_cast<int64_t>(v);
    }
    uint8_t u8() { return static_cast<uint8_t>(le(1)); }
    void    skip(size_t n) {
        if (i + n > b.size()) ok = false;
        else i += n;
    }
    std::string_view str() {
        auto n = static_cast<size_t>(uvarint());
        if (!ok || i + n > b.size()) {
            ok = false;
            return {};
        }
        std::string_view s = b.substr(i, n);
        i += n;
        return s;
    }
};

uint64_t fnv(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) h = (h ^ static_cast<uint8_t>(c)) * 0x100000001b3ULL;
    return h;
}

// Order-independent hash of the attribute names an update_attributes carries (0 = unreadable).
int64_t attributeSet(Reader& r) {
    uint64_t h = 0, n = r.uvarint();
    for (uint64_t k = 0; k < n && r.ok; ++k) {
        r.skip(24); // min, max, current, default_min, default_max, default
        h += fnv(r.str());
        uint64_t mods = r.uvarint();
        for (uint64_t m = 0; m < mods && r.ok; ++m) {
            r.str(); // id
            r.str(); // name
            r.skip(13); // amount, operation, operand, serializable
        }
    }
    return r.ok ? static_cast<int64_t>(h | 1) : 0;
}

// Order-independent hash of the metadata keys a set_entity_data carries (0 = unreadable, e.g. NBT).
int64_t metadataSet(Reader& r) {
    uint64_t h = 0, n = r.uvarint();
    for (uint64_t k = 0; k < n && r.ok; ++k) {
        uint64_t key = r.uvarint();
        h += fnv(std::string_view(reinterpret_cast<char const*>(&key), sizeof(key)));
        switch (r.uvarint()) {
        case 0: r.skip(1); break; // byte
        case 1: r.skip(2); break; // short
        case 2: r.uvarint(); break; // int (zigzag)
        case 3: r.skip(4); break; // float
        case 4: r.str(); break; // string
        case 6: r.uvarint(), r.uvarint(), r.uvarint(); break; // vec3i
        case 7: r.uvarint(); break; // long (zigzag)
        case 8: r.skip(12); break; // vec3f
        default: return 0; // 5 = NBT: not walked
        }
    }
    return r.ok ? static_cast<int64_t>(h | 1) : 0;
}

struct Key {
    uint32_t id = 0;
    int64_t  a = 0, b = 0, c = 0, d = 0;
    bool     operator==(Key const&) const = default;
};

struct KeyHash {
    size_t operator()(Key const& k) const {
        uint64_t h = k.id;
        for (int64_t v : {k.a, k.b, k.c, k.d}) h = (h ^ static_cast<uint64_t>(v)) * 0x100000001b3ULL;
        return static_cast<size_t>(h);
    }
};

struct Parsed {
    uint32_t                id = 0;
    std::optional<Key>      key;
    std::vector<Key>        parts; // subchunk: one key per sub-chunk it carries (dimension, x, y, z)
    std::optional<uint64_t> entity; // runtime id the frame belongs to (dropped with the entity)
    int64_t                 unique = 0; // remove_entity
};

Parsed parse(std::string_view bytes) {
    Reader r{bytes};
    Parsed p;
    p.id     = static_cast<uint32_t>(r.uvarint() & 0x3ff);
    auto key = [&](int64_t a = 0, int64_t b = 0, int64_t c = 0, int64_t d = 0) { p.key = Key{p.id, a, b, c, d}; };
    auto rt  = [&] {
        p.entity = r.uvarint();
        return static_cast<int64_t>(*p.entity);
    };
    switch (p.id) {
    case kLevelChunk: {
        int64_t x = r.zigzag(), z = r.zigzag(), dim = r.zigzag();
        key(x, z, dim);
        break;
    }
    case kSubChunk: {
        // The client asks for many sub-chunks relative to one origin (its own sub-chunk), so the
        // origin is no key: every subchunk packet would replace the one before. Each entry is a
        // sub-chunk at origin + (dx, dy, dz); a frame is dropped once newer frames carry all of its
        // sub-chunks.
        r.u8(); // cache_enabled
        int64_t dim    = r.zigzag();
        int64_t ox = r.le(4), oy = r.le(4), oz = r.le(4); // origin: vec3li
        uint64_t n = r.uvarint();
        for (uint64_t k = 0; k < n && r.ok; ++k) {
            auto dx = static_cast<int8_t>(r.u8()), dy = static_cast<int8_t>(r.u8()), dz = static_cast<int8_t>(r.u8());
            r.u8(); // result
            if (r.u8()) r.str(); // payload
            for (int map = 0; map < 2 && r.ok; ++map) { // heightmap, render heightmap
                r.u8(); // type
                if (r.u8())
                    for (int row = 0; row < 16 && r.ok; ++row) r.skip(static_cast<size_t>(r.uvarint()));
            }
            if (r.u8()) r.skip(8); // blob id
            p.parts.push_back(Key{kSubChunk, dim, ox + dx, oy + dy, oz + dz});
        }
        if (!r.ok) p.parts.clear();
        break;
    }
    case kUpdateBlock: {
        int64_t x = r.zigzag(), y = r.zigzag(), z = r.zigzag();
        r.uvarint(); // block_runtime_id
        r.uvarint(); // flags
        int64_t layer = static_cast<int64_t>(r.uvarint());
        key(x, y, z, layer);
        break;
    }
    case kAddEntity:
    case kAddItemEntity: {
        int64_t unique = r.zigzag();
        rt();
        key(unique);
        break;
    }
    case kAddPlayer:
        r.skip(16); // uuid
        r.str(); // username
        key(rt());
        break;
    case kRemoveEntity: p.unique = r.zigzag(); break;
    case kInventoryContent: key(static_cast<int64_t>(r.uvarint())); break; // window id
    case kInventorySlot: {
        int64_t window = static_cast<int64_t>(r.uvarint());
        int64_t slot   = static_cast<int64_t>(r.uvarint());
        key(window, slot);
        break;
    }
    case kLevelEvent: key(r.zigzag()); break; // weather start/stop keep their last state
    case kUpdateAbilities: key(r.le(8)); break;
    case kSetTime:
    case kSetHealth:
    case kNetworkChunkPublisherUpdate:
    case kCurrentStructureFeature: key(); break;
    case kSetEntityMotion:
    case kMoveEntity:
    case kMovePlayer:
    case kMobArmorEquipment: key(rt()); break;
    case kMoveEntityDelta: {
        // Optional x, y, z (lf32) and rot x, y, z (u8), each behind a presence bool: the key is
        // the set of fields present, so every field keeps its last value.
        int64_t e = rt(), mask = 0;
        for (int f = 0; f < 6 && r.ok; ++f) {
            if (!r.u8()) continue;
            mask |= 1 << f;
            r.skip(f < 3 ? 4 : 1);
        }
        key(e, mask);
        break;
    }
    case kUpdateAttributes: {
        int64_t e = rt();
        if (int64_t set = attributeSet(r)) key(e, set);
        break;
    }
    case kSetEntityData: {
        int64_t e = rt();
        if (int64_t set = metadataSet(r)) key(e, set);
        break;
    }
    case kMobEffect: {
        int64_t e = rt();
        r.u8(); // event: add/update/remove
        key(e, r.zigzag()); // effect id
        break;
    }
    case kEntityEvent:
    case kMobEquipment:
    case kTakeItemEntity: rt(); break;
    default: break;
    }
    // A packet too short for its key is kept as it is (never merged with another one).
    if (!r.ok) {
        p.key.reset();
        if (p.id != kRemoveEntity) p.unique = 0;
    }
    return p;
}

struct Entry {
    std::shared_ptr<tap::Frame const> frame;
    std::optional<Key>                key;
    std::optional<uint64_t>           entity;
    uint32_t                          id = 0;
    std::vector<Key>                  parts;
};

std::atomic<bool>        gInstalled{false};
bool                     gChained = false; // journalEmit is in the gEmit chain (install/uninstall only)
std::atomic<tap::EmitFn> gPrev{nullptr};

std::mutex                                           gMutex;
std::map<uint64_t, Entry>                            gFrames;   // by seq
std::unordered_map<Key, uint64_t, KeyHash>           gKeyed;    // key -> seq
std::unordered_map<uint64_t, std::set<uint64_t>>     gEntities; // runtime id -> seqs
std::set<uint64_t>                                   gPlayers;  // seqs of add_player frames
std::unordered_map<Key, uint64_t, KeyHash>           gParts;    // sub-chunk -> seq of the subchunk frame that has it last
std::unordered_map<uint64_t, size_t>                 gLiveParts; // subchunk seq -> sub-chunks no newer frame carries
uint64_t                                             gBytes    = 0;
bool                                                 gComplete = true;
int                                                  gLastLink = 0;
TapJournalStats                                      gStats;

void eraseLocked(std::map<uint64_t, Entry>::iterator it) {
    Entry const& e = it->second;
    gBytes -= e.frame->bytes.size();
    if (e.key) gKeyed.erase(*e.key);
    if (e.entity) {
        if (auto eit = gEntities.find(*e.entity); eit != gEntities.end()) {
            eit->second.erase(it->first);
            if (eit->second.empty()) gEntities.erase(eit);
        }
    }
    if (e.id == kAddPlayer) gPlayers.erase(it->first);
    for (Key const& part : e.parts)
        if (auto pit = gParts.find(part); pit != gParts.end() && pit->second == it->first) gParts.erase(pit);
    gLiveParts.erase(it->first);
    gFrames.erase(it);
}

void clearLocked() {
    gFrames.clear();
    gKeyed.clear();
    gEntities.clear();
    gPlayers.clear();
    gParts.clear();
    gLiveParts.clear();
    gBytes    = 0;
    gComplete = true;
}

void resetLocked(char const* why) {
    if (!gFrames.empty() && options().trace)
        log().info("tap journal: reset ({}; {} frames, {} bytes dropped)", why, gFrames.size(), gBytes);
    clearLocked();
    ++gStats.resets;
}

void dropEntityLocked(uint64_t runtime) {
    auto eit = gEntities.find(runtime);
    if (eit == gEntities.end()) return;
    std::set<uint64_t> seqs = std::move(eit->second);
    gEntities.erase(eit);
    for (uint64_t seq : seqs)
        if (auto it = gFrames.find(seq); it != gFrames.end()) eraseLocked(it);
    ++gStats.pruned;
}

// add_player carries its unique id (li64) only after its metadata. It is followed by the
// permission level (0-4), the command permission (0-5) and the ability layer count (1-8).
bool addPlayerHasUnique(std::string const& bytes, int64_t unique) {
    char pattern[8];
    for (int k = 0; k < 8; ++k) pattern[k] = static_cast<char>(static_cast<uint64_t>(unique) >> (8 * k));
    std::string_view needle(pattern, sizeof(pattern));
    for (size_t at = bytes.find(needle, 18); at != std::string::npos; at = bytes.find(needle, at + 1)) {
        if (at + 11 > bytes.size()) break;
        auto perm = static_cast<uint8_t>(bytes[at + 8]), cmd = static_cast<uint8_t>(bytes[at + 9]);
        auto layers = static_cast<uint8_t>(bytes[at + 10]);
        if (perm <= 4 && cmd <= 5 && layers >= 1 && layers <= 8) return true;
    }
    return false;
}

// remove_entity names the unique id: drop the entity's add_* frame and every frame of its runtime id.
void removeEntityLocked(int64_t unique) {
    for (uint32_t id : {kAddEntity, kAddItemEntity}) {
        auto kit = gKeyed.find(Key{id, unique});
        if (kit == gKeyed.end()) continue;
        auto it = gFrames.find(kit->second);
        if (it == gFrames.end()) continue;
        if (it->second.entity) dropEntityLocked(*it->second.entity);
        else eraseLocked(it);
        return;
    }
    for (uint64_t seq : gPlayers) {
        auto it = gFrames.find(seq);
        if (it == gFrames.end() || !addPlayerHasUnique(it->second.frame->bytes, unique)) continue;
        dropEntityLocked(*it->second.entity);
        return;
    }
}

void recordLocked(std::shared_ptr<tap::Frame const> const& f, Parsed const& p) {
    ++gStats.seen;
    if (p.id == kStartGame) resetLocked("start_game");
    if (transient(p.id)) {
        ++gStats.transient;
        return;
    }
    if (p.id == kRemoveEntity) {
        removeEntityLocked(p.unique);
        return;
    }
    std::map<uint64_t, Entry>::iterator old = gFrames.end();
    if (p.key) {
        if (auto kit = gKeyed.find(*p.key); kit != gKeyed.end()) old = gFrames.find(kit->second);
    }
    uint64_t freed = old != gFrames.end() ? old->second.frame->bytes.size() : 0;
    uint64_t cap   = static_cast<uint64_t>(std::max(0, options().tapJournalMiB)) << 20;
    if (gBytes - freed + f->bytes.size() > cap) {
        if (gComplete)
            log().warn("tap journal: over tapJournalMiB ({}); a late attach is incomplete until the next join", options().tapJournalMiB);
        gComplete = false;
        ++gStats.overCap;
        return;
    }
    if (old != gFrames.end()) {
        eraseLocked(old);
        ++gStats.compacted;
    }
    gBytes += f->bytes.size();
    if (p.key) gKeyed[*p.key] = f->seq;
    if (p.entity) gEntities[*p.entity].insert(f->seq);
    if (p.id == kAddPlayer) gPlayers.insert(f->seq);
    gFrames[f->seq] = Entry{f, p.key, p.entity, p.id, p.parts};
    if (!p.parts.empty()) {
        gLiveParts[f->seq] = p.parts.size();
        for (Key const& part : p.parts) {
            auto [pit, added] = gParts.try_emplace(part, f->seq);
            if (added || pit->second == f->seq) continue;
            uint64_t older = pit->second;
            pit->second    = f->seq;
            auto live      = gLiveParts.find(older);
            if (live == gLiveParts.end() || --live->second > 0) continue;
            if (auto oit = gFrames.find(older); oit != gFrames.end()) {
                eraseLocked(oit);
                ++gStats.compacted;
            }
        }
    }
}

void journalEmit(tap::Frame&& f) {
    if (gInstalled.load(std::memory_order_acquire) && !(f.flags & tap::Replay)) {
        bool keep = f.kind == tap::Kind::Clientbound;
        // Parse and copy outside the lock: the lock only guards the maps.
        Parsed                            p;
        std::shared_ptr<tap::Frame const> copy;
        if (keep) {
            p = parse(f.bytes);
            if (!transient(p.id) && p.id != kRemoveEntity) copy = std::make_shared<tap::Frame const>(f);
        }
        std::lock_guard lock(gMutex);
        int             link = tap::gLinkType.load(std::memory_order_acquire);
        if (link != gLastLink) {
            gLastLink = link;
            if (link != static_cast<int>(tap::Link::None)) resetLocked("new link");
        }
        if (f.kind == tap::Kind::Marker && f.bytes.find("\"LINK_UP\"") != std::string::npos) resetLocked("LINK_UP");
        if (keep) recordLocked(copy, p);
    }
    if (auto prev = gPrev.load(std::memory_order_acquire)) prev(std::move(f));
}

void journalReplay(tap::EmitFn sink) {
    if (!sink) return;
    std::vector<std::shared_ptr<tap::Frame const>> frames;
    {
        std::lock_guard lock(gMutex);
        frames.reserve(gFrames.size());
        for (auto const& [seq, e] : gFrames) frames.push_back(e.frame);
        ++gStats.replays;
    }
    for (auto const& p : frames) {
        tap::Frame f = *p;
        f.flags      = static_cast<uint8_t>(f.flags | tap::Replay);
        sink(std::move(f));
    }
}

} // namespace

void installTapJournal() {
    if (gInstalled.exchange(true)) return;
    // Still chained from an earlier install that could not unlink: recording again is enough.
    if (!gChained) {
        // gPrev is set before journalEmit becomes reachable, so no frame misses the previous sink.
        tap::EmitFn prev = tap::gEmit.load(std::memory_order_acquire);
        do gPrev.store(prev, std::memory_order_release);
        while (!tap::gEmit.compare_exchange_weak(prev, &journalEmit, std::memory_order_acq_rel));
        gChained = true;
    }
    tap::gReplay.store(&journalReplay, std::memory_order_release);
    log().info("tap journal: on ({} MiB)", options().tapJournalMiB);
}

void uninstallTapJournal() {
    if (!gInstalled.exchange(false)) return;
    tap::ReplayFn replay = &journalReplay;
    tap::gReplay.compare_exchange_strong(replay, nullptr);
    // Unlink only while nothing chained on top of the journal; otherwise journalEmit stays in the
    // chain and just forwards (gInstalled is off).
    tap::EmitFn self = &journalEmit;
    if (tap::gEmit.compare_exchange_strong(self, gPrev.load())) gChained = false;
    std::lock_guard lock(gMutex);
    clearLocked();
}

TapJournalStats tapJournalStats() {
    std::lock_guard lock(gMutex);
    TapJournalStats s = gStats;
    s.enabled         = gInstalled.load();
    s.frames          = gFrames.size();
    s.bytes           = gBytes;
    s.complete        = gComplete;
    return s;
}

} // namespace bedd
