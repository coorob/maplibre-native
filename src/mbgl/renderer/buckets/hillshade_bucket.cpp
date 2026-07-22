#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/renderer/layers/render_hillshade_layer.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/texture2d.hpp>

namespace mbgl {

using namespace style;

HillshadeBucket::HillshadeBucket(PremultipliedImage&& image_, Tileset::RasterEncoding encoding)
    : demdata(image_, encoding) {}

HillshadeBucket::HillshadeBucket(DEMData&& demdata_)
    : demdata(std::move(demdata_)) {}

HillshadeBucket::~HillshadeBucket() {
    sharedVertices->release();
}

const DEMData& HillshadeBucket::getDEMData() const {
    return demdata;
}

DEMData& HillshadeBucket::getDEMData() {
    return demdata;
}

std::shared_ptr<gfx::Texture2D> HillshadeBucket::getOrCreateDEMTexture(gfx::Context& context) {
    if (demTexture) {
        return demTexture;
    }

    const auto image = demdata.getImagePtr();
    if (!image || image->size.isEmpty()) {
        return nullptr;
    }

    auto texture = context.createTexture2D();
    if (!texture) {
        return nullptr;
    }

    texture->setImage(image);
    // Hillshade preparation requires exact Terrain-RGB texels. Terrain
    // displacement manually decodes and bilinearly interpolates four texels,
    // so it does not depend on this sampler's filter mode.
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});
    demTexture = std::move(texture);
    return demTexture;
}

void HillshadeBucket::upload([[maybe_unused]] gfx::UploadPass& uploadPass) {
    if (!hasData()) {
        return;
    }

    uploaded = true;
}

void HillshadeBucket::clear() {
    segments.clear();
    vertices.clear();
    indices.clear();

    vertices.updateModified();

    uploaded = false;
}

void HillshadeBucket::setMask(TileMask&& mask_) {
    if (mask == mask_) {
        return;
    }

    mask = std::move(mask_);
    clear();

    if (mask == TileMask{{0, 0, 0}}) {
        // We want to render the full tile, and keeping the
        // segments/vertices/indices empty means using the global shared buffers
        // for covering the entire tile.
        return;
    }

    // Create a new segment so that we will upload (empty) buffers even when
    // there is nothing to draw for this tile.
    segments.emplace_back(0, 0);

    constexpr const uint16_t vertexLength = 4;

    // Create the vertex buffer for the specified tile mask.
    for (const auto& id : mask) {
        // Create a quad for every masked tile.
        const int32_t vertexExtent = util::EXTENT >> id.z;

        const Point<int16_t> tlVertex = {static_cast<int16_t>(id.x * vertexExtent),
                                         static_cast<int16_t>(id.y * vertexExtent)};
        const Point<int16_t> brVertex = {static_cast<int16_t>(tlVertex.x + vertexExtent),
                                         static_cast<int16_t>(tlVertex.y + vertexExtent)};

        if (segments.back().vertexLength + vertexLength > std::numeric_limits<uint16_t>::max()) {
            // Move to a new segments because the old one can't hold the geometry.
            segments.emplace_back(vertices.elements(), indices.elements());
        }

        vertices.emplace_back(HillshadeBucket::layoutVertex(
            {tlVertex.x, tlVertex.y}, {static_cast<uint16_t>(tlVertex.x), static_cast<uint16_t>(tlVertex.y)}));
        vertices.emplace_back(HillshadeBucket::layoutVertex(
            {brVertex.x, tlVertex.y}, {static_cast<uint16_t>(brVertex.x), static_cast<uint16_t>(tlVertex.y)}));
        vertices.emplace_back(HillshadeBucket::layoutVertex(
            {tlVertex.x, brVertex.y}, {static_cast<uint16_t>(tlVertex.x), static_cast<uint16_t>(brVertex.y)}));
        vertices.emplace_back(HillshadeBucket::layoutVertex(
            {brVertex.x, brVertex.y}, {static_cast<uint16_t>(brVertex.x), static_cast<uint16_t>(brVertex.y)}));

        auto& segment = segments.back();
        assert(segment.vertexLength <= std::numeric_limits<uint16_t>::max());
        const auto offset = static_cast<uint16_t>(segment.vertexLength);

        // 0, 1, 2
        // 1, 2, 3
        indices.emplace_back(offset, offset + 1, offset + 2);
        indices.emplace_back(offset + 1, offset + 2, offset + 3);

        segment.vertexLength += vertexLength;
        segment.indexLength += 6;
    }

    vertices.updateModified();
}

bool HillshadeBucket::hasData() const {
    return demdata.getImage()->valid();
}

} // namespace mbgl
