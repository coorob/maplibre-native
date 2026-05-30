#include <mbgl/style/conversion/raster_dem_options.hpp>
#include <mbgl/style/conversion_impl.hpp>
#include <mbgl/style/expression/dsl.hpp>

#include <limits>
#include <sstream>

namespace mbgl {
namespace style {
namespace conversion {

std::optional<RasterDEMOptions> Converter<RasterDEMOptions>::operator()(const Convertible& value, Error& error) const {
    RasterDEMOptions options;

    auto encodingValue = objectMember(value, "encoding");
    if (encodingValue) {
        std::optional<std::string> encoding = toString(*encodingValue);
        if (encoding && *encoding == "terrarium") {
            options.encoding = {Tileset::DEMEncoding::Terrarium};
        } else if (encoding && *encoding == "mapbox") {
            options.encoding = {Tileset::DEMEncoding::Mapbox};
        } else {
            error.message =
                "invalid raster-dem encoding type - valid types are 'mapbox' "
                "and 'terrarium'";
            return std::nullopt;
        }
    }

    auto minzoomValue = objectMember(value, "minzoom");
    if (minzoomValue) {
        std::optional<float> minzoom = toNumber(*minzoomValue);
        if (!minzoom || *minzoom < 0 || *minzoom > std::numeric_limits<uint8_t>::max()) {
            error.message = "invalid raster-dem minzoom";
            return std::nullopt;
        }
        options.minzoom = static_cast<uint8_t>(*minzoom);
    }

    auto maxzoomValue = objectMember(value, "maxzoom");
    if (maxzoomValue) {
        std::optional<float> maxzoom = toNumber(*maxzoomValue);
        if (!maxzoom || *maxzoom < 0 || *maxzoom > std::numeric_limits<uint8_t>::max()) {
            error.message = "invalid raster-dem maxzoom";
            return std::nullopt;
        }
        options.maxzoom = static_cast<uint8_t>(*maxzoom);
    }

    return {std::move(options)};
}

} // namespace conversion
} // namespace style
} // namespace mbgl
