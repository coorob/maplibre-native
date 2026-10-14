#pragma once

#include <mbgl/util/image.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace mbgl::terrain_surface {

// Cells per side of the terrain mesh (RenderTerrain::MESH_SIZE).
inline constexpr size_t gridSize = 128;

// Mirrors sampleElevationBilinear in the terrain shaders (mtl/terrain.hpp and
// gl/terrain.hpp): UV spans the whole bound image, border texels included,
// texel centres at half-integers, the right/bottom neighbour clamped after the
// left/top one, metres interpolated after decoding. Keep the two identical, or
// lifted lines drift off the mesh.
inline float sampleDEM(const PremultipliedImage& image, float u, float v) {
    const int width = static_cast<int>(image.size.width);
    const int height = static_cast<int>(image.size.height);
    const float x = std::clamp(u, 0.0f, 1.0f) * width - 0.5f;
    const float y = std::clamp(v, 0.0f, 1.0f) * height - 0.5f;
    const float floorX = std::floor(x), floorY = std::floor(y);
    const int x0 = std::clamp(static_cast<int>(floorX), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(floorY), 0, height - 1);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
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
    // The terrain vertex shader flattens invalid samples to 0 m.
    return result <= -500.0f || result >= 9000.0f ? 0.0f : result;
}

// Height of the rendered mesh surface at (u, v) of a mesh tile whose binding
// maps its UV to the DEM image as `uv * demScale + demTL`. The mesh samples
// the DEM only at its grid vertices and is planar inside each triangle, so
// interpolate the triangle the point falls in rather than the DEM itself.
inline std::optional<float> sample(const PremultipliedImage& image,
                                   float u,
                                   float v,
                                   const std::array<float, 2>& demTL,
                                   float demScale) {
    if (!image.valid() || !std::isfinite(u) || !std::isfinite(v) || !std::isfinite(demTL[0]) ||
        !std::isfinite(demTL[1]) || !std::isfinite(demScale) || demScale <= 0.0f || u < 0.0f || u > 1.0f ||
        v < 0.0f || v > 1.0f) {
        return std::nullopt;
    }
    const float x = u * gridSize, y = v * gridSize;
    const int x0 = std::min(static_cast<int>(std::floor(x)), static_cast<int>(gridSize) - 1);
    const int y0 = std::min(static_cast<int>(std::floor(y)), static_cast<int>(gridSize) - 1);
    const float fx = x - x0, fy = y - y0;
    const auto vertex = [&](int px, int py) {
        return sampleDEM(image,
                         (static_cast<float>(px) / gridSize) * demScale + demTL[0],
                         (static_cast<float>(py) / gridSize) * demScale + demTL[1]);
    };
    const float topRight = vertex(x0 + 1, y0), bottomLeft = vertex(x0, y0 + 1);
    // Mesh index order (RenderTerrain::generateMesh): TL-BL-TR, then TR-BL-BR,
    // so each cell splits along its anti-diagonal.
    if (fx + fy <= 1.0f) {
        const float topLeft = vertex(x0, y0);
        return topLeft + (topRight - topLeft) * fx + (bottomLeft - topLeft) * fy;
    }
    const float bottomRight = vertex(x0 + 1, y0 + 1);
    return bottomRight + (bottomLeft - bottomRight) * (1.0f - fx) + (topRight - bottomRight) * (1.0f - fy);
}

} // namespace mbgl::terrain_surface
