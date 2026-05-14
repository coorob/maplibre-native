#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/util/convert.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/projection.hpp>

namespace mbgl {

using namespace shaders;

void TerrainLayerTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    Log::Info(Event::Render, "TerrainLayerTweaker::execute called, layerGroup.empty()=" + std::to_string(layerGroup.empty()) +
              ", terrain=" + std::to_string(terrain != nullptr));

    if (layerGroup.empty() || !terrain) {
        Log::Warning(Event::Render, "TerrainLayerTweaker::execute early return - empty or no terrain");
        return;
    }

    Log::Info(Event::Render, "TerrainLayerTweaker processing " + std::to_string(layerGroup.getDrawableCount()) + " drawables");

    const auto& state = parameters.state;
    auto& context = parameters.context;

#if defined(DEBUG)
    const auto label = layerGroup.getName() + "-update-uniforms";
    const auto debugGroup = parameters.encoder->createDebugGroup(label.c_str());
#endif

    // Get terrain properties
    const float baseExaggeration = terrain->getExaggeration();
    // Apply moderate exaggeration for visible 3D terrain (3x)
    const float exaggeration = baseExaggeration * 3.0f;
    const float elevationOffset = 0.0f;

    static bool hasLoggedExaggeration = false;
    if (!hasLoggedExaggeration) {
        Log::Info(Event::Render, "Terrain exaggeration: base=" + std::to_string(baseExaggeration) +
                  ", multiplied=" + std::to_string(exaggeration));
        hasLoggedExaggeration = true;
    }

    // Populate layer-level UBO with terrain properties
    auto& layerUniforms = layerGroup.mutableUniformBuffers();
    const TerrainEvaluatedPropsUBO propsUBO = {
        .exaggeration = exaggeration,
        .elevation_offset = elevationOffset,
        .pad1 = 0.0f,
        .pad2 = 0.0f
    };
    layerUniforms.createOrUpdate(idTerrainEvaluatedPropsUBO, &propsUBO, context);

#if MLN_UBO_CONSOLIDATION
    int i = 0;
    std::vector<TerrainDrawableUBO> drawableUBOVector(layerGroup.getDrawableCount());
#endif

    // Visit each drawable to populate per-drawable UBOs
    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        if (!drawable.getTileID()) {
            return;
        }

        const UnwrappedTileID tileID = drawable.getTileID()->toUnwrapped();

        // Calculate transformation matrix for this terrain tile
        // This uses the same matrix calculation as other layers
        mat4 matrix = parameters.matrixForTile(tileID);

        // Vertices feed elevation in metres on the Z axis. The base tile
        // matrix scales X/Y from tile units to mercator world-pixels but
        // leaves Z scale at 1, so meters would feed into clip-space
        // unscaled and the mesh collapses to a near-flat plane. Scale
        // the Z column by pixelsPerMeter (same trick Camera uses for
        // its world-to-camera matrix), so metres feed in correctly.
        const double pixelsPerMeter = 1.0 / Projection::getMetersPerPixelAtLatitude(
            state.getLatLng().latitude(), state.getZoom());
        matrix[8] *= pixelsPerMeter;
        matrix[9] *= pixelsPerMeter;
        matrix[10] *= pixelsPerMeter;
        matrix[11] *= pixelsPerMeter;

#if !MLN_UBO_CONSOLIDATION
        auto& drawableUniforms = drawable.mutableUniformBuffers();
#endif

#if MLN_UBO_CONSOLIDATION
        drawableUBOVector[i] = {
#else
        const TerrainDrawableUBO drawableUBO = {
#endif
            .matrix = util::cast<float>(matrix)
        };

#if !MLN_UBO_CONSOLIDATION
        drawableUniforms.createOrUpdate(idTerrainDrawableUBO, &drawableUBO, context);
#endif

#if MLN_UBO_CONSOLIDATION
        drawable.setUBOIndex(i++);
#endif
    });

#if MLN_UBO_CONSOLIDATION
    const size_t drawableUBOVectorSize = sizeof(TerrainDrawableUBO) * drawableUBOVector.size();
    if (!drawableUniformBuffer || drawableUniformBuffer->getSize() < drawableUBOVectorSize) {
        drawableUniformBuffer = context.createUniformBuffer(
            drawableUBOVector.data(), drawableUBOVectorSize, false, true);
    } else {
        drawableUniformBuffer->update(drawableUBOVector.data(), drawableUBOVectorSize);
    }

    layerUniforms.set(idTerrainDrawableUBO, drawableUniformBuffer);
#endif
}

} // namespace mbgl
