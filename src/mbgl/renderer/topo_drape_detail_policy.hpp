#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace mbgl::topo_vector {

struct DrapeZoomPolicy {
    uint8_t detailZoom;
    uint8_t overviewZoom;
};

// Diagnostic opt-in only: keep the shipped/recovered policy by default, and
// reject malformed or out-of-range overrides rather than silently promoting
// an unknown value. A one-zoom gap bounds each atomic sibling group to four
// children at every accepted detail level.
inline DrapeZoomPolicy drapeZoomPolicyForOverride(const char* value) noexcept {
    uint8_t detail = 10;
    if (value) {
        const std::string_view text(value);
        if (text == "11") detail = 11;
        if (text == "12") detail = 12;
    }
    return {detail, static_cast<uint8_t>(detail - 1)};
}

// Source selection and terrain targets must share the same launch-scoped
// policy; otherwise the source normalizer can collapse diagnostic z12
// targets back to z10 and still omit labels/trails from the detailed tiles.
inline DrapeZoomPolicy drapeZoomPolicy() noexcept {
    static const auto policy = drapeZoomPolicyForOverride(std::getenv("KLATTRA_TOPO_DRAPE_DETAIL_ZOOM"));
    return policy;
}

} // namespace mbgl::topo_vector
