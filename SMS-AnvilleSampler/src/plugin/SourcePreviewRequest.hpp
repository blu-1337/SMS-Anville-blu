#pragma once

#include <cstdint>
#include <type_traits>

namespace midichopper::plugin {

/** Latest-wins Main Sample preview command for the audio thread (MP1). */
struct SourcePreviewRequest {
    bool play = false;
    bool loop = false;
    std::uint64_t sourceId = 0U;
    std::uint32_t start = 0U;
    std::uint32_t end = 0U;
};

static_assert(std::is_trivially_copyable_v<SourcePreviewRequest>);

} // namespace midichopper::plugin
