#pragma once

#include <mbgl/map/transform_state.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/style/types.hpp>
#include <mbgl/util/geometry.hpp>
#include <mbgl/util/range.hpp>

#include <vector>
#include <memory>
#include <numbers>
#include <optional>
#include <cstddef>

namespace mbgl {

class LatLngBounds;

namespace util {

// Helper class to stream tile-cover results per row
class TileCover {
public:
    TileCover(const LatLngBounds&, uint8_t z);
    // When project == true, projects the geometry points to tile coordinates
    TileCover(const Geometry<double>&, uint8_t z, bool project = true);
    ~TileCover();

    std::optional<UnwrappedTileID> next();
    bool hasNext();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

struct TileCoverParameters {
    TransformState transformState;
    double tileLodMinRadius = 3;
    double tileLodScale = 1;
    double tileLodPitchThreshold = (60.0 / 180.0) * std::numbers::pi;
    TileLodMode tileLodMode = TileLodMode::Default;
    // Floor on the variable-zoom emission level. When `transform.getPitch()`
    // exceeds `tileLodPitchThreshold`, the cover algorithm is normally free
    // to emit tiles down to the source minzoom for the distant horizon.
    // Sources that are sensitive to large zoom gaps (e.g. raster-dem, where
    // the terrain mesh visibly pops between a cached coarse parent and the
    // streaming fine ideal) can raise this floor to cap how far down
    // variable-zoom goes. 0 = no extra floor (source minzoom still applies).
    uint8_t tileLodMinZoom = 0;
    // Optional vertical range, in metres, used when the tile surface is not a
    // flat z=0 plane. Raster DEM terrain uses this to keep pitched frustum
    // cover conservative until per-tile min/max elevation is available.
    double tileCoverMinElevationMeters = 0.0;
    double tileCoverMaxElevationMeters = 0.0;
    // Optional cap on returned tiles. 0 preserves the full cover.
    std::size_t tileCoverMaxTiles = 0;
};

int32_t coveringZoomLevel(double z, style::SourceType type, uint16_t tileSize) noexcept;

std::vector<OverscaledTileID> tileCover(const TileCoverParameters& state,
                                        uint8_t z,
                                        const Range<uint8_t> zoomRange,
                                        const std::optional<uint8_t>& overscaledZ = std::nullopt);
std::vector<UnwrappedTileID> tileCover(const LatLngBounds&, uint8_t z);
std::vector<UnwrappedTileID> tileCover(const Geometry<double>&, uint8_t z);

// Compute only the count of tiles needed for tileCover
uint64_t tileCount(const LatLngBounds&, uint8_t z) noexcept;
uint64_t tileCount(const Geometry<double>&, uint8_t z);

} // namespace util
} // namespace mbgl
