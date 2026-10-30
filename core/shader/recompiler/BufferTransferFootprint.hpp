#pragma once

#include <cstdint>

namespace ShaderRecompiler {

// Descriptor-relative half-open byte hulls. Missing/unproved summaries remain Whole.
// A bounded empty hull denotes no accesses of that class, not an unknown range.
struct BufferTransferFootprint {
    bool bounded = false;
    std::uint64_t readBegin = 0;
    std::uint64_t readEnd = 0;
    std::uint64_t writeBegin = 0;
    std::uint64_t writeEnd = 0;

    bool operator==(const BufferTransferFootprint&) const = default;
};

}
