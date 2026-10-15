#pragma once

#include <cstdint>

namespace cp::features::trigger_safety {

// Fail-closed whitelist for the original 3.04 executable. Whitelisted leaves have no calls or
// writes outside their stack; AND/OR/NOT only dispatch to their children. In particular,
// CContextTrigger is NOT a pure container: ally selection can advance the INPUT scope's seed.
// See docs/cull-cb-checks.md#shared-purity-whitelist for the intentionally limited coverage.
struct Inspector {
    uintptr_t base;
    uint32_t (*read32)(uintptr_t);
    uint8_t (*read8)(uintptr_t);
    uintptr_t and_alias = 0; // our private CAndTrigger vtable (profiling/event dispatch only)

    static bool valid_range(uintptr_t begin, uintptr_t end) {
        return end >= begin && (end - begin) % 4 == 0 && (begin != 0 || end == 0);
    }

    bool pure_children(uintptr_t block, unsigned depth = 0) const {
        uintptr_t begin = read32(block + 0x08), end = read32(block + 0x0C);
        if (depth >= 64 || !valid_range(begin, end)) {
            return false;
        }
        for (uintptr_t it = begin; it < end; it += 4) {
            if (!pure_tree(read32(it), depth + 1)) {
                return false;
            }
        }
        return true;
    }

    bool pure_tree(uintptr_t trigger, unsigned depth = 0) const {
        if (!trigger || depth >= 64) {
            return false;
        }
        uintptr_t vtable = read32(trigger);
        if (and_alias && vtable == and_alias) {
            return pure_children(trigger, depth);
        }
        if (vtable < base) {
            return false;
        }
        uint32_t evaluate_rva = 0;
        switch (vtable - base) {
        case 0x9F9994:
            evaluate_rva = 0x4B8260;
            break; // AND
        case 0xA2FB70:
            evaluate_rva = 0x4B88A0;
            break; // OR
        case 0xA2FBA8:
            evaluate_rva = 0x4B9240;
            break; // NOT
        case 0xA2FE80:
            evaluate_rva = 0x4BB4F0;
            break; // always
        case 0xA2FC18:
            evaluate_rva = 0x4BA3F0;
            break; // year
        case 0xA2FC50:
            evaluate_rva = 0x4BA970;
            break; // month
        case 0xA30EE8:
            evaluate_rva = 0x4D7450;
            break;     // ai
        case 0xA30C80: // literal tag only
            if (read8(trigger + 0x18) || read8(trigger + 0x19)) {
                return false;
            }
            evaluate_rva = 0x4D4000;
            break;
        default:
            return false; // unknown leaves are not evidence of purity
        }
        if (read32(vtable + 0x18) != base + evaluate_rva) {
            return false; // another implementation/hook has not been audited
        }
        if (vtable == base + 0x9F9994 || vtable == base + 0xA2FB70 || vtable == base + 0xA2FBA8) {
            return pure_children(trigger, depth);
        }
        return true;
    }

    // AI_DecisionUpdate shares one scope across B then A, including ai_will_do (0x00874EA0).
    // Its modifier objects are NOT normal ANDs, but that routine directly walks their +08/+0C
    // children. Require all of those trees too, before any speculative native evaluation.
    bool decision_parallel_safe(uintptr_t decision) const {
        if (read32(read32(decision) + 0x18) != base + 0x631490 || !pure_tree(decision + 0x24) ||
            !pure_tree(decision + 0x40)) {
            return false;
        }
        uintptr_t begin = read32(decision + 0x11C), end = read32(decision + 0x120);
        if (!valid_range(begin, end)) {
            return false;
        }
        for (uintptr_t it = begin; it < end; it += 4) {
            if (!pure_children(read32(it))) {
                return false;
            }
        }
        return true;
    }
};

} // namespace cp::features::trigger_safety
