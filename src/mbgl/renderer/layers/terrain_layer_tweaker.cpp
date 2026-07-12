#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/renderer/buckets/fill_extrusion_bucket.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/style/light_impl.hpp>
#include <mbgl/util/convert.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/projection.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mbgl {

using namespace shaders;

namespace {

bool klattraLogTerrainFinal() {
    static const bool enabled = std::getenv("KLATTRA_LOG_TERRAIN_FINAL") != nullptr;
    return enabled;
}

bool klattraLogTerrainFinalRepeat() {
    static const bool enabled = std::getenv("KLATTRA_LOG_TERRAIN_FINAL_REPEAT") != nullptr;
    return enabled;
}

bool klattraTraceStderr() {
    static const bool enabled = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
    return enabled;
}

void klattraTrace(const std::string& message) {
    if (!klattraTraceStderr()) return;
    std::fprintf(stderr, "[KLATTRA_TRACE] %s\n", message.c_str());
}

float klattraTerrainDebugColorMode() {
    static const float mode = [] {
        if (std::getenv("KLATTRA_TERRAIN_DEBUG_FALLBACK")) return 3.0f;
        if (std::getenv("KLATTRA_TERRAIN_SAMPLE_LOD0")) return 2.0f;
        if (std::getenv("KLATTRA_TERRAIN_DEBUG_COLOR")) return 1.0f;
        return 0.0f;
    }();
    return mode;
}

float klattraTerrainDebugVertexMode() {
    static const float mode = [] {
        if (std::getenv("KLATTRA_TERRAIN_REAL_DEPTH")) return 1.0f;
        if (std::getenv("KLATTRA_TERRAIN_CLAMP_DEPTH")) return 2.0f;
        if (std::getenv("KLATTRA_TERRAIN_DEBUG_FLAT")) return 3.0f;
        if (std::getenv("KLATTRA_TERRAIN_DEBUG_NO_SKIRTS")) return 4.0f;
        if (std::getenv("KLATTRA_TERRAIN_FORCE_FLAT_DEPTH")) return 5.0f;
        return 0.0f;
    }();
    return mode;
}

float klattraTerrainLightIntensity(float styleIntensity) {
    static const auto overrideValue = []() -> std::optional<float> {
        if (!std::getenv("KLATTRA_ALLOW_TERRAIN_LIGHT_OVERRIDE")) {
            return std::nullopt;
        }
        const char* enabled = std::getenv("KLATTRA_TERRAIN_SHADER_LIGHT");
        if (enabled && (std::strcmp(enabled, "0") == 0 || std::strcmp(enabled, "false") == 0 ||
                        std::strcmp(enabled, "FALSE") == 0)) {
            return 0.0f;
        }
        if (const char* strength = std::getenv("KLATTRA_TERRAIN_LIGHT_STRENGTH")) {
            return std::clamp(std::strtof(strength, nullptr), 0.0f, 1.5f);
        }
        return std::nullopt;
    }();
    return overrideValue.value_or(std::clamp(styleIntensity, 0.0f, 1.5f));
}

float metersPerTileAtCenter(const CanonicalTileID& id) {
    const LatLng center = LatLngBounds(id).center();
    return static_cast<float>(Projection::getMetersPerPixelAtLatitude(center.latitude(), id.z) * util::tileSize_D);
}

std::string klattraTileString(const OverscaledTileID& id) {
    return "z" + std::to_string(id.canonical.z) +
           "/" + std::to_string(id.canonical.x) +
           "/" + std::to_string(id.canonical.y) +
           "=>z" + std::to_string(id.overscaledZ);
}

std::string klattraTexturePtrString(const std::shared_ptr<gfx::Texture2D>& texture) {
    return texture ? std::to_string(reinterpret_cast<uintptr_t>(texture.get())) : "0";
}

} // namespace

void TerrainLayerTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty() || !terrain) {
        return;
    }

    const auto& state = parameters.state;
    auto& context = parameters.context;

#if defined(DEBUG)
    const auto label = layerGroup.getName() + "-update-uniforms";
    const auto debugGroup = parameters.encoder->createDebugGroup(label.c_str());
#endif

    const float exaggeration = terrain->getExaggeration();
    // Native camera altitude is anchored to the flat map plane. On high
    // mountain routes, absolute sea-level DEM heights can place a close,
    // pitched drone camera inside the terrain. Draw the mesh relative to the
    // loaded DEM height under the current map center; this keeps the camera
    // above the local surface while preserving nearby relief and slope.
    const auto maybeElevationOrigin = terrain->getElevationOriginMeters();
    const float fallbackElevationOrigin =
        std::abs(state.getLatLng().latitude()) >= 60.0 ? 1000.0f : 0.0f;
    const float elevationOrigin = maybeElevationOrigin.value_or(fallbackElevationOrigin);
    const float elevationOffset =
        std::getenv("KLATTRA_TERRAIN_ABSOLUTE_HEIGHTS") != nullptr ? 0.0f : -elevationOrigin * exaggeration;

    // Reuse fill-extrusion's interpretation of the global style light's
    // colour and intensity, but always compute the light position as if
    // it were map-anchored. Fill-extrusion's helper rotates the light
    // by `-state.getBearing()` when the style says `anchor: viewport`,
    // which makes sense for buildings (they're small and the user
    // intuitively expects facade highlights to stay constant as the
    // map rotates). On a continuous terrain mesh, viewport anchoring
    // makes the entire landscape's shading flicker every time the
    // camera rotates — the sun shouldn't move with the camera. So we
    // bypass that rotation: terrain always sees a sun-fixed-in-world
    // light regardless of the style's anchor setting.
    const auto& evaluatedLight = parameters.evaluatedLight;
    const auto lightColor = FillExtrusionBucket::lightColor(evaluatedLight);
    const auto lightPos = evaluatedLight.get<style::LightPosition>().getCartesian();
    const auto lightIntensity = klattraTerrainLightIntensity(FillExtrusionBucket::lightIntensity(evaluatedLight));
    const float debugColorMode = klattraTerrainDebugColorMode();
    const float debugVertexMode = klattraTerrainDebugVertexMode();

    // .50 horizon haze: knobs in kilometres, converted per frame to the
    // clip-w units the shader sees (clip w = view distance in world-pixel
    // units, so km / metersPerPixel at the current latitude+zoom).
    static const double hazeStartKm = [] {
        const char* v = std::getenv("KLATTRA_HAZE_START_KM");
        return v ? std::atof(v) : 12.0;
    }();
    static const double hazeEndKm = [] {
        const char* v = std::getenv("KLATTRA_HAZE_END_KM");
        return v ? std::atof(v) : 34.0;
    }();
    static const float hazeAlpha = [] {
        if (std::getenv("KLATTRA_DISABLE_HAZE") != nullptr) return 0.0f;
        const char* v = std::getenv("KLATTRA_HAZE_ALPHA");
        return v ? static_cast<float>(std::atof(v)) : 0.85f;
    }();
    const double metersPerPixel = Projection::getMetersPerPixelAtLatitude(
        parameters.state.getLatLng().latitude(), parameters.state.getZoom());
    const float hazeStartW = static_cast<float>(hazeStartKm * 1000.0 / metersPerPixel);
    const float hazeEndW = static_cast<float>(std::max(hazeEndKm, hazeStartKm + 0.5) * 1000.0 / metersPerPixel);
    const float hazeInvRange = 1.0f / std::max(hazeEndW - hazeStartW, 1.0f);
    Color fallback = terrain ? terrain->getDrapeFallbackColor()
                             : Color{0.95686275f, 0.91764706f, 0.81568627f, 1.0f};
    // .60 tint (opt-in since .61): cyan no-valid-drape-pixel attribution.
    if (std::getenv("KLATTRA_TINT_WRITERS") != nullptr) {
        fallback = Color{0.0f, 1.0f, 1.0f, 1.0f};
    }

    auto& layerUniforms = layerGroup.mutableUniformBuffers();
    const TerrainEvaluatedPropsUBO propsUBO = {
        .exaggeration = exaggeration,
        .elevation_offset = elevationOffset,
        .pad1 = debugColorMode,
        .pad2 = debugVertexMode,
        .light_color_pad = {lightColor[0], lightColor[1], lightColor[2], 0.0f},
        .light_position_intensity = {lightPos[0], lightPos[1], lightPos[2], lightIntensity},
        .fallback_color = {fallback.r, fallback.g, fallback.b, 1.0f},
        // #8FC3DE — the app's 3D sea/sky backdrop tone, so the horizon
        // merges into the backdrop instead of banding against it.
        .haze_color = {0.56078431f, 0.76470588f, 0.87058824f, hazeAlpha},
        .haze_params = {hazeStartW, hazeInvRange, 0.0f, 0.0f},
    };
    layerUniforms.createOrUpdate(idTerrainEvaluatedPropsUBO, &propsUBO, context);

    static uint64_t finalTraceFrame = 0;
    const bool logTerrainFinal = klattraLogTerrainFinal();
    const bool logTerrainFinalRepeat = klattraLogTerrainFinalRepeat();
    const uint64_t traceFrame = logTerrainFinal ? ++finalTraceFrame : 0;
    static uint64_t stderrTraceFrame = 0;
    const bool stderrTrace = klattraTraceStderr();
    const uint64_t stderrFrame = stderrTrace ? ++stderrTraceFrame : 0;
    const bool shouldTraceStderr = stderrTrace && (stderrFrame <= 24 || stderrFrame % 60 == 0);
    if (logTerrainFinal && (logTerrainFinalRepeat || traceFrame <= 12 || traceFrame % 60 == 0)) {
        Log::Info(Event::Render,
                  "[KLATTRA TERRAIN_FINAL] frame=" + std::to_string(traceFrame) +
                      " drawables=" + std::to_string(layerGroup.getDrawableCount()) +
                      " zoom=" + std::to_string(state.getZoom()) +
                      " pitch=" + std::to_string(state.getPitch()) +
                      " bearing=" + std::to_string(state.getBearing()) +
                      " exaggeration=" + std::to_string(exaggeration) +
                      " elevationOrigin=" + std::to_string(elevationOrigin) +
                      " elevationOffset=" + std::to_string(elevationOffset) +
                      " shaderRelief=" + std::to_string(lightIntensity) +
                      " debugColor=" + std::to_string(debugColorMode) +
                      " debugVertex=" + std::to_string(debugVertexMode));
    }
    if (shouldTraceStderr) {
        klattraTrace("terrain final-begin frame=" + std::to_string(stderrFrame) +
                     " drawables=" + std::to_string(layerGroup.getDrawableCount()) +
                     " zoom=" + std::to_string(state.getZoom()) +
                     " pitch=" + std::to_string(state.getPitch()) +
                     " bearing=" + std::to_string(state.getBearing()) +
                     " exaggeration=" + std::to_string(exaggeration) +
                     " elevationOrigin=" + std::to_string(elevationOrigin) +
                     " elevationOffset=" + std::to_string(elevationOffset) +
                     " debugColor=" + std::to_string(debugColorMode) +
                     " debugVertex=" + std::to_string(debugVertexMode));
    }

#if MLN_UBO_CONSOLIDATION
    int i = 0;
    std::vector<TerrainDrawableUBO> drawableUBOVector(layerGroup.getDrawableCount());
#endif

    // Visit each drawable to populate per-drawable UBOs
    std::size_t stderrDrawableCount = 0;
    std::size_t repairedTextureBindings = 0;
    std::string firstTextureRepair;
    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        if (!drawable.getTileID()) {
            return;
        }

        const UnwrappedTileID tileID = drawable.getTileID()->toUnwrapped();

        // Calculate transformation matrix for this terrain tile.
        // This uses the same matrix calculation as other layers.
        mat4 matrix = parameters.matrixForTile(tileID);

#if !MLN_UBO_CONSOLIDATION
        auto& drawableUniforms = drawable.mutableUniformBuffers();
#endif

        // Look up the per-drawable DEM binding so we can pass the UV
        // remap (sub-rect inside a parent-fallback source) through to the
        // vertex shader. Identity (`{0,0}, 1`) when this tile's exact
        // DEM is loaded; sub-rect when the binding is borrowing from an
        // ancestor while the exact-zoom data streams in.
        std::array<float, 2> demTL{{0.0f, 0.0f}};
        float demScale = 1.0f;
        float metersPerTile = 1.0f;
        std::array<float, 2> drapeTL{{0.0f, 0.0f}};
        float drapeScale = 1.0f;
        float elevationOffsetScale = 1.0f;
        if (const auto* binding = terrain->getDEMBinding(*drawable.getTileID())) {
            const auto& actualDEMTexture = drawable.getTexture(0);
            const auto& actualDrapeTexture = drawable.getTexture(1);
            const bool demMismatch = binding->texture && actualDEMTexture != binding->texture;
            const bool drapeMismatch = binding->drapeTexture && actualDrapeTexture != binding->drapeTexture;
            if (demMismatch || drapeMismatch) {
                if (firstTextureRepair.empty()) {
                    firstTextureRepair = " tile=" + klattraTileString(*drawable.getTileID()) +
                                         " demActual=" + klattraTexturePtrString(actualDEMTexture) +
                                         " demExpected=" + klattraTexturePtrString(binding->texture) +
                                         " drapeActual=" + klattraTexturePtrString(actualDrapeTexture) +
                                         " drapeExpected=" + klattraTexturePtrString(binding->drapeTexture);
                }
                if (demMismatch) {
                    drawable.setTexture(binding->texture, 0);
                }
                if (drapeMismatch) {
                    drawable.setTexture(binding->drapeTexture, 1);
                }
                ++repairedTextureBindings;
            }

            demTL = binding->demTL;
            demScale = binding->demScale;
            metersPerTile = metersPerTileAtCenter(binding->sourceID.canonical);
            drapeTL = binding->drapeTL;
            drapeScale = binding->drapeScale;
            // Empty-DEM placeholder: keep the flat plane at origin level
            // instead of sinking it by the (negative) elevation_offset —
            // fork review 2026-07-03 finding 11 (black pit / beige slab).
            if (binding->usedEmptyDEM) {
                elevationOffsetScale = 0.0f;
            }
            if (binding->drapeID) {
                if (auto drape = terrain->getDrapeTarget(*binding->drapeID)) {
                    drape->inspectDebugPixels();
                }
            } else if (auto drape = terrain->getDrapeTarget(*drawable.getTileID())) {
                drape->inspectDebugPixels();
            }
            if (logTerrainFinal && (logTerrainFinalRepeat || traceFrame <= 12 || traceFrame % 60 == 0)) {
                Log::Info(Event::Render,
                          "[KLATTRA TERRAIN_FINAL] frame=" + std::to_string(traceFrame) +
                              " tile=" + klattraTileString(*drawable.getTileID()) +
                              " source=" + klattraTileString(binding->sourceID) +
                              " drape=" +
                                  (binding->drapeID ? klattraTileString(*binding->drapeID)
                                                    : std::string("none")) +
                              " demTexture=" + klattraTexturePtrString(binding->texture) +
                              " emptyDEM=" + std::to_string(binding->usedEmptyDEM) +
                              " drapeReady=" + std::to_string(binding->drapeReady) +
                              " drapeTexture=" + klattraTexturePtrString(binding->drapeTexture) +
                              " drapeFallback=" + std::to_string(binding->usedDrapeFallback) +
                              " demTL=" + std::to_string(demTL[0]) + "," + std::to_string(demTL[1]) +
                              " demScale=" + std::to_string(demScale) +
                              " drapeTL=" + std::to_string(drapeTL[0]) + "," +
                                  std::to_string(drapeTL[1]) +
                              " drapeScale=" + std::to_string(drapeScale) +
                              " metersPerTile=" + std::to_string(metersPerTile) +
                              " matrixZ=" + std::to_string(matrix[8]) + "," +
                                  std::to_string(matrix[9]) + "," +
                                  std::to_string(matrix[10]) + "," +
                                  std::to_string(matrix[11]) +
                              " matrixW=" + std::to_string(matrix[12]) + "," +
                                  std::to_string(matrix[13]) + "," +
                                  std::to_string(matrix[14]) + "," +
                                  std::to_string(matrix[15]));
            }
            if (shouldTraceStderr && stderrDrawableCount < 4) {
                klattraTrace("terrain final-drawable frame=" + std::to_string(stderrFrame) +
                             " tile=" + klattraTileString(*drawable.getTileID()) +
                             " source=" + klattraTileString(binding->sourceID) +
                             " emptyDEM=" + std::to_string(binding->usedEmptyDEM) +
                             " drapeReady=" + std::to_string(binding->drapeReady) +
                             " demScale=" + std::to_string(demScale) +
                             " metersPerTile=" + std::to_string(metersPerTile) +
                             " matrixZ=" + std::to_string(matrix[8]) + "," +
                                 std::to_string(matrix[9]) + "," +
                                 std::to_string(matrix[10]) + "," +
                                 std::to_string(matrix[11]) +
                             " matrixW=" + std::to_string(matrix[12]) + "," +
                                 std::to_string(matrix[13]) + "," +
                                 std::to_string(matrix[14]) + "," +
                                 std::to_string(matrix[15]));
            }
        }
        stderrDrawableCount++;

#if MLN_UBO_CONSOLIDATION
        drawableUBOVector[i] = {
#else
        const TerrainDrawableUBO drawableUBO = {
#endif
            .matrix = util::cast<float>(matrix),
            .dem_tl = demTL,
            .dem_scale = demScale,
            .meters_per_tile = metersPerTile,
            .drape_tl = drapeTL,
            .drape_scale = drapeScale,
            .elevation_offset_scale = elevationOffsetScale
        };

#if !MLN_UBO_CONSOLIDATION
        drawableUniforms.createOrUpdate(idTerrainDrawableUBO, &drawableUBO, context);
#endif

#if MLN_UBO_CONSOLIDATION
        drawable.setUBOIndex(i++);
#endif
    });

    if (repairedTextureBindings > 0) {
        static std::chrono::steady_clock::time_point lastRepairLog{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRepairLog >= std::chrono::seconds(1)) {
            lastRepairLog = now;
            Log::Warning(Event::Render,
                         "[KLATTRA .72 BINDING_REPAIR] count=" + std::to_string(repairedTextureBindings) +
                             firstTextureRepair);
        }
    }

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
