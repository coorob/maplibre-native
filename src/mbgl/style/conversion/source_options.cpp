#include <mbgl/style/conversion/source_options.hpp>
#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/style/expression/dsl.hpp>

#include <limits>

namespace mbgl {
namespace style {
namespace conversion {

std::optional<SourceOptions> Converter<SourceOptions>::operator()(const Convertible& value, Error& error) const {
    SourceOptions options;
    bool hasAnyOption = false;

    const auto encodingValue = objectMember(value, "encoding");
    if (encodingValue) {
        const auto encoding = toString(*encodingValue);
        if (encoding && *encoding == "terrarium") {
            options.rasterEncoding = Tileset::RasterEncoding::Terrarium;
        } else if (encoding && *encoding == "mapbox") {
            options.rasterEncoding = Tileset::RasterEncoding::Mapbox;
        } else if (encoding && *encoding == "mvt") {
            options.vectorEncoding = Tileset::VectorEncoding::Mapbox;
        } else if (encoding && *encoding == "mlt") {
            options.vectorEncoding = Tileset::VectorEncoding::MLT;
        } else {
            error.message =
                "invalid encoding - valid types are 'mapbox' and 'terrarium' for raster sources, 'mvt' and 'mlt' for "
                "vector sources";
            return std::nullopt;
        }
        hasAnyOption = true;
    }

    // Fork: optional per-source zoom-range overrides (see SourceOptions).
    const auto minzoomValue = objectMember(value, "minzoom");
    if (minzoomValue) {
        const std::optional<float> minzoom = toNumber(*minzoomValue);
        if (!minzoom || *minzoom < 0 || *minzoom > std::numeric_limits<uint8_t>::max()) {
            error.message = "invalid source minzoom";
            return std::nullopt;
        }
        options.minzoom = static_cast<uint8_t>(*minzoom);
        hasAnyOption = true;
    }

    const auto maxzoomValue = objectMember(value, "maxzoom");
    if (maxzoomValue) {
        const std::optional<float> maxzoom = toNumber(*maxzoomValue);
        if (!maxzoom || *maxzoom < 0 || *maxzoom > std::numeric_limits<uint8_t>::max()) {
            error.message = "invalid source maxzoom";
            return std::nullopt;
        }
        options.maxzoom = static_cast<uint8_t>(*maxzoom);
        hasAnyOption = true;
    }

    if (!hasAnyOption) {
        return {};
    }
    return {std::move(options)};
}

} // namespace conversion
} // namespace style
} // namespace mbgl
