#include <mbgl/renderer/terrain_perf_diagnostic.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/style/layer_impl.hpp>
#include <mbgl/algorithm/update_tile_masks.hpp>
#include <mbgl/renderer/cover_hold_budget.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/renderer/render_source.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/render_pass.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_orchestrator.hpp>
#include <mbgl/renderer/raster_drape_cohort.hpp>
#include <mbgl/renderer/topo_vector_cohort.hpp>
#include <mbgl/renderer/topo_drape_detail_policy.hpp>
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/map/transform_state.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/tile/raster_dem_tile.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/gfx/drawable_impl.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/gfx/shader_registry.hpp>
#include <mbgl/gfx/color_mode.hpp>
#include <mbgl/gfx/texture2d.hpp>
#include <mbgl/gfx/vertex_attribute.hpp>
#include <mbgl/gfx/vertex_vector.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/shaders/segment.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/projection.hpp>
#include <mbgl/util/tile_cover.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/mat4.hpp>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mbgl {

namespace {

uint64_t nextTerrainElevationRevision() {
    // A symbol bucket may survive source/style replacement. A process-wide
    // generation also distinguishes a new terrain object at a recycled address.
    static std::atomic<uint64_t> revision{0};
    return revision.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool klattraLogDrapeTrace() {
    static const bool enabled = std::getenv("KLATTRA_LOG_DRAPE_TRACE") != nullptr;
    return enabled;
}

// Shared with tile_cover.cpp / render_raster_dem_source.cpp telemetry (each
// file carries its own copy — anonymous namespace). Explicit diagnostic opt-in:
// KLATTRA_LOG_COVER_SUMMARY=1.
bool klattraLogCoverSummary() {
    static const bool enabled = [] {
        const char* v = std::getenv("KLATTRA_LOG_COVER_SUMMARY");
        return v && *v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    return enabled;
}

std::string klattraZoomHistogramString(const std::array<uint32_t, 26>& counts) {
    std::string out;
    for (std::size_t z = 0; z < counts.size(); ++z) {
        if (!counts[z]) continue;
        if (!out.empty()) out += ' ';
        out += 'z' + std::to_string(z) + ':' + std::to_string(counts[z]);
    }
    return out.empty() ? std::string("-") : out;
}

bool klattraTraceStderr() {
    static const bool enabled = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
    return enabled;
}

void klattraTrace(const std::string& message) {
    if (!klattraTraceStderr()) return;
    std::fprintf(stderr, "[KLATTRA_TRACE] %s\n", message.c_str());
}

// Frame-dump emitter: Warning for the device syslog AND the stderr trace
// path for the simulator, where mbgl Log::Warning never reaches the unified
// log (2026-07-04 finding: klattraTrace lines flow, Warning lines vanish).
void klattraDumpEmit(const std::string& message) {
    Log::Warning(Event::Render, message);
    if (std::getenv("KLATTRA_TRACE_STDERR") != nullptr) {
        fprintf(stderr, "[KLATTRA_TRACE] %s\n", message.c_str());
    }
}

// .46-diag (flyover campaign): device-visible flight diagnostics, default
// ON in this diag dist (KLATTRA_FLYDIAG=0 disables). Emitted via
// klattraDumpEmit (Warning + stderr) with a per-second budget so a flight
// cannot flood the syslog. Render-thread only — plain statics.
bool klattraFlyDiag() {
    static const bool enabled = [] {
        const char* v = std::getenv("KLATTRA_FLYDIAG");
        return v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    return enabled;
}

bool klattraFlyDiagBudget() {
    static int64_t windowStart = 0;
    static uint32_t count = 0;
    const int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now != windowStart) {
        windowStart = now;
        count = 0;
    }
    return count++ < 40;
}

// .53 stage-diag: jetsam watches phys_footprint, so report that (fallback
// to resident size). Render-thread only, called at 1 Hz.
double klattraPhysFootprintMB() {
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<double>(info.phys_footprint) / (1024.0 * 1024.0);
    }
#endif
    return -1.0;
}

float klattraStageMs(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<float, std::milli>(to - from).count();
}

void klattraStagePush(std::vector<float>& samples, float ms) {
    if (samples.size() < 4096) samples.push_back(ms);
}

std::string klattraStagePercentiles(std::vector<float>& samples) {
    if (samples.empty()) return "-";
    std::vector<float> copy(samples);
    const auto nth = [&](double q) {
        const std::size_t i = static_cast<std::size_t>(q * (copy.size() - 1));
        std::nth_element(copy.begin(), copy.begin() + i, copy.end());
        return static_cast<int64_t>(std::lround(copy[i]));
    };
    return std::to_string(nth(0.5)) + "/" + std::to_string(nth(0.9)) + "(n" + std::to_string(copy.size()) + ")";
}

std::string klattraTileString(const OverscaledTileID& id) {
    return "z" + std::to_string(id.canonical.z) + "/" + std::to_string(id.canonical.x) + "/" +
           std::to_string(id.canonical.y) + "=>z" + std::to_string(id.overscaledZ);
}

std::string klattraTexturePtrString(const std::shared_ptr<gfx::Texture2D>& texture) {
    return texture ? std::to_string(reinterpret_cast<uintptr_t>(texture.get())) : "0";
}

struct TerrainLayoutVertex {
    std::array<int16_t, 2> pos;
    std::array<int16_t, 2> texturePos;
};

std::size_t klattraDrapeDrawableCount(const TerrainDrapeTargetPtr& target) {
    if (!target) return 0;
    std::size_t count = 0;
    target->visitLayerGroups([&](LayerGroupBase& layerGroup) { count += layerGroup.getDrawableCount(); });
    return count;
}

bool klattraDrapeTargetReady(const TerrainDrapeTargetPtr& target) {
    return RenderTerrain::isDrapeTargetReady(target);
}

bool klattraDrapeTargetReadyForTile(const TerrainDrapeTargetPtr& target, const OverscaledTileID& tileID) {
    return RenderTerrain::isDrapeTargetReadyForTile(target, tileID);
}

// Once a drape target is retired it no longer renders: the matching
// RemoveRenderTargetRequest has been queued, and the object is retained only
// so terrain drawables can keep sampling its last published texture until a
// successor is ready. OpenGL drawables retain per-target buffers, UBOs, VAOs,
// buckets and render-tile snapshots; keeping those build groups beside the
// immutable texture multiplied dense Topo memory across every parked target.
// Preserve the baked texture and publication metadata, but release the build
// graph as soon as the target crosses that lifecycle boundary. Metal keeps its
// established lifecycle unchanged.
void klattraReleaseRetiredDrapeBuildState(const TerrainDrapeTargetPtr& target) {
#if MLN_RENDER_BACKEND_OPENGL
    if (target) {
        target->removeLayerGroupsIf([](int32_t, const LayerGroupBase&) { return true; });
    }
#else
    (void)target;
#endif
}

// .84 diagnostic: classify the official Lantmateriet land-cover fills that
// visibly disappear on the physical iPhone. This observes the CPU-side groups
// on the exact RenderTargets sampled by terrain; it does not alter readiness,
// routing, ordering, or draw state.
struct KlattraTopoFillTargetStats {
    bool hasLandOpenGroup = false;
    bool hasLandAkerGroup = false;
    std::size_t forestGroups = 0;
    std::size_t landOpenDrawables = 0;
    std::size_t landAkerDrawables = 0;
    std::size_t forestDrawables = 0;
    std::array<uint32_t, 26> landOpenSourceZooms{};
    std::array<uint32_t, 26> landAkerSourceZooms{};
    std::array<uint32_t, 26> forestSourceZooms{};
};

bool klattraIsForestDrapeGroup(const std::string& name) {
    constexpr const char* suffix = "-drape";
    constexpr std::size_t suffixLength = 6;
    return name.rfind("skog-", 0) == 0 && name.size() >= suffixLength &&
           name.compare(name.size() - suffixLength, suffixLength, suffix) == 0;
}

void klattraRecordDrawableSourceZooms(LayerGroupBase& group, std::array<uint32_t, 26>& counts) {
    if (group.getType() != LayerGroupBase::Type::TileLayerGroup) return;
    static_cast<TileLayerGroup&>(group).visitDrawables([&](gfx::Drawable& drawable) {
        if (const auto& tileID = drawable.getTileID(); tileID && tileID->canonical.z < counts.size()) {
            counts[tileID->canonical.z]++;
        }
    });
}

KlattraTopoFillTargetStats klattraTopoFillTargetStats(const TerrainDrapeTargetPtr& target) {
    KlattraTopoFillTargetStats stats;
    if (!target) return stats;

    target->visitLayerGroups([&](LayerGroupBase& group) {
        const auto& name = group.getName();
        if (name == "land-open-drape") {
            stats.hasLandOpenGroup = true;
            stats.landOpenDrawables += group.getDrawableCount();
            klattraRecordDrawableSourceZooms(group, stats.landOpenSourceZooms);
        } else if (name == "land-aker-drape") {
            stats.hasLandAkerGroup = true;
            stats.landAkerDrawables += group.getDrawableCount();
            klattraRecordDrawableSourceZooms(group, stats.landAkerSourceZooms);
        } else if (klattraIsForestDrapeGroup(name)) {
            stats.forestGroups++;
            stats.forestDrawables += group.getDrawableCount();
            klattraRecordDrawableSourceZooms(group, stats.forestSourceZooms);
        }
    });
    return stats;
}

void klattraAccumulateZooms(std::array<uint32_t, 26>& into, const std::array<uint32_t, 26>& from) {
    for (std::size_t i = 0; i < into.size(); ++i) into[i] += from[i];
}

// .62: does this canvas hold ANY raster drape drawable? The .61 flight
// showed level-0 canvas regions that are pure cleared background (black
// under relief shade) — this names the population directly at bind time.
bool klattraDrapeHasRasterContent(const TerrainDrapeTargetPtr& target) {
    if (!target) return false;
    bool has = false;
    target->visitLayerGroups([&](LayerGroupBase& group) {
        if (!has && group.getName().find("-raster-drape") != std::string::npos && group.getDrawableCount() > 0) {
            has = true;
        }
    });
    return has;
}

bool klattraIsAncestorOf(const OverscaledTileID& ancestor, const OverscaledTileID& child) {
    return ancestor.canonical.z < child.canonical.z && LayerTweaker::tilesOverlap(ancestor, child);
}

bool klattraUseSharedTopoDrapeCohort(const RenderTree& renderTree) {
#if MLN_RENDER_BACKEND_OPENGL
    bool hasOfficialTopo = false;
    bool hasSatelliteBasemap = false;
    for (const auto& item : renderTree.getLayerRenderItemMap()) {
        if (!item.source) continue;
        const auto& sourceID = item.source->baseImpl->id;
        hasOfficialTopo = hasOfficialTopo || topo_vector::isOfficialSourceID(sourceID);
        hasSatelliteBasemap = hasSatelliteBasemap ||
                              raster_drape::isAuthoritativeTerrainRaster(item.layer.get().getID(), sourceID);
    }
    return topo_vector::useSharedDrapeCohort(hasOfficialTopo, hasSatelliteBasemap);
#else
    (void)renderTree;
    return false;
#endif
}

// The publication hold needs an authoritative writer that can prove a drape
// canvas complete: an official or Finnish Topo fill layer, or the Esri
// terrain raster. Styles without one (Germany and its neighbours, the native
// aerial blend) publish every drape generation immediately. Read from the
// full style rather than the render tree: official Topo fills start at z10,
// and a zoomed-out camera must keep the hold rather than bake background.
bool klattraStyleHasDrapeAuthority(const UpdateParameters& parameters) {
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
    constexpr bool androidOpenGL = true;
#else
    constexpr bool androidOpenGL = false;
#endif
    for (const auto& layer : *parameters.layers) {
        if (layer->visibility == style::VisibilityType::None) continue;
        if (raster_drape::isAuthoritativeTerrainRaster(layer->id, layer->source)) return true;
        if (std::strcmp(layer->getTypeInfo()->type, "fill") == 0 &&
            topo_vector::isAuthoritativeFillSourceID(layer->source, androidOpenGL)) {
            return true;
        }
    }
    return false;
}

// Terrain cannot draw the full geometry for every entry in the source's raw
// render set: stock fade retention, parent fallback, and Traska's fragmented-
// cover hold intentionally keep ancestors beside descendants. Flat layers use
// TileMask to make that set spatially disjoint. .68 applies the same partition
// to terrain by turning each uncovered mask cell into its own ideal mesh.
struct KlattraTerrainMaskEntry {
    bool usedByRenderedLayers = true;
    TileMask mask{{0, 0, 0}};

    void setMask(TileMask&& value) { mask = std::move(value); }
};

OverscaledTileID klattraAbsoluteMaskLeaf(const UnwrappedTileID& base, const CanonicalTileID& relative) {
    const uint8_t leafZ = static_cast<uint8_t>(base.canonical.z + relative.z);
    const uint64_t scale = uint64_t{1} << relative.z;
    return OverscaledTileID(leafZ,
                            base.wrap,
                            CanonicalTileID(leafZ,
                                static_cast<uint32_t>(base.canonical.x * scale + relative.x),
                                static_cast<uint32_t>(base.canonical.y * scale + relative.y)));
}

std::pair<std::array<float, 2>, float> klattraSubrectForChildInAncestor(const OverscaledTileID& child,
                                                                        const OverscaledTileID& ancestor) {
    const uint8_t dz = static_cast<uint8_t>(child.canonical.z - ancestor.canonical.z);
    const uint32_t mask = (1u << dz) - 1u;
    const uint32_t subX = child.canonical.x & mask;
    const uint32_t subY = child.canonical.y & mask;
    const float scale = 1.0f / static_cast<float>(1u << dz);
    return {{{static_cast<float>(subX) * scale, static_cast<float>(subY) * scale}}, scale};
}

// The corrected OpenGL offscreen allocator now honors every requested target
// size. The old 1536/1024/512 tiers therefore materialized roughly three times
// the intended Android target memory and pushed the app above 1.2 GB when Topo
// mounted its vector drape set. Keep the accepted Metal/iOS budget unchanged;
// OpenGL uses smaller canvases and fewer retained overscan/ancestor targets.
#if MLN_RENDER_BACKEND_OPENGL
constexpr int32_t klattraDefaultDrapeNearSize = 1024;
constexpr int32_t klattraDefaultDrapeMidSize = 768;
constexpr int32_t klattraDefaultDrapeFarSize = 384;
constexpr uint32_t klattraDefaultDrapeNearTiles = 8;
constexpr uint32_t klattraDefaultDrapeMidTiles = 32;
constexpr uint32_t klattraDefaultDrapeTotalCapStable = 160;
constexpr uint32_t klattraDefaultDrapeTotalCapMoving = 144;
constexpr int32_t klattraDefaultTopoDrapeNearSize = 1024;
constexpr int32_t klattraDefaultTopoDrapeMidSize = 512;
constexpr int32_t klattraDefaultTopoDrapeFarSize = 256;
constexpr uint32_t klattraDefaultTopoDrapeNearTiles = 6;
constexpr uint32_t klattraDefaultTopoDrapeMidTiles = 24;
#else
constexpr int32_t klattraDefaultDrapeNearSize = 1536;
constexpr int32_t klattraDefaultDrapeMidSize = 1024;
constexpr int32_t klattraDefaultDrapeFarSize = 512;
constexpr uint32_t klattraDefaultDrapeNearTiles = 12;
constexpr uint32_t klattraDefaultDrapeMidTiles = 48;
constexpr uint32_t klattraDefaultDrapeTotalCapStable = 240;
constexpr uint32_t klattraDefaultDrapeTotalCapMoving = 200;
// Compiled for shared code below, but the Topo cohort is OpenGL-only and
// therefore never selects these aliases on Metal/iOS.
constexpr int32_t klattraDefaultTopoDrapeNearSize = klattraDefaultDrapeNearSize;
constexpr int32_t klattraDefaultTopoDrapeMidSize = klattraDefaultDrapeMidSize;
constexpr int32_t klattraDefaultTopoDrapeFarSize = klattraDefaultDrapeFarSize;
constexpr uint32_t klattraDefaultTopoDrapeNearTiles = klattraDefaultDrapeNearTiles;
constexpr uint32_t klattraDefaultTopoDrapeMidTiles = klattraDefaultDrapeMidTiles;
#endif

int32_t klattraEnvTargetSize(const char* name, int32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<int32_t>(std::clamp<long>(parsed, 256, 4096));
}

uint32_t klattraEnvTileCount(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 0, 512));
}

uint32_t klattraDrapePressureTotalCap() {
    static const uint32_t cap = static_cast<uint32_t>(
        cover_hold::countOverride(std::getenv("KLATTRA_DRAPE_PRESSURE_TOTAL_CAP"), 128, 512));
    return cap;
}

uint32_t klattraEnvFrameCount(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 0, 120));
}

bool klattraPacked565DrapeEnabled() {
    // The simulator cannot exercise B5G6R5Unorm (it maps the format to RGBA8),
    // but the packed mid/far targets were visually and memory accepted on
    // physical devices in build 134. Keep the old disable switch as an
    // overriding rollback and continue to honor ENABLE=0 for controlled A/Bs.
    static const bool enabled = [] {
        if (std::getenv("KLATTRA_DISABLE_DRAPE_565") != nullptr) {
            return false;
        }
        return cover_hold::booleanOverride(std::getenv("KLATTRA_ENABLE_DRAPE_565"), true);
    }();
    return enabled;
}

uint32_t klattraEnvLevelCount(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 0, 4));
}

uint32_t klattraEnvTilePadding(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 0, 2));
}

double klattraEnvSeconds(const char* name, double fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value) return fallback;
    return std::clamp(parsed, 0.0, 10.0);
}

bool klattraLookaheadDisabled() {
    static const bool disabled = std::getenv("KLATTRA_DISABLE_LOOKAHEAD") != nullptr;
    return disabled;
}

void klattraAddDrapeOverscan(std::unordered_set<OverscaledTileID>& drapeIDs,
                             const std::unordered_set<OverscaledTileID>& idealIDs,
                             uint32_t padding) {
    if (padding == 0 || idealIDs.empty()) {
        return;
    }

    const int32_t pad = static_cast<int32_t>(padding);
    const std::size_t ringArea = static_cast<std::size_t>((pad * 2 + 1) * (pad * 2 + 1) - 1);
    drapeIDs.reserve(drapeIDs.size() + idealIDs.size() * ringArea);

    for (const auto& tileID : idealIDs) {
        const auto z = tileID.canonical.z;
        const auto worldSize = int64_t{1} << z;
        const auto baseX = static_cast<int64_t>(tileID.wrap) * worldSize + static_cast<int64_t>(tileID.canonical.x);
        const auto baseY = static_cast<int64_t>(tileID.canonical.y);

        for (int32_t dy = -pad; dy <= pad; ++dy) {
            const auto y = baseY + dy;
            if (y < 0 || y >= worldSize) {
                continue;
            }
            for (int32_t dx = -pad; dx <= pad; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const UnwrappedTileID neighbor(z, baseX + dx, y);
                drapeIDs.emplace(tileID.overscaledZ, neighbor.wrap, neighbor.canonical);
            }
        }
    }
}

} // namespace

bool RenderTerrain::stageDiagEnabled() {
    // The instrumentation maintains per-tile maps and performs renderer-wide
    // scans. Keep it out of normal builds and RAM measurements unless a
    // diagnostic launch explicitly opts in.
    static const bool enabled = [] {
        const char* v = std::getenv("KLATTRA_STAGEDIAG");
        return v && *v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    return enabled;
}

void RenderTerrain::diagNoteRasterOverlap(const OverscaledTileID& drapeID, uint8_t rasterZ, bool paintable) const {
    if (!stageDiagEnabled()) return;
    const auto it = drapeStageByTile.find(drapeID);
    if (it == drapeStageByTile.end()) return; // only track canvases we saw created
    auto& e = it->second;
    const auto now = std::chrono::steady_clock::now();
    if (e.rasterOverlap == std::chrono::steady_clock::time_point{}) e.rasterOverlap = now;
    if (paintable) {
        e.bestAvailZ = std::max(e.bestAvailZ, rasterZ);
        if (e.rasterAvail == std::chrono::steady_clock::time_point{}) {
            e.rasterAvail = now;
            klattraStagePush(stageAvailMs, klattraStageMs(e.created, now));
        }
    }
}

void RenderTerrain::diagNoteRasterRouted(const OverscaledTileID& drapeID, uint8_t rasterZ) const {
    if (!stageDiagEnabled()) return;
    const auto it = drapeStageByTile.find(drapeID);
    if (it == drapeStageByTile.end()) return;
    auto& e = it->second;
    if (e.rasterRouted == std::chrono::steady_clock::time_point{}) {
        const auto now = std::chrono::steady_clock::now();
        e.rasterRouted = now;
        e.routedFromZ = rasterZ;
        const auto from = e.rasterAvail != std::chrono::steady_clock::time_point{} ? e.rasterAvail : e.created;
        klattraStagePush(stageRouteMs, klattraStageMs(from, now));
    }
}

void RenderTerrain::diagNoteRasterRevoked(const OverscaledTileID& drapeID, std::size_t removed) const {
    if (!stageDiagEnabled() || removed == 0) return;
    stageRevokeEvents++;
    const auto it = drapeStageByTile.find(drapeID);
    if (it != drapeStageByTile.end()) {
        it->second.revokes++;
    }
}

void RenderTerrain::noteDrapeSourceCoverage(const std::string& sourceID, const RenderTiles& sourceTiles) {
    if (sourceID.empty() || !drapeSurfaceFrame) return;

    std::vector<OverscaledTileID> completeTileIDs;
    if (sourceTiles) {
        completeTileIDs.reserve(sourceTiles->size());
        for (const auto& tileRef : *sourceTiles) {
            const auto& tile = tileRef.get();
            if (tile.getTile().isComplete()) {
                completeTileIDs.push_back(tile.getOverscaledTileID());
            }
        }
    }

    noteDrapeSourceTileCoverage(sourceID, completeTileIDs);
}

void RenderTerrain::declareDrapeSourceRequirement(const std::string& sourceKey, const RenderTiles& sourceTiles) {
    if (sourceKey.empty() || !drapeSurfaceFrame) return;

    std::vector<OverscaledTileID> sourceTileIDs;
    if (sourceTiles) {
        sourceTileIDs.reserve(sourceTiles->size());
        for (const auto& tileRef : *sourceTiles) {
            sourceTileIDs.push_back(tileRef.get().getOverscaledTileID());
        }
    }

    std::size_t targetCount = 0;
    std::size_t declaredTargetCount = 0;
    drapeCache.visitAll([&](const OverscaledTileID& targetID, const TerrainDrapeTargetPtr& target) {
        if (!target) return;
        ++targetCount;
        const bool intersects = std::any_of(sourceTileIDs.begin(), sourceTileIDs.end(), [&](const auto& sourceTileID) {
            return sourceTileID.wrap == targetID.wrap && LayerTweaker::tilesOverlap(sourceTileID, targetID);
        });
        if (intersects) {
            target->declareDrapeSourceRequirement(drapeSurfaceFrame, sourceKey);
            ++declaredTargetCount;
        }
    });

    // The failed Android candidate inferred obligations only from already
    // present source tiles. This diagnostic names the otherwise invisible
    // targets: they can publish hillshade/background without any official
    // Topo land-fill requirement. Restrict output to the representative
    // land-cover layers so the device log remains readable.
    const bool representativeTopoLayer = sourceKey.find("/land-open") != std::string::npos ||
                                         sourceKey.find("/land-aker") != std::string::npos ||
                                         sourceKey.find("/skog-") != std::string::npos;
    if (stageDiagEnabled() && representativeTopoLayer) {
        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastEmitBySource;
        auto& lastEmit = lastEmitBySource[sourceKey];
        const auto now = std::chrono::steady_clock::now();
        if (now - lastEmit >= std::chrono::seconds(1)) {
            lastEmit = now;
            std::array<uint32_t, 26> sourceZooms{};
            for (const auto& sourceTileID : sourceTileIDs) {
                if (sourceTileID.canonical.z < sourceZooms.size()) sourceZooms[sourceTileID.canonical.z]++;
            }
            klattraDumpEmit("[KLATTRA TOPO_OBLIGATION] key=" + sourceKey +
                            " sourceTiles=" + std::to_string(sourceTileIDs.size()) +
                            " targets=" + std::to_string(targetCount) +
                            " declared=" + std::to_string(declaredTargetCount) +
                            " invisible=" + std::to_string(targetCount - declaredTargetCount) +
                            " sourceZ=" + klattraZoomHistogramString(sourceZooms));
        }
    }
}

void RenderTerrain::noteDrapeSourceTileCoverage(const std::string& sourceID,
                                                const std::vector<OverscaledTileID>& completeTileIDs) {
    if (sourceID.empty() || !drapeSurfaceFrame) return;
    drapeCache.visitAll([&](const OverscaledTileID& targetID, const TerrainDrapeTargetPtr& target) {
        if (!target) return;
        target->noteDrapeSourceCoverage(
            drapeSurfaceFrame, sourceID, raster_drape::coversTargetAtAnyResolution(targetID, completeTileIDs));
    });
}

void RenderTerrain::noteDrapeSourceTargetReadiness(const std::string& sourceID,
                                                   const std::vector<OverscaledTileID>& readyTargetIDs) {
    if (sourceID.empty() || !drapeSurfaceFrame) return;
    drapeCache.visitAll([&](const OverscaledTileID& targetID, const TerrainDrapeTargetPtr& target) {
        if (!target) return;
        target->noteDrapeSourceCoverage(
            drapeSurfaceFrame, sourceID, raster_drape::containsExactTarget(targetID, readyTargetIDs));
    });
}

RenderTerrain::RenderTerrain(Immutable<style::Terrain::Impl> impl_)
    : impl(std::move(impl_)), elevationRevision(nextTerrainElevationRevision()) {}

RenderTerrain::~RenderTerrain() = default;

void RenderTerrain::update(const UpdateParameters& /*parameters*/) {
    // Find the DEM source if we haven't already
    if (!demSource && !impl->sourceID.empty()) {
        // In a full implementation, we would look up the source from parameters.sources
        // and cache the RenderSource pointer
        // For now, this is a placeholder
    }
}

void RenderTerrain::update(RenderOrchestrator& orchestrator,
                           gfx::ShaderRegistry& shaders,
                           gfx::Context& context,
                           const TransformState& state,
                           const std::shared_ptr<UpdateParameters>& updateParameters,
                           const RenderTree& renderTree,
                           UniqueChangeRequestVec& changes) {
    // Re-resolve the DEM source every update. The orchestrator owns the
    // RenderSources and can destroy them behind us (source removal,
    // RenderOrchestrator::clearData on style swap) while this RenderTerrain
    // survives — Terrain::Impl compares equal across styles sharing
    // sourceID + exaggeration — so a pointer cached across frames can
    // dangle (same UAF class as the PMTilesFileSource teardown bug). One
    // map lookup per frame is cheap; a vanished source then routes through
    // the clearRenderState path below, which also releases GPU targets.
    demSource = impl->sourceID.empty() ? nullptr : orchestrator.getRenderSource(impl->sourceID);
    if (!demSource && !impl->sourceID.empty()) {
        Log::Warning(Event::Render, "Terrain could not find DEM source: " + impl->sourceID);
        if (klattraTraceStderr()) {
            klattraTrace("terrain no-dem-source source=" + impl->sourceID);
        }
    }

    // Create layer group if we don't have one
    if (!layerGroup) {
        if (auto layerGroup_ = context.createLayerGroup(TERRAIN_LAYER_INDEX, /*initialCapacity=*/1, "terrain")) {
            layerGroup = std::move(layerGroup_);
            activateLayerGroup(true, changes);
        } else {
            Log::Error(Event::Render, "Failed to create terrain layer group");
            return;
        }
    }

    if (!tweaker) {
        tweaker = std::make_unique<TerrainLayerTweaker>(this);
    }

    const bool traceDrape = klattraLogDrapeTrace();
    static uint64_t drapeTraceFrameCounter = 0;
    const uint64_t drapeTraceFrame = traceDrape ? ++drapeTraceFrameCounter : 0;

    // If we don't have a DEM source, we can't create terrain drawables
    if (!demSource) {
        if (klattraTraceStderr()) {
            klattraTrace("terrain update-no-dem source=" + impl->sourceID);
        }
        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] update-no-dem frame=" + std::to_string(drapeTraceFrame) +
                          " source=" + impl->sourceID);
        }
        clearRenderState(changes);
        return;
    }

    auto renderTiles = demSource->getRawRenderTiles();
    if (renderTiles->empty()) {
        if (klattraTraceStderr()) {
            klattraTrace("terrain update-empty-cover source=" + impl->sourceID +
                         " previousBindings=" + std::to_string(currentBindings.size()) +
                         " previousDrapeTargets=" + std::to_string(drapeCache.size()));
        }
        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] update-empty-cover frame=" + std::to_string(drapeTraceFrame) +
                          " source=" + impl->sourceID + " previousBindings=" + std::to_string(currentBindings.size()) +
                          " previousDrapeTargets=" + std::to_string(drapeCache.size()));
        }
        clearRenderState(changes);
        return;
    }

    auto* lg = static_cast<LayerGroup*>(layerGroup.get());
    if (!lg) {
        return;
    }

    // A target's first publication happens after the preceding terrain
    // update. Reaching this update completes its one-frame producer ->
    // consumer handoff, including for retained/overscan targets that are not
    // in today's exact cover. Visible targets can create their mesh drawables
    // below; off-cover targets need no consumer until a later normal update.
    drapeCache.visitAll([](const OverscaledTileID&, const TerrainDrapeTargetPtr& target) {
        if (target && target->hasPublishedDrapeSurface()) {
            target->acknowledgeFirstDrapePublication();
        }
    });

    if (traceDrape) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] update-begin frame=" + std::to_string(drapeTraceFrame) +
                      " source=" + impl->sourceID + " renderTiles=" + std::to_string(renderTiles->size()) +
                      " previousBindings=" + std::to_string(currentBindings.size()) +
                      " previousDemTextures=" + std::to_string(demTexturesByTile.size()) +
                      " previousDrapeTargets=" + std::to_string(drapeCache.size()) +
                      " terrainDrawables=" + std::to_string(lg->getDrawableCount()));
    }
    if (klattraTraceStderr()) {
        klattraTrace("terrain update-begin source=" + impl->sourceID +
                     " renderTiles=" + std::to_string(renderTiles->size()) +
                     " previousBindings=" + std::to_string(currentBindings.size()) +
                     " previousDemTextures=" + std::to_string(demTexturesByTile.size()) + " previousDrapeTargets=" +
                     std::to_string(drapeCache.size()) + " terrainDrawables=" + std::to_string(lg->getDrawableCount()));
    }

    // The raw render set intentionally contains overlapping IDs: available
    // children, a parent fallback for missing siblings, stock fade-held tiles,
    // and Traska's fragmented-cover hold. Flat raster/DEM drawing consumes the
    // TileMask generated for that set. Terrain used to discard the mask and
    // build a full displaced surface for every raw key, which is the broad
    // green/coarse island defect measured in .67.
    std::unordered_set<OverscaledTileID> rawIdealIDs;
    rawIdealIDs.reserve(renderTiles->size());
    std::map<UnwrappedTileID, KlattraTerrainMaskEntry> terrainMasks;
    for (const auto& renderTile : *renderTiles) {
        rawIdealIDs.emplace(renderTile.id.canonical.z, renderTile.id.wrap, renderTile.id.canonical);
        terrainMasks.try_emplace(renderTile.id);
    }
    algorithm::updateTileMasks(terrainMasks);

    // Expand every uncovered relative mask cell into an absolute ideal mesh.
    // The resulting leaves exactly partition the union of the raw render set:
    // no parent/child surfaces overlap, while a held parent still contributes
    // geometry in every child quadrant the replacement cover has not supplied.
    std::unordered_set<OverscaledTileID> currentIdealIDs;
    currentIdealIDs.reserve(renderTiles->size());
    std::size_t terrainMaskRoots = 0;
    std::size_t terrainMaskLeaves = 0;
    uint8_t terrainMaskMaxDepth = 0;
    for (const auto& [baseID, entry] : terrainMasks) {
        if (entry.mask.empty()) {
            continue; // this raw root is completely covered by descendants
        }
        ++terrainMaskRoots;
        for (const auto& relative : entry.mask) {
            currentIdealIDs.insert(klattraAbsoluteMaskLeaf(baseID, relative));
            ++terrainMaskLeaves;
            terrainMaskMaxDepth = std::max(terrainMaskMaxDepth, relative.z);
        }
    }

    // Terrain meshes stay keyed by the disjoint fine DEM leaves. Android Topo
    // keeps complete sibling groups at its configured detail zoom around the
    // camera, then collapses whole far groups by one overview level. Every
    // fine descendant mesh
    // samples the matching UV sub-rectangle from that shared canvas. This
    // keeps official detail where it is legible without decoding and fanning
    // the dense vector cohort across the entire pitched horizon. Defaults
    // remain z10/z9; the launch-scoped diagnostic can select z11/z10 or z12/z11.
    // Satellite and Metal retain the fine target topology.
    const auto topoDrapeZoomPolicy = topo_vector::drapeZoomPolicy();
#if MLN_RENDER_BACKEND_OPENGL
    const bool useSharedTopoDrapeCohort = klattraUseSharedTopoDrapeCohort(renderTree);
#else
    const bool useSharedTopoDrapeCohort = false;
#endif
    const LatLng cameraCenter = state.getLatLng();
    const double centerX = (cameraCenter.longitude() + 180.0) / 360.0;
    const double drapeCenterLatRad = cameraCenter.latitude() * M_PI / 180.0;
    const double centerY = 0.5 - std::log(std::tan(M_PI / 4.0 + drapeCenterLatRad / 2.0)) / (2.0 * M_PI);

    std::size_t finlandSurfaceSplits = 0;
    std::size_t finlandSplitBudget = 0;
    topo_vector::FinnishSurfaceDetail finlandSurfaceDetail;
    std::unordered_set<OverscaledTileID> exactDrapeIDs;
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
    bool hasRenderedFinnishBase = false;
    bool hasRenderedOfficialTopo = false;
    for (const auto& item : renderTree.getLayerRenderItemMap()) {
        if (!item.source) continue;
        const auto sourceTiles = item.source->getRenderTiles();
        if (!sourceTiles || sourceTiles->empty()) continue;
        const auto& sourceID = item.source->baseImpl->id;
        hasRenderedFinnishBase = hasRenderedFinnishBase || sourceID == "finland-openmaptiles";
        hasRenderedOfficialTopo = hasRenderedOfficialTopo || topo_vector::isOfficialSourceID(sourceID);
    }
    const auto cameraDetailZoom = static_cast<uint8_t>(std::clamp(
        util::coveringZoomLevel(state.getZoom(), style::SourceType::Vector, util::tileSize_I), 0, 25));
    static const uint32_t closeNearTextureSize = static_cast<uint32_t>(
        klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_NEAR", klattraDefaultDrapeNearSize));
    if (topo_vector::useFinnishCloseSurface(true,
                                          hasRenderedFinnishBase,
                                          hasRenderedOfficialTopo,
                                          useSharedTopoDrapeCohort,
                                          cameraDetailZoom,
                                          demSource->getMaxZoom(),
                                          closeNearTextureSize,
                                          util::tileSize_I)) {
        // Keep every ordinary visible camera footprint when selecting colour
        // detail. Truncating this selector to the eight near texture slots can
        // omit foreground tiles and also allow their donor groups to coarsen.
        // The elevation-expanded DEM cover still owns geometry, and the same
        // bounded split budget exchanges near children with safe far quartets.
        util::TileCoverParameters detailParameters{state};
        detailParameters.tileLodMinZoom = cameraDetailZoom;
        detailParameters.tileCoverMaxTiles = 0;
        const auto detailTiles = util::tileCover(detailParameters,
                                                 cameraDetailZoom,
                                                 Range<uint8_t>{cameraDetailZoom, cameraDetailZoom});
        finlandSplitBudget = topo_vector::finnishCloseSurfaceSplitBudget;
        auto refined = topo_vector::refineFinnishSurfaceCover(currentIdealIDs,
                                                               detailTiles,
                                                               demSource->getMaxZoom(),
                                                               finlandSplitBudget,
                                                               centerX,
                                                               centerY);
        finlandSurfaceSplits = refined.splitCount;
        if (stageDiagEnabled()) {
            finlandSurfaceDetail = topo_vector::inspectFinnishSurfaceDetail(refined.meshLeaves, detailTiles);
        }
        currentIdealIDs = std::move(refined.meshLeaves);
        exactDrapeIDs = std::move(refined.drapeLeaves);
    }
#endif
    if (useSharedTopoDrapeCohort) {
        static const uint32_t topoDetailTargets =
            klattraEnvTileCount("KLATTRA_TOPO_DETAIL_TARGETS", 32);
        const auto cohort = topo_vector::distanceLimitedCohortForTerrainLeaves(currentIdealIDs,
                                                                                topoDrapeZoomPolicy.detailZoom,
                                                                                topoDrapeZoomPolicy.overviewZoom,
                                                                                topoDetailTargets,
                                                                                centerX,
                                                                                centerY);
        exactDrapeIDs.insert(cohort.begin(), cohort.end());
    } else if (exactDrapeIDs.empty()) {
        exactDrapeIDs = currentIdealIDs;
    }

    // Topo and Satellite intentionally share the same DEM definition, so the
    // RenderTerrain object survives their style swap. Their surface target
    // topology must not: a z10/z11 Topo cohort target can share a key with a
    // Satellite fallback and otherwise be reused with stale pixels/groups.
    // Retire a cross-mode generation on its own frame. Keeping the published
    // old-mode bindings while allocating the replacement cohort made both full
    // texture generations resident together; on Goldfish that transient was
    // enough to kill the process. Cross-style pixels are not a valid continuity
    // surface, so expose the ordinary flat new-style pass for this one frame,
    // let end-of-frame GL cleanup retire the old targets, and mint the new
    // cohort on the single follow-up requested below. Same-mode cover changes
    // retain the atomic previous-cover hold later in this function.
    if (lastSharedTopoDrapeCohort && *lastSharedTopoDrapeCohort != useSharedTopoDrapeCohort) {
        lg->removeDrawablesIf([](gfx::Drawable&) { return true; });
        if (!currentBindings.empty()) elevationRevision = nextTerrainElevationRevision();
        currentBindings.clear();

        auto staleModeTargets = drapeCache.pruneIf([](const OverscaledTileID&) { return true; });
        for (auto& [tileID, target] : staleModeTargets) {
            (void)tileID;
            klattraReleaseRetiredDrapeBuildState(target);
            changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(target)));
        }

        retiredDrapeTargetsByTile.clear();
        drapeRingByTile.clear();
        pruneHoldAgeByTile.clear();
        // Sources update before terrain. Publish both the new mesh cover and
        // its exact adaptive drape cohort now so the following frame's
        // official Topo source requests every replacement target before those
        // targets are allocated. Clearing either set can strand the map on the
        // flat transition pass.
        previousIdealIDs = currentIdealIDs;
        previousSourceDrapeIDs = exactDrapeIDs;
        stableDrapeCoverFrames = 0;
        lastSharedTopoDrapeCohort = useSharedTopoDrapeCohort;
        drapeWorkPending = true;
        return;
    }
    lastSharedTopoDrapeCohort = useSharedTopoDrapeCohort;

    // Exact drape targets give the final sharp topo texture. Coarser parent
    // targets give fast-moving cameras something stable to sample while new
    // exact targets bake. This mirrors the render-to-texture tile pyramid in
    // mature terrain renderers: show a cached parent immediately, sharpen to
    // the child only after it has completed.
    std::unordered_set<OverscaledTileID> currentDrapeIDs = exactDrapeIDs;
    // Default 1: bake a ring of targets just outside the visible cover so a
    // moving camera reaches tiles whose drape is already ready. Ring targets
    // rank as far in the distance-ranked budgets, so they stay at the small
    // end of the size buckets. (2 was tried on-device 2026-07-04: no visible
    // gain over 1 — the persistent edge artifacts turned out to be
    // cover-limit voids, not bake latency — while the extra ring showed up
    // as compressor pressure. See the DEM LOD pitch gate in
    // render_raster_dem_source.cpp for the actual cover fix.)
    static const uint32_t normalDrapeOverscanTiles =
        klattraEnvTilePadding("KLATTRA_DRAPE_OVERSCAN_TILES", 1);
    static const uint32_t topoDrapeOverscanTiles =
        klattraEnvTilePadding("KLATTRA_TOPO_DRAPE_OVERSCAN_TILES", 0);
    const uint32_t drapeOverscanTiles =
        useSharedTopoDrapeCohort ? topoDrapeOverscanTiles : normalDrapeOverscanTiles;
    klattraAddDrapeOverscan(currentDrapeIDs, exactDrapeIDs, drapeOverscanTiles);
    const std::vector<OverscaledTileID> exactAndOverscanDrapeIDs(currentDrapeIDs.begin(), currentDrapeIDs.end());
    // Raw source IDs are allowed to overlap: parent fallback, stock fade
    // retention, and the cover-fragment hold all need those ancestors. The
    // terrain geometry is partitioned into masked leaves before drape
    // allocation, with an atomic previous-cover hold below if any new leaf is
    // unbound. Keep this raw count as a diagnostic; it may stay non-zero while
    // the post-mask mesh overlap must be zero.
    const bool structuralDiagnostics = stageDiagEnabled() || klattraFlyDiag() || klattraTraceStderr();
    std::size_t rawSourceOverlapPairs = 0;
    if (structuralDiagnostics) {
        for (auto a = rawIdealIDs.begin(); a != rawIdealIDs.end(); ++a) {
            for (auto b = std::next(a); b != rawIdealIDs.end(); ++b) {
                if (klattraIsAncestorOf(*a, *b) || klattraIsAncestorOf(*b, *a)) {
                    ++rawSourceOverlapPairs;
                }
            }
        }
    }
    std::unordered_set<OverscaledTileID> terrainMeshIDs = currentIdealIDs;
    std::size_t sourceMeshOverlapPairs = 0;
    if (structuralDiagnostics) {
        for (auto a = terrainMeshIDs.begin(); a != terrainMeshIDs.end(); ++a) {
            for (auto b = std::next(a); b != terrainMeshIDs.end(); ++b) {
                if (klattraIsAncestorOf(*a, *b) || klattraIsAncestorOf(*b, *a)) {
                    ++sourceMeshOverlapPairs;
                }
            }
        }
    }
    // Satellite keeps one parent fallback level to bridge raster streaming.
    // The adaptive Topo cohort already contains explicit far overview parents
    // and publishes atomically, so adding another complete parent generation
    // retains vector build graphs without covering a new visible slot.
    static const uint32_t normalDrapeFallbackLevels =
        klattraEnvLevelCount("KLATTRA_DRAPE_FALLBACK_LEVELS", 1);
    static const uint32_t topoDrapeFallbackLevels =
        klattraEnvLevelCount("KLATTRA_TOPO_DRAPE_FALLBACK_LEVELS", 0);
    const uint32_t drapeFallbackLevels =
        useSharedTopoDrapeCohort ? topoDrapeFallbackLevels : normalDrapeFallbackLevels;
    if (drapeFallbackLevels > 0) {
        for (const auto& tileID : exactAndOverscanDrapeIDs) {
            for (uint32_t level = 1; level <= drapeFallbackLevels; ++level) {
                if (tileID.canonical.z < level) {
                    break;
                }
                currentDrapeIDs.insert(tileID.scaledTo(static_cast<uint8_t>(tileID.canonical.z - level)));
            }
        }
    }

    // .51 velocity-biased drape lookahead: a canvas is created only when its
    // tile ENTERS the cover, then needs routing + a bake before it shows real
    // imagery. At flight speed that latency lands on screen as the leading-
    // edge smear band (ancestor-fallback bindings) and background-only
    // plates (measured on the cabled .48 flight: 30-45 canvases perpetually
    // unready with all tile data local — fetch exonerated). Estimate the
    // camera's ground velocity from the recent update history and pre-insert
    // a forward strip of ideal-zoom drape tiles along the projected path so
    // they exist-and-bake BEFORE their tiles become visible. Velocity-based,
    // not path-based: works for any sustained motion — flyover, inertial pan
    // — and idles to a no-op via the crossing-rate gate below.
    double drapeProjX = centerX;
    double drapeProjY = centerY;
    uint32_t lookaheadStripAdded = 0;
    double lookaheadSpanTiles = 0.0;
    uint32_t activeDrapePopulationCap = 0;
    std::size_t drapePopulationCapEvicted = 0;
    {
        const auto nowTime = std::chrono::steady_clock::now();
        drapeCameraSamples.push_back({centerX, centerY, nowTime});
        constexpr std::size_t velocityWindowUpdates = 10;
        while (drapeCameraSamples.size() > velocityWindowUpdates) {
            drapeCameraSamples.pop_front();
        }
        double velX = 0.0;
        double velY = 0.0;
        if (drapeCameraSamples.size() >= 2) {
            const auto& oldest = drapeCameraSamples.front();
            const double dt = std::chrono::duration<double>(nowTime - oldest.time).count();
            // A long span means renders were sparse (idle map) — the window
            // says nothing about current motion, so treat it as standstill.
            if (dt > 0.0 && dt < 2.0) {
                velX = (centerX - oldest.x) / dt;
                velY = (centerY - oldest.y) / dt;
            }
        }
        // DEFAULT 0 = dormant (.52). Robert's first .51 flyover jetsammed on
        // the first frames: the tour's initial fly-in is fast sustained
        // translation, so the gate opens there — and segment-ranked strip
        // tiles are born NEAR-ring (2048² +mips ≈ 21 MB each) instead of the
        // far-tier 512² the .47 cap's byte budget implicitly assumed, while
        // gate flap re-ranks rings en masse (resize churn + parked retired
        // targets). The count cap held; the BYTES didn't. Opt back in via
        // env for pan experiments only after create-size clamping and gate
        // hysteresis exist. With 0 the strip, projection, and segment
        // ranking are all inert — measured bit-for-bit .50 behaviour.
        static const double lookaheadSeconds = klattraEnvSeconds("KLATTRA_DRAPE_LOOKAHEAD_S", 0.0);
        if (!klattraLookaheadDisabled() && lookaheadSeconds > 0.0 && !exactDrapeIDs.empty()) {
            // The strip bakes at the finest LOD present in the cover — the
            // zoom tiles enter at on the leading edge, where the smear lives.
            uint8_t stripZoom = 0;
            for (const auto& idealID : exactDrapeIDs) {
                stripZoom = std::max(stripZoom, idealID.canonical.z);
            }
            const auto worldSizeI = int64_t{1} << stripZoom;
            const double worldTiles = static_cast<double>(worldSizeI);
            const double speed = std::hypot(velX, velY); // mercator units/s
            const double aheadTilesExact = speed * lookaheadSeconds * worldTiles;
            // Sustained-motion gate: only project when the camera will cross
            // into new tile territory within the lookahead horizon. Slow
            // gestures and idle stay exactly on the pre-.51 path. The cap
            // bounds canvas minting under a violent fling.
            static const double maxAheadTiles = static_cast<double>(
                klattraEnvTileCount("KLATTRA_DRAPE_LOOKAHEAD_MAX_TILES", 6));
            if (aheadTilesExact >= 0.75 && maxAheadTiles > 0.0) {
                const double aheadTiles = std::min(aheadTilesExact, maxAheadTiles);
                lookaheadSpanTiles = aheadTiles;
                const double dirX = velX / speed;
                const double dirY = velY / speed;
                drapeProjX = centerX + dirX * aheadTiles / worldTiles;
                drapeProjY = centerY + dirY * aheadTiles / worldTiles;
                const double perpX = -dirY;
                const double perpY = dirX;
                // Three lanes (centre, ±1 tile lateral), sampled every half
                // tile along the corridor so no tile the path crosses is
                // skipped between samples.
                for (double s = 0.0; s <= aheadTiles + 1e-9; s += 0.5) {
                    for (int32_t lane = -1; lane <= 1; ++lane) {
                        const double px = centerX + (dirX * s + perpX * lane) / worldTiles;
                        const double py = centerY + (dirY * s + perpY * lane) / worldTiles;
                        const auto uy = static_cast<int64_t>(std::floor(py * worldTiles));
                        if (uy < 0 || uy >= worldSizeI) {
                            continue;
                        }
                        const auto ux = static_cast<int64_t>(std::floor(px * worldTiles));
                        const UnwrappedTileID aheadTile(stripZoom, ux, uy);
                        if (currentDrapeIDs.emplace(stripZoom, aheadTile.wrap, aheadTile.canonical).second) {
                            lookaheadStripAdded++;
                        }
                    }
                }
            }
        }
    }

    // Pre-bake variants measured and REJECTED on-sim 2026-07-12 — do not
    // rebuild without new evidence (see the .51 handover):
    //  - LOD-boundary pre-child (mint maxZ children of next-coarser ideals):
    //    every candidate child is already in currentDrapeIDs via the ideal /
    //    overscan structure — the ideal set interleaves z11 and z12 slots
    //    over the same ground, so there is no boundary to lead. prechild
    //    counter stayed 0 across 40+ in-flight samples with the reach limit
    //    at 16 tiles.
    //  - Pending-DEM pre-mint (canvases for requested-not-yet-renderable DEM
    //    tiles): RenderTile.id is already the IDEAL id while the actual tile
    //    is a fallback parent, so pending slots already have canvases; the
    //    lead time equals what the overscan ring provides (~7 s at tour
    //    speed) and the `.48` deficit is a route+bake THROUGHPUT ceiling,
    //    not a lead-time gap (constant 30-45 backlog; `.50b` fetch warmer
    //    "no difference"; 2026-07-04 overscan-2 "no visible gain").

    const bool drapeCoverStable = currentIdealIDs == previousIdealIDs;
    const bool cameraChanging = state.isChanging() || state.isGestureInProgress();
    if (!cameraChanging && drapeCoverStable) {
        stableDrapeCoverFrames++;
    } else {
        stableDrapeCoverFrames = 0;
    }
    previousIdealIDs = currentIdealIDs;
    previousSourceDrapeIDs = exactDrapeIDs;

    static const uint32_t qualityUpgradeFrames = klattraEnvFrameCount("KLATTRA_DRAPE_QUALITY_UPGRADE_FRAMES", 8);
    const bool useHighQualityDrape = !cameraChanging && stableDrapeCoverFrames >= qualityUpgradeFrames;

    // A published terrain drawable samples the exact drape texture recorded
    // in currentBindings.  The drawable keeps that texture allocation alive,
    // but not the RenderTarget that owns its layer groups / publication
    // generation.  If the target is pruned first, an atomic previous-cover
    // hold degrades into an orphaned texture that can neither be inspected nor
    // used as a coherent predecessor.  Track the published texture identities
    // so every prune/resize path below retains the owning target until the
    // binding is replaced at the end of a later update.
    std::unordered_set<const gfx::Texture2D*> publishedDrapeTextures;
    publishedDrapeTextures.reserve(currentBindings.size());
    for (const auto& [idealID, binding] : currentBindings) {
        (void)idealID;
        if (binding.drapeTexture) {
            publishedDrapeTextures.insert(binding.drapeTexture.get());
        }
    }
    const auto targetOwnsPublishedTexture = [&](const TerrainDrapeTargetPtr& target) {
        return target && target->getTexture() &&
               publishedDrapeTextures.find(target->getTexture().get()) != publishedDrapeTextures.end();
    };
    std::size_t frozenPublishedTargetsThisUpdate = 0;

    if (traceDrape) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] target-quality frame=" + std::to_string(drapeTraceFrame) + " cameraChanging=" +
                      std::to_string(cameraChanging) + " coverStable=" + std::to_string(drapeCoverStable) +
                      " stableFrames=" + std::to_string(stableDrapeCoverFrames) + " fallbackLevels=" +
                      std::to_string(drapeFallbackLevels) + " overscanTiles=" + std::to_string(drapeOverscanTiles) +
                      " topoShared=" + std::to_string(useSharedTopoDrapeCohort) +
                      " terrainMeshes=" + std::to_string(currentIdealIDs.size()) +
                      " exactDrapeTargets=" + std::to_string(exactAndOverscanDrapeIDs.size()) + " activeDrapeTargets=" +
                      std::to_string(currentDrapeIDs.size()) + " highQuality=" + std::to_string(useHighQualityDrape));
    }

    // Phase 1 of the drape pass (see FINISH_TERRAIN.md / TERRAIN_PROGRESS.md):
    // ensure a per-tile RenderTarget exists before we create drawables for
    // those tiles, so the drawable-creation step can bind the matching
    // target's texture as its `mapTexture` (Phase 3). Phase 2 — routing
    // 2D layer drawables into these targets so they have real surface
    // content — is the next step.
    // Allocate a per-ideal-tile drape RenderTarget for each visible cover
    // slot if one doesn't already exist, and prune any targets whose ideal
    // tiles have dropped out of the cover set. The terrain mesh fragment
    // shader samples this target as the surface colour; basemap layers route
    // drawables into it (RenderBackgroundLayer / RenderFillLayer / ...).
    {
        // Distance-ranked drape budgets: spend texture memory where the user
        // is looking instead of by tile zoom. Every drape tile is ranked by
        // its centre's mercator distance to the camera PATH — the segment
        // from the current centre to the +lookahead projected centre — so
        // under sustained motion the ahead strip ranks like near tiles and
        // the total cap sheds far-BEHIND tiles first. At standstill the
        // segment collapses to the camera centre and ranking is exactly the
        // pre-.51 behaviour. The nearest KLATTRA_DRAPE_NEAR_TILES bake at
        // nearSize even while the camera moves (the old moving/still split
        // made gestures soft, then popped sharp on settle), the next
        // KLATTRA_DRAPE_MID_TILES at midSize, and the rest at farSize.
        // Worst-case GPU memory is bounded by the ring counts regardless of
        // zoom or pitch — the per-zoom-bucket budgets this replaces let a
        // pitched 72-tile cover jetsam the app when the z10 bucket was
        // raised to 2048 (measured 3.3 GB on an iPhone 16 Pro).
        const double segX = drapeProjX - centerX;
        const double segY = drapeProjY - centerY;
        const double segLen2 = segX * segX + segY * segY;
        std::vector<std::pair<double, OverscaledTileID>> rankedDrapeIDs;
        rankedDrapeIDs.reserve(currentDrapeIDs.size());
        for (const auto& tileID : currentDrapeIDs) {
            const double scale = static_cast<double>(1u << tileID.canonical.z);
            const double dx = (tileID.canonical.x + 0.5) / scale + tileID.wrap - centerX;
            const double dy = (tileID.canonical.y + 0.5) / scale - centerY;
            double rx = dx;
            double ry = dy;
            if (segLen2 > 0.0) {
                const double t = std::clamp((dx * segX + dy * segY) / segLen2, 0.0, 1.0);
                rx = dx - segX * t;
                ry = dy - segY * t;
            }
            rankedDrapeIDs.emplace_back(rx * rx + ry * ry, tileID);
        }
        std::sort(rankedDrapeIDs.begin(), rankedDrapeIDs.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        // .47 flight fix (device 2026-07-11: 322 live canvases at flyover
        // start = the jetsam): cap the TOTAL drape population by camera
        // rank. Visible-cover (ideal) tiles are never dropped — the cap
        // sheds the farthest overscan/ancestor extras first, and tightens
        // while the camera is in motion, when a sweep would otherwise mint
        // canvases faster than the prune releases them.
        static const uint32_t drapeTotalCapStable =
            klattraEnvTileCount("KLATTRA_DRAPE_TOTAL_CAP", klattraDefaultDrapeTotalCapStable);
        static const uint32_t drapeTotalCapMoving =
            klattraEnvTileCount("KLATTRA_DRAPE_TOTAL_CAP_MOVING", klattraDefaultDrapeTotalCapMoving);
        const uint32_t normalDrapeTotalCap = useHighQualityDrape ? drapeTotalCapStable : drapeTotalCapMoving;
        const bool inheritedDrapePressure = cover_hold::nextDrapePressureCapState(
            drapePressureCapActive, klattraDrapePressureTotalCap(), false, cover_hold::processPressureActive());
        if (!drapePressureCapActive && inheritedDrapePressure && (stageDiagEnabled() || klattraTraceStderr())) {
            klattraDumpEmit("[KLATTRA DRAPE PRESSURE] active=1 reason=shared-highwater cap=" +
                            std::to_string(klattraDrapePressureTotalCap()));
        }
        drapePressureCapActive = inheritedDrapePressure;
        const uint32_t drapeTotalCap = static_cast<uint32_t>(cover_hold::pressureDrapePopulationBudget(
            normalDrapeTotalCap, klattraDrapePressureTotalCap(), drapePressureCapActive, exactDrapeIDs.size()));
        activeDrapePopulationCap = drapeTotalCap;
        if (drapeTotalCap > 0 && rankedDrapeIDs.size() > drapeTotalCap) {
            std::size_t kept = rankedDrapeIDs.size();
            for (std::size_t i = rankedDrapeIDs.size(); i > 0 && kept > drapeTotalCap; --i) {
                const auto& candidate = rankedDrapeIDs[i - 1].second;
                if (exactDrapeIDs.find(candidate) != exactDrapeIDs.end()) {
                    continue; // never drop a required surface canvas
                }
                currentDrapeIDs.erase(candidate);
                kept--;
                drapePopulationCapEvicted++;
            }
            if (kept < rankedDrapeIDs.size()) {
                rankedDrapeIDs.erase(
                    std::remove_if(
                        rankedDrapeIDs.begin(),
                                                    rankedDrapeIDs.end(),
                        [&](const auto& entry) { return currentDrapeIDs.find(entry.second) == currentDrapeIDs.end(); }),
                                     rankedDrapeIDs.end());
            }
        }
        std::unordered_map<OverscaledTileID, uint32_t> drapeRankByTile;
        drapeRankByTile.reserve(rankedDrapeIDs.size());
        for (uint32_t i = 0; i < rankedDrapeIDs.size(); ++i) {
            drapeRankByTile.emplace(rankedDrapeIDs[i].second, i);
        }

        static const int32_t normalNearSize =
            klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_NEAR", klattraDefaultDrapeNearSize);
        static const int32_t normalMidSize =
            klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_MID", klattraDefaultDrapeMidSize);
        static const int32_t normalFarSize =
            klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_FAR", klattraDefaultDrapeFarSize);
        static const int32_t topoNearSize =
            klattraEnvTargetSize("KLATTRA_TOPO_DRAPE_TARGET_SIZE_NEAR", klattraDefaultTopoDrapeNearSize);
        static const int32_t topoMidSize =
            klattraEnvTargetSize("KLATTRA_TOPO_DRAPE_TARGET_SIZE_MID", klattraDefaultTopoDrapeMidSize);
        static const int32_t topoFarSize =
            klattraEnvTargetSize("KLATTRA_TOPO_DRAPE_TARGET_SIZE_FAR", klattraDefaultTopoDrapeFarSize);
        const int32_t nearSize = useSharedTopoDrapeCohort ? topoNearSize : normalNearSize;
        const int32_t midSize = useSharedTopoDrapeCohort ? topoMidSize : normalMidSize;
        const int32_t farSize = useSharedTopoDrapeCohort ? topoFarSize : normalFarSize;
        static const uint32_t normalNearTiles =
            klattraEnvTileCount("KLATTRA_DRAPE_NEAR_TILES", klattraDefaultDrapeNearTiles);
        // .49: 24 -> 48. At flyover pitch the visible mid-screen spans ranks
        // ~30-70, which under the old split wore 512² far-tier canvases —
        // Robert's "blur so bad I can't see the trail". The .47 population
        // cap freed the memory (322 -> ~200 canvases); spend part of it
        // widening the 1024² ring (~+130 MB incl. mipmaps).
        static const uint32_t normalMidTiles =
            klattraEnvTileCount("KLATTRA_DRAPE_MID_TILES", klattraDefaultDrapeMidTiles);
        static const uint32_t topoNearTiles =
            klattraEnvTileCount("KLATTRA_TOPO_DRAPE_NEAR_TILES", klattraDefaultTopoDrapeNearTiles);
        static const uint32_t topoMidTiles =
            klattraEnvTileCount("KLATTRA_TOPO_DRAPE_MID_TILES", klattraDefaultTopoDrapeMidTiles);
        const uint32_t nearTiles = useSharedTopoDrapeCohort ? topoNearTiles : normalNearTiles;
        const uint32_t midTiles = useSharedTopoDrapeCohort ? topoMidTiles : normalMidTiles;
        static const uint32_t ringSlack = klattraEnvTileCount("KLATTRA_DRAPE_RING_SLACK", 6);

        const auto drapeTargetSizeForTile = [&](const OverscaledTileID& tileID) -> int32_t {
            const auto rankIt = drapeRankByTile.find(tileID);
            const uint32_t rank = rankIt != drapeRankByTile.end() ? rankIt->second
                                                                  : std::numeric_limits<uint32_t>::max();
            const uint8_t desiredRing = rank < nearTiles ? 0 : (rank < nearTiles + midTiles ? 1 : 2);
            uint8_t ring = desiredRing;
            const auto ringIt = drapeRingByTile.find(tileID);
            if (ringIt != drapeRingByTile.end() && desiredRing > ringIt->second) {
                // Promote immediately, demote with slack: a tile keeps its
                // ring until its rank falls RING_SLACK past the boundary, so
                // panning does not flap targets at ring edges (every flip
                // reallocates and rebakes a render target).
                const uint32_t boundary = ringIt->second == 0 ? nearTiles : nearTiles + midTiles;
                ring = rank >= boundary + ringSlack ? desiredRing : ringIt->second;
            }
            drapeRingByTile[tileID] = ring;
            if (ring == 0) return nearSize;
            if (ring == 1) return midSize;
            return farSize;
        };

        // A fast pan/zoom can reshuffle every ring assignment in a single
        // update; resizing them all at once keeps old+new generations alive
        // simultaneously (retired copy + fresh allocation per tile) and can
        // transiently spike hundreds of MB above steady state. Cap resizes
        // per frame — skipped tiles keep rendering at their old size and are
        // picked up on following frames (0 = uncapped).
        static const uint32_t maxResizesPerFrame = klattraEnvTileCount("KLATTRA_DRAPE_MAX_RESIZES_PER_FRAME", 3);
        uint32_t resizesThisFrame = 0;
        // .48: per-tile resize cooldown state (see below). Function-static is
        // fine: one terrain instance on the render thread; stale entries are
        // a few bytes per tile and get overwritten on the next resize.
        static uint64_t drapeUpdateCounter = 0;
        ++drapeUpdateCounter;
        static std::unordered_map<OverscaledTileID, uint64_t> lastDrapeResizeUpdate;
        drapeWorkPending = false;
        for (const auto& tileID : currentDrapeIDs) {
            const int32_t targetSize = drapeTargetSizeForTile(tileID);
            const Size desiredSize{static_cast<uint32_t>(targetSize), static_cast<uint32_t>(targetSize)};
            if (auto existing = drapeCache.get(tileID)) {
                const Size existingSize = existing->getSize();
                // Resize both ways: ring promotions sharpen the tile, ring
                // demotions release the big target again — without them, long
                // pans accumulate near-ring targets until jetsam. The retired
                // target keeps rendering until its successor bakes.
                if (existingSize != desiredSize) {
                    // .48 (reworks the .47 stability freeze, which starved
                    // promotions for the whole flight — the cover never
                    // stabilises mid-flight, so tiles created far stayed
                    // far-coarse while filling the screen): a per-tile
                    // cooldown instead. Promotions keep flowing under the
                    // per-frame cap; only rapid re-resize thrash of the SAME
                    // tile (ring-boundary jitter) is damped.
                    static const uint32_t resizeCooldownUpdates = klattraEnvFrameCount("KLATTRA_DRAPE_RESIZE_COOLDOWN",
                                                                                       30);
                    const auto lastResizeIt = lastDrapeResizeUpdate.find(tileID);
                    const bool coolingDown = lastResizeIt != lastDrapeResizeUpdate.end() &&
                                             drapeUpdateCounter - lastResizeIt->second < resizeCooldownUpdates;
                    const auto parkedIt = retiredDrapeTargetsByTile.find(tileID);
                    const bool publishedPredecessorParked =
                        parkedIt != retiredDrapeTargetsByTile.end() && targetOwnsPublishedTexture(parkedIt->second);
                    if (coolingDown || publishedPredecessorParked) {
                        // retiredDrapeTargetsByTile has one slot per tile. Do
                        // not overwrite the predecessor still sampled by the
                        // published cover with another resize generation.
                        drapeWorkPending = true;
                    } else if (maxResizesPerFrame == 0 || resizesThisFrame < maxResizesPerFrame) {
                        resizesThisFrame++;
                        if (stageDiagEnabled()) {
                            stageResizeEvents++;
                        }
                        lastDrapeResizeUpdate[tileID] = drapeUpdateCounter;
                        auto oldTarget = drapeCache.take(tileID);
                        const bool retainOldTarget =
                            klattraDrapeTargetReady(oldTarget) || targetOwnsPublishedTexture(oldTarget);
                        if (retainOldTarget) {
                            retiredDrapeTargetsByTile[tileID] = oldTarget;
                        } else {
                            retiredDrapeTargetsByTile.erase(tileID);
                        }
                        if (traceDrape) {
                            Log::Info(Event::Render,
                                      "[KLATTRA DRAPE_TRACE] cache-resize frame=" + std::to_string(drapeTraceFrame) +
                                          " tile=" + klattraTileString(tileID) +
                                          " oldSize=" + std::to_string(existingSize.width) + "x" +
                                          std::to_string(existingSize.height) + " newSize=" +
                                          std::to_string(desiredSize.width) + "x" + std::to_string(desiredSize.height) +
                                          " retiredReady=" + std::to_string(klattraDrapeTargetReady(oldTarget)) +
                                          " stableFrames=" + std::to_string(stableDrapeCoverFrames));
                        }
                        if (klattraFlyDiag() && klattraFlyDiagBudget()) {
                            klattraDumpEmit("[KLATTRA FLYDIAG] resize tile=" + klattraTileString(tileID) +
                                " old=" + std::to_string(existingSize.width) +
                                " new=" + std::to_string(desiredSize.width) +
                                " retiredReady=" + std::to_string(klattraDrapeTargetReady(oldTarget)));
                        }
                        changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(oldTarget));
                        if (retainOldTarget) {
                            klattraReleaseRetiredDrapeBuildState(oldTarget);
                        }
                    } else {
                        // Over the per-frame cap — this tile's resize (and
                        // re-bake) happens on a LATER frame, so one must come.
                        drapeWorkPending = true;
                    }
                }
            }

            const bool wasAllocated = drapeCache.get(tileID) != nullptr;
            // .58 memory diet: mid/far tiers can bake into packed-565 targets —
            // half the bytes of RGBA8 across the ~85% of the population that
            // isn't the near ring (device flights ran 720-910 MB of canvases
            // at a 2.4-2.7 GB footprint vs the ~3.4 GB kill line; this is
            // the headroom for sharper near tiles and the far cover). Drape
            // targets clear to the style background, so the missing alpha
            // channel is never consulted. Near ring stays RGBA8 for
            // gradient fidelity where the camera looks.
            const bool drape565 = klattraPacked565DrapeEnabled();
            const auto channelType = (drape565 && targetSize < nearSize) ? gfx::TextureChannelDataType::UnsignedShort565
                                         : gfx::TextureChannelDataType::UnsignedByte;
            auto target = drapeCache.getOrCreate(context, tileID, desiredSize, channelType);
            if (!target) {
                // .56: allocation failed (memory pressure) — retry next
                // frame; count it so green plates are attributable.
                stageAllocFailEvents++;
                drapeWorkPending = true;
            }
            if (!wasAllocated && target) {
                if (stageDiagEnabled() && drapeStageByTile.size() < 8192) {
                    auto& stage = drapeStageByTile[tileID];
                    if (stage.created == std::chrono::steady_clock::time_point{}) {
                        stage.created = std::chrono::steady_clock::now();
                    }
                }
                if (klattraFlyDiag() && klattraFlyDiagBudget()) {
                    klattraDumpEmit("[KLATTRA FLYDIAG] create tile=" + klattraTileString(tileID) +
                                    " size=" + std::to_string(targetSize));
                }
                changes.emplace_back(std::make_unique<AddRenderTargetRequest>(target));
            }
        }
        // Retain ready out-of-cover ancestors as fallback content, but CAP
        // them: uncapped retention accumulates one generation per zoom level
        // on a continuous zoom-in (~100–400 MB that no ring budget covers).
        // Deepest ancestors first — a z-1 parent is a far better fallback
        // than a z-5 one.
        static const uint32_t maxRetainedAncestors = klattraEnvTileCount("KLATTRA_DRAPE_MAX_RETAINED_ANCESTORS", 8);
        std::vector<OverscaledTileID> retainedAncestors;
        drapeCache.visitAll([&](const OverscaledTileID& id, const TerrainDrapeTargetPtr& target) {
            if (currentDrapeIDs.find(id) != currentDrapeIDs.end()) return;
            if (!klattraDrapeTargetReady(target)) return;
            for (const auto& idealID : currentIdealIDs) {
                if (klattraIsAncestorOf(id, idealID)) {
                    retainedAncestors.push_back(id);
                    return;
                }
            }
        });
        if (retainedAncestors.size() > maxRetainedAncestors) {
            std::sort(
                retainedAncestors.begin(),
                      retainedAncestors.end(),
                [](const OverscaledTileID& a, const OverscaledTileID& b) { return a.canonical.z > b.canonical.z; });
            retainedAncestors.erase(retainedAncestors.begin() + maxRetainedAncestors, retainedAncestors.end());
        }
        const std::unordered_set<OverscaledTileID> ancestorKeep(retainedAncestors.begin(), retainedAncestors.end());
        auto evicted = drapeCache.pruneIf([&](const OverscaledTileID& id) {
            if (currentDrapeIDs.find(id) != currentDrapeIDs.end()) {
                return false;
            }
            return ancestorKeep.find(id) == ancestorKeep.end();
        });
        for (auto& [evictedID, evictedTarget] : evicted) {
            if (traceDrape) {
                Log::Info(
                    Event::Render,
                          "[KLATTRA DRAPE_TRACE] cache-evict frame=" + std::to_string(drapeTraceFrame) +
                              " tile=" + klattraTileString(evictedID) +
                              " target=" + (evictedTarget ? evictedTarget->getDebugName() : std::string("null")) +
                              " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(evictedTarget.get())) +
                              " completed=" + std::to_string(evictedTarget ? evictedTarget->getCompletedRenderCount() : 0) +
                              " groups=" + std::to_string(evictedTarget ? evictedTarget->numLayerGroups() : 0) +
                              " drawables=" + std::to_string(klattraDrapeDrawableCount(evictedTarget)));
            }
            // Freeze a target that is still sampled by the published terrain
            // cover. It leaves the orchestrator (so stale source drawables
            // cannot mutate the predecessor); only its published texture and
            // publication metadata stay owned until currentBindings atomically
            // switches to the replacement cover.
            TerrainDrapeTargetPtr retainedTarget;
            if (targetOwnsPublishedTexture(evictedTarget)) {
                retainedTarget = evictedTarget;
                retiredDrapeTargetsByTile[evictedID] = retainedTarget;
                ++frozenPublishedTargetsThisUpdate;
            }
            // The orchestrator still holds the AddRenderTargetRequest's
            // shared_ptr to this target — emit a matching remove request.
            changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(evictedTarget)));
            klattraReleaseRetiredDrapeBuildState(retainedTarget);
        }
        for (auto it = retiredDrapeTargetsByTile.begin(); it != retiredDrapeTargetsByTile.end();) {
            if (currentDrapeIDs.find(it->first) == currentDrapeIDs.end() &&
                !targetOwnsPublishedTexture(it->second)) {
                it = retiredDrapeTargetsByTile.erase(it);
            } else {
                ++it;
            }
        }
        // Release parked resize-predecessors as soon as their successor has
        // baked — for EVERY cover member, not just ideals. The DEM-binding
        // loop's erase only covers ideal tiles; fallback parents are never
        // ideals, so their retired 2048² copies used to survive until the
        // parent left cover entirely (~50–150 MB of dead targets during long
        // pitched pans).
        for (auto it = retiredDrapeTargetsByTile.begin(); it != retiredDrapeTargetsByTile.end();) {
            if (klattraDrapeTargetReady(drapeCache.get(it->first)) &&
                !targetOwnsPublishedTexture(it->second)) {
                it = retiredDrapeTargetsByTile.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = drapeRingByTile.begin(); it != drapeRingByTile.end();) {
            if (currentDrapeIDs.find(it->first) == currentDrapeIDs.end()) {
                it = drapeRingByTile.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Layer updates run after this terrain update and before offscreen target
    // rendering. Start a publication generation now so those layers can
    // prove source coverage; an incomplete generation will leave the prior
    // texture pixels untouched instead of clearing them to a partial map.
    ++drapeSurfaceFrame;
    const bool styleHasDrapeAuthority = updateParameters && klattraStyleHasDrapeAuthority(*updateParameters);
    drapeCache.visitAll([&](const OverscaledTileID& tileID, const TerrainDrapeTargetPtr& target) {
        if (target) target->beginDrapeSurfaceFrame(drapeSurfaceFrame, tileID, styleHasDrapeAuthority);
    });

    // Drape render targets are separate offscreen targets, so normal
    // RenderLayer::markLayerRenderable(false) only removes the main
    // framebuffer layer group. Prune stale drape groups here as well, or
    // hidden/zoomed-out style layers can stay baked into the terrain texture.
    // .50: remember the current style's solid background for the shader's
    // no-drape-pixel fallback; ignore the transparent default so mutation
    // instants keep the last good tone.
    {
        const Color& solidBg = renderTree.getParameters().backgroundColor;
        if (solidBg.a > 0.0f) {
            drapeFallbackColor = solidBg;
        }
    }

    {
        std::unordered_set<int32_t> activeDrapeLayerIndices;
        std::unordered_set<std::string> activeDrapeLayerNames;
        for (const auto& item : renderTree.getLayerRenderItemMap()) {
            activeDrapeLayerIndices.insert(item.layer.get().getLayerIndex());
            activeDrapeLayerNames.insert(item.layer.get().getID());
        }
        // .49 (the topo-plate fix): match by layer NAME as well as index.
        // Layer indices are positional — after a style swap the new style
        // occupies overlapping indices, so the old style's groups survived
        // an index-only prune and their canvases kept showing the previous
        // style's bake (device 2026-07-11: topo paper/vegetation mosaics
        // mid-satellite-flyover). Group names carry the layer ID (drape
        // variants add a "-drape" suffix); a group whose name matches no
        // active layer is from a dead style and goes, which in turn lets
        // the zero-content-group eviction below drop the stale canvas.
        const auto layerNameActive = [&](const std::string& groupName) {
            if (activeDrapeLayerNames.find(groupName) != activeDrapeLayerNames.end()) {
                return true;
            }
            // Drape group names = layer ID + a routing suffix (fill/line/
            // hillshade/background use "-drape", raster uses
            // "-raster-drape"). Strip whichever matches before the lookup.
            static const std::array<std::string, 2> drapeSuffixes = {std::string("-raster-drape"),
                                                                     std::string("-drape")};
            for (const auto& suffix : drapeSuffixes) {
                if (groupName.size() > suffix.size() &&
                    groupName.compare(groupName.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    if (activeDrapeLayerNames.find(groupName.substr(0, groupName.size() - suffix.size())) !=
                        activeDrapeLayerNames.end()) {
                        return true;
                    }
                }
            }
            return false;
        };

        std::size_t removedGroups = 0;
        std::vector<OverscaledTileID> staleBakeIDs;
        drapeCache.visitAll([&](const OverscaledTileID& drapeID, const TerrainDrapeTargetPtr& target) {
            if (!target) return;
            const auto removed = target->removeLayerGroupsIf(
                [&](const int32_t layerIndex, const LayerGroupBase& group) {
                    if (layerIndex == std::numeric_limits<int32_t>::max()) {
                        return false;
                    }
                    if (activeDrapeLayerIndices.find(layerIndex) == activeDrapeLayerIndices.end()) {
                        return true;
                    }
                    return !layerNameActive(group.getName());
                });
            removedGroups += removed;
            if (removed > 0 && target->hasCompletedRender()) {
                // .47 flight fix (the beige plates): if the prune left this
                // canvas with NO content groups (a style swap removed them
                // all), its texture still holds the OLD style's bake — the
                // own-baked binding fallback would show those stale pixels
                // (device 2026-07-11: topo-paper plates bound mid-satellite
                // flight, bgOnly≈40/165 bindings). Collect it for eviction;
                // the create path rebuilds it next update with the current
                // style's clear colour and content routing. Partial prunes
                // (a layer leaving its zoom range) keep their canvas.
                std::size_t contentGroups = 0;
                target->visitLayerGroups([&](LayerGroupBase& group) {
                    if (group.getLayerIndex() != std::numeric_limits<int32_t>::max()) {
                        contentGroups++;
                    }
                });
                if (contentGroups == 0) {
                    staleBakeIDs.push_back(drapeID);
                }
            }
            if (traceDrape && removed) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] prune-inactive-groups frame=" + std::to_string(drapeTraceFrame) +
                              " tile=" + klattraTileString(drapeID) + " removed=" + std::to_string(removed));
            }
        });
        for (const auto& staleID : staleBakeIDs) {
            if (auto stale = drapeCache.take(staleID)) {
                changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(stale)));
            }
            retiredDrapeTargetsByTile.erase(staleID);
        }
        if (traceDrape && removedGroups) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] prune-inactive-groups-total frame=" + std::to_string(drapeTraceFrame) +
                          " removed=" + std::to_string(removedGroups) +
                          " activeLayers=" + std::to_string(activeDrapeLayerIndices.size()));
        }
    }

    // Refresh the CPU-side DEM image cache so getElevation() returns valid
    // values for every tile in cover. Holds a shared_ptr to the image so
    // sampling stays valid even if the source bucket gets torn down between
    // frames. Drops entries that left cover.
    {
        std::unordered_map<OverscaledTileID, std::shared_ptr<const PremultipliedImage>> next;
        std::unordered_map<OverscaledTileID, uint64_t> nextRevisions;
        next.reserve(renderTiles->size());
        nextRevisions.reserve(renderTiles->size());
        std::size_t imageReady = 0;
        std::size_t imageMissing = 0;
        for (const auto& renderTile : *renderTiles) {
            const auto& tileID = renderTile.getOverscaledTileID();
            const auto& tile = renderTile.getTile();
            if (tile.kind != Tile::Kind::RasterDEM) {
                imageMissing++;
                continue;
            }
            const auto* demTile = static_cast<const RasterDEMTile*>(&tile);
            const auto* hillshadeBucket = const_cast<RasterDEMTile*>(demTile)->getBucket();
            if (!hillshadeBucket) {
                imageMissing++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-image-missing frame=" + std::to_string(drapeTraceFrame) +
                                  " tile=" + klattraTileString(tileID) + " reason=no-bucket");
                }
                continue;
            }
            const auto& imagePtr = hillshadeBucket->getDEMData().getImagePtr();
            if (imagePtr && !imagePtr->size.isEmpty()) {
                next.emplace(tileID, imagePtr);
                nextRevisions.emplace(tileID, hillshadeBucket->getDEMData().getRevision());
                imageReady++;
            } else {
                imageMissing++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-image-missing frame=" + std::to_string(drapeTraceFrame) +
                                  " tile=" + klattraTileString(tileID) + " reason=no-image");
                }
            }
        }
        if (next != demImagesByTile || nextRevisions != demImageRevisionsByTile) {
            terrain_perf::count(terrain_perf::Counter::RawDEMRevision);
            elevationRevision = nextTerrainElevationRevision();
        }
        demImagesByTile = std::move(next);
        demImageRevisionsByTile = std::move(nextRevisions);
        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] dem-image-summary frame=" + std::to_string(drapeTraceFrame) +
                          " ready=" + std::to_string(imageReady) + " missing=" + std::to_string(imageMissing));
        }
        if (auto sampledElevation = getElevationAtLatLng(state.getLatLng())) {
            elevationOriginMeters = *sampledElevation;
            if (klattraTraceStderr()) {
                klattraTrace("terrain origin elevation=" + std::to_string(*sampledElevation));
            }
        }

    }

    // Refresh the GPU DEM texture cache. Keyed by the actual DEM tile's
    // OverscaledTileID (not the cover slot's ideal), so a single texture
    // can back multiple ideal drawables that all parent-fallback into the
    // same source. The texture itself lives on HillshadeBucket and is shared
    // with the hillshade prepare pass; previously terrain uploaded the same
    // 514x514 Terrain-RGB image a second time for every rendered DEM tile.
    std::size_t bucketSharedDEMTextures = 0;
    {
        std::unordered_map<OverscaledTileID, std::shared_ptr<gfx::Texture2D>> nextTextures;
        nextTextures.reserve(renderTiles->size());
        std::size_t reusedTextures = 0;
        std::size_t createdTextures = 0;
        std::size_t missingTextures = 0;
        for (const auto& renderTile : *renderTiles) {
            const auto& tile = renderTile.getTile();
            if (tile.kind != Tile::Kind::RasterDEM) {
                missingTextures++;
                continue;
            }
            const auto& sourceID = tile.id;
            if (nextTextures.find(sourceID) != nextTextures.end()) continue;

            auto* demTile = const_cast<RasterDEMTile*>(static_cast<const RasterDEMTile*>(&tile));
            auto* hillshadeBucket = demTile ? demTile->getBucket() : nullptr;
            if (!hillshadeBucket) {
                missingTextures++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-texture-missing frame=" + std::to_string(drapeTraceFrame) +
                                  " source=" + klattraTileString(sourceID) + " reason=no-bucket");
                }
                continue;
            }
            const auto& demData = hillshadeBucket->getDEMData();
            auto imagePtr = demData.getImagePtr();
            if (!imagePtr || imagePtr->size.isEmpty()) {
                missingTextures++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-texture-missing frame=" + std::to_string(drapeTraceFrame) +
                                  " source=" + klattraTileString(sourceID) + " reason=no-image");
                }
                continue;
            }

            const bool reusedBucketTexture = hillshadeBucket->getDEMTexture() != nullptr;
            auto texture = hillshadeBucket->getOrCreateDEMTexture(context);
            if (!texture) {
                missingTextures++;
                continue;
            }

            if (traceDrape) {
                Log::Info(Event::Render,
                          std::string("[KLATTRA DRAPE_TRACE] dem-texture-bucket-") +
                              (reusedBucketTexture ? "reuse" : "create") + " frame=" + std::to_string(drapeTraceFrame) +
                              " source=" + klattraTileString(sourceID) +
                              " texture=" + klattraTexturePtrString(texture));
            }
            nextTextures.emplace(sourceID, std::move(texture));
            ++bucketSharedDEMTextures;
            reusedTextures += reusedBucketTexture ? 1 : 0;
            createdTextures += reusedBucketTexture ? 0 : 1;
        }
        demTexturesByTile = std::move(nextTextures);
        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] dem-texture-summary frame=" + std::to_string(drapeTraceFrame) +
                          " reused=" + std::to_string(reusedTextures) + " created=" + std::to_string(createdTextures) +
                          " bucketShared=" + std::to_string(bucketSharedDEMTextures) + " missing=" +
                          std::to_string(missingTextures) + " active=" + std::to_string(demTexturesByTile.size()));
        }
    }

    // Compute per-ideal DEM bindings. Each cover slot resolves to either
    // the exact source tile's texture (identity UV remap) or — when the
    // exact-zoom data is still in flight — to whichever ancestor's texture
    // happens to be available, with a sub-rect UV remap.
    std::unordered_map<OverscaledTileID, DEMBinding> nextBindings;
    nextBindings.reserve(terrainMeshIDs.size());
    std::size_t readyBindings = 0;
    std::size_t rasterEmptyBindings = 0;
    std::size_t rasterPartialBindings = 0;
    std::size_t emptyDemBindings = 0;
    std::size_t fallbackDrapeBindings = 0;
    // .46-diag: flicker-window classes. bgOnly = own baked canvas bound with
    // no content-ready canvas anywhere (renders as the drape clear + basemap
    // groups only); zeroGroup = that canvas had NO layer groups at all (raw
    // clear plate — the .17-era class); unbound = no drape texture at all.
    std::size_t bgOnlyBindings = 0;
    std::size_t bgOnlyZeroGroupBindings = 0;
    std::size_t unboundBindings = 0;
    std::size_t unboundNoTarget = 0;
    std::size_t unboundNeverAttempted = 0;
    std::size_t unboundUnrenderable = 0;
    std::size_t unboundNoAuthority = 0;
    std::size_t unboundWriterUnacknowledged = 0;
    std::size_t unboundSourceIncomplete = 0;
    std::size_t unboundMissingRasterFloor = 0;
    std::size_t unboundPublishedOther = 0;
    std::array<std::size_t, 4> unboundTargetSizes{}; // near / mid / far / other
    std::string unboundDiagnosticSample;
    std::size_t finlandPlannedAncestorBindings = 0;
    std::size_t finlandRetiredCleanupRequests = 0;
    for (const auto& idealOS : terrainMeshIDs) {
        std::optional<OverscaledTileID> resolvedSourceID;
        std::shared_ptr<gfx::Texture2D> resolvedTexture;
        if (auto exact = demTexturesByTile.find(idealOS); exact != demTexturesByTile.end()) {
            resolvedSourceID = idealOS;
            resolvedTexture = exact->second;
        } else {
            for (const auto& [candidateID, texture] : demTexturesByTile) {
                if (!texture || !klattraIsAncestorOf(candidateID, idealOS)) {
                    continue;
                }
                if (!resolvedSourceID || candidateID.canonical.z > resolvedSourceID->canonical.z) {
                    resolvedSourceID = candidateID;
                    resolvedTexture = texture;
                }
            }
        }

        DEMBinding binding(resolvedTexture, resolvedSourceID.value_or(idealOS));
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
        if (resolvedSourceID) {
            if (auto image = demImagesByTile.find(*resolvedSourceID); image != demImagesByTile.end()) {
                binding.demImage = image->second;
                binding.demImageRevision = demImageRevisionsByTile.at(*resolvedSourceID);
            }
        }
#endif
        if (binding.texture && resolvedSourceID && idealOS.canonical.z > resolvedSourceID->canonical.z) {
            auto [demTL, demScale] = klattraSubrectForChildInAncestor(idealOS, *resolvedSourceID);
            binding.demTL = demTL;
            binding.demScale = demScale;
        } else if (!binding.texture) {
            // No DEM data anywhere in the loaded parent chain for this
            // overscanned tile. Keep the mesh alive with a flat DEM texture
            // for the frame instead of leaving a hole in the pitched view.
            binding.texture = getOrCreateEmptyDEMTexture(context);
            binding.sourceID = idealOS;
            binding.usedEmptyDEM = true;
        }

        TerrainDrapeTargetPtr drape = drapeCache.get(idealOS);
        OverscaledTileID resolvedDrapeID = idealOS;
        if (finlandSurfaceSplits > 0) {
            const auto plannedID = topo_vector::plannedDrapeForSurfaceMesh(idealOS, exactDrapeIDs);
            const bool hasPlannedAncestor = plannedID && plannedID->canonical.z < idealOS.canonical.z;
            const auto plannedTarget = hasPlannedAncestor ? drapeCache.get(*plannedID) : nullptr;
            const auto retiredOwn = retiredDrapeTargetsByTile.find(idealOS);
            const bool retiredOwnPublished = retiredOwn != retiredDrapeTargetsByTile.end() &&
                                             targetOwnsPublishedTexture(retiredOwn->second);
            const auto selection = topo_vector::selectPlannedDrapeAncestor(
                hasPlannedAncestor,
                klattraDrapeTargetReadyForTile(plannedTarget, idealOS),
                retiredOwnPublished);
            if (selection.usePlannedAncestor) {
                // A planned far parent supersedes even a READY retired own
                // child. Keep the existing continuity fallback until this
                // parent is publishable, then switch texture and UV together.
                drape = plannedTarget;
                resolvedDrapeID = *plannedID;
                binding.usedDrapeFallback = true;
                auto [drapeTL, drapeScale] = klattraSubrectForChildInAncestor(idealOS, resolvedDrapeID);
                binding.drapeTL = drapeTL;
                binding.drapeScale = drapeScale;
                ++finlandPlannedAncestorBindings;
                if (selection.needsRetiredCleanupUpdate) {
                    // Retired cleanup precedes atomic publication. One update
                    // after the successful handoff releases its old texture;
                    // an atomic hold keeps continuity and this request alive.
                    drapeWorkPending = true;
                    ++finlandRetiredCleanupRequests;
                }
            }
        }
        if (!klattraDrapeTargetReadyForTile(drape, idealOS)) {
            auto retired = retiredDrapeTargetsByTile.find(idealOS);
            if (retired != retiredDrapeTargetsByTile.end() &&
                klattraDrapeTargetReadyForTile(retired->second, idealOS)) {
                drape = retired->second;
            } else {
                TerrainDrapeTargetPtr bestAncestor;
                std::optional<OverscaledTileID> bestAncestorID;
                drapeCache.visitAll([&](const OverscaledTileID& candidateID, const TerrainDrapeTargetPtr& candidate) {
                    if (!klattraDrapeTargetReadyForTile(candidate, idealOS) ||
                        !klattraIsAncestorOf(candidateID, idealOS)) {
                        return;
                    }
                    if (!bestAncestorID || candidateID.canonical.z > bestAncestorID->canonical.z) {
                        bestAncestor = candidate;
                        bestAncestorID = candidateID;
                    }
                });
                // A freshly-resized ancestor's live target has no content for
                // a frame or two, but its READY predecessor is parked in the
                // retired map — use it, or children flash empty at the pan
                // leading edge (the exact artifact parent fallback exists to
                // prevent). Live candidates win ties via the strict > above.
                for (const auto& [retiredID, retiredTarget] : retiredDrapeTargetsByTile) {
                    if (!klattraDrapeTargetReadyForTile(retiredTarget, idealOS) ||
                        !klattraIsAncestorOf(retiredID, idealOS)) {
                        continue;
                    }
                    if (!bestAncestorID || retiredID.canonical.z > bestAncestorID->canonical.z) {
                        bestAncestor = retiredTarget;
                        bestAncestorID = retiredID;
                    }
                }
                if (bestAncestor && bestAncestorID) {
                    drape = bestAncestor;
                    resolvedDrapeID = *bestAncestorID;
                    binding.usedDrapeFallback = true;
                    auto [drapeTL, drapeScale] = klattraSubrectForChildInAncestor(idealOS, resolvedDrapeID);
                    binding.drapeTL = drapeTL;
                    binding.drapeScale = drapeScale;
                }
            }
        } else {
            // The live successor can be selected for the candidate cover now,
            // but an atomic hold later in this update may still keep the
            // published predecessor. Release it only after currentBindings no
            // longer names its exact texture (on the following update).
            if (auto retired = retiredDrapeTargetsByTile.find(idealOS);
                retired != retiredDrapeTargetsByTile.end() && !targetOwnsPublishedTexture(retired->second)) {
                retiredDrapeTargetsByTile.erase(retired);
            }
        }
        if (klattraDrapeTargetReadyForTile(drape, idealOS) && drape->getTexture()) {
            binding.drapeReady = true;
            binding.drapeID = resolvedDrapeID;
            binding.drapeTexture = drape->getTexture();
        }
        if (!binding.drapeTexture) {
            unboundBindings++;
            const TerrainDrapeTargetPtr ownTarget = drapeCache.get(idealOS);
            if (!ownTarget) {
                ++unboundNoTarget;
            } else {
                const auto size = ownTarget->getSize();
                if (size.width == 1536) {
                    ++unboundTargetSizes[0];
                } else if (size.width == 1024) {
                    ++unboundTargetSizes[1];
                } else if (size.width == 512) {
                    ++unboundTargetSizes[2];
                } else {
                    ++unboundTargetSizes[3];
                }

                const auto& diagnostic = ownTarget->getLastDrapeRenderDiagnostic();
                const bool previousGenerationAttempt =
                    drapeSurfaceFrame > 0 && diagnostic.generation + 1 == drapeSurfaceFrame;
                if (!previousGenerationAttempt) {
                    ++unboundNeverAttempted;
                } else {
                    switch (diagnostic.reason) {
                        case RenderTarget::DrapeRenderDiagnosticReason::NeverAttempted:
                            ++unboundNeverAttempted;
                            break;
                        case RenderTarget::DrapeRenderDiagnosticReason::Unrenderable:
                            ++unboundUnrenderable;
                            break;
                        case RenderTarget::DrapeRenderDiagnosticReason::NoAuthoritativeRequirement:
                            ++unboundNoAuthority;
                            break;
                        case RenderTarget::DrapeRenderDiagnosticReason::WriterUnacknowledged:
                            ++unboundWriterUnacknowledged;
                            break;
                        case RenderTarget::DrapeRenderDiagnosticReason::SourceIncomplete:
                            ++unboundSourceIncomplete;
                            break;
                        case RenderTarget::DrapeRenderDiagnosticReason::MissingRasterFloor:
                            ++unboundMissingRasterFloor;
                            break;
                        case RenderTarget::DrapeRenderDiagnosticReason::Published:
                            ++unboundPublishedOther;
                            break;
                    }
                }
                if (unboundDiagnosticSample.empty()) {
                    unboundDiagnosticSample = klattraTileString(idealOS) + "@" + std::to_string(size.width) +
                                              " gen=" + std::to_string(diagnostic.generation) +
                                              " declared=" + std::to_string(diagnostic.declaredRequirements) +
                                              " ack=" + std::to_string(diagnostic.acknowledgedRequirements) +
                                              " incomplete=" + std::to_string(diagnostic.incompleteRequirements) +
                                              " key=" +
                                              (diagnostic.blockedSourceKey.empty() ? "-"
                                                                                  : diagnostic.blockedSourceKey);
                }
            }
        }
        if (binding.drapeReady) {
            readyBindings++;
        }
        if (binding.usedEmptyDEM) {
            emptyDemBindings++;
        }
        if (binding.usedDrapeFallback) {
            fallbackDrapeBindings++;
        }
        if (stageDiagEnabled()) {
            const auto stageIt = drapeStageByTile.find(idealOS);
            if (stageIt != drapeStageByTile.end() &&
                stageIt->second.firstBound == std::chrono::steady_clock::time_point{}) {
                auto& stage = stageIt->second;
                stage.firstBound = std::chrono::steady_clock::now();
                stage.firstBoundState = binding.usedDrapeFallback  ? 1
                                        : binding.drapeReady       ? 0
                                        : binding.drapeTexture     ? 2
                                                                   : 3;
            }
            // .62: the direct unpainted-canvas gauge. A bound canvas that
            // wants raster content but holds NO raster drawable renders its
            // cleared background through the relief shader — the .61 black.
            if (const TerrainDrapeTargetPtr own = drapeCache.get(idealOS); own && own->requiresRasterDrapeContent()) {
                const bool hasAnyRaster = klattraDrapeHasRasterContent(own);
                const bool hasFullRaster = hasAnyRaster && own->hasRasterDrawableCoveringTile(idealOS);
                if (!hasAnyRaster) {
                    rasterEmptyBindings++;
                } else if (!hasFullRaster) {
                    // .67: the old empty-only gauge missed a canvas with one
                    // z+ child covering only a quarter (or 1/16) of the RTT;
                    // the remaining pixels are exactly satellite-background.
                    rasterPartialBindings++;
                }
                if ((!hasAnyRaster || !hasFullRaster) &&
                    (klattraFlyDiag() ? klattraFlyDiagBudget() : (rasterEmptyBindings + rasterPartialBindings <= 3))) {
                    // One named incomplete canvas per second even without
                    // FLYDIAG, classified as empty vs partial.
                    static std::chrono::steady_clock::time_point lastIncompleteEmit{};
                    const auto nowE = std::chrono::steady_clock::now();
                    if (nowE - lastIncompleteEmit >= std::chrono::seconds(1)) {
                        lastIncompleteEmit = nowE;
                        klattraDumpEmit(std::string("[KLATTRA STAGE] raster") + (hasAnyRaster ? "Partial" : "Empty") +
                                        " tile=" + klattraTileString(idealOS) +
                                        " size=" + std::to_string(own->getSize().width) +
                                        " groups=" + std::to_string(own->numLayerGroups()) +
                                        " drawables=" + std::to_string(klattraDrapeDrawableCount(own)));
                    }
                }
            }
        }

        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] binding frame=" + std::to_string(drapeTraceFrame) +
                          " ideal=" + klattraTileString(idealOS) + " source=" + klattraTileString(binding.sourceID) +
                          " drape=" + (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                          " demTexture=" + klattraTexturePtrString(binding.texture) + " emptyDEM=" +
                          std::to_string(binding.usedEmptyDEM) + " demTL=" + std::to_string(binding.demTL[0]) + "," +
                          std::to_string(binding.demTL[1]) + " demScale=" + std::to_string(binding.demScale) +
                          " drapeTexture=" + klattraTexturePtrString(binding.drapeTexture) +
                          " drapeFallback=" + std::to_string(binding.usedDrapeFallback) +
                          " drapeTL=" + std::to_string(binding.drapeTL[0]) + "," + std::to_string(binding.drapeTL[1]) +
                          " drapeScale=" + std::to_string(binding.drapeScale) +
                          " drapeReady=" + std::to_string(binding.drapeReady) +
                          " drapePtr=" + std::to_string(reinterpret_cast<uintptr_t>(drape.get())) +
                          " drapeCompleted=" + std::to_string(drape ? drape->getCompletedRenderCount() : 0) +
                          " drapeGroups=" + std::to_string(drape ? drape->numLayerGroups() : 0) +
                          " drapeContentGroups=" + std::to_string(drape ? drape->numContentLayerGroups() : 0) +
                          " drapeDrawables=" + std::to_string(klattraDrapeDrawableCount(drape)));
        }

        nextBindings.emplace(idealOS, std::move(binding));
    }

    const std::size_t candidateBindingCount = nextBindings.size();
    const std::size_t candidateReadyBindings = readyBindings;
    const std::size_t candidateUnboundBindings = unboundBindings;
    const std::size_t candidateEmptyDEMBindings = emptyDemBindings;
    const std::size_t candidateOwnRasterEmpty = rasterEmptyBindings;
    const std::size_t candidateOwnRasterPartial = rasterPartialBindings;

    // A mask is only a coverage guarantee if every emitted leaf can actually
    // draw. If a newly-partitioned cover has an unbound drape leaf, swapping
    // piecemeal would mask out its paintable parent and recreate .67's black
    // hole. Keep the previous disjoint cover atomically for this update while
    // the new targets continue baking; switch all leaves together once every
    // binding has a texture. This hold never mixes the old and new covers.
    std::unordered_set<OverscaledTileID> previousDrawableIDs;
    lg->visitDrawables([&](const gfx::Drawable& drawable) {
        if (const auto& tileID = drawable.getTileID()) {
            previousDrawableIDs.insert(*tileID);
        }
    });
    bool previousCoverDrawableBacked = !currentBindings.empty();
    bool previousCoverHasOnlyRealDEM = !currentBindings.empty();
    for (const auto& [tileID, binding] : currentBindings) {
        if (!binding.drapeTexture || previousDrawableIDs.find(tileID) == previousDrawableIDs.end()) {
            previousCoverDrawableBacked = false;
        }
        if (binding.usedEmptyDEM) previousCoverHasOnlyRealDEM = false;
    }
    bool terrainAtomicHold = false;
    bool terrainAtomicDrop = false;
    const auto incompleteDEMAction = raster_drape::selectIncompleteDEMAction(
        candidateBindingCount, candidateEmptyDEMBindings, previousCoverDrawableBacked, previousCoverHasOnlyRealDEM);
    const bool holdForUnboundDrape = candidateUnboundBindings > 0 && previousCoverDrawableBacked;
    const bool holdForEmptyDEM = incompleteDEMAction == raster_drape::IncompleteDEMAction::HoldPrevious;
    const bool dropForEmptyDEM = incompleteDEMAction == raster_drape::IncompleteDEMAction::DropTerrain;
    if (holdForEmptyDEM || (holdForUnboundDrape && candidateEmptyDEMBindings == 0)) {
        terrainAtomicHold = true;
        terrainMeshIDs.clear();
        terrainMeshIDs.reserve(currentBindings.size());
        for (const auto& [tileID, binding] : currentBindings) {
            (void)binding;
            terrainMeshIDs.insert(tileID);
        }
        nextBindings = currentBindings;
    } else if (dropForEmptyDEM) {
        // Never publish a generation that mixes real relief with flat empty
        // DEM leaves. With no coherent all-real predecessor, keep terrain out
        // of the main pass entirely so ordinary 2D layers remain visible
        // until the whole DEM cover is ready (or indefinitely at terminal
        // no-data). This is the startup/new-pan counterpart of the hold above.
        terrainAtomicDrop = true;
        terrainMeshIDs.clear();
        nextBindings.clear();
    }

    // A terrain drawable owns the exact texture generation that was current
    // when it was created. If a replacement target exists but is not content
    // ready yet, keep both the drawable AND its old binding (texture + UV
    // remap) together. Publishing the replacement binding while retaining the
    // old drawable makes the final shader combine new UVs with an old texture;
    // the next update can then mistake that mismatched pair for up-to-date.
    uint32_t preservedDrawableBindings = 0;
    for (auto& [idealID, binding] : nextBindings) {
        const auto existing = currentBindings.find(idealID);
        const bool regressedToEmptyDEM = existing != currentBindings.end() && !existing->second.usedEmptyDEM &&
                                         binding.usedEmptyDEM;
        if ((!regressedToEmptyDEM && binding.drapeReady) ||
            previousDrawableIDs.find(idealID) == previousDrawableIDs.end()) {
            continue;
        }
        if (existing == currentBindings.end() || !existing->second.drapeTexture) {
            continue;
        }
        const bool bindingChanged = existing->second.sourceID != binding.sourceID ||
                                    existing->second.texture != binding.texture ||
                                    existing->second.drapeTexture != binding.drapeTexture ||
                                    existing->second.drapeID != binding.drapeID ||
                                    existing->second.drapeTL != binding.drapeTL ||
                                    existing->second.drapeScale != binding.drapeScale ||
                                    existing->second.drapeReady != binding.drapeReady;
        if (bindingChanged) {
            binding = existing->second;
            ++preservedDrawableBindings;
        }
    }

    // The counters accumulated above describe the candidate cover. Recompute
    // the visible binding classes after the optional atomic hold so STAGE does
    // not attribute a rejected child's state to the old cover on screen.
    readyBindings = 0;
    emptyDemBindings = 0;
    fallbackDrapeBindings = 0;
    bgOnlyBindings = 0;
    bgOnlyZeroGroupBindings = 0;
    unboundBindings = 0;
    std::size_t boundRasterEmptyBindings = 0;
    std::size_t boundRasterPartialBindings = 0;
    std::size_t boundRasterUnknownBindings = 0;
    std::unordered_map<const RenderTarget*, KlattraTopoFillTargetStats> topoFillTargets;
    std::size_t topoFillResolvedBindings = 0;
    std::size_t landOpenBindings = 0;
    std::size_t landAkerBindings = 0;
    std::size_t forestBindings = 0;
    std::size_t noTopoFillBindings = 0;
    std::size_t landOpenTargets = 0;
    std::size_t landAkerTargets = 0;
    std::size_t forestTargets = 0;
    std::size_t zeroLandOpenTargets = 0;
    std::size_t zeroLandAkerTargets = 0;
    std::size_t zeroForestTargets = 0;
    std::size_t landOpenDrawables = 0;
    std::size_t landAkerDrawables = 0;
    std::size_t forestDrawables = 0;
    std::array<uint32_t, 26> landOpenSourceZooms{};
    std::array<uint32_t, 26> landAkerSourceZooms{};
    std::array<uint32_t, 26> forestSourceZooms{};
    std::array<uint32_t, 26> boundDrapeZooms{};
    static std::chrono::steady_clock::time_point lastTopoFillScan{};
    const auto topoFillNow = std::chrono::steady_clock::now();
    const bool topoFillScanDue = stageDiagEnabled() &&
        (lastTopoFillScan == std::chrono::steady_clock::time_point{} ||
         topoFillNow - lastTopoFillScan >= std::chrono::milliseconds(250));
    for (const auto& [idealID, binding] : nextBindings) {
        if (binding.drapeReady) ++readyBindings;
        if (binding.usedEmptyDEM) ++emptyDemBindings;
        if (binding.usedDrapeFallback) ++fallbackDrapeBindings;
        if (!binding.drapeTexture) {
            ++unboundBindings;
            continue;
        }
        if (!binding.drapeReady && !binding.usedDrapeFallback) {
            ++bgOnlyBindings;
        }
        if (!structuralDiagnostics) {
            continue;
        }

        TerrainDrapeTargetPtr boundTarget;
        if (binding.drapeID) {
            if (auto live = drapeCache.get(*binding.drapeID); live && live->getTexture() == binding.drapeTexture) {
                boundTarget = live;
            } else if (auto retired = retiredDrapeTargetsByTile.find(*binding.drapeID);
                       retired != retiredDrapeTargetsByTile.end() && retired->second &&
                       retired->second->getTexture() == binding.drapeTexture) {
                boundTarget = retired->second;
            }
        }
        if (!boundTarget) {
            drapeCache.visitAll([&](const OverscaledTileID&, const TerrainDrapeTargetPtr& candidate) {
                if (!boundTarget && candidate && candidate->getTexture() == binding.drapeTexture) {
                    boundTarget = candidate;
                }
            });
        }
        if (!boundTarget) {
            for (const auto& [retiredID, retired] : retiredDrapeTargetsByTile) {
                (void)retiredID;
                if (retired && retired->getTexture() == binding.drapeTexture) {
                    boundTarget = retired;
                    break;
                }
            }
        }
        if (!boundTarget) {
            ++boundRasterUnknownBindings;
            continue;
        }

        if (topoFillScanDue) {
            ++topoFillResolvedBindings;
            if (binding.drapeID && binding.drapeID->canonical.z < boundDrapeZooms.size()) {
                boundDrapeZooms[binding.drapeID->canonical.z]++;
            }
            auto [fillIt, inserted] = topoFillTargets.try_emplace(boundTarget.get());
            if (inserted) {
                fillIt->second = klattraTopoFillTargetStats(boundTarget);
                const auto& fill = fillIt->second;
                if (fill.hasLandOpenGroup) {
                    ++landOpenTargets;
                    if (!fill.landOpenDrawables) ++zeroLandOpenTargets;
                }
                if (fill.hasLandAkerGroup) {
                    ++landAkerTargets;
                    if (!fill.landAkerDrawables) ++zeroLandAkerTargets;
                }
                if (fill.forestGroups) {
                    ++forestTargets;
                    if (!fill.forestDrawables) ++zeroForestTargets;
                }
                landOpenDrawables += fill.landOpenDrawables;
                landAkerDrawables += fill.landAkerDrawables;
                forestDrawables += fill.forestDrawables;
                klattraAccumulateZooms(landOpenSourceZooms, fill.landOpenSourceZooms);
                klattraAccumulateZooms(landAkerSourceZooms, fill.landAkerSourceZooms);
                klattraAccumulateZooms(forestSourceZooms, fill.forestSourceZooms);
            }
            const auto& fill = fillIt->second;
            if (fill.landOpenDrawables) ++landOpenBindings;
            if (fill.landAkerDrawables) ++landAkerBindings;
            if (fill.forestDrawables) ++forestBindings;
            if (!fill.landOpenDrawables && !fill.landAkerDrawables && !fill.forestDrawables) {
                ++noTopoFillBindings;
            }
        }

        if (!binding.drapeReady && !binding.usedDrapeFallback && boundTarget->numLayerGroups() == 0) {
            ++bgOnlyZeroGroupBindings;
        }
        if (!boundTarget->requiresRasterDrapeContent()) {
            continue;
        }
        const bool hasAnyRaster = klattraDrapeHasRasterContent(boundTarget);
        if (!hasAnyRaster) {
            ++boundRasterEmptyBindings;
        } else if (!boundTarget->hasRasterDrawableCoveringTile(idealID)) {
            ++boundRasterPartialBindings;
        }
    }

    // Explicit stage telemetry. Emit at most 4 Hz while values change and at
    // 1 Hz while stable when KLATTRA_STAGEDIAG=1.
    if (topoFillScanDue) {
        lastTopoFillScan = topoFillNow;
        const std::string signature =
            std::to_string(nextBindings.size()) + ":" + std::to_string(topoFillResolvedBindings) + ":" +
            std::to_string(topoFillTargets.size()) + ":" + std::to_string(fallbackDrapeBindings) + ":" +
            std::to_string(landOpenBindings) + ":" + std::to_string(landAkerBindings) + ":" +
            std::to_string(forestBindings) + ":" + std::to_string(noTopoFillBindings) + ":" +
            std::to_string(landOpenTargets) + ":" + std::to_string(landAkerTargets) + ":" +
            std::to_string(forestTargets) + ":" + std::to_string(zeroLandOpenTargets) + ":" +
            std::to_string(zeroLandAkerTargets) + ":" + std::to_string(zeroForestTargets) + ":" +
            std::to_string(landOpenDrawables) + ":" + std::to_string(landAkerDrawables) + ":" +
            std::to_string(forestDrawables) + ":" + klattraZoomHistogramString(boundDrapeZooms) + ":" +
            klattraZoomHistogramString(landOpenSourceZooms) + ":" + klattraZoomHistogramString(landAkerSourceZooms) +
            ":" + klattraZoomHistogramString(forestSourceZooms);
        static std::string lastSignature;
        static std::chrono::steady_clock::time_point lastEmit{};
        const auto now = std::chrono::steady_clock::now();
        const bool changed = signature != lastSignature;
        const bool changeDue = changed && (lastEmit == std::chrono::steady_clock::time_point{} ||
                                           now - lastEmit >= std::chrono::milliseconds(250));
        const bool heartbeatDue = lastEmit == std::chrono::steady_clock::time_point{} ||
                                  now - lastEmit >= std::chrono::seconds(1);
        if (changeDue || heartbeatDue) {
            lastSignature = signature;
            lastEmit = now;
            klattraDumpEmit(
                "[KLATTRA FILL_COVER] diag=84 bindings=" + std::to_string(nextBindings.size()) + " resolved=" +
                std::to_string(topoFillResolvedBindings) + " targets=" + std::to_string(topoFillTargets.size()) +
                " fallback=" + std::to_string(fallbackDrapeBindings) + " openBindings=" +
                std::to_string(landOpenBindings) + " akerBindings=" + std::to_string(landAkerBindings) +
                " forestBindings=" + std::to_string(forestBindings) + " noTopoFillBindings=" +
                std::to_string(noTopoFillBindings) + " openTargets=" + std::to_string(landOpenTargets) +
                " akerTargets=" + std::to_string(landAkerTargets) + " forestTargets=" + std::to_string(forestTargets) +
                " zeroTargets=" + std::to_string(zeroLandOpenTargets) + "/" + std::to_string(zeroLandAkerTargets) +
                "/" + std::to_string(zeroForestTargets) + " drawables=" + std::to_string(landOpenDrawables) + "/" +
                    std::to_string(landAkerDrawables) + "/" + std::to_string(forestDrawables) +
                " drapeZ=" + klattraZoomHistogramString(boundDrapeZooms) +
                " openZ=" + klattraZoomHistogramString(landOpenSourceZooms) +
                " akerZ=" + klattraZoomHistogramString(landAkerSourceZooms) + " forestZ=" +
                klattraZoomHistogramString(forestSourceZooms) + " zoom=" + std::to_string(state.getZoom()) +
                " pitchDeg=" + std::to_string(state.getPitch() * 180.0 / M_PI) +
                " bearingDeg=" + std::to_string(state.getBearing() * 180.0 / M_PI));
        }
    }

    // Create or refresh terrain drawables, one per IDEAL tile. Recreate
    // when the source DEM tile changes (typically an upgrade from a
    // parent-fallback texture to the exact-zoom texture, or empty
    // fallback → real DEM); otherwise reuse the existing drawable.
    // Tiles that end this update WITH a drawable — the prune below holds a
    // leaving tile's drawable until the ideals covering it appear here.
    std::unordered_set<OverscaledTileID> drawableBackedTiles;
    drawableBackedTiles.reserve(nextBindings.size());
    uint32_t drawableHolds = preservedDrawableBindings;
    uint32_t drawableSkipsNotReady = 0;
    for (auto& [idealID, binding] : nextBindings) {
        if (auto existing = currentBindings.find(idealID); existing != currentBindings.end()) {
            if (existing->second.sourceID == binding.sourceID && existing->second.texture == binding.texture &&
                existing->second.drapeTexture == binding.drapeTexture && existing->second.drapeID == binding.drapeID &&
                existing->second.drapeTL == binding.drapeTL && existing->second.drapeScale == binding.drapeScale &&
                existing->second.drapeReady == binding.drapeReady) {
                if (traceDrape) {
                    Log::Info(
                        Event::Render,
                        "[KLATTRA DRAPE_TRACE] terrain-drawable-keep frame=" + std::to_string(drapeTraceFrame) +
                            " ideal=" + klattraTileString(idealID) + " source=" + klattraTileString(binding.sourceID) +
                                  " demTexture=" + klattraTexturePtrString(binding.texture) +
                            " drape=" + (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                                  " drapeTexture=" + klattraTexturePtrString(binding.drapeTexture) +
                                  " drapeFallback=" + std::to_string(binding.usedDrapeFallback) +
                                  " drapeReady=" + std::to_string(binding.drapeReady));
                }
                drawableBackedTiles.insert(idealID);
                continue; // drawable already up to date
            }
            if (traceDrape) {
                Log::Info(
                    Event::Render,
                    "[KLATTRA DRAPE_TRACE] terrain-drawable-refresh frame=" + std::to_string(drapeTraceFrame) +
                              " ideal=" + klattraTileString(idealID) +
                              " oldSource=" + klattraTileString(existing->second.sourceID) +
                              " newSource=" + klattraTileString(binding.sourceID) +
                              " oldTexture=" + klattraTexturePtrString(existing->second.texture) +
                        " newTexture=" + klattraTexturePtrString(binding.texture) + " oldDrape=" +
                                  (existing->second.drapeID ? klattraTileString(*existing->second.drapeID)
                                                            : std::string("none")) +
                        " newDrape=" + (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                              " oldDrapeTexture=" + klattraTexturePtrString(existing->second.drapeTexture) +
                              " newDrapeTexture=" + klattraTexturePtrString(binding.drapeTexture) +
                              " oldDrapeReady=" + std::to_string(existing->second.drapeReady) +
                              " newDrapeReady=" + std::to_string(binding.drapeReady));
            }
            if (!binding.drapeReady) {
                // The refreshed binding lost its content-ready drape —
                // typically a source or ring upgrade whose new bake hasn't
                // completed. Keep showing the existing drawable (its textures
                // stay alive through the drawable's refs) instead of
                // downgrading to a background-only bake or pruning to a hole;
                // the refresh re-runs on a later frame once a content-ready
                // drape is back. Holding is only valid if a drawable actually
                // EXISTS — a tile that entered the cover not-ready was never
                // created, and holding its phantom left a permanent silent
                // hole (the stored binding matched frame after frame, so
                // creation never re-ran — 2026-07-04 flight telemetry).
                bool hasExistingDrawable = false;
                lg->visitDrawables([&](const gfx::Drawable& d) {
                    if (!hasExistingDrawable && d.getTileID().has_value() && *d.getTileID() == idealID) {
                        hasExistingDrawable = true;
                    }
                });
                if (hasExistingDrawable) {
                    if (traceDrape) {
                        Log::Info(
                            Event::Render,
                            "[KLATTRA DRAPE_TRACE] terrain-drawable-hold frame=" + std::to_string(drapeTraceFrame) +
                                      " ideal=" + klattraTileString(idealID) +
                                " source=" + klattraTileString(binding.sourceID) + " reason=refresh-drape-not-ready");
                    }
                    ++drawableHolds;
                    // Keep the CPU binding record physically aligned with the
                    // drawable that is intentionally retained. This is a
                    // safety net for any transition not covered by the
                    // pre-pass above.
                    binding = existing->second;
                    drawableBackedTiles.insert(idealID);
                    continue;
                }
            } else {
                lg->removeDrawablesIf(
                    [&idealID](gfx::Drawable& d) { return d.getTileID().has_value() && *d.getTileID() == idealID; });
            }
        }
        if (!binding.drapeTexture) {
            ++drawableSkipsNotReady;
            if (klattraTraceStderr()) {
                klattraTrace("terrain drawable-skip ideal=" + klattraTileString(idealID) + " source=" +
                             klattraTileString(binding.sourceID) + " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                             " drapeFallback=" + std::to_string(binding.usedDrapeFallback));
            }
            if (traceDrape) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] terrain-drawable-skip frame=" + std::to_string(drapeTraceFrame) +
                              " ideal=" + klattraTileString(idealID) +
                              " source=" + klattraTileString(binding.sourceID) + " reason=drape-not-ready");
            }
            continue;
        }
        if (auto drawable = createDrawableForTile(context, shaders, idealID, binding)) {
            if (klattraTraceStderr()) {
                klattraTrace("terrain drawable-add ideal=" + klattraTileString(idealID) + " source=" +
                             klattraTileString(binding.sourceID) + " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                             " drapeFallback=" + std::to_string(binding.usedDrapeFallback));
            }
            if (traceDrape) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] terrain-drawable-add frame=" + std::to_string(drapeTraceFrame) +
                              " ideal=" + klattraTileString(idealID) +
                              " source=" + klattraTileString(binding.sourceID) +
                              " terrainDrawablesBefore=" + std::to_string(lg->getDrawableCount()));
            }
            lg->addDrawable(std::move(drawable));
            drawableBackedTiles.insert(idealID);
        }
    }

    // Prune drawables for ideals that left the cover. The drape cache's
    // RenderTargets are pruned separately above; this only handles the
    // terrain mesh drawables themselves.
    //
    // Zoom-level transitions used to hold a leaving terrain mesh until every
    // replacement ideal was drawable-backed. With a partial child cohort that
    // means the ready children and the full parent are rendered together — two
    // intersecting displaced surfaces carrying different drape canvases. DEM
    // and drape ancestor texture fallback now bridge not-ready ideals without
    // keeping the old geometry, so the safe default is zero. The env knob can
    // restore the legacy hold for a controlled diagnostic only.
    if (!currentBindings.empty()) {
        static const uint32_t pruneHoldFrames = klattraEnvFrameCount("KLATTRA_DRAPE_PRUNE_HOLD_FRAMES", 0);
        std::unordered_map<OverscaledTileID, uint32_t> nextPruneHoldAges;
        std::size_t held = 0;
        const auto removed = lg->removeDrawablesIf([&](gfx::Drawable& d) {
            const auto& maybeID = d.getTileID();
            if (!maybeID.has_value()) return false;
            const auto& leavingID = *maybeID;
            if (terrainMeshIDs.find(leavingID) != terrainMeshIDs.end()) {
                return false; // still an ideal
            }
            if (pruneHoldFrames > 0) {
                bool overlapsAny = false;
                bool covered = true;
                for (const auto& idealID : terrainMeshIDs) {
                    if (!klattraIsAncestorOf(idealID, leavingID) && !klattraIsAncestorOf(leavingID, idealID)) {
                        continue;
                    }
                    overlapsAny = true;
                    if (drawableBackedTiles.find(idealID) == drawableBackedTiles.end()) {
                        covered = false;
                        break;
                    }
                }
                if (overlapsAny && !covered) {
                    const auto ageIt = pruneHoldAgeByTile.find(leavingID);
                    const uint32_t age = (ageIt == pruneHoldAgeByTile.end()) ? 1 : ageIt->second + 1;
                    if (age <= pruneHoldFrames) {
                        nextPruneHoldAges.emplace(leavingID, age);
                        held++;
                        return false; // hold: replacement not ready yet
                    }
                }
            }
            return true;
        });
        pruneHoldAgeByTile = std::move(nextPruneHoldAges);
        if (traceDrape && (removed > 0 || held > 0)) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] terrain-drawable-prune frame=" + std::to_string(drapeTraceFrame) +
                          " removed=" + std::to_string(removed) + " held=" + std::to_string(held) +
                          " remaining=" + std::to_string(lg->getDrawableCount()));
        }
    }

    // Count the meshes the GPU will actually draw after pruning. Raw overlap
    // is now expected while held parents bridge cover fragmentation; both the
    // mask-leaf and actual drawable overlap must remain zero in .68.
    std::unordered_set<OverscaledTileID> drawableMeshIDs;
    std::size_t terrainLayerDrawableCount = 0;
    std::size_t drawableMeshCount = 0;
    std::size_t drawableMeshDuplicates = 0;
    std::size_t drawableMeshNoTile = 0;
    std::size_t drawableMeshOverlapPairs = 0;
    std::size_t drawableMeshMissing = 0;
    std::size_t drawableMeshExtra = 0;
    if (structuralDiagnostics) {
        lg->visitDrawables([&](const gfx::Drawable& drawable) {
            ++terrainLayerDrawableCount;
            if (const auto& tileID = drawable.getTileID()) {
                ++drawableMeshCount;
                drawableMeshIDs.insert(*tileID);
            }
        });
        drawableMeshDuplicates = drawableMeshCount - drawableMeshIDs.size();
        drawableMeshNoTile = terrainLayerDrawableCount - drawableMeshCount;
        for (auto a = drawableMeshIDs.begin(); a != drawableMeshIDs.end(); ++a) {
            for (auto b = std::next(a); b != drawableMeshIDs.end(); ++b) {
                if (klattraIsAncestorOf(*a, *b) || klattraIsAncestorOf(*b, *a)) {
                    ++drawableMeshOverlapPairs;
                }
            }
        }
        for (const auto& tileID : terrainMeshIDs) {
            if (drawableMeshIDs.find(tileID) == drawableMeshIDs.end()) {
                ++drawableMeshMissing;
            }
        }
        for (const auto& tileID : drawableMeshIDs) {
            if (terrainMeshIDs.find(tileID) == terrainMeshIDs.end()) {
                ++drawableMeshExtra;
            }
        }
    }

    if (stageDiagEnabled()) {
        // Sub-second defects cannot rely on the 1 Hz STAGE sample. Emit the
        // first bad frame immediately, a 1 Hz heartbeat while it persists,
        // and a recovery edge.
        const bool badFrame = sourceMeshOverlapPairs > 0 || drawableMeshOverlapPairs > 0 ||
                              drawableMeshDuplicates > 0 || drawableMeshNoTile > 0 || drawableMeshMissing > 0 ||
                              drawableMeshExtra > 0 || unboundBindings > 0 || boundRasterEmptyBindings > 0 ||
                              boundRasterPartialBindings > 0 || boundRasterUnknownBindings > 0;
        static bool wasBad = false;
        static std::chrono::steady_clock::time_point lastBadEmit{};
        const auto now = std::chrono::steady_clock::now();
        if (badFrame && (!wasBad || now - lastBadEmit >= std::chrono::seconds(1))) {
            lastBadEmit = now;
            klattraDumpEmit(
                "[KLATTRA BADFRAME] leafOverlap=" + std::to_string(sourceMeshOverlapPairs) + " meshOverlap=" +
                std::to_string(drawableMeshOverlapPairs) + " duplicates=" + std::to_string(drawableMeshDuplicates) +
                " noTile=" + std::to_string(drawableMeshNoTile) + " missing=" + std::to_string(drawableMeshMissing) +
                " extra=" + std::to_string(drawableMeshExtra) + " unbound=" + std::to_string(unboundBindings) +
                " rasterEmpty=" + std::to_string(boundRasterEmptyBindings) +
                " rasterPartial=" + std::to_string(boundRasterPartialBindings) + " rasterUnknown=" +
                std::to_string(boundRasterUnknownBindings) + " atomicHold=" + std::to_string(terrainAtomicHold));
        } else if (!badFrame && wasBad) {
            klattraDumpEmit("[KLATTRA BADFRAME] recovered");
        }
        wasBad = badFrame;

        static bool previousAtomicHold = false;
        static bool previousAtomicDrop = false;
        if (terrainAtomicHold != previousAtomicHold || terrainAtomicDrop != previousAtomicDrop) {
            klattraDumpEmit("[KLATTRA TRANSITION] atomicHold=" + std::to_string(terrainAtomicHold) +
                            " atomicDrop=" + std::to_string(terrainAtomicDrop) +
                            " candidateBindings=" + std::to_string(candidateBindingCount) +
                            " candidateUnbound=" + std::to_string(candidateUnboundBindings) +
                            " candidateEmptyDEM=" + std::to_string(candidateEmptyDEMBindings) + " holdUnbound=" +
                            std::to_string(holdForUnboundDrape) + " holdEmptyDEM=" + std::to_string(holdForEmptyDEM) +
                            " activeBindings=" + std::to_string(nextBindings.size()));
            previousAtomicHold = terrainAtomicHold;
            previousAtomicDrop = terrainAtomicDrop;
        }
    }

    // Cached symbol heights depend on the rendered mesh resolution and DEM
    // remap as well as image contents. A held cover can change independently
    // of the raw source cache; invalidate on that actual surface transition.
    const bool sameElevationCover = currentBindings.size() == nextBindings.size() &&
        std::all_of(nextBindings.begin(), nextBindings.end(), [&](const auto& entry) {
            const auto previous = currentBindings.find(entry.first);
            return previous != currentBindings.end() && previous->second.sameElevationSurface(entry.second);
        });
    if (!sameElevationCover) {
        terrain_perf::count(terrain_perf::Counter::MeshRevision);
        elevationRevision = nextTerrainElevationRevision();
    }
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
    // Only the Finnish policy creates meshes beyond the real DEM maximum.
    // Include the outgoing virtual cover when this update returns to base.
    const bool finnishSurfaceTransition = finlandSurfaceSplits > 0 ||
        std::any_of(currentBindings.begin(), currentBindings.end(), [&](const auto& entry) {
            return entry.first.canonical.z > demSource->getMaxZoom();
        });
#endif
    currentBindings = std::move(nextBindings);
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
    if ((hasRenderedFinnishBase || finnishSurfaceTransition) && !retiredDrapeTargetsByTile.empty()) {
        std::unordered_set<const gfx::Texture2D*> nextPublishedTextures;
        std::unordered_set<const gfx::Texture2D*> retiredTextures;
        for (const auto& [meshID, binding] : currentBindings) {
            (void)meshID;
            if (binding.drapeTexture) nextPublishedTextures.insert(binding.drapeTexture.get());
        }
        for (const auto& [tileID, retired] : retiredDrapeTargetsByTile) {
            (void)tileID;
            if (retired && retired->getTexture()) retiredTextures.insert(retired->getTexture().get());
        }
        if (topo_vector::needsFinnishSurfaceCleanup(hasRenderedFinnishBase,
                                                   finnishSurfaceTransition,
                                                   publishedDrapeTextures,
                                                   nextPublishedTextures,
                                                   retiredTextures)) {
            drapeWorkPending = true;
            ++finlandRetiredCleanupRequests;
        }
    }
#endif
    if (klattraTraceStderr()) {
        klattraTrace(
            "terrain update-end source=" + impl->sourceID + " bindings=" + std::to_string(currentBindings.size()) +
            " ready=" + std::to_string(readyBindings) + " emptyDEM=" + std::to_string(emptyDemBindings) +
            " drapeFallback=" + std::to_string(fallbackDrapeBindings) + " bgOnly=" + std::to_string(bgOnlyBindings) +
                     " bgOnlyZeroGroup=" + std::to_string(bgOnlyZeroGroupBindings) +
            " unbound=" + std::to_string(unboundBindings) + " rawOverlap=" + std::to_string(rawSourceOverlapPairs) +
            " leafOverlap=" + std::to_string(sourceMeshOverlapPairs) + " meshOverlap=" +
            std::to_string(drawableMeshOverlapPairs) + " meshDuplicates=" + std::to_string(drawableMeshDuplicates) +
                     " meshNoTile=" + std::to_string(drawableMeshNoTile) +
            " meshMissing=" + std::to_string(drawableMeshMissing) + " meshExtra=" + std::to_string(drawableMeshExtra) +
            " rawIDs=" + std::to_string(rawIdealIDs.size()) + " maskRoots=" + std::to_string(terrainMaskRoots) +
            " maskLeaves=" + std::to_string(terrainMaskLeaves) + " leafIDs=" + std::to_string(currentIdealIDs.size()) +
            " maskDepth=" + std::to_string(terrainMaskMaxDepth) + " atomicHold=" + std::to_string(terrainAtomicHold) +
            " atomicDrop=" + std::to_string(terrainAtomicDrop) +
            " atomicEligible=" + std::to_string(previousCoverDrawableBacked) + " candidateBindings=" +
            std::to_string(candidateBindingCount) + " candidateReady=" + std::to_string(candidateReadyBindings) +
                     " candidateUnbound=" + std::to_string(candidateUnboundBindings) +
            " candidateEmptyDEM=" + std::to_string(candidateEmptyDEMBindings) +
            " holdUnbound=" + std::to_string(holdForUnboundDrape) + " holdEmptyDEM=" + std::to_string(holdForEmptyDEM) +
                     " candidateOwnRasterEmpty=" + std::to_string(candidateOwnRasterEmpty) +
                     " candidateOwnRasterPartial=" + std::to_string(candidateOwnRasterPartial) +
                     " boundRasterEmpty=" + std::to_string(boundRasterEmptyBindings) +
            " boundRasterPartial=" + std::to_string(boundRasterPartialBindings) + " boundRasterUnknown=" +
            std::to_string(boundRasterUnknownBindings) + " parked=" + std::to_string(retiredDrapeTargetsByTile.size()) +
            " demTextures=" + std::to_string(demTexturesByTile.size()) + " demShared=" +
            std::to_string(bucketSharedDEMTextures) + " drapeTargets=" + std::to_string(drapeCache.size()) +
                     " terrainDrawables=" + std::to_string(lg->getDrawableCount()));
    }
    // .46-diag: 1 Hz device-visible summary — the per-class bind counts are
    // the flicker signature (any sustained bgOnly/zeroGroup/unbound during a
    // flight = meshes showing clear/basemap plates instead of imagery).
    if (klattraFlyDiag()) {
        static std::chrono::steady_clock::time_point lastEmit{};
        const auto nowTp = std::chrono::steady_clock::now();
        if (nowTp - lastEmit >= std::chrono::seconds(1)) {
            lastEmit = nowTp;
            // ahead= strip tiles added this update; aheadSpan10= corridor
            // length in TENTHS of a tile (0 = lookahead gate closed).
            klattraDumpEmit(
                "[KLATTRA FLYDIAG] terrain bindings=" + std::to_string(currentBindings.size()) +
                " ready=" + std::to_string(readyBindings) + " fallback=" + std::to_string(fallbackDrapeBindings) +
                " bgOnly=" + std::to_string(bgOnlyBindings) + " zeroGroup=" + std::to_string(bgOnlyZeroGroupBindings) +
                " unbound=" + std::to_string(unboundBindings) + " parked=" +
                std::to_string(retiredDrapeTargetsByTile.size()) + " targets=" + std::to_string(drapeCache.size()) +
                " demTex=" + std::to_string(demTexturesByTile.size()) + " demShared=" +
                std::to_string(bucketSharedDEMTextures) + " rawOverlap=" + std::to_string(rawSourceOverlapPairs) +
                " leafOverlap=" + std::to_string(sourceMeshOverlapPairs) + " meshOverlap=" +
                std::to_string(drawableMeshOverlapPairs) + " meshDuplicates=" + std::to_string(drawableMeshDuplicates) +
                            " meshNoTile=" + std::to_string(drawableMeshNoTile) +
                            " meshMissing=" + std::to_string(drawableMeshMissing) +
                " meshExtra=" + std::to_string(drawableMeshExtra) + " rawIDs=" + std::to_string(rawIdealIDs.size()) +
                " maskRoots=" + std::to_string(terrainMaskRoots) + " maskLeaves=" + std::to_string(terrainMaskLeaves) +
                " leafIDs=" + std::to_string(currentIdealIDs.size()) + " maskDepth=" +
                std::to_string(terrainMaskMaxDepth) + " atomicHold=" + std::to_string(terrainAtomicHold) +
                            " candidateBindings=" + std::to_string(candidateBindingCount) +
                            " candidateUnbound=" + std::to_string(candidateUnboundBindings) +
                " candidateEmptyDEM=" + std::to_string(candidateEmptyDEMBindings) +
                            " boundRasterEmpty=" + std::to_string(boundRasterEmptyBindings) +
                " boundRasterPartial=" + std::to_string(boundRasterPartialBindings) + " boundRasterUnknown=" +
                std::to_string(boundRasterUnknownBindings) + " ahead=" + std::to_string(lookaheadStripAdded) +
                            " aheadSpan10=" + std::to_string(static_cast<int64_t>(std::lround(lookaheadSpanTiles * 10.0))));
        }
    }

    // .53 stage diag: advance bake timestamps every frame (frame-accurate
    // latency), decompose the live backlog by pipeline stage at 1 Hz.
    // .54: also track readiness REGRESSIONS — the .53 flight proved the
    // maiden pipeline is ~30 ms while bgOnly/fallback waves persist, so the
    // artifact must be canvases LOSING readiness (resize recreation, raster
    // cover-shift revocations) and rebinding to plates/ancestors until they
    // re-ready. The ready→unready→ready duration distribution is the smear.
    if (stageDiagEnabled()) {
        const auto nowTp = std::chrono::steady_clock::now();
        for (auto& [stageID, stage] : drapeStageByTile) {
            const TerrainDrapeTargetPtr target = drapeCache.get(stageID);
            if (!target) {
                // Pruned from the cache: not visible, no artifact — close any
                // open unready window without recording it.
                stage.wasReadyForTile = false;
                stage.unreadySince = std::chrono::steady_clock::time_point{};
                continue;
            }
            const bool readyNow = klattraDrapeTargetReadyForTile(target, stageID);
            if (stage.wasReadyForTile && !readyNow) {
                stage.regressions++;
                stageRegressionEvents++;
                stage.unreadySince = nowTp;
            } else if (!stage.wasReadyForTile && readyNow &&
                       stage.unreadySince != std::chrono::steady_clock::time_point{}) {
                klattraStagePush(stageReReadyMs, klattraStageMs(stage.unreadySince, nowTp));
                stage.unreadySince = std::chrono::steady_clock::time_point{};
            }
            stage.wasReadyForTile = readyNow;

            if (stage.rasterRouted == std::chrono::steady_clock::time_point{} ||
                stage.bakedWithRaster != std::chrono::steady_clock::time_point{}) {
                continue;
            }
            if (readyNow) {
                stage.bakedWithRaster = nowTp;
                klattraStagePush(stageBakeMs, klattraStageMs(stage.rasterRouted, nowTp));
                if (stage.firstBound != std::chrono::steady_clock::time_point{} && stage.firstBound < nowTp) {
                    // The canvas was NEEDED on screen before its content
                    // finished — this duration IS the visible artifact.
                    klattraStagePush(stageLateMs, klattraStageMs(stage.firstBound, nowTp));
                }
            }
        }
        static std::chrono::steady_clock::time_point lastStageEmit{};
        bool emitFinalRetiredState = false;
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
        const topo_vector::SurfaceDiagnosticState diagnosticState{
            .bindings = currentBindings.size(),
            .ready = readyBindings,
            .candidateBindings = candidateBindingCount,
            .candidateReady = candidateReadyBindings,
            .unbound = unboundBindings,
            .candidateUnbound = candidateUnboundBindings,
            .emptyDEM = emptyDemBindings,
            .candidateEmptyDEM = candidateEmptyDEMBindings,
            .meshMissing = drawableMeshMissing,
            .meshExtra = drawableMeshExtra,
            .meshDuplicates = drawableMeshDuplicates,
            .meshOverlap = drawableMeshOverlapPairs,
            .retiredTargets = retiredDrapeTargetsByTile.size(),
            .cleanupRequests = finlandRetiredCleanupRequests,
            .atomicHold = terrainAtomicHold,
            .atomicDrop = terrainAtomicDrop,
            .pending = drapeWorkPending,
        };
        const bool surfaceQuiescent = topo_vector::isSurfaceDiagnosticQuiescent(diagnosticState);
        emitFinalRetiredState = topo_vector::shouldEmitFinalRetiredSurfaceState(
            stageLastEmittedRetiredTargets,
            stageLastEmittedFinlandCleanup,
            diagnosticState.retiredTargets,
            diagnosticState.cleanupRequests,
            stageLastEmittedSurfaceQuiescent,
            surfaceQuiescent);
#endif
        if (emitFinalRetiredState || nowTp - lastStageEmit >= std::chrono::seconds(1)) {
            lastStageEmit = nowTp;
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
            stageLastEmittedRetiredTargets = retiredDrapeTargetsByTile.size();
            stageLastEmittedFinlandCleanup = finlandRetiredCleanupRequests;
            stageLastEmittedSurfaceQuiescent = surfaceQuiescent;
#endif
            uint32_t waitCover = 0, waitTex = 0, availUnrouted = 0, routedUnbaked = 0, contentReady = 0;
            uint32_t bound0 = 0, bound1 = 0, bound2 = 0, bound3 = 0;
            for (const auto& [stageID, stage] : drapeStageByTile) {
                switch (stage.firstBoundState) {
                    case 0:
                        bound0++;
                        break;
                    case 1:
                        bound1++;
                        break;
                    case 2:
                        bound2++;
                        break;
                    case 3:
                        bound3++;
                        break;
                    default:
                        break;
                }
                if (!drapeCache.get(stageID)) continue; // live canvases only below
                if (stage.bakedWithRaster != std::chrono::steady_clock::time_point{})
                    contentReady++;
                else if (stage.rasterRouted != std::chrono::steady_clock::time_point{})
                    routedUnbaked++;
                else if (stage.rasterAvail != std::chrono::steady_clock::time_point{})
                    availUnrouted++;
                else if (stage.rasterOverlap != std::chrono::steady_clock::time_point{})
                    waitTex++;
                else
                    waitCover++;
            }
            // Format-aware gauge (.58): near-ring-sized targets are RGBA8
            // (4 B/px), everything smaller is packed 565 (2 B/px) unless
            // the diet is disabled.
            const bool gauge565 = klattraPacked565DrapeEnabled();
            static const int32_t normalGaugeNearSize =
                klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_NEAR", klattraDefaultDrapeNearSize);
            static const int32_t normalGaugeMidSize =
                klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_MID", klattraDefaultDrapeMidSize);
            static const int32_t topoGaugeNearSize =
                klattraEnvTargetSize("KLATTRA_TOPO_DRAPE_TARGET_SIZE_NEAR", klattraDefaultTopoDrapeNearSize);
            static const int32_t topoGaugeMidSize =
                klattraEnvTargetSize("KLATTRA_TOPO_DRAPE_TARGET_SIZE_MID", klattraDefaultTopoDrapeMidSize);
            const int32_t gaugeNearSize = useSharedTopoDrapeCohort ? topoGaugeNearSize : normalGaugeNearSize;
            const int32_t gaugeMidSize = useSharedTopoDrapeCohort ? topoGaugeMidSize : normalGaugeMidSize;
            const auto targetBytes = [&](const Size& s) {
                const uint64_t px = static_cast<uint64_t>(s.width) * s.height;
                return px * ((gauge565 && static_cast<int32_t>(s.width) < gaugeNearSize) ? 2 : 4);
            };
            const auto targetTier = [&](const Size& s) {
                if (static_cast<int32_t>(s.width) >= gaugeNearSize) return 0;
                if (static_cast<int32_t>(s.width) >= gaugeMidSize) return 1;
                return 2;
            };
            uint64_t drapeBytes = 0;
            std::array<uint32_t, 3> liveTiers{};
            std::array<uint32_t, 3> retiredTiers{};
            drapeCache.visitAll([&](const OverscaledTileID&, TerrainDrapeTargetPtr& target) {
                if (!target) return;
                drapeBytes += targetBytes(target->getSize());
                ++liveTiers[targetTier(target->getSize())];
            });
            for (const auto& [retiredID, retiredTarget] : retiredDrapeTargetsByTile) {
                (void)retiredID;
                if (!retiredTarget) continue;
                drapeBytes += targetBytes(retiredTarget->getSize());
                ++retiredTiers[targetTier(retiredTarget->getSize())];
            }
            // TerrainDrapeCache has defaulted mipmaps off since .61. Only
            // include the approximate 33% full-chain overhead when its
            // existing opt-in switch is present.
            const bool drapeMips = std::getenv("KLATTRA_DRAPE_MIPS") != nullptr;
            const uint64_t estimatedDrapeBytes = drapeBytes * (drapeMips ? 133 : 100) / 100;
            klattraDumpEmit(
                "[KLATTRA STAGE] now waitCover=" + std::to_string(waitCover) + " waitTex=" + std::to_string(waitTex) +
                " availUnrouted=" + std::to_string(availUnrouted) + " routedUnbaked=" + std::to_string(routedUnbaked) +
                " contentReady=" + std::to_string(contentReady) + " bgOnly=" + std::to_string(bgOnlyBindings) +
                " fallback=" + std::to_string(fallbackDrapeBindings) + " ready=" + std::to_string(readyBindings) +
                " bindings=" + std::to_string(currentBindings.size()) + " rawOverlap=" +
                std::to_string(rawSourceOverlapPairs) + " leafOverlap=" + std::to_string(sourceMeshOverlapPairs) +
                " meshOverlap=" + std::to_string(drawableMeshOverlapPairs) + " meshDuplicates=" +
                std::to_string(drawableMeshDuplicates) + " meshNoTile=" + std::to_string(drawableMeshNoTile) +
                " meshMissing=" + std::to_string(drawableMeshMissing) +
                " meshExtra=" + std::to_string(drawableMeshExtra) + " rawIDs=" + std::to_string(rawIdealIDs.size()) +
                " maskRoots=" + std::to_string(terrainMaskRoots) + " maskLeaves=" + std::to_string(terrainMaskLeaves) +
                " finlandSplits=" + std::to_string(finlandSurfaceSplits) +
                " finlandSplitBudget=" + std::to_string(finlandSplitBudget) +
                " finlandDetailRequested=" + std::to_string(finlandSurfaceDetail.requestedFootprints) +
                " finlandDetailCovered=" + std::to_string(finlandSurfaceDetail.coveredFootprints) +
                " finlandDetailResolved=" + std::to_string(finlandSurfaceDetail.resolvedFootprints) +
                " finlandDetailMinZ=" + std::to_string(finlandSurfaceDetail.minimumLeafZoom) +
                " finlandDetailMaxZ=" + std::to_string(finlandSurfaceDetail.maximumLeafZoom) +
                " finlandDetailRemainingSplits=" + std::to_string(finlandSurfaceDetail.remainingSplits) +
                " finlandExactDrapes=" + std::to_string(exactDrapeIDs.size()) +
                " finlandPlannedAncestors=" + std::to_string(finlandPlannedAncestorBindings) +
                " finlandRetiredCleanup=" + std::to_string(finlandRetiredCleanupRequests) +
                " retiredDrapeTargets=" + std::to_string(retiredDrapeTargetsByTile.size()) +
                " leafIDs=" + std::to_string(currentIdealIDs.size()) + " maskDepth=" +
                std::to_string(terrainMaskMaxDepth) + " atomicHold=" + std::to_string(terrainAtomicHold) +
                " atomicDrop=" + std::to_string(terrainAtomicDrop) +
                " atomicEligible=" + std::to_string(previousCoverDrawableBacked) + " candidateBindings=" +
                std::to_string(candidateBindingCount) + " candidateReady=" + std::to_string(candidateReadyBindings) +
                " candidateUnbound=" + std::to_string(candidateUnboundBindings) + " candidateEmptyDEM=" +
                std::to_string(candidateEmptyDEMBindings) + " holdUnbound=" + std::to_string(holdForUnboundDrape) +
                " holdEmptyDEM=" + std::to_string(holdForEmptyDEM) +
                " candidateOwnRasterEmpty=" + std::to_string(candidateOwnRasterEmpty) + " candidateOwnRasterPartial=" +
                std::to_string(candidateOwnRasterPartial) + " rasterEmpty=" + std::to_string(boundRasterEmptyBindings) +
                " rasterPartial=" + std::to_string(boundRasterPartialBindings) +
                " rasterUnknown=" + std::to_string(boundRasterUnknownBindings) +
                " publishedDrapeTextures=" + std::to_string(publishedDrapeTextures.size()) +
                " frozenPublished=" + std::to_string(frozenPublishedTargetsThisUpdate) +
                " unbound=" + std::to_string(unboundBindings) + " allocFail=" + std::to_string(stageAllocFailEvents) +
                " demShared=" + std::to_string(bucketSharedDEMTextures) + " drape565=" + std::to_string(gauge565) +
                " drapeMips=" + std::to_string(drapeMips) +
                " drapePressureMode=" + std::to_string(drapePressureCapActive) +
                " drapePressureCap=" + std::to_string(klattraDrapePressureTotalCap()) + " drapeCap=" +
                std::to_string(activeDrapePopulationCap) + " drapeVisible=" + std::to_string(terrainMeshIDs.size()) +
                " drapeCapEvicted=" + std::to_string(drapePopulationCapEvicted) +
                " unboundWhy=" + std::to_string(unboundNoTarget) + "/" +
                    std::to_string(unboundNeverAttempted) + "/" + std::to_string(unboundUnrenderable) + "/" +
                    std::to_string(unboundNoAuthority) + "/" + std::to_string(unboundWriterUnacknowledged) + "/" +
                    std::to_string(unboundSourceIncomplete) + "/" + std::to_string(unboundMissingRasterFloor) + "/" +
                    std::to_string(unboundPublishedOther) + " unboundSizes=" +
                    std::to_string(unboundTargetSizes[0]) + "/" + std::to_string(unboundTargetSizes[1]) + "/" +
                    std::to_string(unboundTargetSizes[2]) + "/" + std::to_string(unboundTargetSizes[3]) +
                    " unboundSample=" + (unboundDiagnosticSample.empty() ? "-" : unboundDiagnosticSample) +
                " liveTiers=" + std::to_string(liveTiers[0]) + "/" + std::to_string(liveTiers[1]) + "/" +
                std::to_string(liveTiers[2]) + " retiredTiers=" + std::to_string(retiredTiers[0]) + "/" +
                    std::to_string(retiredTiers[1]) + "/" + std::to_string(retiredTiers[2]) +
                " physMB=" + std::to_string(static_cast<int64_t>(std::lround(klattraPhysFootprintMB()))) +
                " drapeMB=" + std::to_string(static_cast<int64_t>(estimatedDrapeBytes / 1048576)));
            klattraDumpEmit("[KLATTRA STAGE] lat availMs=" + klattraStagePercentiles(stageAvailMs) + " routeMs=" +
                            klattraStagePercentiles(stageRouteMs) + " bakeMs=" + klattraStagePercentiles(stageBakeMs) +
                " lateMs=" + klattraStagePercentiles(stageLateMs) +
                            " firstBind ok=" + std::to_string(bound0) + " ancestor=" + std::to_string(bound1) +
                            " bgOnly=" + std::to_string(bound2) + " hole=" + std::to_string(bound3) +
                " tracked=" + std::to_string(drapeStageByTile.size()));
            uint32_t inCycle = 0;
            for (const auto& [stageID, stage] : drapeStageByTile) {
                if (stage.unreadySince != std::chrono::steady_clock::time_point{} && drapeCache.get(stageID)) {
                    inCycle++;
                }
            }
            klattraDumpEmit("[KLATTRA STAGE] churn regressions=" + std::to_string(stageRegressionEvents) +
                " revokes=" + std::to_string(stageRevokeEvents) +
                            " resizes=" + std::to_string(stageResizeEvents) + " inCycle=" + std::to_string(inCycle) +
                " reReadyMs=" + klattraStagePercentiles(stageReReadyMs));
        }
    }
    if (traceDrape) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] update-end frame=" + std::to_string(drapeTraceFrame) +
                      " bindings=" + std::to_string(currentBindings.size()) + " demTextures=" +
                      std::to_string(demTexturesByTile.size()) + " drapeTargets=" + std::to_string(drapeCache.size()) +
                      " terrainDrawables=" + std::to_string(lg->getDrawableCount()));
    }

    // 1 Hz drawable-side summary at Warning (passes the release log filter),
    // pairing with [KLATTRA COVER] (emission/cap) and [KLATTRA DEM]
    // (rendered pyramid): ideals != backed means on-screen holes right now;
    // emptyDEM counts flat placeholder tiles (origin-level slabs after the
    // 2026-07-04 offset fix). Opt in: KLATTRA_LOG_COVER_SUMMARY=1.
    // Texture-less skips are intentionally not a blanket repaint request.
    // Fresh targets, cap-deferred work, and a target's first publication
    // handoff each request the precise frame they need. A target held on
    // source coverage or a raster floor waits for that external update rather
    // than rebuilding the flat main pass indefinitely.

    if (klattraLogCoverSummary()) {
        static std::chrono::steady_clock::time_point lastLog{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLog >= std::chrono::seconds(1)) {
            lastLog = now;
            std::array<uint32_t, 26> byZ{};
            for (const auto& idealID : currentIdealIDs) {
                ++byZ[std::min<std::size_t>(idealID.canonical.z, byZ.size() - 1)];
            }
            Log::Warning(Event::Render,
                         "[KLATTRA TERRAIN] ideals=" + std::to_string(currentIdealIDs.size()) + " byZ=" +
                             klattraZoomHistogramString(byZ) + " backed=" + std::to_string(drawableBackedTiles.size()) +
                             " ready=" + std::to_string(readyBindings) +
                             " emptyDEM=" + std::to_string(emptyDemBindings) + " drapeFallback=" +
                             std::to_string(fallbackDrapeBindings) + " holds=" + std::to_string(drawableHolds) +
                             " skipsNotReady=" + std::to_string(drawableSkipsNotReady) +
                             " drawables=" + std::to_string(lg->getDrawableCount()) +
                             " pending=" + std::to_string(drapeWorkPending ? 1 : 0));
        }
    }

    // ---- KLATTRA FRAME DUMP -----------------------------------------------
    // Fires once per camera-stillness period (a paused flyover): one Warning
    // line per ideal binding with its full state AND its projected screen
    // rect, plus a header carrying the complete camera state. A user
    // screenshot taken during the same pause can then be attributed
    // pixel-for-pixel: any frozen artifact region either maps to a specific
    // tile line (inspect its state) or to NO tile (cover/frustum gap), and
    // the header parameters are sufficient to replay the cover math offline.
    // Statics are acceptable here: one active terrain per style in practice,
    // and this is a diagnostic aid. Opt in: KLATTRA_FRAME_DUMP=1.
    static const bool frameDumpEnabled = [] {
        const char* v = std::getenv("KLATTRA_FRAME_DUMP");
        return v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    if (frameDumpEnabled) {
        static double lastZoom = -1.0;
        static double lastPitch = -1.0;
        static double lastBearing = 999.0;
        static double lastLat = 999.0;
        static double lastLon = 999.0;
        static uint32_t stillUpdates = 0;
        static bool dumpedThisStillness = false;
        const LatLng dumpCenter = state.getLatLng();
        const bool cameraStill = std::abs(state.getZoom() - lastZoom) < 1e-9 &&
                                 std::abs(state.getPitch() - lastPitch) < 1e-9 &&
                                 std::abs(state.getBearing() - lastBearing) < 1e-9 &&
                                 std::abs(dumpCenter.latitude() - lastLat) < 1e-9 &&
                                 std::abs(dumpCenter.longitude() - lastLon) < 1e-9;
        lastZoom = state.getZoom();
        lastPitch = state.getPitch();
        lastBearing = state.getBearing();
        lastLat = dumpCenter.latitude();
        lastLon = dumpCenter.longitude();
        if (!cameraStill) {
            stillUpdates = 0;
            dumpedThisStillness = false;
        } else if (++stillUpdates >= 3 && !dumpedThisStillness) {
            dumpedThisStillness = true;
            const Size sizePx = state.getSize();
            // Measure the LIVE frustum: unproject the four far-plane clip
            // corners through the same inverse projection the tile cover
            // culls against, and report each corner's ground distance from
            // the map centre. No theory — this is where the far plane
            // actually is this frame.
            std::string farCornersKm;
            {
                const mat4& invProj = state.getInvProjectionMatrix();
                const double worldSize = Projection::worldSize(state.getScale());
                const double centerLatRad = dumpCenter.latitude() * M_PI / 180.0;
                const double mPerWorldPx = std::cos(centerLatRad) * 2.0 * M_PI * util::EARTH_RADIUS_M / worldSize;
                const double centerWX = (dumpCenter.longitude() + 180.0) / 360.0 * worldSize;
                const double centerWY = (0.5 - std::log(std::tan(M_PI / 4.0 + centerLatRad / 2.0)) / (2.0 * M_PI)) *
                                        worldSize;
                for (const auto& [cx, cy] : {std::pair<double, double>{-1.0, 1.0},
                                             std::pair<double, double>{1.0, 1.0},
                                             std::pair<double, double>{-1.0, -1.0},
                                             std::pair<double, double>{1.0, -1.0}}) {
                    const vec4 clip{{cx, cy, 1.0, 1.0}};
                    vec4 world;
                    matrix::transformMat4(world, clip, invProj);
                    if (std::abs(world[3]) < 1e-12) {
                        farCornersKm += " inf";
                        continue;
                    }
                    const double wx = world[0] / world[3];
                    const double wy = world[1] / world[3];
                    const double distKm = std::hypot(wx - centerWX, wy - centerWY) * mPerWorldPx / 1000.0;
                    if (!farCornersKm.empty()) farCornersKm += ",";
                    farCornersKm += std::to_string(distKm);
                }
            }
            klattraDumpEmit(
                "[KLATTRA DUMP] begin size=" + std::to_string(sizePx.width) + "x" + std::to_string(sizePx.height) +
                " zoom=" + std::to_string(state.getZoom()) +
                             " pitchDeg=" + std::to_string(state.getPitch() * 180.0 / M_PI) +
                " bearing=" + std::to_string(state.getBearing()) + " lat=" + std::to_string(dumpCenter.latitude()) +
                " lon=" + std::to_string(dumpCenter.longitude()) + " fov=" + std::to_string(state.getFieldOfView()) +
                " farCornersKm=" + farCornersKm + " ideals=" + std::to_string(currentIdealIDs.size()) + " bindings=" +
                std::to_string(currentBindings.size()) + " drawables=" + std::to_string(lg->getDrawableCount()));
            for (const auto& [idealID, binding] : currentBindings) {
                const double tilesAtZ = std::ldexp(1.0, idealID.canonical.z);
                double minX = 1e12, minY = 1e12, maxX = -1e12, maxY = -1e12;
                bool behindCamera = false;
                for (int corner = 0; corner < 4; ++corner) {
                    const double xf = (idealID.canonical.x + (corner % 2)) / tilesAtZ;
                    const double yf = (idealID.canonical.y + (corner / 2)) / tilesAtZ;
                    const double lon = xf * 360.0 - 180.0;
                    const double latRad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * yf)));
                    vec4 clip;
                    const ScreenCoordinate sc = state.latLngToScreenCoordinate(LatLng{latRad * 180.0 / M_PI, lon},
                                                                               clip);
                    if (clip[3] <= 0.0) {
                        behindCamera = true;
                    }
                    minX = std::min(minX, sc.x);
                    maxX = std::max(maxX, sc.x);
                    minY = std::min(minY, sc.y);
                    maxY = std::max(maxY, sc.y);
                }
                const TerrainDrapeTargetPtr dumpTarget = drapeCache.get(idealID);
                klattraDumpEmit(
                    "[KLATTRA DUMP] tile=" + klattraTileString(idealID) +
                    " src=" + klattraTileString(binding.sourceID) + " empty=" + std::to_string(binding.usedEmptyDEM) +
                        " ready=" + std::to_string(binding.drapeReady) +
                        " fallback=" + std::to_string(binding.usedDrapeFallback) +
                        " drape=" + (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                        " tex=" + std::to_string(binding.drapeTexture ? 1 : 0) +
                        " tgtCompleted=" + std::to_string(dumpTarget ? dumpTarget->getCompletedRenderCount() : 0) +
                        " tgtGroups=" + std::to_string(dumpTarget ? dumpTarget->numLayerGroups() : 0) +
                        " tgtContent=" + std::to_string(dumpTarget ? dumpTarget->numContentLayerGroups() : 0) +
                    " scrX=" + std::to_string(static_cast<int>(minX)) + ".." + std::to_string(static_cast<int>(maxX)) +
                    " scrY=" + std::to_string(static_cast<int>(minY)) + ".." + std::to_string(static_cast<int>(maxY)) +
                        " behind=" + std::to_string(behindCamera ? 1 : 0));
            }
            klattraDumpEmit("[KLATTRA DUMP] end");
        }
        if (stillUpdates > 0 && !dumpedThisStillness) {
            // Keep frames alive until the dump for this stillness has fired —
            // without this, a pause with no pending drape work idles the loop
            // before the third still update and the dump never emits.
            drapeWorkPending = true;
        }
    }
}

float RenderTerrain::getElevation(const UnwrappedTileID& tileID, float x, float y) const {
    // x, y are normalised within the tile in [0, 1] — bilinearly samples the
    // cached DEM image's Mapbox-RGB encoded pixels. Returns 0 if the tile
    // isn't in the current cover set or the encoding can't be decoded yet.
    std::shared_ptr<const PremultipliedImage> image;
    for (const auto& [cachedID, cachedImage] : demImagesByTile) {
        if (cachedID.toUnwrapped() == tileID) {
            image = cachedImage;
            break;
        }
    }
    if (!image || image->size.isEmpty()) {
        return 0.0f;
    }

    const auto w = static_cast<int32_t>(image->size.width);
    const auto h = static_cast<int32_t>(image->size.height);
    const auto* px = image->data.get();
    if (!px) return 0.0f;

#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
    // The source image has DEMData's one-texel neighbor border. Preserve
    // raw invalid heights below: the LatLng query uses them to retain the
    // previous terrain origin when elevation is unavailable.
    const bool padded = w > 2 && h > 2;
    const float offset = padded ? 0.5f : 0.0f;
    const float pX = std::max(0.0f, std::min(1.0f, x)) * static_cast<float>(padded ? w - 2 : w - 1) + offset;
    const float pY = std::max(0.0f, std::min(1.0f, y)) * static_cast<float>(padded ? h - 2 : h - 1) + offset;
#else
    const float pX = std::max(0.0f, std::min(1.0f, x)) * static_cast<float>(w - 1);
    const float pY = std::max(0.0f, std::min(1.0f, y)) * static_cast<float>(h - 1);
#endif
    const int32_t x0 = static_cast<int32_t>(std::floor(pX));
    const int32_t y0 = static_cast<int32_t>(std::floor(pY));
    const int32_t x1 = std::min(x0 + 1, w - 1);
    const int32_t y1 = std::min(y0 + 1, h - 1);
    const float fx = pX - static_cast<float>(x0);
    const float fy = pY - static_cast<float>(y0);

    const auto sample = [&](int32_t xi, int32_t yi) -> float {
        const size_t offset = (static_cast<size_t>(yi) * w + xi) * 4;
        const float r = px[offset + 0];
        const float g = px[offset + 1];
        const float b = px[offset + 2];
        // Mapbox Terrain RGB: height = -10000 + ((R*256*256 + G*256 + B) * 0.1)
        return -10000.0f + (r * 256.0f * 256.0f + g * 256.0f + b) * 0.1f;
    };

    const float e00 = sample(x0, y0);
    const float e10 = sample(x1, y0);
    const float e01 = sample(x0, y1);
    const float e11 = sample(x1, y1);
    const float e0 = e00 + (e10 - e00) * fx;
    const float e1 = e01 + (e11 - e01) * fx;
    return e0 + (e1 - e0) * fy;
}

float RenderTerrain::getElevationWithExaggeration(const UnwrappedTileID& tileID, float x, float y) const {
    return getElevation(tileID, x, y) * getExaggeration();
}

std::optional<float> RenderTerrain::getElevationAtLatLng(const LatLng& latLng) const {
    if (demImagesByTile.empty()) {
        return std::nullopt;
    }

    std::optional<UnwrappedTileID> bestTile;
    float bestX = 0.5f;
    float bestY = 0.5f;
    uint8_t bestZ = 0;

    for (const auto& [cachedID, cachedImage] : demImagesByTile) {
        if (!cachedImage || cachedImage->size.isEmpty()) {
            continue;
        }

        const auto z = cachedID.canonical.z;
        const auto projected = Projection::project(latLng, static_cast<int32_t>(z));
        const auto unwrapped = cachedID.toUnwrapped();
        const auto scale = int64_t{1} << z;
        const double tileX = static_cast<double>(unwrapped.canonical.x) +
                             static_cast<double>(unwrapped.wrap) * static_cast<double>(scale);
        const double tileY = static_cast<double>(unwrapped.canonical.y);
        const double localX = projected.x - tileX;
        const double localY = projected.y - tileY;
        if (localX < 0.0 || localX > 1.0 || localY < 0.0 || localY > 1.0) {
            continue;
        }
        if (!bestTile || z >= bestZ) {
            bestTile = unwrapped;
            bestX = static_cast<float>(localX);
            bestY = static_cast<float>(localY);
            bestZ = z;
        }
    }

    if (!bestTile) {
        return std::nullopt;
    }

    const float elevation = getElevation(*bestTile, bestX, bestY);
    if (elevation <= -500.0f || elevation >= 9000.0f) {
        return std::nullopt;
    }
    return elevation;
}

std::optional<float> RenderTerrain::getRenderedElevationAtLatLng(const LatLng& latLng) const {
    terrain_perf::count(terrain_perf::Counter::AnchorSamples);
    terrain_perf::count(terrain_perf::Counter::BindingCandidates, currentBindings.size());
    const DEMBinding* best = nullptr;
    std::optional<OverscaledTileID> bestID;
    float u = 0.0f, v = 0.0f;
    for (const auto& [idealID, binding] : currentBindings) {
        const auto z = idealID.canonical.z;
        const auto projected = Projection::project(latLng, static_cast<int32_t>(z));
        const double scale = std::exp2(static_cast<double>(z));
        const double x = projected.x - (idealID.canonical.x + idealID.wrap * scale);
        const double y = projected.y - idealID.canonical.y;
        // Only account for round-trip floating point error at a tile edge.
        constexpr double edgeRoundoff = 1e-9;
        if (x < -edgeRoundoff || x > 1.0 + edgeRoundoff || y < -edgeRoundoff || y > 1.0 + edgeRoundoff) continue;
        if (!bestID || z > bestID->canonical.z || (z == bestID->canonical.z && idealID < *bestID)) {
            best = &binding;
            bestID = idealID;
            u = static_cast<float>(std::clamp(x, 0.0, 1.0));
            v = static_cast<float>(std::clamp(y, 0.0, 1.0));
        }
    }
    if (!best || best->usedEmptyDEM || !best->demImage) return std::nullopt;
    return terrain_surface::sample(*best->demImage, u, v, best->demTL, best->demScale);
}

float RenderTerrain::getExaggeration() const {
    return impl->exaggeration;
}

const std::string& RenderTerrain::getSourceID() const {
    return impl->sourceID;
}

bool RenderTerrain::isEnabled() const {
    return !impl->sourceID.empty();
}

const RenderTerrain::TerrainMesh& RenderTerrain::getMesh(gfx::Context& context) {
    if (!mesh) {
        generateMesh(context);
    }
    return *mesh;
}

void RenderTerrain::generateMesh(gfx::Context& context) {
    (void)context;
    // Generate a regular grid mesh for terrain in tile-extent coordinates
    // (0..8192). Each vertex stores [pos.x, pos.y, tex.u, tex.v]; the vertex
    // shader samples the DEM at pos / EXTENT and displaces the grid in metres.
    //
    // Earlier builds added 5 km vertical skirts at every tile edge. At the
    // close, high-pitch drone camera used by trail preview, those curtains
    // routinely crossed the camera frustum and hid the real surface. Terrain
    // now relies on a padded cover plus drape fallback instead of skirt walls.

    const size_t gridSize = MESH_SIZE;
    const size_t verticesPerSide = gridSize + 1;
    const size_t totalMainVertices = verticesPerSide * verticesPerSide;

    std::vector<int16_t> vertices;
    vertices.reserve(totalMainVertices * 4);

    const float posStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);
    const float texStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);

    for (size_t y = 0; y < verticesPerSide; ++y) {
        for (size_t x = 0; x < verticesPerSide; ++x) {
            vertices.push_back(static_cast<int16_t>(x * posStep));
            vertices.push_back(static_cast<int16_t>(y * posStep));
            vertices.push_back(static_cast<int16_t>(x * texStep));
            vertices.push_back(static_cast<int16_t>(y * texStep));
        }
    }

    std::vector<uint16_t> indices;
    indices.reserve(gridSize * gridSize * 6);

    for (size_t y = 0; y < gridSize; ++y) {
        for (size_t x = 0; x < gridSize; ++x) {
            const uint16_t topLeft = static_cast<uint16_t>(y * verticesPerSide + x);
            const uint16_t topRight = topLeft + 1;
            const uint16_t bottomLeft = static_cast<uint16_t>((y + 1) * verticesPerSide + x);
            const uint16_t bottomRight = bottomLeft + 1;
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    mesh = TerrainMesh{
        nullptr, // vertexBuffer
        nullptr, // indexBuffer
        vertices.size() / 4,
        indices.size(),
        std::move(vertices),
        std::move(indices),
        nullptr, // sharedLayoutVertices, built below
        nullptr  // sharedIndexes, built below
    };

    // Build the shared per-drawable inputs once. Every tile drawable hands
    // these same vectors to its builder; the vectors carry their GPU buffer,
    // so all terrain tiles share one vertex and one index buffer.
    auto layoutVertices = std::make_shared<gfx::VertexVector<TerrainLayoutVertex>>();
    layoutVertices->reserve(mesh->vertexCount);
    const auto& rawVertices = mesh->vertices;
    for (size_t index = 0; index + 3 < rawVertices.size(); index += 4) {
        layoutVertices->emplace_back(TerrainLayoutVertex{
            {rawVertices[index + 0], rawVertices[index + 1]},
            {rawVertices[index + 2], rawVertices[index + 3]},
        });
    }
    mesh->sharedLayoutVertices = std::move(layoutVertices);

    auto sharedIndexes = std::make_shared<gfx::IndexVector<gfx::Triangles>>();
    sharedIndexes->reserve(mesh->indexCount);
    const auto& rawIndices = mesh->indices;
    for (size_t index = 0; index + 2 < rawIndices.size(); index += 3) {
        sharedIndexes->emplace_back(rawIndices[index], rawIndices[index + 1], rawIndices[index + 2]);
    }
    mesh->sharedIndexes = std::move(sharedIndexes);
}

std::shared_ptr<gfx::Texture2D> RenderTerrain::getOrCreateEmptyDEMTexture(gfx::Context& context) {
    if (emptyDEMTexture) {
        return emptyDEMTexture;
    }

    // 1×1 RGBA pixel encoding Mapbox Terrain-RGB elevation 0 m:
    //   height = -10000 + (R*65536 + G*256 + B) * 0.1
    // To produce height = 0: (R*65536 + G*256 + B) * 0.1 = 10000  →
    //   R*65536 + G*256 + B = 100000 = 0x0186A0.
    // So (R, G, B) = (0x01, 0x86, 0xA0) = (1, 134, 160). Alpha is unused
    // by the terrain vertex shader's Mapbox-RGB decode.
    auto texture = context.createTexture2D();
    if (!texture) {
        Log::Error(Event::Render, "Failed to create empty DEM texture");
        return nullptr;
    }

    std::array<uint8_t, 4> pixel{{1, 134, 160, 255}};
    auto image = std::make_shared<PremultipliedImage>(Size(1, 1), pixel.data(), pixel.size());
    texture->setImage(image);
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});

    emptyDEMTexture = std::move(texture);
    return emptyDEMTexture;
}

std::unique_ptr<gfx::Drawable> RenderTerrain::createDrawableForTile(gfx::Context& context,
                                                                      gfx::ShaderRegistry& shaders,
                                                                      const OverscaledTileID& idealID,
                                                                      const DEMBinding& binding) {
    auto demTexture = binding.texture;
    // Ensure mesh is generated
    const auto& terrainMesh = getMesh(context);

    if (terrainMesh.vertices.empty() || terrainMesh.indices.empty()) {
        Log::Error(Event::Render, "Terrain mesh is empty, cannot create drawable");
        return nullptr;
    }

    // Get terrain shader
    auto terrainShader = context.getGenericShader(shaders, "TerrainShader");
    if (!terrainShader) {
        Log::Error(Event::Render, "Terrain shader not found");
        return nullptr;
    }

    // Create drawable builder
    auto builder = context.createDrawableBuilder("terrain-tile");
    if (!builder) {
        Log::Error(Event::Render, "Failed to create drawable builder for terrain tile");
        return nullptr;
    }

    // Configure builder — terrain is an opaque 3D mesh that occludes itself.
    // The Phase-4 matrix Z-scale fix puts elevation into clip-space depth at
    // the same scale as X/Y world-pixels, so the depth buffer can resolve
    // mountains-in-front vs mountains-behind correctly. setIs3D bypasses the
    // 2D sublayer depth-offset hack that LayerTweaker applies for stacked 2D
    // layers — we want the actual perspective depth.
    builder->setShader(terrainShader);
    builder->setRenderPass(RenderPass::Translucent);
    builder->setDepthType(gfx::DepthMaskType::ReadWrite);
    builder->setColorMode(gfx::ColorMode::unblended());
    builder->setEnableDepth(true);
    builder->setIs3D(true);

    // The mesh is identical for every tile: hand the builder the shared
    // vectors built in generateMesh() so all terrain drawables reference one
    // vertex and one index GPU buffer instead of uploading fresh copies.
    const auto& sharedVertices = terrainMesh.sharedLayoutVertices;
    if (!sharedVertices || !terrainMesh.sharedIndexes) {
        Log::Error(Event::Render, "Terrain mesh shared buffers missing, cannot create drawable");
        return nullptr;
    }

    auto vertexAttributes = context.createVertexAttributeArray();
    if (const auto& attr = vertexAttributes->set(shaders::idTerrainPosVertexAttribute)) {
        attr->setSharedRawData(sharedVertices,
                               offsetof(TerrainLayoutVertex, pos),
                               /*vertexOffset=*/0,
                               sizeof(TerrainLayoutVertex),
                               gfx::AttributeDataType::Short2);
    }
    if (const auto& attr = vertexAttributes->set(shaders::idTerrainTexturePosVertexAttribute)) {
        attr->setSharedRawData(sharedVertices,
                               offsetof(TerrainLayoutVertex, texturePos),
                               /*vertexOffset=*/0,
                               sizeof(TerrainLayoutVertex),
                               gfx::AttributeDataType::Short2);
    }
    builder->setVertexAttributes(std::move(vertexAttributes));
    builder->setRawVertices({}, terrainMesh.vertexCount, gfx::AttributeDataType::Short4);

    // Set index data and segments
    // Create a single segment covering the entire terrain mesh
    SegmentVector segments;
    segments.emplace_back(0, // vertex offset
                          0, // index offset
                          terrainMesh.vertexCount, // vertex count
                          terrainMesh.indexCount); // index count

    builder->setSegments(gfx::Triangles(), terrainMesh.sharedIndexes, segments.data(), segments.size());

    if (demTexture) {
        builder->setTexture(demTexture, 0); // slot 0 = demTexture
    }

    // Bind the resolved drape texture as the surface colour input. Usually
    // this is the ideal tile's own target, but during zoom-in it can be a
    // ready ancestor target with binding.drapeTL / binding.drapeScale
    // remapping the child's UVs into the parent sub-rect. A not-content-ready
    // texture (own target baked with background only, imagery still
    // streaming) is intentionally accepted — relief in basemap colours beats
    // a hole, and the target rebakes in place when content arrives.
    if (binding.drapeTexture) {
        if (klattraLogDrapeTrace()) {
            auto drape = binding.drapeID ? drapeCache.get(*binding.drapeID) : nullptr;
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] create-terrain-drawable-bind ideal=" + klattraTileString(idealID) +
                          " source=" + klattraTileString(binding.sourceID) +
                          " drape=" + (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                          " demTexture=" + klattraTexturePtrString(demTexture) +
                          " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                          " drapeTexture=" + klattraTexturePtrString(binding.drapeTexture) +
                          " drapeFallback=" + std::to_string(binding.usedDrapeFallback) +
                          " drapeTL=" + std::to_string(binding.drapeTL[0]) + "," + std::to_string(binding.drapeTL[1]) +
                          " drapeScale=" + std::to_string(binding.drapeScale) +
                          " drapeTarget=" + (drape ? drape->getDebugName() : std::string("none")) +
                          " drapePtr=" + std::to_string(reinterpret_cast<uintptr_t>(drape.get())) +
                          " drapeCompleted=" + std::to_string(drape ? drape->getCompletedRenderCount() : 0) +
                          " drapeGroups=" + std::to_string(drape ? drape->numLayerGroups() : 0) +
                          " drapeContentGroups=" + std::to_string(drape ? drape->numContentLayerGroups() : 0) +
                          " drapeDrawables=" + std::to_string(klattraDrapeDrawableCount(drape)));
        }
        builder->setTexture(binding.drapeTexture, 1); // slot 1 = mapTexture
    } else {
        Log::Warning(Event::Render, "Drape target missing or not ready for ideal tile " + util::toString(idealID));
        return nullptr;
    }

    // Flush to create the drawable
    builder->flush(context);

    // Get the drawable
    auto drawables = builder->clearDrawables();
    if (drawables.empty()) {
        Log::Error(Event::Render, "Failed to create terrain drawable for tile");
        return nullptr;
    }

    if (klattraLogDrapeTrace()) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] create-terrain-drawable-complete ideal=" + klattraTileString(idealID) +
                      " source=" + klattraTileString(binding.sourceID) +
                      " drawableCount=" + std::to_string(drawables.size()));
    }

    // Set tile ID on the drawable — this is the IDEAL tile, used by the
    // tweaker to compute the world-to-tile matrix and to look up the
    // per-drawable DEM binding (texture + UV remap).
    auto& drawable = drawables[0];
    drawable->setTileID(idealID);

    return std::move(drawable);
}

RenderSource* RenderTerrain::findDEMSource(const UpdateParameters& /*parameters*/) {
    // TODO: Implement source lookup
    // This would iterate through parameters.sources to find the raster-dem source
    // matching impl->sourceID
    return nullptr;
}

void RenderTerrain::activateLayerGroup(bool activate, UniqueChangeRequestVec& changes) {
    if (layerGroup) {
        if (activate) {
            changes.emplace_back(std::make_unique<AddLayerGroupRequest>(layerGroup));
        } else {
            changes.emplace_back(std::make_unique<RemoveLayerGroupRequest>(layerGroup));
        }
    }
}

void RenderTerrain::teardown(UniqueChangeRequestVec& changes) {
    // Drop drawables + every drape render target (clearRenderState emits the
    // RemoveRenderTargetRequests), then deregister the terrain layer group.
    // retiredDrapeTargetsByTile needs no requests here: retired targets had
    // their RemoveRenderTargetRequest emitted when they were retired; the
    // map only keeps the texture alive for sampling and is cleared inside
    // clearRenderState.
    clearRenderState(changes);
    activateLayerGroup(false, changes);
    layerGroup.reset();
    demSource = nullptr;
}

void RenderTerrain::reduceMemoryUse(UniqueChangeRequestVec& changes, const bool memoryPressure) {
    // Both ordinary lifecycle cleanup and real memory-pressure warnings drop
    // parked resize predecessors and out-of-cover ancestor fallbacks. Only
    // the explicit pressure path may tighten the sticky drape budget:
    // willResignActive/didEnterBackground also call reduceMemoryUse(), and
    // activating the cap there would permanently reduce visual continuity
    // after opening Control Center or backgrounding the app.
    const bool warnedDrapePressure = cover_hold::nextDrapePressureCapState(
        drapePressureCapActive, klattraDrapePressureTotalCap(), memoryPressure, false);
    if (!drapePressureCapActive && warnedDrapePressure && (stageDiagEnabled() || klattraTraceStderr())) {
        klattraDumpEmit("[KLATTRA DRAPE PRESSURE] active=1 reason=platform-warning cap=" +
                        std::to_string(klattraDrapePressureTotalCap()));
    }
    drapePressureCapActive = warnedDrapePressure;
    const std::size_t cacheBefore = drapeCache.size();
    const std::size_t retiredBefore = retiredDrapeTargetsByTile.size();
    const std::size_t ringSize = drapeRingByTile.size();
    retiredDrapeTargetsByTile.clear();
    auto evicted = drapeCache.pruneIf(
        [&](const OverscaledTileID& id) { return drapeRingByTile.find(id) == drapeRingByTile.end(); });
    if (stageDiagEnabled() || klattraTraceStderr()) {
        klattraDumpEmit("[KLATTRA MEMORY_PRUNE] diag=84 cacheBefore=" + std::to_string(cacheBefore) + " cacheAfter=" +
                        std::to_string(drapeCache.size()) + " evicted=" + std::to_string(evicted.size()) +
                        " retiredCleared=" + std::to_string(retiredBefore) + " ring=" + std::to_string(ringSize) +
                        " bindings=" + std::to_string(currentBindings.size()));
    }
    for (auto& [tileID, target] : evicted) {
        (void)tileID;
        changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(target)));
    }
}

void RenderTerrain::clearRenderState(UniqueChangeRequestVec& changes) {
    if (auto* lg = static_cast<LayerGroup*>(layerGroup.get())) {
        lg->removeDrawablesIf([](gfx::Drawable&) { return true; });
    }

    drapeWorkPending = false;
    lastSharedTopoDrapeCohort.reset();
    if (!currentBindings.empty() || !demImagesByTile.empty()) elevationRevision = nextTerrainElevationRevision();
    currentBindings.clear();
    demImagesByTile.clear();
    demImageRevisionsByTile.clear();
    demTexturesByTile.clear();
    previousIdealIDs.clear();
    previousSourceDrapeIDs.clear();
    stableDrapeCoverFrames = 0;
    retiredDrapeTargetsByTile.clear();
    // Ring hysteresis must not survive a full eviction: stale "was near-ring"
    // entries make re-covered tiles allocate near-ring targets they no longer
    // rank for (avoidable overshoot after a style swap / DEM loss).
    drapeRingByTile.clear();
    pruneHoldAgeByTile.clear();

    auto evicted = drapeCache.pruneIf([](const OverscaledTileID&) { return true; });
    for (auto& [tileID, target] : evicted) {
        (void)tileID;
        changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(target)));
    }
}

} // namespace mbgl
