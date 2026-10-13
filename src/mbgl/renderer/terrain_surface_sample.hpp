#pragma once

#include <mbgl/util/image.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace mbgl::terrain_surface {

inline constexpr size_t gridSize = 128;

// Match terrain.hpp's sampleElevationBilinear texel-center transform and
// edge clamps, including the padded image bound to the actual drawable.
inline float sampleDEM(const PremultipliedImage& image, float u, float v) {
    const int width = static_cast<int>(image.size.width);
    const int height = static_cast<int>(image.size.height);
    // Input UV addresses the source tile, not its one-texel neighbor border.
    // Keep this transform identical to both OpenGL terrain shader stages.
    const bool padded = width > 2 && height > 2;
    const float offset = padded ? 0.5f : -0.5f;
    const float x = std::clamp(u, 0.0f, 1.0f) * (padded ? width - 2 : width) + offset;
    const float y = std::clamp(v, 0.0f, 1.0f) * (padded ? height - 2 : height) + offset;
    const float floorX = std::floor(x), floorY = std::floor(y);
    const int x0 = std::clamp(static_cast<int>(floorX), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(floorY), 0, height - 1);
    const int x1 = std::clamp(static_cast<int>(floorX) + 1, 0, width - 1);
    const int y1 = std::clamp(static_cast<int>(floorY) + 1, 0, height - 1);
    const auto at = [&](int px, int py) {
        const auto offset = (static_cast<size_t>(py) * width + px) * 4;
        const float r = image.data[offset], g = image.data[offset + 1], b = image.data[offset + 2];
        return -10000.0f + (r * 65536.0f + g * 256.0f + b) * 0.1f;
    };
    const float fx = x - floorX, fy = y - floorY;
    const float h00 = at(x0, y0), h10 = at(x1, y0), h01 = at(x0, y1), h11 = at(x1, y1);
    const float top = h00 + (h10 - h00) * fx;
    const float bottom = h01 + (h11 - h01) * fx;
    const float result = top + (bottom - top) * fy;
    // Terrain applies this validity rule at each displaced mesh vertex.
    return result <= -500.0f || result >= 9000.0f ? 0.0f : result;
}

inline std::optional<float> sample(const PremultipliedImage& image,
                                   float u,
                                   float v,
                                   const std::array<float, 2>& demTL,
                                   float demScale) {
    if (!image.valid() || !std::isfinite(u) || !std::isfinite(v) ||
        !std::isfinite(demTL[0]) || !std::isfinite(demTL[1]) ||
        !std::isfinite(demScale) || demScale <= 0.0f || u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return std::nullopt;
    }
    const float x = u * gridSize, y = v * gridSize;
    const int x0 = std::min(static_cast<int>(std::floor(x)), static_cast<int>(gridSize) - 1);
    const int y0 = std::min(static_cast<int>(std::floor(y)), static_cast<int>(gridSize) - 1);
    const float fx = x - x0, fy = y - y0;
    const auto vertex = [&](int px, int py) {
        return sampleDEM(image, (static_cast<float>(px) / gridSize) * demScale + demTL[0],
                         (static_cast<float>(py) / gridSize) * demScale + demTL[1]);
    };
    const float topRight = vertex(x0 + 1, y0), bottomLeft = vertex(x0, y0 + 1);
    // Actual index order: TL-BL-TR, then TR-BL-BR. Direct bilinear DEM
    // sampling can fall beneath this piecewise planar mesh on a saddle.
    if (fx + fy <= 1.0f) {
        const float topLeft = vertex(x0, y0);
        return topLeft + (topRight - topLeft) * fx + (bottomLeft - topLeft) * fy;
    }
    const float bottomRight = vertex(x0 + 1, y0 + 1);
    return bottomRight + (bottomLeft - bottomRight) * (1.0f - fx) +
           (topRight - bottomRight) * (1.0f - fy);
}

} // namespace mbgl::terrain_surface
