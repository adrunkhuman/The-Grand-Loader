#pragma once

#include <cstdint>

#if defined(_MSC_VER) && defined(_M_IX86)
#include <xmmintrin.h>
#endif

#include "trigger_safety_data.h"

namespace cp::features::trigger_safety {

// Deterministic read-only execution only, NOT attacker independence, cache lifetime,
// or script scope-type validity. RNG-only rejection is not a claim of shared-state mutation.
// Random paths remain native until ownership, stream isolation and single-use consumption
// are independently established; no cache or worker permission follows from an effect label.
// Evidence: docs/cull-cb-checks.md#shared-purity-whitelist and the 2026-10-15
// trigger-whitelist audit of original Victoria II 3.04 (native VAs use image base 00400000).
// Requires valid original native objects/indices/code, fully constructed published
// singletons, immutable definitions and frozen world/lifetimes through execution.
// Never call an initializer to satisfy a guard.
// Native receiver type domains are the audited RTTI domains; arbitrary replacement objects
// or helper hooks invalidate that contract even when a trigger's Evaluate slot is unchanged.
enum class BarrierReason : uint8_t {
    None, ScopeRng, ScopeWrite, SharedWrite, RuntimeGuard, Unclosed, Count
};

struct Inspector {
    uintptr_t base;
    uint32_t (*read32)(uintptr_t);
    uint8_t (*read8)(uintptr_t);
    uintptr_t and_alias = 0; // our private CAndTrigger vtable (feature dispatch only)
    // Raw aggregate inspectors remain uncached. snapshot()/certify_for() issue receiver certificates.
    int8_t cached_country_flags = -1;
    int8_t cached_global_flags = -1;

    // Opt in only after freezing the world, receiver lifetimes/vtables and native runtime.
    // The returned value may be copied/read across workers; it never lazily mutates caches.
    // Discard ALL copies BEFORE any unsafe native call, game mutation, load or hook change.
    // Locale is still checked on every use (even with a certificate); numeric environment
    // is never cached and must satisfy its guard on the executing thread. snapshot() on an
    // existing snapshot recertifies from current memory, not from the old certificates.
    Inspector snapshot() const {
        Inspector result = *this;
        result.cached_country_flags = -1;
        result.cached_global_flags = -1;
        result.cached_country_flags = result.country_flags() ? 1 : 0;
        result.cached_global_flags = result.global_flags() ? 1 : 0;
        return result;
    }

    // Main-thread builder for a frozen interval: certify only receiver domains this tree
    // can reach, retaining previous certificates for the same interval. Publish the returned
    // value to workers only AFTER the run is fully classified. Never mutate it during work.
    Inspector certify_for(uintptr_t trigger) const {
        Inspector result = *this;
        unsigned remaining = max_nodes;
        uint8_t domains = flag_domains(trigger, 0, remaining);
        if ((domains & 1) && cached_country_flags < 0) {
            result.cached_country_flags = country_flags() ? 1 : 0;
        }
        if ((domains & 2) && cached_global_flags < 0) {
            result.cached_global_flags = global_flags() ? 1 : 0;
        }
        return result;
    }

    static constexpr unsigned max_nodes = 65536;

    static bool valid_range(uintptr_t begin, uintptr_t end) {
        return end >= begin && (begin & 3) == 0 && (end & 3) == 0 &&
               (begin != 0 || end == 0) && end <= UINT32_MAX &&
               (end - begin) / 4 <= max_nodes;
    }

    // 008a8650 constructs a scope even before it knows whether any child will run.
    // Worker callers constructing their own scopes must check this BEFORE dispatch too.
    bool scope_constructor_safe() const {
        uintptr_t pop = read32(base + 0xE58A10);
        return pop && read32(pop) == base + 0x9FD2E8 &&
               read32(base + 0x9FD2E8 + 0x20) == base + 0x18CFD0;
    }

    // H-FP: exact hardware state, not a CRT translation of its control flags. Classification
    // must hold on the executing thread too: a main-thread check does not set worker FP state.
    // Finite inputs follow from the audited integer-count ratio / constant sub_unit paths.
    static bool numeric_environment_safe() {
        uint16_t control = 0;
        uint32_t mxcsr = 0;
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
        __asm__ __volatile__("fnstcw %0" : "=m"(control));
        __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr));
#elif defined(_MSC_VER) && defined(_M_IX86)
        __asm { fnstcw control }
        mxcsr = _mm_getcsr();
#else
        return false; // no certified exact x87/MXCSR reader on this target
#endif
        return (mxcsr & 0x7F80) == 0x1F80 && (control & 0x7F) == 0x7F;
    }

    bool numeric_runtime_safe() const {
        return read32(base + 0xFF3AB8) != 0 && numeric_environment_safe();
    }

    bool pure_children(uintptr_t block, unsigned depth = 0) const {
        unsigned remaining = max_nodes;
        return children(block, depth, remaining);
    }

    bool pure_tree(uintptr_t trigger, unsigned depth = 0) const {
        unsigned remaining = max_nodes;
        return tree(trigger, depth, remaining);
    }

    // Only potential is precomputed. Native allow/ai_will_do are serial barriers after a
    // true potential, so they do not need to pass the worker certificate.
    bool decision_potential_safe(uintptr_t decision) const {
        if (!object(decision, 0x40)) {
            return false;
        }
        uintptr_t vtable = read32(decision);
        return object(vtable, 0x1C) && read32(vtable + 0x18) == base + 0x631490 &&
               pure_tree(decision + 0x24);
    }

    // First reason a path stays native, not a complete effect summary or a cache certificate.
    // A label denotes a MAY-write path; input-dependent read-only subsets need separate proofs.
    BarrierReason barrier_reason(uintptr_t trigger) const {
        unsigned remaining = max_nodes;
        return reason(trigger, 0, remaining);
    }

private:
    // These checks bound arithmetic/traversal, not OS memory accessibility. read32/read8 and
    // the native-object contract still supply readable storage (including in host fixtures).
    static bool object(uintptr_t p, uintptr_t size) {
        return p != 0 && size <= UINT32_MAX && p <= UINT32_MAX - size;
    }

    bool children(uintptr_t block, unsigned depth, unsigned& remaining) const {
        if (!object(block, 0x10) || depth >= 64) {
            return false;
        }
        uintptr_t begin = read32(block + 0x08), end = read32(block + 0x0C);
        if (!valid_range(begin, end) || (end - begin) / 4 > remaining) {
            return false;
        }
        for (uintptr_t it = begin; it < end; it += 4) {
            if (!tree(read32(it), depth + 1, remaining)) {
                return false;
            }
        }
        return true;
    }

    bool tree(uintptr_t trigger, unsigned depth, unsigned& remaining) const {
        if (!object(trigger, 0x34) || depth >= 64 || remaining == 0) {
            return false;
        }
        --remaining;
        uintptr_t vtable = read32(trigger);
        if (and_alias && vtable == and_alias) {
            return children(trigger, depth, remaining);
        }
        if (!object(vtable, 0x1C) || vtable < base) {
            return false;
        }
        const auto* entry = lookup(vtable);
        return entry && guarded(entry->guard, trigger) &&
               (!entry->recursive || children(trigger, depth, remaining));
    }

    const detail::Entry* lookup(uintptr_t vtable) const {
        if (!object(vtable, 0x1C) || vtable < base) {
            return nullptr;
        }
        unsigned first = 0, last = sizeof(detail::entries) / sizeof(detail::entries[0]);
        while (first < last) {
            unsigned mid = first + (last - first) / 2;
            const auto& entry = detail::entries[mid];
            if (vtable - base < entry.vtable) {
                last = mid;
            } else if (vtable - base > entry.vtable) {
                first = mid + 1;
            } else {
                return read32(vtable + 0x18) == base + entry.evaluate ? &entry : nullptr;
            }
        }
        return nullptr;
    }

    BarrierReason reason(uintptr_t trigger, unsigned depth, unsigned& remaining) const {
        using R = BarrierReason;
        if (!object(trigger, 0x34) || depth >= 64 || remaining == 0) {
            return R::Unclosed;
        }
        --remaining;
        uintptr_t vt = read32(trigger);
        const auto* entry = lookup(vt);
        bool recursive = and_alias && vt == and_alias;
        if (entry) {
            recursive = entry->recursive;
            if (!guarded(entry->guard, trigger)) {
                if (entry->guard == detail::Guard::Context && scope_constructor_safe()) {
                    uintptr_t selector = read32(trigger + 0x1C);
                    if (object(selector, 0x110) && read32(selector + 0x10C) == 0 &&
                        (read32(selector) == 0x3E8 || read32(selector) == 0x717)) {
                        return R::ScopeRng;
                    }
                }
                return R::RuntimeGuard;
            }
        } else if (!recursive) {
            if (!object(vt, 0x1C)) {
                return R::Unclosed;
            }
            uint32_t eval = read32(vt + 0x18);
            if ((vt == base + 0xA30F90 && eval == base + 0x4D7D40) ||
                (vt == base + 0xA30FC8 && eval == base + 0x4D8030) ||
                (vt == base + 0xA31150 && eval == base + 0x4DA5C0) ||
                (vt == base + 0xA32880 && eval == base + 0x4FF670)) {
                return R::SharedWrite;
            }
            if (vt == base + 0xA32D18 && eval == base + 0x502790) {
                return R::ScopeWrite;
            }
            return R::Unclosed;
        }
        if (recursive) {
            uintptr_t begin = read32(trigger + 8), end = read32(trigger + 12);
            if (!valid_range(begin, end)) {
                return R::Unclosed;
            }
            for (uintptr_t it = begin; it < end; it += 4) {
                auto result = reason(read32(it), depth + 1, remaining);
                if (result != R::None) {
                    return result;
                }
            }
        }
        return R::None;
    }

    uint8_t flag_domains(uintptr_t trigger, unsigned depth, unsigned& remaining) const {
        if (!object(trigger, 0x34) || depth >= 64 || remaining == 0) {
            return 0;
        }
        --remaining;
        uintptr_t vt = read32(trigger);
        if (vt == base + 0xA307E8) {
            return 1;
        }
        if (vt == base + 0xA30820) {
            return 2;
        }
        const auto* entry = lookup(vt);
        if (!(and_alias && vt == and_alias) && (!entry || !entry->recursive)) {
            return 0;
        }
        uintptr_t begin = read32(trigger + 8), end = read32(trigger + 12);
        if (!valid_range(begin, end)) {
            return 0;
        }
        uint8_t domains = 0;
        for (uintptr_t it = begin; it < end && domains != 3 && remaining; it += 4) {
            domains |= flag_domains(read32(it), depth + 1, remaining);
        }
        return domains;
    }

    bool country_range(uintptr_t& begin, uintptr_t& end) const {
        uintptr_t manager = read32(base + 0xE587E4);
        if (!object(manager, 0x0C)) {
            return false;
        }
        begin = read32(manager + 4);
        end = read32(manager + 8);
        return begin != end && valid_range(begin, end);
    }

    bool flags(uintptr_t receiver) const {
        return object(receiver, 4) && read32(receiver) == base + 0x9F817C &&
               read32(base + 0x9F817C + 8) == base + 0x5E6B10;
    }

    bool country_flags() const {
        if (read32(base + 0xB1FC74) != 0) {
            return false; // tolower's non-C-locale route is not closed, even for snapshots
        }
        if (cached_country_flags >= 0) {
            return cached_country_flags != 0;
        }
        uintptr_t begin = 0, end = 0;
        if (!country_range(begin, end)) {
            return false;
        }
        // No scope argument is available. Certify every possible country receiver rather
        // than guessing which THIS/FROM/context country a descendant will eventually select.
        for (uintptr_t it = begin; it < end; it += 4) {
            uintptr_t country = read32(it);
            if (!object(country, 0x1B4) || !flags(country + 0x1B0)) {
                return false;
            }
        }
        return true;
    }

    bool global_flags() const {
        if (read32(base + 0xB1FC74) != 0) {
            return false;
        }
        if (cached_global_flags >= 0) {
            return cached_global_flags != 0;
        }
        uintptr_t world = read32(base + 0xE588E8);
        return object(world, 0x68) && flags(world + 0x64);
    }

    bool context(uintptr_t trigger) const {
        if (!scope_constructor_safe()) {
            return false;
        }
        uintptr_t selector = read32(trigger + 0x1C), begin = 0, end = 0;
        if (!object(selector, 0x110) || !country_range(begin, end)) {
            return false;
        }
        uint32_t token = read32(selector), index = read32(selector + 0x10C);
        // Reject both random selectors even when nested inside a private scope. No scope
        // ownership certificate is inferred from an ancestor's shallow stack copy.
        if (token == 0x3E8 || token == 0x717) {
            return false;
        }
        if (index != 0) { // native resolved-country override precedes token dispatch
            if (index >= (end - begin) / 4) {
                return false;
            }
            uintptr_t country = read32(begin + index * 4);
            return object(country, 0x24) && read32(country + 0x20) == index &&
                   read32(country + 0x1C) == read32(selector + 0x108);
        }
        switch (token) {
        case 0x0C: { // parsed positive literal province, not token text re-parsing
            uint32_t id = read32(selector + 0x104);
            uintptr_t world = read32(base + 0xE588E8);
            uintptr_t map = read32(base + 0xE5870C);
            if (static_cast<int32_t>(id) <= 0 || !object(world, 0xAD4) ||
                !object(map, 0x223C)) {
                return false;
            }
            uintptr_t provinces = read32(world + 0xACC), stop = read32(world + 0xAD0);
            uintptr_t map_provinces = read32(map + 0x2238);
            // CWorld's province vector bounds are shared in game/world.h. The map's raw
            // array has no guessed end field: use the loaded world's same province domain.
            return valid_range(provinces, stop) && id < (stop - provinces) / 4 &&
                   object(map_provinces, (uintptr_t(id) + 1) * 4) &&
                   read32(provinces + id * 4) != 0 && read32(map_provinces + id * 4) != 0;
        }
        case 0x295: // owner
        case 0x296: // controller
        case 0x2C9: // location
        case 0x2F0: // country
        case 0x3DE: // from (current input FROM, not THIS->FROM)
        case 0x405: // this (retains input pop/state and shallow aliases)
        case 0x442: // capital_scope
        case 0x476: // sea_zone
        case 0x655: // sphere_owner
        case 0x6A5: // cultural_union
        case 0x727: // overlord
        case 0x747: // independence (existing rebel lookup, no insertion)
        case 0x879: // state_scope
        case 0x9EB: // crisis_state_scope
        case 0xA26: // flashpoint_tag_scope
            return true; // valid native input scope/indices remain caller preconditions
        default:
            return false;
        }
    }

    bool building_type(uintptr_t definition) const {
        if (!object(definition, 4)) {
            return false;
        }
        uintptr_t vtable = read32(definition);
        return (vtable == base + 0x9FDAC0 && read32(vtable + 0x24) == base + 0x631490) ||
               (vtable == base + 0x9FDAF0 && read32(vtable + 0x24) == base + 0x18CFD0);
    }

    bool string_bytes(uintptr_t str, uintptr_t& data, uint32_t& length) const {
        if (!object(str, 0x18)) {
            return false;
        }
        length = read32(str + 0x10);
        uint32_t capacity = read32(str + 0x14);
        data = capacity > 15 ? read32(str) : str;
        return length <= capacity && length <= max_nodes &&
               (capacity > 15 || length <= 15) && object(data, uintptr_t(length) + 1);
    }

    bool building(uintptr_t trigger) const {
        uintptr_t manager = read32(base + 0xE5CE80);
        uintptr_t fallback = read32(base + 0xE58730);
        if (!object(manager, 0x0C) || !building_type(fallback)) {
            return false;
        }
        // Reproduce ONLY the read-only 004dc420 lookup: signed-byte hash, bucket list,
        // case-sensitive length/byte equality. Do not run 008ce560 (it can initialize).
        uintptr_t text = 0;
        uint32_t length = 0;
        if (!string_bytes(trigger + 0x1C, text, length)) {
            return false;
        }
        uint32_t hash = 0;
        for (uint32_t i = 0; i < length; ++i) {
            hash = (hash + static_cast<uint32_t>(static_cast<int8_t>(read8(text + i)))) * 0x3D;
        }
        uint32_t count = read32(manager + 4);
        uintptr_t buckets = read32(manager + 8);
        if (count == 0 || count > max_nodes || !object(buckets, uintptr_t(count) * 4)) {
            return false;
        }
        uintptr_t node = read32(buckets + (hash % count) * 4);
        for (unsigned n = 0; node && n < max_nodes; ++n) {
            if (!object(node, 8)) {
                return false;
            }
            uintptr_t definition = read32(node), name = 0;
            uint32_t size = 0;
            if (!object(definition, 0x38) || !string_bytes(definition + 0x20, name, size)) {
                return false;
            }
            bool equal = size == length;
            for (uint32_t i = 0; equal && i < length; ++i) {
                equal = read8(text + i) == read8(name + i);
            }
            if (equal) {
                return building_type(definition);
            }
            node = read32(node + 4);
        }
        return node == 0; // genuine miss uses the already type-checked null definition
    }

    bool construction_getters() const {
        // Closed RTTI domain from audit-group-3.md; no arbitrary virtual getter approval.
        return read32(base + 0x9FBFCC + 0x30) == base + 0x18CFD0 &&
               read32(base + 0x9FC034 + 0x30) == base + 0x18CFD0 &&
               read32(base + 0x9FC09C + 0x30) == base + 0x631490;
    }

    bool guarded(detail::Guard guard, uintptr_t trigger) const {
        using G = detail::Guard;
        switch (guard) {
        case G::None:
            return true;
        case G::Numeric:
            return numeric_runtime_safe();
        case G::NotRebel:
            return read8(trigger + 0x20) == 0; // excludes ALL rebel numeric routes, not THIS/FROM
        case G::CountryFlags:
            return country_flags();
        case G::GlobalFlags:
            return global_flags();
        case G::Context:
            return context(trigger);
        case G::NullPop:
            return scope_constructor_safe(); // required even for empty containers
        case G::Building:
            return building(trigger);
        case G::Culture:
            return read32(base + 0xE587EC) != 0; // null-culture fallback, NOT just its manager
        case G::PopCulture:
            return read8(trigger + 0x18) == 0 || read32(base + 0xE587EC) != 0;
        case G::Goods:
            return read32(base + 0xE58728) != 0;
        case G::Religion:
            return read32(base + 0xE5EA24) != 0 && read32(base + 0xE5880C) != 0;
        case G::Ideology:
            return read32(base + 0xE58A34) != 0;
        case G::PopTypes:
            return read32(base + 0xF1BB34) != 0;
        case G::PopTypeFallback:
            return read32(base + 0xF1BB34) != 0 && read32(base + 0xE58A2C) != 0;
        case G::CapitalBuilding:
            return read32(base + 0xE5CE80) != 0 && construction_getters();
        case G::CapitalFactory:
            return read32(base + 0xE5CE80) != 0 && read32(base + 0xE58730) != 0;
        case G::Factory:
            // No scope-specific balance bypass: conservatively require the manager and
            // native nonzero 64-bit divisor even when a particular scope would skip both.
            return read32(base + 0xE5CE80) != 0 &&
                   (read32(base + 0xF1A798) != 0 || read32(base + 0xF1A79C) != 0);
        }
        return false;
    }
};

} // namespace cp::features::trigger_safety
