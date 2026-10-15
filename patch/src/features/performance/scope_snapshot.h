#pragma once

#include <cstdint>
#include <cstring>

namespace cp::features::scope_snapshot {

constexpr size_t size = 0x48;

// A frozen borrowed scope, not CEventScope_Copy (which resets the seed). THIS at +24
// is the only pointer to another scope. Keep external aliases, including the daily AI's
// companion scope; rebind only a genuine self-reference. Never relocate scalar seeds,
// tags or IDs merely because their bit pattern falls inside a stack-address range.
inline void copy(void* destination, const void* bytes, uintptr_t origin) {
    std::memcpy(destination, bytes, size);
    auto* out = static_cast<unsigned char*>(destination);
    uint32_t this_scope;
    std::memcpy(&this_scope, out + 0x24, 4);
    if (this_scope == origin) {
        this_scope = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(destination));
        std::memcpy(out + 0x24, &this_scope, 4);
    }
}

} // namespace cp::features::scope_snapshot
