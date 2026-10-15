#include "features/performance/events_parallel.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "features/performance/trigger_safety.h"
#include "features/performance/scope_snapshot.h"
#include "game/world.h"
#include "hooks/memory.h"
#include "log.h"
#include "parallel/worker_pool.h"

// v2game.exe 3.04 (docs/re/systems/events.md, docs/re/systems/parallel-triggers.md):
//
//   DayTick, CALL EventManager_DailyScan (0x008A7CD0, stdcall 1, RET 4) at 0x006861E8.
//   The scan builds one CEventScope on its stack, then
//     odd days ((date - 0x029C55C0) / 24 % 2 == 1): for i in 1..countries-1, c = countries[i], idx = c+0x20;
//       skipped when idx == 0 or countries[idx] has no provinces; scope+0x08 = c+0x1C (tag), +0x0C = idx;
//       for each event e of countries[idx]+0x08..+0x0C (re-read every iteration):
//     even days: for p in 0..g_provinceSource->vt[0x24]()-1, province = world+0xACC[p], skipped when
//       province+0x12C (owner) == 0; scope+0x10 = p; for each event e of province+0x14..+0x18:
//   and per event: if e+0x30 (embedded CAndTrigger).Evaluate(scope): MTTH (0x008AE380), one Mersenne
//   Twister draw, and maybe Event_Fire (0x008A8120, thiscall 2, RET 8; CALLs at 0x008A7EF4 / 0x008A80D5),
//   which runs the event's immediate effects at once.
//
// parallel_events: before the scan, the (trigger, entity) sequence it will evaluate is listed in order and
// the trigger blocks are pointed at a private copy of the CAndTrigger vtable. When the scan calls one of
// them, the next chunk of the sequence is evaluated on the worker pool with copies of the scan's own
// scope, and each call is answered from it as long as it matches the next entry. A fired event changes
// the game, so the results computed before it are dropped and the next chunk is computed afresh. Calls
// that do not match (nested evaluations, a candidate list changed by an effect) run the original code.
// MTTH, the draw and Event_Fire stay on the game thread in the original order. Shadow mode (2) answers
// every eligible call with the original code and compares. Only contiguous pure runs enter a
// batch. Unsupported checks run once, natively, and form serial barriers. A true trigger also
// ends the batch: its following MTTH modifiers are not certified and can change the shared scope
// even when no event fires. The next run starts from the resulting live scope/world.

namespace cp::features {

namespace {

constexpr uint32_t kScanCallRva = 0x2861E8;
constexpr uint32_t kScanRva = 0x4A7CD0;
constexpr uint32_t kFireRva = 0x4A8120;
constexpr uint32_t kFireCallRvas[] = {0x4A7EF4, 0x4A80D5};
constexpr uint32_t kAndVtableRva = 0x9F9994;           // CAndTrigger::vftable
constexpr uint32_t kProvinceSourcePtrRva = 0xE5870C;   // object; vt[0x24] = province count
constexpr int32_t kDateBase = 0x029C55C0;
constexpr uint32_t kCountryEvents = 0x08;              // vector<CEvent*>
constexpr uint32_t kProvinceEvents = 0x14;
constexpr uint32_t kEventTrigger = 0x30;               // embedded CAndTrigger
constexpr uint32_t kScopeTag = 0x08;
constexpr uint32_t kScopeCountry = 0x0C;
constexpr uint32_t kScopeProvince = 0x10;
constexpr size_t kScopeSize = 0x48;                    // what CEventScope::ctor writes and EventScope_Copy copies
constexpr size_t kScopeBuffer = 0x60;                  // what the other trigger features allocate for a scope
constexpr size_t kResyncWindow = 256;                  // entries searched ahead when the scan skipped some
constexpr uint32_t kVtableEvaluate = 0x18;
constexpr size_t kVtableSlotsCopied = 16;
constexpr size_t kChunk = 1024;                        // entries evaluated per batch
constexpr int kPerTask = 16;                           // entries per worker task
constexpr int kReportEveryScans = 30;
constexpr int kMaxMismatchLogs = 20;

using EvaluateFn = char(__fastcall*)(void* self, void* edx, void* scope);
using CountFn = int(__fastcall*)(void* self, void* edx);

int g_mode = 0;  // 1 on, 2 shadow
EvaluateFn g_original_evaluate = nullptr;
uintptr_t g_fake_vtable[kVtableSlotsCopied];
uintptr_t g_and_vtable = 0;
uintptr_t g_scan = 0;
uintptr_t g_fire = 0;

struct Entry {
    uintptr_t self;  // the event's trigger block
    uint32_t key;    // country index (country pass) or province index
    uint32_t tag;    // country pass: the tag the scan writes to scope+0x08
    uint8_t result;
};
std::vector<Entry> g_seq;
size_t g_next = 0;        // next entry the scan should ask for
size_t g_ready_end = 0;   // results valid for [g_next, g_ready_end)
bool g_in_scan = false;
bool g_country_pass = false;
volatile uint8_t g_fired = 0;     // an event fired since the current results were computed
volatile int32_t g_fire_depth = 0;
int g_native_depth = 0; // nested checks during serial barriers/shadow evaluation pass through
alignas(16) unsigned char g_template[kScopeSize];

// statistics since the last report
uint64_t g_scans = 0, g_entries = 0, g_chunks = 0, g_served = 0, g_passthrough = 0, g_fires = 0;
uint64_t g_compared = 0, g_mismatches = 0, g_batch_ticks = 0;
uint64_t g_requests = 0, g_computed = 0, g_guard_fallbacks = 0;
uint64_t g_barriers[static_cast<size_t>(trigger_safety::BarrierReason::Count)]{};
int g_mismatch_logs = 0;
int64_t g_qpc_freq = 0;

using game::read32;
inline uint8_t read8(uintptr_t at) { return *reinterpret_cast<const uint8_t*>(at); }
using game::kWorldProvinces;
using game::kCountryTag;
using game::kCountryIndex;
using game::kCountryProvincesEnd;
using game::kProvinceOwner;
constexpr uint32_t kCountryProvincesBegin = game::kCountryProvinces;
constexpr uint32_t kWorldDate = game::kWorldDate;

inline int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline char evaluate_original(uintptr_t self, void* scope) {
    return g_original_evaluate(reinterpret_cast<void*>(self), nullptr, scope);
}

uintptr_t g_template_origin = 0;  // address of the live scope the template was copied from

// Rebind a genuine THIS self-reference, retaining external aliases and every scalar seed/ID.
void fill_scope(unsigned char* scope, const Entry& e) {
    scope_snapshot::copy(scope, g_template, g_template_origin);
    if (g_country_pass) {
        std::memcpy(scope + kScopeTag, &e.tag, 4);
        std::memcpy(scope + kScopeCountry, &e.key, 4);
    } else {
        std::memcpy(scope + kScopeProvince, &e.key, 4);
    }
}

// Worker task: entries [base + index * kPerTask, ...) of the current chunk.
size_t g_chunk_begin = 0, g_chunk_end = 0;
void evaluate_task(int index, void* context) {
    size_t begin = g_chunk_begin + static_cast<size_t>(index) * kPerTask;
    size_t end = begin + kPerTask < g_chunk_end ? begin + kPerTask : g_chunk_end;
    alignas(16) unsigned char scope[kScopeBuffer];
    for (size_t i = begin; i < end; i++) {
        Entry& e = g_seq[i];
        const auto& safety = *static_cast<const trigger_safety::Inspector*>(context);
        // Numeric guards belong to the actual worker thread, not just the classifier.
        if (!safety.pure_tree(e.self)) {
            continue; // result stays unavailable; the main thread will run it natively
        }
        fill_scope(scope, e);
        e.result = evaluate_original(e.self, scope) ? 1 : 0;
    }
}

bool compute_chunk(void* live_scope) {
    int64_t start = qpc();
    auto safety = trigger_safety::Inspector{hooks::exe_base(), read32, read8,
        reinterpret_cast<uintptr_t>(g_fake_vtable)};
    g_chunk_begin = g_chunk_end = g_next;
    size_t limit = g_next + kChunk < g_seq.size() ? g_next + kChunk : g_seq.size();
    while (g_chunk_end < limit) {
        safety = safety.certify_for(g_seq[g_chunk_end].self);
        if (!safety.pure_tree(g_seq[g_chunk_end].self)) {
            break;
        }
        g_seq[g_chunk_end++].result = 2; // no result until a certified worker finishes it
    }
    if (g_chunk_end == g_next) {
        ++g_barriers[static_cast<size_t>(safety.barrier_reason(g_seq[g_next].self))];
        return false;
    }
    std::memcpy(g_template, live_scope, kScopeSize);
    g_template_origin = reinterpret_cast<uintptr_t>(live_scope);
    int tasks = static_cast<int>((g_chunk_end - g_chunk_begin + kPerTask - 1) / kPerTask);
    parallel::run(tasks, &evaluate_task, &safety);
    g_ready_end = g_chunk_end;
    g_chunks++;
    g_computed += g_chunk_end - g_chunk_begin;
    g_batch_ticks += static_cast<uint64_t>(qpc() - start);
    return true;
}

char evaluate_serial(uintptr_t self, void* scope) {
    g_passthrough++;
    g_ready_end = g_next; // the next run must observe all native scope/world writes
    g_native_depth++;
    char result = evaluate_original(self, scope);
    g_native_depth--;
    return result;
}

char __fastcall evaluate_hook(void* self, void*, void* scope) {
    if (!g_in_scan || g_fire_depth > 0 || g_native_depth > 0) {
        return g_original_evaluate(self, nullptr, scope);
    }
    g_requests++;
    uintptr_t s = reinterpret_cast<uintptr_t>(self);
    uint32_t key = read32(reinterpret_cast<uintptr_t>(scope) + (g_country_pass ? kScopeCountry : kScopeProvince));
    if (g_next >= g_seq.size() || g_seq[g_next].self != s || g_seq[g_next].key != key) {
        // The scan skipped entries (a candidate list changed since it was listed): look a little ahead.
        size_t limit = g_next + kResyncWindow < g_seq.size() ? g_next + kResyncWindow : g_seq.size();
        size_t found = g_next;
        while (found < limit && (g_seq[found].self != s || g_seq[found].key != key)) {
            found++;
        }
        if (found >= limit) {
            g_passthrough++;
            g_in_scan = false;  // an unlisted evaluation may mutate the shared scope: abandon the batch
            return g_original_evaluate(self, nullptr, scope);
        }
        g_next = found;  // the entries in between are not used
    }
    if (g_fired) {
        g_fired = 0;
        g_ready_end = g_next;  // computed before the event ran its effects
    }
    if (g_next >= g_ready_end && !compute_chunk(scope)) {
        g_next++;
        return evaluate_serial(s, scope);
    }
    char result = static_cast<char>(g_seq[g_next++].result);
    if (result == 2) {
        g_guard_fallbacks++;
        return evaluate_serial(s, scope);
    }
    if (g_mode == 2) {
        g_native_depth++;
        char serial = g_original_evaluate(self, nullptr, scope) ? 1 : 0;
        g_native_depth--;
        g_compared++;
        if (serial != result) {
            g_mismatches++;
            if (g_mismatch_logs++ < kMaxMismatchLogs) {
                logf("parallel_events: MISMATCH %s pass, %s %u, trigger %08X: serial %d, parallel %d",
                     g_country_pass ? "country" : "province", g_country_pass ? "country" : "province", key, s,
                     serial, result);
            }
        }
        if (serial || serial != result) {
            g_ready_end = g_next;
        }
        return serial;
    }
    if (result) {
        g_ready_end = g_next; // native MTTH evaluation follows before the next trigger
    }
    g_served++;
    return result;
}

bool add_entry(uintptr_t event, uint32_t key, uint32_t tag) {
    uintptr_t block = event + kEventTrigger;
    uintptr_t vtable = read32(block);
    if (vtable != g_and_vtable && vtable != reinterpret_cast<uintptr_t>(g_fake_vtable)) {
        return false;
    }
    g_seq.push_back(Entry{block, key, tag, 0});
    return true;
}

// Lists, in the scan's order, every trigger it will evaluate today (as long as nothing changes).
void __cdecl scan_begin() {
    g_in_scan = false;
    g_seq.clear();
    g_next = g_ready_end = 0;
    g_fired = 0;
    uintptr_t world = game::world();
    uintptr_t manager = game::country_manager();
    if (!world || !manager) {
        return;
    }
    int32_t days = (static_cast<int32_t>(read32(world + kWorldDate)) - kDateBase) / 24;
    g_country_pass = days % 2 == 1;
    uintptr_t countries = read32(manager + 4);
    if (g_country_pass) {
        int32_t count = static_cast<int32_t>(read32(manager));
        for (int32_t i = 1; i < count; i++) {
            uintptr_t c = read32(countries + i * 4);
            uint32_t index = read32(c + kCountryIndex);
            if (index == 0) {
                continue;
            }
            uintptr_t owner = read32(countries + index * 4);
            if (static_cast<int32_t>((read32(owner + kCountryProvincesEnd) - read32(owner + kCountryProvincesBegin)) &
                                     0xFFFFFFFCu) <= 0) {
                continue;
            }
            uintptr_t begin = read32(owner + kCountryEvents), end = read32(owner + kCountryEvents + 4);
            for (uintptr_t it = begin; it < end; it += 4) {
                if (!add_entry(read32(it), index, read32(c + kCountryTag))) {
                    return;  // unknown root layout: leave the scan native before publishing hooks
                }
            }
        }
    } else {
        uintptr_t source = read32(hooks::exe_addr(kProvinceSourcePtrRva));
        if (!source) {
            return;
        }
        auto count_fn = reinterpret_cast<CountFn>(read32(read32(source) + 0x24));
        int32_t count = count_fn(reinterpret_cast<void*>(source), nullptr);
        uintptr_t provinces = read32(world + kWorldProvinces);
        for (int32_t p = 0; p < count; p++) {
            uintptr_t province = read32(provinces + p * 4);
            if (read32(province + kProvinceOwner) == 0) {
                continue;
            }
            uintptr_t begin = read32(province + kProvinceEvents), end = read32(province + kProvinceEvents + 4);
            for (uintptr_t it = begin; it < end; it += 4) {
                if (!add_entry(read32(it), static_cast<uint32_t>(p), 0)) {
                    return;
                }
            }
        }
    }
    // Publish only after all root layouts were checked. Child purity is checked per run,
    // after the preceding serial barrier; unsupported children do not disable the whole scan.
    for (const Entry& e : g_seq) {
        *reinterpret_cast<uintptr_t*>(e.self) = reinterpret_cast<uintptr_t>(g_fake_vtable);
    }
    g_entries += g_seq.size();
    g_in_scan = true;
}

void __cdecl scan_end() {
    g_in_scan = false;
    if (++g_scans % kReportEveryScans != 0) {
        return;
    }
    auto ull = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    double ms_day = g_qpc_freq ? 1000.0 * static_cast<double>(g_batch_ticks) / g_qpc_freq / kReportEveryScans : 0.0;
    logf("parallel_events: last %d scans: %llu triggers/day listed, %llu served, %llu passed through, %llu chunks, "
         "%llu events fired, batches %.2f ms/day (%d workers + game thread)",
         kReportEveryScans, ull(g_entries / kReportEveryScans), ull(g_served / kReportEveryScans),
         ull(g_passthrough / kReportEveryScans), ull(g_chunks), ull(g_fires), ms_day, parallel::workers());
    logf("parallel_events: coverage: %llu/%llu tracked trigger calls served, %llu submitted entries, "
         "%llu worker guard fallbacks; serial barriers: scope RNG %llu, scope writes %llu, shared writes %llu, "
         "runtime guards %llu, unclosed %llu", ull(g_served), ull(g_requests), ull(g_computed), ull(g_guard_fallbacks),
         ull(g_barriers[1]), ull(g_barriers[2]), ull(g_barriers[3]), ull(g_barriers[4]), ull(g_barriers[5]));
    if (g_mode == 2) {
        logf("parallel_events: shadow: %llu compared, %llu mismatches", ull(g_compared), ull(g_mismatches));
    }
    g_entries = g_served = g_passthrough = g_chunks = g_fires = g_compared = g_mismatches = g_batch_ticks = 0;
    g_requests = g_computed = g_guard_fallbacks = 0;
    for (auto& count : g_barriers) {
        count = 0;
    }
}

void __cdecl fire_done() {
    g_fire_depth--;
    g_fired = 1;
    g_fires++;
}

// DayTick's call: list the sequence, run the scan, close it. Registers and flags are kept around both helpers.
__declspec(naked) void scan_thunk() {
    __asm {
        pushad
        pushfd
        call scan_begin
        popfd
        popad
        push dword ptr [esp + 4]
        call dword ptr [g_scan]
        pushad
        pushfd
        call scan_end
        popfd
        popad
        ret 4
    }
}

// The scan's Event_Fire calls (ECX = event, two stack arguments). Evaluations made while the event runs
// pass through; afterwards the results computed before it are dropped.
__declspec(naked) void fire_thunk() {
    __asm {
        inc dword ptr [g_fire_depth]
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        call dword ptr [g_fire]
        pushad
        pushfd
        call fire_done
        popfd
        popad
        ret 8
    }
}

}  // namespace

void install_events_parallel(const Config& config) {
    g_mode = config.parallel_events;
    if (g_mode != 1 && g_mode != 2) {
        logf("parallel_events: disabled");
        return;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpc_freq = freq.QuadPart;
    g_and_vtable = hooks::exe_addr(kAndVtableRva);
    g_scan = hooks::exe_addr(kScanRva);
    g_fire = hooks::exe_addr(kFireRva);
    std::memcpy(g_fake_vtable, reinterpret_cast<const void*>(g_and_vtable), sizeof(g_fake_vtable));
    g_original_evaluate = reinterpret_cast<EvaluateFn>(g_fake_vtable[kVtableEvaluate / 4]);
    if (reinterpret_cast<uintptr_t>(g_original_evaluate) != hooks::exe_addr(0x4B8260) ||
        !hooks::expect_bytes(reinterpret_cast<uintptr_t>(g_original_evaluate), "\x55\x8B\xEC", 3) ||
        !hooks::expect_bytes(hooks::exe_addr(kScanCallRva), "\xE8", 1)) {
        logf("parallel_events: CAndTrigger::Evaluate or the scan call not recognised, feature off");
        return;
    }
    for (uint32_t site : kFireCallRvas) {
        if (!hooks::expect_bytes(hooks::exe_addr(site), "\xE8", 1)) {
            logf("parallel_events: Event_Fire call %08X not recognised, feature off", site + 0x400000);
            return;
        }
    }
    g_fake_vtable[kVtableEvaluate / 4] = reinterpret_cast<uintptr_t>(&evaluate_hook);
    g_seq.reserve(1 << 16);
    // Event_Fire first: with the scan hooked but not these, a fired event would leave stale results.
    for (uint32_t site : kFireCallRvas) {
        if (!hooks::redirect_call(hooks::exe_addr(site), g_fire, reinterpret_cast<uintptr_t>(&fire_thunk))) {
            logf("parallel_events: Event_Fire call %08X does not call Event_Fire or is already hooked, feature off",
                 site + 0x400000);
            return;  // a redirected fire site alone only marks results stale, which nothing reads
        }
    }
    if (!hooks::redirect_call(hooks::exe_addr(kScanCallRva), g_scan, reinterpret_cast<uintptr_t>(&scan_thunk))) {
        logf("parallel_events: scan call site already hooked (tick_profiler?), feature off");
        return;
    }
    logf("parallel_events: active (%s): pure runs, serial barriers and fresh batches after MTTH",
         g_mode == 2 ? "shadow mode, the game still uses its own evaluation" : "results used");
}

}  // namespace cp::features
