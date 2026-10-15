#include "features/performance/decision_cull.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "features/performance/trigger_safety.h"
#include "features/performance/scope_snapshot.h"
#include "hooks/hourly_tick.h"
#include "game/world.h"
#include "hooks/memory.h"
#include "log.h"
#include "parallel/worker_pool.h"

// Background (docs/re/systems/decisions.md, docs/cull-decision-checks.md, docs/re/raw/decision-keyword-map.txt):
//
//   AI_DecisionUpdate (0x00821190), once per AI country per day, driven from the per-frame AI job
//   (CInGameIdler::Update -> FUN_006874C0 -> FUN_0081E9E0 -> CALL at 0x0081EA9B, ECX = AI object):
//     reads DAT_01319730 directly; for every decision d in both lists:
//       if (d->vt[6]() && d->potential.Evaluate(scope) && d->allow.Evaluate(scope)) { ai_will_do; roll; execute }
//   This is where the ~529 k potential evaluations a day come from (1651 decisions x ~320 countries).
//   AI_DiplomacyUpdate (0x0085D270) has the same two loops through DecisionManager_Get() (CALL
//   0x00875040 at 0x0085D34F / 0x0085D459, ESI = AI object, +0x0C index) but runs for a few countries
//   a day only. The player's alert (FUN_005399C0) and the UI read the global too; they are left alone.
//
// cull_decision_checks rejects failing deterministic leading gates at their native evaluation point.
// parallel_decisions consumes read-only potential runs in native B-then-A order, resuming after
// serial barriers. A true potential invalidates later results before native allow/ai_will_do.
// No private manager or cross-country eligibility cache; diplomacy and UI stay native.
//
// A private potential vtable dispatches live AI checks; other consumers pass through. Optional
// decision_profiling times that dispatch. No decision object or manager list is removed/reordered.

namespace cp::features {

namespace {

// --- v2game.exe 3.04 (RVA = VA - 0x00400000) ---------------------------------------------------
constexpr uint32_t kDecisionManagerPtrRva = 0xF19730;    // DAT_01319730; lists at +0xC/+0x10 and +0x28/+0x2C
constexpr uint32_t kAiDecisionsRva = 0x421190;            // AI_DecisionUpdate(ai): the daily per-country loop (frame-driven)
constexpr uint32_t kAiDecisionsCallSiteRva = 0x41EA9B;    // in FUN_0081E9E0, `MOV ECX,ESI; CALL`; ESI = AI object
constexpr uint32_t kAndVtableRva = 0x9F9994;             // CAndTrigger::vftable
constexpr uint32_t kNotVtableRva = 0xA2FBA8;             // CNotTrigger::vftable
constexpr uint32_t kTagVtableRva = 0xA30C80;             // CTagTrigger::vftable (+0x1C tag, +0x20 country index)
constexpr uint32_t kHasCountryFlagVtableRva = 0xA307E8;  // CHasCountryFlagTrigger::vftable
constexpr uint32_t kHasGlobalFlagVtableRva = 0xA30820;   // CHasGlobalFlagTrigger::vftable
constexpr uint32_t kIsAiVtableRva = 0xA30EE8;            // CIsAiTrigger::vftable (`ai = yes/no`, value at +0x1C)

constexpr uint32_t kListA = 0x0C;             // vector<CDecision*>: begin +0, end +4, capacity +8
constexpr uint32_t kListB = 0x28;
constexpr uint32_t kDecisionName = 0x08;      // std::string (SSO): buffer/pointer +0x08, size +0x18, capacity +0x1C
constexpr uint32_t kDecisionPotential = 0x24; // embedded CAndTrigger
constexpr uint32_t kBlockBegin = 0x08;        // trigger block: vector<CTrigger*> begin
constexpr uint32_t kBlockEnd = 0x0C;          //                                 end
constexpr uint32_t kTriggerThisFlag = 0x18;   // value was `THIS`
constexpr uint32_t kTriggerFromFlag = 0x19;   // value was `FROM`
constexpr uint32_t kTagTriggerIndex = 0x20;   // CTagTrigger: country index of the literal tag
constexpr uint32_t kVtableEvaluate = 0x18;    // CTrigger slot 6: bool Evaluate(scope)
constexpr size_t kVtableSlotsCopied = 16;     // CAndTrigger has 12 used slots; extra entries are never called

constexpr int32_t kReportEveryDays = 30;  // profiling report period
constexpr uint64_t kSample = 16;          // profiling: 1 in 16 evaluations is timed (power of two)

// __thiscall in the game; __fastcall takes `this` in ecx and ignores edx.
using EvaluateFn = char(__fastcall*)(void* self, void* edx, void* scope);

bool g_cull = false;
bool g_profiling = false;
EvaluateFn g_original_evaluate = nullptr;
uint32_t g_fake_vtable[kVtableSlotsCopied];
uintptr_t g_and_vtable = 0;
uintptr_t g_not_vtable = 0;
uintptr_t g_tag_vtable = 0;
uintptr_t g_has_country_flag_vtable = 0;
uintptr_t g_has_global_flag_vtable = 0;
uintptr_t g_is_ai_vtable = 0;
int64_t g_qpc_freq = 0;

struct DecisionInfo {
    uintptr_t decision = 0;
    int32_t locked_country = -1;  // top-level `tag = X` with a literal tag: only X can ever pass
    uintptr_t gate = 0;           // top-level NOT block made only of has_country_flag / has_global_flag
    uintptr_t ai_child = 0;       // top-level `ai = yes/no`
    // profiling, accumulated since the decisions were loaded (sampled: calls, passed, ticks)
    uint64_t calls = 0;
    uint64_t passed = 0;
    uint64_t ticks = 0;
};
std::vector<DecisionInfo> g_decisions;
size_t g_tag_locked = 0;
size_t g_gated = 0;
size_t g_ai_gated = 0;

// potential address -> decision index (profiling only); open addressing, usual probe length one.
struct LookupSlot {
    uintptr_t key = 0;
    uint32_t index = 0;
};
std::vector<LookupSlot> g_lookup;
uint32_t g_lookup_shift = 32;

// cull statistics since load (exact)
uint64_t g_list_builds = 0;
uint64_t g_list_build_ticks = 0;
uint64_t g_removed_tag = 0;   // live potential rejections by leading gates
uint64_t g_removed_ai = 0;
uint64_t g_removed_dead = 0;
uint64_t g_ai_requests = 0;   // native daily AI updates
uint64_t g_ai_decisions_seen = 0;  // tracked native potential calls

// profiling totals since load
uint64_t g_total_calls = 0;  // exact: evaluations that went through the timed hook
uint64_t g_sample_counter = 0;
int32_t g_first_day = -1;
int64_t g_first_qpc = 0;
int32_t g_last_day = -1;
int32_t g_period_start_day = -1;
std::wstring g_report_path;

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

size_t country_count() {
    uintptr_t manager = game::country_manager();
    return manager ? read32(manager) : 0;
}

uintptr_t decision_manager() { return read32(hooks::exe_addr(kDecisionManagerPtrRva)); }

std::string decision_name(uintptr_t decision) {
    uint32_t size = read32(decision + kDecisionName + 0x10);
    uint32_t capacity = read32(decision + kDecisionName + 0x14);
    const char* data = capacity > 15 ? reinterpret_cast<const char*>(read32(decision + kDecisionName))
                                     : reinterpret_cast<const char*>(decision + kDecisionName);
    return std::string(data, size > 64 ? 64 : size);
}

inline bool evaluate(uintptr_t trigger, void* scope) {
    auto fn = reinterpret_cast<EvaluateFn>(read32(read32(trigger) + kVtableEvaluate));
    return fn(reinterpret_cast<void*>(trigger), nullptr, scope) != 0;
}

// --- guarded leading-gate classification (repeated at use) -----------------------------------------

bool block_is_only_flag_checks(uintptr_t block) {
    uintptr_t begin = read32(block + kBlockBegin), end = read32(block + kBlockEnd);
    if (begin == end || !trigger_safety::Inspector::valid_range(begin, end)) {
        return false;
    }
    for (uintptr_t it = begin; it < end; it += 4) {
        uintptr_t child = read32(it);
        uintptr_t vtable = read32(child);
        if (vtable != g_has_country_flag_vtable && vtable != g_has_global_flag_vtable) {
            return false;
        }
        if (read8(child + kTriggerThisFlag) != 0 || read8(child + kTriggerFromFlag) != 0) {
            return false;
        }
    }
    return true;
}

void classify(DecisionInfo& info, trigger_safety::Inspector safety, uint32_t country = UINT32_MAX) {
    uintptr_t block = info.decision + kDecisionPotential;
    uintptr_t vtable = read32(block);
    uintptr_t begin = read32(block + kBlockBegin), end = read32(block + kBlockEnd);
    if (read32(read32(info.decision) + kVtableEvaluate) != hooks::exe_addr(0x631490) ||
        (vtable != g_and_vtable && vtable != reinterpret_cast<uintptr_t>(g_fake_vtable)) ||
        read32(vtable + kVtableEvaluate) != (vtable == g_and_vtable
            ? reinterpret_cast<uintptr_t>(g_original_evaluate) : g_fake_vtable[kVtableEvaluate / 4]) ||
        !trigger_safety::Inspector::valid_range(begin, end)) {
        return;
    }
    for (uintptr_t it = begin; it < end; it += 4) {
        uintptr_t child = read32(it);
        safety = safety.certify_for(child);
        if (!safety.pure_tree(child)) {
            break;  // NEVER search past an impure/unsupported child for a later gate
        }
        uintptr_t vtable = read32(child);
        if (vtable == g_tag_vtable && info.locked_country < 0 && read8(child + kTriggerThisFlag) == 0 &&
            read8(child + kTriggerFromFlag) == 0) {
            info.locked_country = static_cast<int32_t>(read32(child + kTagTriggerIndex));
            if (country != UINT32_MAX && static_cast<uint32_t>(info.locked_country) != country) {
                return; // no need to certify later flag domains after a known failing tag
            }
        } else if (vtable == g_not_vtable && info.gate == 0 && block_is_only_flag_checks(child)) {
            info.gate = child;
        } else if (vtable == g_is_ai_vtable && info.ai_child == 0 && read8(child + kTriggerThisFlag) == 0) {
            info.ai_child = child;
        }
    }
}

// --- live short-circuit culling and worker runs --------------------------------------------------

constexpr int kMaxMismatchLogs = 20;
constexpr size_t kChunk = 1024;
constexpr int kPerTask = 16;
int g_parallel = 0;  // 0 off, 1 on, 2 shadow
bool g_frame_open = false;
bool g_in_ai = false;
DWORD g_ai_thread = 0;
int g_native_depth = 0;

struct PotentialCheck {
    uintptr_t decision;
    int result = -1;
};
std::vector<PotentialCheck> g_checks;
size_t g_next = 0, g_ready_end = 0;
alignas(16) unsigned char g_template[scope_snapshot::size];
uintptr_t g_template_origin = 0;

uint64_t g_par_frames = 0, g_par_countries = 0, g_par_ticks = 0;
uint64_t g_par_checked = 0, g_par_passed = 0, g_par_used = 0;
uint64_t g_shadow_compared = 0, g_shadow_mismatches = 0;
uint64_t g_native_checks = 0, g_gate_checks = 0, g_guard_fallbacks = 0;
uint64_t g_barriers[static_cast<size_t>(trigger_safety::BarrierReason::Count)]{};
int g_mismatch_logs = 0;

trigger_safety::Inspector inspector() {
    return {hooks::exe_base(), read32, read8, reinterpret_cast<uintptr_t>(g_fake_vtable)};
}

bool prepare_country(uintptr_t manager) {
    g_checks.clear();
    g_next = g_ready_end = 0;
    if (!manager) {
        return false;
    }
    // Do not replace the global manager. Native slot checks, order and loop control remain intact.
    for (uint32_t offset : {kListB, kListA}) {
        uintptr_t begin = read32(manager + offset), end = read32(manager + offset + 4);
        if (!trigger_safety::Inspector::valid_range(begin, end)) {
            g_checks.clear();
            return false;
        }
        for (uintptr_t it = begin; it < end; it += 4) {
            uintptr_t decision = read32(it);
            if (!decision) {
                g_checks.clear();
                return false;
            }
            g_checks.push_back({decision, -1});
        }
    }
    for (const PotentialCheck& check : g_checks) {
        uintptr_t potential = check.decision + kDecisionPotential;
        if (read32(potential) == g_and_vtable) {
            *reinterpret_cast<uint32_t*>(potential) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_fake_vtable));
        }
    }
    return true;
}

// Native slot 6 runs before potential, twice. Unknown implementations are barriers even if they
// eventually skip that entry; they might mutate state without ever calling our potential hook.
bool potential_parallel_safe(uintptr_t decision, const trigger_safety::Inspector& safety) {
    return safety.decision_potential_safe(decision);
}

struct WorkerContext {
    size_t begin, end;
    const trigger_safety::Inspector safety;
};

void precompute_decisions(int index, void* context) {
    const auto& work = *static_cast<const WorkerContext*>(context);
    size_t begin = work.begin + static_cast<size_t>(index) * kPerTask;
    size_t end = std::min(begin + kPerTask, work.end);
    alignas(16) unsigned char scope[scope_snapshot::size];
    for (size_t i = begin; i < end; ++i) {
        PotentialCheck& check = g_checks[i];
        if (!potential_parallel_safe(check.decision, work.safety)) {
            continue;  // guard must hold on the actual execution thread
        }
        scope_snapshot::copy(scope, g_template, g_template_origin);
        check.result = g_original_evaluate(reinterpret_cast<void*>(check.decision + kDecisionPotential),
                                           nullptr, scope) ? 1 : 0;
    }
}

bool compute_run(void* live_scope, trigger_safety::Inspector safety) {
    size_t end = g_next, limit = std::min(g_next + kChunk, g_checks.size());
    while (end < limit) {
        safety = safety.certify_for(g_checks[end].decision + kDecisionPotential);
        if (!potential_parallel_safe(g_checks[end].decision, safety)) {
            break;
        }
        g_checks[end++].result = -1;
    }
    if (end == g_next) {
        return false;
    }
    std::memcpy(g_template, live_scope, scope_snapshot::size);
    g_template_origin = reinterpret_cast<uintptr_t>(live_scope);
    WorkerContext context{g_next, end, safety};
    int64_t start = qpc();
    parallel::run(static_cast<int>((end - g_next + kPerTask - 1) / kPerTask), &precompute_decisions, &context);
    g_par_ticks += static_cast<uint64_t>(qpc() - start);
    g_ready_end = end;
    g_par_frames++;
    g_par_checked += end - g_next;
    return true;
}

char native_potential(void* self, void* scope) {
    g_native_depth++;
    char result = g_original_evaluate(self, nullptr, scope);
    g_native_depth--;
    return result;
}

// Only deterministic, read-only leading gates may be inspected out of order. In particular, never
// evaluate a random condition to decide whether to keep it and then reroll it natively.
char serial_potential(uintptr_t self, void* scope, const trigger_safety::Inspector& safety) {
    g_native_checks++;
    if (g_cull && g_parallel != 2) {
        DecisionInfo info;
        info.decision = self - kDecisionPotential;
        classify(info, safety, read32(reinterpret_cast<uintptr_t>(scope) + 0x0C));
        bool rejected = false;
        if (info.locked_country >= 0 && static_cast<uint32_t>(info.locked_country) !=
                read32(reinterpret_cast<uintptr_t>(scope) + 0x0C)) {
            g_removed_tag++;
            rejected = true;
        } else if (info.ai_child && !evaluate(info.ai_child, scope)) {
            g_removed_ai++;
            rejected = true;
        } else if (info.gate && !evaluate(info.gate, scope)) {
            g_removed_dead++;
            rejected = true;
        }
        if (rejected) {
            g_gate_checks++;
            return 0;
        }
    }
    if (g_parallel) {
        ++g_barriers[static_cast<size_t>(safety.barrier_reason(self))];
    }
    return native_potential(reinterpret_cast<void*>(self), scope);
}

char live_potential(uintptr_t self, void* scope) {
    if (!g_in_ai || g_native_depth) {
        return native_potential(reinterpret_cast<void*>(self), scope);
    }
    g_ai_decisions_seen++;
    if (g_next >= g_checks.size() || g_checks[g_next].decision + kDecisionPotential != self) {
        // A skipped custom slot or changed list may have mutated state. Never use results across it.
        g_ready_end = g_next;
        while (g_next < g_checks.size() && g_checks[g_next].decision + kDecisionPotential != self) {
            ++g_next;
        }
        if (g_next == g_checks.size()) {
            g_in_ai = false;
            return native_potential(reinterpret_cast<void*>(self), scope);
        }
    }
    if (g_parallel && g_frame_open) {
        if (g_next >= g_ready_end) {
            const auto safety = inspector();
            if (!compute_run(scope, safety)) {
                ++g_next;
                g_ready_end = g_next;
                return serial_potential(self, scope, safety);
            }
        }
        if (g_next < g_ready_end) {
            int result = g_checks[g_next++].result;
            if (result >= 0) {
                g_par_passed += result ? 1 : 0;
                if (g_parallel == 2) {
                    char serial = native_potential(reinterpret_cast<void*>(self), scope) ? 1 : 0;
                    g_shadow_compared++;
                    if (serial != result) {
                        g_shadow_mismatches++;
                        if (g_mismatch_logs++ < kMaxMismatchLogs) {
                            logf("parallel_decisions: MISMATCH day %d %s: serial %d, parallel %d", current_day(),
                                 decision_name(self - kDecisionPotential).c_str(), serial, result);
                        }
                    }
                    if (serial || serial != result) {
                        g_ready_end = g_next;
                    }
                    return serial;
                }
                g_par_used++;
                if (result) {
                    // Native allow and ai_will_do follow. They need no purity certificate because
                    // they stay serial, but may change the world/scope before the next potential.
                    g_ready_end = g_next;
                }
                return static_cast<char>(result);
            }
            g_guard_fallbacks++;
        } else {
            ++g_next;
        }
    } else {
        ++g_next;
    }
    g_ready_end = g_next;
    const auto safety = inspector();
    return serial_potential(self, scope, safety);
}

void frame_begin(void* queue_ref) { g_frame_open = g_parallel && queue_ref; }
void frame_end() { g_frame_open = false; }

void log_parallel_summary(int32_t last_day) {
    int32_t days = last_day - g_first_day + 1;
    if (!g_parallel || g_first_day < 0 || days <= 0 || g_par_frames == 0) {
        return;
    }
    auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    logf("parallel_decisions: days %d-%d: %llu AI updates, %llu batches, %.0f potentials/batch, %.2f ms/day batching "
         "(%d workers + game thread); %llu/%llu reached potentials served, %llu worker guard fallbacks",
         g_first_day, last_day, ull(g_par_countries), ull(g_par_frames),
         static_cast<double>(g_par_checked) / g_par_frames, ms(g_par_ticks) / days, parallel::workers(),
         ull(g_par_used), ull(g_ai_decisions_seen), ull(g_guard_fallbacks));
    logf("parallel_decisions: serial barriers: scope RNG %llu, scope writes %llu, shared writes %llu, "
         "runtime guards %llu, unclosed %llu", ull(g_barriers[1]), ull(g_barriers[2]), ull(g_barriers[3]),
         ull(g_barriers[4]), ull(g_barriers[5]));
    if (g_parallel == 2) {
        logf("parallel_decisions: shadow: %llu checks compared, %llu mismatches", ull(g_shadow_compared),
             ull(g_shadow_mismatches));
    }
}

// Native keeps its own manager, scopes, slot calls, chance rolls and command handling.
using AiDecisionsFn = int(__fastcall*)(void* ai, void* edx);
AiDecisionsFn g_original_ai_decisions = nullptr;

int __fastcall ai_decisions_hook(void* ai, void* edx) {
    if (g_in_ai) {
        // Re-entrant native AI work must not consume the outer country's cursor or results.
        g_ready_end = g_next;
        g_native_depth++;
        int nested = 0;
        __try {
            nested = g_original_ai_decisions(ai, edx);
        } __finally {
            g_native_depth--;
        }
        return nested;
    }
    if (current_day() < 0) {
        return g_original_ai_decisions(ai, edx);
    }
    int64_t start = qpc();
    bool ready = prepare_country(decision_manager());
    g_list_builds++;
    g_list_build_ticks += static_cast<uint64_t>(qpc() - start);
    g_ai_requests++;
    g_par_countries++;
    g_ai_thread = GetCurrentThreadId();
    g_in_ai = ready;
    int result = 0;
    __try {
        result = g_original_ai_decisions(ai, edx);
    } __finally {
        g_in_ai = false;
        g_next = g_ready_end = 0;
        g_native_depth = 0;
    }
    return result;
}

// --- potential dispatch and optional profiling ---------------------------------------

inline uint32_t lookup_hash(uintptr_t key) {
    return static_cast<uint32_t>((key >> 2) * 0x9E3779B1u) >> g_lookup_shift;
}

void lookup_build() {
    size_t size = 64;
    uint32_t shift = 26;
    while (size < g_decisions.size() * 4) {
        size <<= 1;
        shift--;
    }
    g_lookup.assign(size, LookupSlot{});
    g_lookup_shift = shift;
    for (size_t i = 0; i < g_decisions.size(); i++) {
        uintptr_t key = g_decisions[i].decision + kDecisionPotential;
        uint32_t at = lookup_hash(key);
        while (g_lookup[at].key != 0) {
            at = (at + 1) & static_cast<uint32_t>(size - 1);
        }
        g_lookup[at] = {key, static_cast<uint32_t>(i)};
    }
}

inline DecisionInfo* lookup(uintptr_t key) {
    if (g_lookup.empty()) {
        return nullptr;
    }
    uint32_t mask = static_cast<uint32_t>(g_lookup.size() - 1);
    for (uint32_t at = lookup_hash(key);; at = (at + 1) & mask) {
        const LookupSlot& slot = g_lookup[at];
        if (slot.key == key) {
            return &g_decisions[slot.index];
        }
        if (slot.key == 0) {
            return nullptr;
        }
    }
}

char __fastcall potential_hook(void* self, void*, void* scope) {
    if (g_in_ai && GetCurrentThreadId() != g_ai_thread) {
        return g_original_evaluate(self, nullptr, scope);
    }
    g_total_calls++;
    bool sampled = g_profiling && (g_sample_counter++ & (kSample - 1)) == 0;
    if (!sampled) {
        return live_potential(reinterpret_cast<uintptr_t>(self), scope);
    }
    int64_t start = qpc();
    char result = live_potential(reinterpret_cast<uintptr_t>(self), scope);
    uint64_t ticks = static_cast<uint64_t>(qpc() - start);
    if (DecisionInfo* info = lookup(reinterpret_cast<uintptr_t>(self))) {
        info->ticks += ticks;
        info->calls++;
        info->passed += result ? 1 : 0;
    }
    return result;
}

// --- reporting -----------------------------------------------------------------------------------

void log_cull_summary(int32_t last_day) {
    int32_t days = last_day - g_first_day + 1;
    if (g_first_day < 0 || days <= 0 || g_ai_requests == 0) {
        return;
    }
    auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    double per_request = static_cast<double>(g_ai_decisions_seen) / static_cast<double>(g_ai_requests);
    logf("decision_cull: days %d-%d: %.0f/%zu potentials reached per AI update (%llu updates/day); "
         "%llu sequence builds (%.2f ms/day), live rejections/update: %llu tag, %llu ai, %llu flag",
         g_first_day, last_day, per_request, g_decisions.size(), ull(g_ai_requests / days),
         ull(g_list_builds), ms(g_list_build_ticks) / days,
         ull(g_list_builds ? g_removed_tag / g_list_builds : 0), ull(g_list_builds ? g_removed_ai / g_list_builds : 0),
         ull(g_list_builds ? g_removed_dead / g_list_builds : 0));
}

void write_report(int32_t last_day) {
    int32_t days = last_day - g_first_day + 1;
    if (g_first_day < 0 || days <= 0 || g_decisions.empty()) {
        return;
    }
    uint64_t passed = 0, ticks = 0;
    for (const DecisionInfo& d : g_decisions) {
        passed += d.passed;
        ticks += d.ticks;
    }
    double wall_ms_day = ms(static_cast<uint64_t>(qpc() - g_first_qpc)) / days;
    double eval_ms_day = ms(ticks) * static_cast<double>(kSample) / days;
    auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };

    logf("decision_profiling: days %d-%d (%d days, %.1f ms/day wall): %llu potential evaluations/day, %.2f ms/day, "
         "%llu passed/day -> performance_patch_decisions.log",
         g_first_day, last_day, days, wall_ms_day, ull(g_total_calls / days), eval_ms_day, ull(passed * kSample / days));

    FILE* out = nullptr;
    if (_wfopen_s(&out, g_report_path.c_str(), L"w") != 0 || !out) {
        logf("decision_profiling: cannot write performance_patch_decisions.log");
        return;
    }
    fprintf(out,
            "# decision profiling: running per-day averages since the decisions were loaded; rewritten every %d game days.\n"
            "# days %d-%d (%d days), wall %.1f ms/day, cull_decision_checks=%d\n"
            "# %llu potential evaluations/day costing %.2f ms/day (1 in %llu timed), %llu passed/day\n"
            "# %zu decisions: %zu tag-locked (T), %zu with a flag gate (F), %zu with a top-level ai check (A)\n",
            kReportEveryDays, g_first_day, last_day, days, wall_ms_day, g_cull ? 1 : 0, ull(g_total_calls / days),
            eval_ms_day, ull(kSample), ull(passed * kSample / days), g_decisions.size(), g_tag_locked, g_gated,
            g_ai_gated);
    if (g_cull && g_ai_requests && g_list_builds) {
        fprintf(out,
                "# cull: %.0f potentials reached/update; live rejections/update: %llu tag, %llu ai, "
                "%llu flag (%llu sequence builds, %.2f ms/day)\n",
                static_cast<double>(g_ai_decisions_seen) / static_cast<double>(g_ai_requests),
                ull(g_removed_tag / g_list_builds), ull(g_removed_ai / g_list_builds), ull(g_removed_dead / g_list_builds),
                ull(g_list_builds), ms(g_list_build_ticks) / days);
    }
    fprintf(out, "#\n#   ms/day  evals/day  ns/eval  passed/day  TFA  decision\n");

    std::vector<size_t> order(g_decisions.size());
    for (size_t i = 0; i < order.size(); i++) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [](size_t a, size_t b) {
        if (g_decisions[a].ticks != g_decisions[b].ticks) {
            return g_decisions[a].ticks > g_decisions[b].ticks;
        }
        return g_decisions[a].calls > g_decisions[b].calls;
    });
    for (size_t i : order) {
        const DecisionInfo& d = g_decisions[i];
        double ns = d.calls ? 1e6 * ms(d.ticks) / static_cast<double>(d.calls) : 0.0;
        fprintf(out, "%9.3f %10llu %8.0f %11llu  %c%c%c  %s\n", ms(d.ticks) * static_cast<double>(kSample) / days,
                ull(d.calls * kSample / days), ns, ull(d.passed * kSample / days), d.locked_country >= 0 ? 'T' : '-',
                d.gate ? 'F' : '-', d.ai_child ? 'A' : '-', decision_name(d.decision).c_str());
    }
    fclose(out);
}

// --- bookkeeping, once per game hour ------------------------------------------------------------

uintptr_t list_begin(uintptr_t manager, uint32_t list) { return read32(manager + list); }
uintptr_t list_end(uintptr_t manager, uint32_t list) { return read32(manager + list + 4); }

// Re-reads the manager lists for reporting and profiling only. These classifications never
// authorize culling; the potential hook rechecks the live scope and guards at consumption.
void refresh_decisions() {
    uintptr_t manager = decision_manager();
    if (!manager) {
        return;
    }
    size_t count = 0;
    for (uint32_t list : {kListA, kListB}) {
        count += (list_end(manager, list) - list_begin(manager, list)) / 4;
    }
    bool same = g_decisions.size() == count;
    size_t n = 0;
    for (uint32_t list : {kListA, kListB}) {
        for (uintptr_t it = list_begin(manager, list), end = list_end(manager, list); same && it < end; it += 4, n++) {
            same = g_decisions[n].decision == read32(it);
        }
    }
    if (same) {
        return;
    }
    if (!g_decisions.empty()) {
        if (g_profiling) {
            write_report(g_last_day);
        }
        if (g_cull) {
            log_cull_summary(g_last_day);
            log_parallel_summary(g_last_day);
        }
    }
    g_decisions.clear();
    g_tag_locked = g_gated = g_ai_gated = 0;
    g_total_calls = 0;
    g_list_builds = g_list_build_ticks = g_removed_tag = g_removed_ai = g_removed_dead = 0;
    g_ai_requests = g_ai_decisions_seen = 0;
    g_par_frames = g_par_countries = g_par_ticks = g_par_checked = g_par_passed = g_par_used = 0;
    g_shadow_compared = g_shadow_mismatches = 0;
    g_native_checks = g_gate_checks = g_guard_fallbacks = 0;
    std::fill(std::begin(g_barriers), std::end(g_barriers), 0);
    g_checks.clear();
    g_first_day = -1;
    g_period_start_day = -1;
    {
        // Classification only; discard the snapshot before installing profiling vtables below.
        const auto safety = inspector().snapshot();
        for (uint32_t list : {kListA, kListB}) {
            for (uintptr_t it = list_begin(manager, list), end = list_end(manager, list); it < end; it += 4) {
                DecisionInfo info;
                info.decision = read32(it);
                uintptr_t potential = info.decision + kDecisionPotential;
                uintptr_t vtable = read32(potential);
                if (vtable != g_and_vtable && vtable != reinterpret_cast<uintptr_t>(g_fake_vtable)) {
                    // unexpected layout: keep it unclassified so the game still sees it
                    g_decisions.push_back(info);
                    continue;
                }
                classify(info, safety);
                g_tag_locked += info.locked_country >= 0 ? 1 : 0;
                g_gated += info.gate ? 1 : 0;
                g_ai_gated += info.ai_child ? 1 : 0;
                g_decisions.push_back(info);
            }
        }
    }
    if (g_profiling) {
        lookup_build();
        for (const DecisionInfo& d : g_decisions) {
            uintptr_t potential = d.decision + kDecisionPotential;
            if (read32(potential) == g_and_vtable) {
                *reinterpret_cast<uint32_t*>(potential) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_fake_vtable));
            }
        }
    }
    logf("decision_cull: %zu decisions (%zu tag-locked, %zu with a flag gate, %zu with an ai check), %zu countries%s",
         g_decisions.size(), g_tag_locked, g_gated, g_ai_gated, country_count(), g_profiling ? ", potentials timed" : "");
}

void hourly_tick() {
    refresh_decisions();
    int32_t day = current_day();
    if (day < 0 || day == g_last_day || g_decisions.empty()) {
        return;
    }
    if (g_first_day < 0) {
        g_first_day = day;
        g_first_qpc = qpc();
        g_period_start_day = day;
    } else {
        if (day - g_period_start_day >= kReportEveryDays) {
            if (g_profiling) {
                write_report(g_last_day);
            }
            if (g_cull) {
                log_cull_summary(g_last_day);
                log_parallel_summary(g_last_day);
            }
            g_period_start_day = day;
        }
    }
    g_last_day = day;
}

}  // namespace

void install_decision_cull(const Config& config) {
    g_cull = config.cull_decision_checks;
    g_profiling = config.decision_profiling;
    g_parallel = config.parallel_decisions;
    if (g_parallel && !g_cull) {
        logf("parallel_decisions: needs cull_decision_checks = 1 (shared potential dispatch), off");
        g_parallel = 0;
    }
    if (g_parallel && g_profiling) {
        logf("parallel_decisions: off while decision_profiling is on (the timing hook is not thread-safe)");
        g_parallel = 0;
    }
    if (g_parallel && !config.fast_ai_queue) {
        logf("parallel_decisions: needs fast_ai_queue = 1 (the batch runs in the frame that drains the AI queue), off");
        g_parallel = 0;
    }
    if (g_cull && !hooks::expect_bytes(hooks::exe_addr(0x631490), "\xB0\x01\xC3", 3)) {
        logf("decision_cull: CDecision slot 6 not recognised, culling/parallel off");
        g_cull = false;
        g_parallel = 0;
    }
    if (!g_cull && !g_profiling) {
        logf("decision_cull: disabled");
        return;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_freq = freq.QuadPart;
    g_report_path = std::wstring(host().game_dir) + L"\\performance_patch_decisions.log";

    g_and_vtable = hooks::exe_addr(kAndVtableRva);
    g_not_vtable = hooks::exe_addr(kNotVtableRva);
    g_tag_vtable = hooks::exe_addr(kTagVtableRva);
    g_has_country_flag_vtable = hooks::exe_addr(kHasCountryFlagVtableRva);
    g_has_global_flag_vtable = hooks::exe_addr(kHasGlobalFlagVtableRva);
    g_is_ai_vtable = hooks::exe_addr(kIsAiVtableRva);
    std::memcpy(g_fake_vtable, reinterpret_cast<const void*>(g_and_vtable), sizeof(g_fake_vtable));
    g_original_evaluate = reinterpret_cast<EvaluateFn>(g_fake_vtable[kVtableEvaluate / 4]);
    if (reinterpret_cast<uintptr_t>(g_original_evaluate) != hooks::exe_addr(0x4B8260) ||
        !hooks::expect_bytes(reinterpret_cast<uintptr_t>(g_original_evaluate), "\x55\x8B\xEC", 3)) {
        logf("decision_cull: CAndTrigger::Evaluate not recognised, feature off");
        return;
    }
    g_fake_vtable[kVtableEvaluate / 4] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&potential_hook));

    if (g_cull) {
        uintptr_t ai_decisions = hooks::exe_addr(kAiDecisionsRva);
        if (!hooks::expect_bytes(hooks::exe_addr(kAiDecisionsCallSiteRva), "\xE8", 1) ||
            !hooks::expect_bytes(ai_decisions, "\x55\x8B\xEC\x6A\xFF", 5)) {
            logf("decision_cull: AI_DecisionUpdate call site not recognised, feature off");
            return;
        }
        g_original_ai_decisions = reinterpret_cast<AiDecisionsFn>(ai_decisions);
        if (!hooks::redirect_call(hooks::exe_addr(kAiDecisionsCallSiteRva), ai_decisions,
                                  reinterpret_cast<uintptr_t>(&ai_decisions_hook))) {
            logf("decision_cull: AI_DecisionUpdate call site does not call it, feature off");
            return;
        }
    }
    if (!hooks::on_hourly_tick(&hourly_tick)) {
        logf("decision_cull: hourly tick hook unavailable, feature off");
        return;
    }
    logf("decision_cull: active (cull %s: live leading gates, no filtered manager; profiling %s)",
         g_cull ? "on" : "off", g_profiling ? "on" : "off");
    if (g_parallel) {
        logf("parallel_decisions: active (%s): live potential runs; native allow, ai_will_do and RNG",
             g_parallel == 2 ? "shadow mode, the game still uses its own evaluation" : "results used");
    }
}

void decisions_frame_begin(void* queue_ref) { frame_begin(queue_ref); }
void decisions_frame_end() { frame_end(); }

}  // namespace cp::features
