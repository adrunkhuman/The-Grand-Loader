#include "features/performance/cb_cull.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "game/world.h"
#include "hooks/memory.h"
#include "log.h"

// Background (docs/re/systems/casus-belli.md, docs/cull-cb-checks.md):
//
//   AI_DailyUpdate -> AI_ScoreWarTargets (0x0085DB40) / AI_ScoreDiplomaticTargets (0x0085E0E0)
//     for every other country D:  CBManager_IsAnyCBUsable(D->rel[self], &D, &self)      0x0092EDE0
//       for every CB type T (manager order):
//         if (!T->IsRealType()) continue;                                            vtable slot 7
//         IsCBUsable(rel, &D, &self; EDI = T.index):                                  0x0092EEC0
//           walk rel+0x50 (CBs self holds against D) for one of type T;
//           if none and !T.always (+0x132) -> false;
//           else build two CEventScopes (self; THIS = D) and return T->CanUse(scope)  vtable slot 8
//       return true at the first usable type
//
// Only known native type layouts/dispatch are eligible; trigger trees need not be pure. Walk types
// in manager order, using a held-type membership index instead of a list walk per type. Rebuild it
// after every false CanUse: an unsupported tree may mutate the held list or other shared state.
// Each CanUse runs natively with a FRESH pair of scopes; no predicate results are cached.

namespace cp::features {

namespace {

// --- v2game.exe 3.04 (RVA = VA - 0x00400000) ---------------------------------------------------
constexpr uint32_t kIsAnyCBUsableRva = 0x52EDE0;    // CBManager_IsAnyCBUsable(rel, defRef, attRef), __stdcall
constexpr uint32_t kScopeCtorRva = 0x4A8650;        // CEventScope::ctor(this, parent = 0)
constexpr uint32_t kScanCallSites[] = {0x45DCDA,    // in AI_ScoreWarTargets
                                       0x45E5FB};   // in AI_ScoreDiplomaticTargets
constexpr uint32_t kCBManagerPtrRva = 0xF1ACAC;     // DAT_0131ACAC, +0xC/+0x10 = vector<CCasusBelliType*>

constexpr uint32_t kTypeIndex = 0x08;       // CCasusBelliType: manager vector index
constexpr uint32_t kCBTypeName = 0x14;       // std::string (SSO): buffer/pointer +0x14, size +0x24, capacity +0x28
constexpr uint32_t kCBTypeAlways = 0x132;    // `always = yes`
constexpr uint32_t kVtableIsRealType = 0x1C; // CCasusBelliType slot 7
constexpr uint32_t kVtableCanUse = 0x20;     // CCasusBelliType slot 8: bool CanUse(scope) = can_use.Evaluate
constexpr uint32_t kRelationHeldList = 0x50; // relation object: singly linked list of held CCasusBelli
constexpr uint32_t kHeldNodeCB = 0x00;       //   node: +0 CCasusBelli*, +8 next
constexpr uint32_t kHeldNodeNext = 0x08;
constexpr uint32_t kCBTypeIndex = 0x24;      // CCasusBelli: index of its type in the manager list
constexpr uint32_t kScopeTag = 0x08;         // CEventScope: country tag
constexpr uint32_t kScopeThis = 0x24;        //              THIS scope pointer
constexpr size_t kScopeSize = 0x60;          // ctor writes up to +0x48

constexpr int32_t kReportEveryDays = 30;
constexpr uint64_t kSample = 64;  // timing sample rate (power of two)

// 3 stack arguments, callee cleans (`RET 0xC`), no register arguments.
using IsAnyCBUsableFn = char(__stdcall*)(void* relation, void* defender_ref, void* attacker_ref);
// __thiscall in the game; __fastcall takes `this` in ecx and ignores edx.
using EvaluateFn = char(__fastcall*)(void* self, void* edx, void* scope);
using ScopeCtorFn = void*(__fastcall*)(void* self, void* edx, void* parent);

IsAnyCBUsableFn g_is_any_cb_usable = nullptr;
ScopeCtorFn g_scope_ctor = nullptr;
bool g_profiling = false;

bool g_types_safe = false;

struct TypeInfo {
    uintptr_t type = 0;
    bool real = false;  // the known CNullCasusBelliType IsRealType returns false
};
std::vector<TypeInfo> g_types;

struct Stats {
    uint64_t attempts = 0, native_fallbacks = 0;
    uint64_t index_builds = 0, held_nodes = 0;
    uint64_t scans = 0;          // fast-path IsAnyCBUsable calls from the AI
    uint64_t can_use_always = 0; // CanUse calls for `always` types
    uint64_t can_use_held = 0;   // CanUse calls for held, non-`always` types
    uint64_t hits = 0;           // scans that returned true
    uint64_t scan_ticks = 0;     // sampled
    std::vector<uint64_t> can_use_per_type;
};
Stats g_stats;
int32_t g_stats_day = -1;
int32_t g_window_start_day = -1;
int64_t g_window_start_qpc = 0;
int64_t g_qpc_freq = 0;

using game::read32;
inline uint8_t read8(uintptr_t at) { return *reinterpret_cast<const uint8_t*>(at); }

inline int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline double ms(uint64_t ticks) {
    return g_qpc_freq ? 1000.0 * static_cast<double>(ticks) / static_cast<double>(g_qpc_freq) : 0.0;
}

int32_t current_day() { return game::day(game::world()); }

std::string cb_type_name(uintptr_t type) {
    uint32_t size = read32(type + kCBTypeName + 0x10);
    uint32_t capacity = read32(type + kCBTypeName + 0x14);
    const char* data = capacity > 15 ? reinterpret_cast<const char*>(read32(type + kCBTypeName))
                                     : reinterpret_cast<const char*>(type + kCBTypeName);
    return std::string(data, size > 64 ? 64 : size);
}

// --- native type validation ----------------------------------------------------------------------

inline bool can_use(uintptr_t type, void* scope) {
    auto fn = reinterpret_cast<EvaluateFn>(read32(read32(type) + kVtableCanUse));
    return fn(reinterpret_cast<void*>(type), nullptr, scope) != 0;
}

// Validate every type and the actual dispatch slots BEFORE any scope construction/evaluation.
// Do not call an unknown IsRealType to classify it, or trust a cached vector address after reload.
uintptr_t g_types_begin = 0;
bool refresh_types() {
    uintptr_t manager = read32(hooks::exe_addr(kCBManagerPtrRva));
    if (!manager) {
        g_types_safe = false;
        return false;  // let the original initialize its singleton
    }
    uintptr_t begin = read32(manager + 0x0C), end = read32(manager + 0x10);
    if (end < begin || (end - begin) % 4 || (!begin && end)) {
        g_types_safe = false;
        return false;
    }
    size_t count = (end - begin) / 4;
    bool changed = begin != g_types_begin || count != g_types.size();
    g_types.resize(count);
    bool safe = true;
    size_t always = 0;
    for (size_t i = 0; i < count; i++) {
        TypeInfo& info = g_types[i];
        uintptr_t type = read32(begin + i * 4);
        changed |= info.type != type;
        info = TypeInfo{type, false};
        if (!type) {
            safe = false;
            continue;
        }
        uintptr_t vtable = read32(type);
        bool real = vtable == hooks::exe_addr(0xA33724);
        if ((!real && vtable != hooks::exe_addr(0xA3374C)) ||
            read32(vtable + kVtableIsRealType) != hooks::exe_addr(real ? 0x631490 : 0x18CFD0) ||
            read32(vtable + kVtableCanUse) != hooks::exe_addr(real ? 0x50A7F0 : 0x233620)) {
            safe = false;
            continue;
        }
        info.real = real;
        // Native passes type.index to IsCBUsable, which indexes the manager again. Require the
        // canonical mapping (including bounds); the null type's unused index need not be valid.
        if (real && (read32(type + kTypeIndex) >= count || read32(type + kTypeIndex) != i)) {
            safe = false;
        }
        always += real && read8(type + kCBTypeAlways) ? 1 : 0;
    }
    if (changed || safe != g_types_safe) {
        logf("cb_cull: %zu CB types, %zu `always`; %s", count, always,
             safe ? "native-order fast path eligible" : "unsupported type layout/dispatch/index: native scans only");
    }
    g_types_begin = begin;
    g_types_safe = safe;
    return safe;
}

// --- scopes --------------------------------------------------------------------------------------

// Exactly what IsCBUsable builds before CanUse: scope = attacker, scope->THIS = defender, no FROM.
struct ScopePair {
    alignas(16) unsigned char main[kScopeSize];
    alignas(16) unsigned char this_scope[kScopeSize];
};

void build_scopes(ScopePair& s, void* attacker_ref, void* defender_ref) {
    std::memset(&s, 0, sizeof(s));
    g_scope_ctor(s.main, nullptr, nullptr);
    g_scope_ctor(s.this_scope, nullptr, nullptr);
    std::memcpy(s.main + kScopeTag, attacker_ref, 8);        // {tag, index}
    std::memcpy(s.this_scope + kScopeTag, defender_ref, 8);
    *reinterpret_cast<unsigned char**>(s.main + kScopeThis) = s.this_scope;
}

// --- held-type membership ------------------------------------------------------------------------

void index_held_types(void* relation, std::vector<uint8_t>& held) {
    if (g_profiling) {
        g_stats.index_builds++;
    }
    std::fill(held.begin(), held.end(), 0);
    for (uintptr_t node = read32(reinterpret_cast<uintptr_t>(relation) + kRelationHeldList); node;
         node = read32(node + kHeldNodeNext)) {
        if (g_profiling) {
            g_stats.held_nodes++;
        }
        uintptr_t cb = read32(node + kHeldNodeCB);
        if (!cb) {
            continue;
        }
        uint32_t index = read32(cb + kCBTypeIndex);
        if (index < held.size()) {
            held[index] = 1;  // duplicates still cause only one CanUse per manager type
        }
    }
}

// --- statistics ----------------------------------------------------------------------------------

void report_window(int32_t last_day) {
    int32_t days = last_day - g_window_start_day + 1;
    if (days <= 0 || g_stats.attempts == 0) {
        return;
    }
    double wall_ms = ms(static_cast<uint64_t>(qpc() - g_window_start_qpc));
    auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    logf("cb_cull: days %d-%d (%d days, %.1f ms/day wall): %llu AI CB scans/day in %.2f ms/day (%llu hits/day); "
         "can_use calls/day: %llu for `always` types, %llu for held types",
         g_window_start_day, last_day, days, wall_ms / days, ull(g_stats.scans / days), ms(g_stats.scan_ticks) / days,
         ull(g_stats.hits / days), ull(g_stats.can_use_always / days), ull(g_stats.can_use_held / days));

    logf("cb_cull: coverage: %llu/%llu scans optimized, %llu native fallbacks; %llu held-list builds, %llu nodes visited",
         ull(g_stats.scans), ull(g_stats.attempts), ull(g_stats.native_fallbacks),
         ull(g_stats.index_builds), ull(g_stats.held_nodes));

    if (!g_types_safe) {
        return; // a reload/unknown layout must not dereference stale type names for profiling
    }
    std::vector<size_t> order;
    for (size_t i = 0; i < g_stats.can_use_per_type.size() && i < g_types.size(); i++) {
        if (g_stats.can_use_per_type[i]) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(),
              [](size_t a, size_t b) { return g_stats.can_use_per_type[a] > g_stats.can_use_per_type[b]; });
    for (size_t n = 0; n < order.size() && n < 8; n++) {
        logf("cb_cull:   %-28s can_use %llu calls/day", cb_type_name(g_types[order[n]].type).c_str(),
             ull(g_stats.can_use_per_type[order[n]] / days));
    }
}

void roll_stats(int32_t day) {
    if (g_stats_day == day) {
        return;
    }
    if (g_window_start_day < 0) {
        g_window_start_day = day;
        g_window_start_qpc = qpc();
    } else if (day - g_window_start_day >= kReportEveryDays) {
        report_window(g_stats_day);
        g_stats = Stats{};
        g_window_start_day = day;
        g_window_start_qpc = qpc();
    }
    g_stats_day = day;
}

// --- the hook ------------------------------------------------------------------------------------

char __stdcall is_any_cb_usable_hook(void* relation, void* defender_ref, void* attacker_ref) {
    int32_t day = current_day();
    if (day < 0) {
        return g_is_any_cb_usable(relation, defender_ref, attacker_ref);
    }
    // Include per-scan type validation and membership indexing in the sampled cost.
    bool sampled = g_profiling && (g_stats.scans & (kSample - 1)) == 0;
    int64_t start = sampled ? qpc() : 0;
    // Refresh before roll_stats: reporting may read type names, including across a reload.
    bool types_ready = refresh_types();
    if (g_profiling) {
        roll_stats(day);
        g_stats.attempts++;
    }
    if (!relation || !types_ready) {
        if (g_profiling) {
            g_stats.native_fallbacks++;
        }
        return g_is_any_cb_usable(relation, defender_ref, attacker_ref);
    }

    if (g_profiling) {
        g_stats.scans++;
        if (g_stats.can_use_per_type.size() < g_types.size()) {
            g_stats.can_use_per_type.resize(g_types.size());
        }
    }

    char result = 0;
    std::vector<uint8_t> held(g_types.size());
    index_held_types(relation, held);
    ScopePair scopes;
    for (size_t i = 0; i < g_types.size(); i++) {
        const TypeInfo& info = g_types[i];
        if (!info.real) {
            continue;
        }
        bool always = read8(info.type + kCBTypeAlways) != 0;
        if (!always && !held[i]) {
            continue;
        }
        if (g_profiling) {
            g_stats.can_use_always += always ? 1 : 0;
            g_stats.can_use_held += always ? 0 : 1;
            g_stats.can_use_per_type[i]++;
        }
        build_scopes(scopes, attacker_ref, defender_ref);
        if (can_use(info.type, scopes.main)) {
            result = 1;
            break;
        }
        // No tree inspection or speculative evaluation: even an impure/unknown tree runs here
        // exactly once in native order. Its writes must be visible to every later ownership gate.
        index_held_types(relation, held);
    }

    if (g_profiling) {
        g_stats.hits += result ? 1 : 0;
        if (sampled) {
            g_stats.scan_ticks += static_cast<uint64_t>(qpc() - start) * kSample;
        }
    }
    return result;
}

}  // namespace

void install_cb_cull(const Config& config) {
    if (!config.cull_cb_checks) {
        logf("cb_cull: disabled");
        return;
    }
    g_profiling = config.cull_cb_profiling;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_freq = freq.QuadPart;

    g_scope_ctor = reinterpret_cast<ScopeCtorFn>(hooks::exe_addr(kScopeCtorRva));

    uintptr_t original_scan = hooks::exe_addr(kIsAnyCBUsableRva);
    if (!hooks::expect_bytes(reinterpret_cast<uintptr_t>(g_scope_ctor), "\x55\x8B\xEC", 3) ||
        !hooks::expect_bytes(original_scan, "\x55\x8B\xEC\x83\xE4\xF8", 6)) {
        logf("cb_cull: IsAnyCBUsable / CEventScope ctor not recognised, feature off");
        return;
    }
    for (uint32_t site : kScanCallSites) {
        if (!hooks::expect_bytes(hooks::exe_addr(site), "\xE8", 1)) {
            logf("cb_cull: AI scan call site %08X not recognised, feature off", site + 0x400000);
            return;
        }
    }
    g_is_any_cb_usable = reinterpret_cast<IsAnyCBUsableFn>(original_scan);
    uintptr_t wrapper = reinterpret_cast<uintptr_t>(&is_any_cb_usable_hook);
    for (uint32_t site : kScanCallSites) {
        if (!hooks::redirect_call(hooks::exe_addr(site), original_scan, wrapper)) {
            logf("cb_cull: AI scan call site %08X does not call IsAnyCBUsable, feature off", site + 0x400000);
            return;  // an already redirected site still behaves correctly
        }
    }
    logf("cb_cull: active (native type order; fresh scopes and held membership per CanUse), profiling %s",
         g_profiling ? "on" : "off");
}

}  // namespace cp::features
