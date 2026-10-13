#include <mbgl/renderer/render_target.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/offscreen_texture.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/gfx/vertex_attribute.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/raster_drape_cohort.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/io.hpp>
#include <mbgl/util/monotonic_timer.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>

namespace mbgl {

namespace {

bool klattraLogDrapeTrace() {
    static const bool enabled = std::getenv("KLATTRA_LOG_DRAPE_TRACE") != nullptr;
    return enabled;
}

// .46-diag (flyover campaign): device-visible flight diagnostics, default
// ON in this diag dist (KLATTRA_FLYDIAG=0 disables). Warning for the phone
// syslog + stderr for the simulator; per-second budget caps the flood.
// Render-thread only — plain statics.
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

void klattraFlyDiagEmit(const std::string& message) {
    Log::Warning(Event::Render, message);
    static const bool stderrTrace = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
    if (stderrTrace) {
        std::fprintf(stderr, "[KLATTRA_TRACE] %s\n", message.c_str());
    }
}

std::string klattraColorString(const Color& color) {
    return std::to_string(color.r) + "," + std::to_string(color.g) + "," + std::to_string(color.b) + "," +
           std::to_string(color.a);
}

int klattraEnvInt(const char* name, const int fallback) {
    const char* value = std::getenv(name);
    if (!value) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end != value ? static_cast<int>(parsed) : fallback;
}

bool klattraTargetMatchesFilter(const std::string& debugName, const char* filter) {
    if (!filter || !*filter) {
        return false;
    }

    const std::string value(filter);
    if (value == "1" || value == "all") {
        return !debugName.empty();
    }
    if (value == "drape" || value == "terrain") {
        return debugName.find("terrain-drape") != std::string::npos;
    }
    if (value == "hillshade") {
        return debugName.find("hillshade-prep") != std::string::npos;
    }

    return debugName.find(value) != std::string::npos;
}

std::string klattraRenderTargetDumpDir() {
    if (const char* explicitDir = std::getenv("KLATTRA_DUMP_DIR")) {
        if (*explicitDir) {
            return explicitDir;
        }
    }

    std::string base = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/klattra_render_targets";
}

bool klattraEnsureDir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
        return true;
    }
    return false;
}

bool klattraDumpTriggerActive(const char* path) {
    if (!path || !*path) {
        return true;
    }

    static std::string lastPath;
    static std::int64_t lastSec = 0;
    static std::int64_t lastNsec = 0;
    static auto armedUntil = std::chrono::steady_clock::time_point{};

    const auto now = std::chrono::steady_clock::now();
    struct stat info {};
    if (::stat(path, &info) != 0) {
        return now < armedUntil;
    }

#if defined(__APPLE__)
    const std::int64_t sec = static_cast<std::int64_t>(info.st_mtimespec.tv_sec);
    const std::int64_t nsec = static_cast<std::int64_t>(info.st_mtimespec.tv_nsec);
#else
    const std::int64_t sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    const std::int64_t nsec = static_cast<std::int64_t>(info.st_mtim.tv_nsec);
#endif

    if (lastPath != path || sec > lastSec || (sec == lastSec && nsec > lastNsec)) {
        lastPath = path;
        lastSec = sec;
        lastNsec = nsec;
        const int windowMs = std::max(1, klattraEnvInt("KLATTRA_DUMP_TRIGGER_WINDOW_MS", 2000));
        armedUntil = now + std::chrono::milliseconds(windowMs);
        Log::Info(
            Event::Render,
            "[KLATTRA TARGET_PIXELS] arm-trigger file=" + std::string(path) + " window_ms=" + std::to_string(windowMs));
    }

    return now < armedUntil;
}

std::string klattraSafeFilename(std::string value) {
    std::replace_if(
        value.begin(),
        value.end(),
        [](const char c) { return !(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'); },
        '_');
    return value;
}

std::string klattraPercent(const uint64_t count, const uint64_t total) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2)
           << (total ? (100.0 * static_cast<double>(count) / static_cast<double>(total)) : 0.0);
    return stream.str();
}

bool klattraTileCoversWholeTarget(const OverscaledTileID& cover, const OverscaledTileID& target) {
    if (cover.wrap != target.wrap || cover.canonical.z > target.canonical.z) {
        return false;
    }
    return LayerTweaker::tilesOverlap(cover, target);
}

bool klattraTileIsStrictChildOf(const OverscaledTileID& child, const OverscaledTileID& parent) {
    return child.wrap == parent.wrap && child.canonical.z > parent.canonical.z &&
           child.canonical.isChildOf(parent.canonical);
}

bool klattraRasterDrawablesCoverTile(const OverscaledTileID& tileID,
                                     const std::vector<OverscaledTileID>& drawableTileIDs,
                                     uint8_t maxDrawableZoom) {
    for (const auto& drawableTileID : drawableTileIDs) {
        if (klattraTileCoversWholeTarget(drawableTileID, tileID)) {
            return true;
        }
    }

    if (tileID.canonical.z >= maxDrawableZoom) {
        return false;
    }

    for (const auto& child : tileID.canonical.children()) {
        const OverscaledTileID childID(child.z, tileID.wrap, child);
        if (!klattraRasterDrawablesCoverTile(childID, drawableTileIDs, maxDrawableZoom)) {
            return false;
        }
    }
    return true;
}

void klattraMaybeInspectTarget(gfx::OffscreenTexture& texture,
                               const std::string& debugName,
                               const uint64_t completedRenderCount) {
    const char* dumpFilter = std::getenv("KLATTRA_DUMP_RENDER_TARGETS");
    const char* statsFilter = std::getenv("KLATTRA_LOG_TARGET_PIXELS");
    const bool shouldDump = klattraTargetMatchesFilter(debugName, dumpFilter);
    const bool shouldLogStats = shouldDump || klattraTargetMatchesFilter(debugName, statsFilter);
    if (!shouldLogStats) {
        return;
    }

    if (!klattraDumpTriggerActive(std::getenv("KLATTRA_DUMP_TRIGGER_FILE"))) {
        return;
    }

    const int delayMs = klattraEnvInt("KLATTRA_DUMP_RENDER_TARGET_DELAY_MS", 0);
    if (delayMs > 0) {
        static const auto start = std::chrono::steady_clock::now();
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsedMs < delayMs) {
            return;
        }
    }

    const bool repeat = std::getenv("KLATTRA_DUMP_RENDER_TARGETS_REPEAT") != nullptr ||
                        std::getenv("KLATTRA_LOG_TARGET_PIXELS_REPEAT") != nullptr;
    if (!repeat && completedRenderCount > 2) {
        return;
    }

    static uint64_t inspectedTargets = 0;
    const uint64_t maxTargets = std::max(0, klattraEnvInt("KLATTRA_DUMP_RENDER_TARGET_MAX", 80));
    if (inspectedTargets >= maxTargets) {
        return;
    }
    const uint64_t sequence = ++inspectedTargets;

    PremultipliedImage image;
    try {
        image = texture.readStillImage();
    } catch (const std::exception& e) {
        Log::Warning(Event::Render,
                     "[KLATTRA TARGET_PIXELS] read-failed target=" + debugName +
                         " completed=" + std::to_string(completedRenderCount) + " error=" + e.what());
        return;
    }

    const uint64_t total = static_cast<uint64_t>(image.size.width) * image.size.height;
    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    uint64_t sumA = 0;
    uint64_t alphaZero = 0;
    uint64_t rgbWithAlphaZero = 0;
    uint64_t bright = 0;
    uint64_t dark = 0;
    uint64_t greyish = 0;

    const uint8_t* data = image.data.get();
    for (uint64_t i = 0; i < total; ++i) {
        const uint8_t r = data[i * 4 + 0];
        const uint8_t g = data[i * 4 + 1];
        const uint8_t b = data[i * 4 + 2];
        const uint8_t a = data[i * 4 + 3];
        sumR += r;
        sumG += g;
        sumB += b;
        sumA += a;
        if (a <= 1) {
            alphaZero++;
            if (std::max({r, g, b}) > 8) {
                rgbWithAlphaZero++;
            }
        }
        if (r > 235 && g > 235 && b > 235) {
            bright++;
        }
        if (r < 70 && g < 70 && b < 70) {
            dark++;
        }
        if (std::max({r, g, b}) - std::min({r, g, b}) < 12 && r > 85 && r < 205) {
            greyish++;
        }
    }

    std::string pngPath;
    if (shouldDump) {
        const std::string dir = klattraRenderTargetDumpDir();
        if (klattraEnsureDir(dir)) {
            std::ostringstream name;
            name << dir << "/" << std::setw(4) << std::setfill('0') << sequence << "_" << klattraSafeFilename(debugName)
                 << "_c" << completedRenderCount << ".png";
            pngPath = name.str();
            try {
                util::write_file(pngPath, encodePNG(image));
            } catch (const std::exception& e) {
                Log::Warning(Event::Render,
                             "[KLATTRA TARGET_PIXELS] dump-failed target=" + debugName + " path=" + pngPath +
                                 " error=" + e.what());
                pngPath.clear();
            }
        } else {
            Log::Warning(Event::Render,
                         "[KLATTRA TARGET_PIXELS] mkdir-failed dir=" + dir + " errno=" + std::to_string(errno));
        }
    }

    Log::Info(Event::Render,
              "[KLATTRA TARGET_PIXELS] target=" + debugName + " completed=" + std::to_string(completedRenderCount) +
                  " size=" + std::to_string(image.size.width) + "x" + std::to_string(image.size.height) +
                  " avg=" + std::to_string(sumR / std::max<uint64_t>(1, total)) + "," +
                      std::to_string(sumG / std::max<uint64_t>(1, total)) + "," +
                      std::to_string(sumB / std::max<uint64_t>(1, total)) + "," +
                  std::to_string(sumA / std::max<uint64_t>(1, total)) + " alpha0_pct=" +
                  klattraPercent(alphaZero, total) + " rgb_with_alpha0_pct=" + klattraPercent(rgbWithAlphaZero, total) +
                  " bright_pct=" + klattraPercent(bright, total) + " dark_pct=" + klattraPercent(dark, total) +
                  " greyish_pct=" + klattraPercent(greyish, total) + (pngPath.empty() ? "" : " png=" + pngPath));
}

} // namespace

RenderTarget::RenderTarget(gfx::Context& context_, const Size size, const gfx::TextureChannelDataType type)
    : context(context_) {
    offscreenTexture = context.createOffscreenTexture(size, type);
}

RenderTarget::~RenderTarget() {}

const gfx::Texture2DPtr& RenderTarget::getTexture() {
    return offscreenTexture->getTexture();
};

void RenderTarget::setDebugName(std::string name_) {
    debugName = std::move(name_);
    if (offscreenTexture) {
        offscreenTexture->getTexture()->diagSetName(debugName);
    }
}

Size RenderTarget::getSize() const noexcept {
    return offscreenTexture ? offscreenTexture->getSize() : Size{};
}

void RenderTarget::setMipmapped(bool enabled) {
    mipmapped = enabled;
    offscreenTexture->setMipmapped(enabled);
}

void RenderTarget::inspectDebugPixels() {
    if (debugName.empty() || completedRenderCount == 0) {
        return;
    }

    klattraMaybeInspectTarget(*offscreenTexture, debugName + " sampled", completedRenderCount);
}

bool RenderTarget::addLayerGroup(LayerGroupBasePtr layerGroup, const bool replace) {
    const auto index = layerGroup->getLayerIndex();
    const auto result = layerGroupsByLayerIndex.insert(std::make_pair(index, LayerGroupBasePtr{}));
    const auto layerName = layerGroup->getName();
    const auto drawableCount = layerGroup->getDrawableCount();
    if (result.second) {
        // added
        result.first->second = std::move(layerGroup);
        if (klattraLogDrapeTrace() && !debugName.empty()) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] target-add-group target=" + debugName + " index=" + std::to_string(index) +
                          " name=" + layerName + " drawables=" + std::to_string(drawableCount) +
                          " replace=" + std::to_string(replace));
        }
        return true;
    } else {
        // not added
        if (replace) {
            result.first->second = std::move(layerGroup);
            if (klattraLogDrapeTrace() && !debugName.empty()) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] target-replace-group target=" + debugName +
                              " index=" + std::to_string(index) + " name=" + layerName +
                              " drawables=" + std::to_string(drawableCount));
            }
            return true;
        } else {
            return false;
        }
    }
}

bool RenderTarget::removeLayerGroup(const int32_t layerIndex) {
    const auto hit = layerGroupsByLayerIndex.find(layerIndex);
    if (hit != layerGroupsByLayerIndex.end()) {
        layerGroupsByLayerIndex.erase(hit);
        return true;
    } else {
        return false;
    }
}

std::size_t RenderTarget::removeLayerGroupsIf(const std::function<bool(int32_t, const LayerGroupBase&)>& predicate) {
    std::size_t removed = 0;
    for (auto it = layerGroupsByLayerIndex.begin(); it != layerGroupsByLayerIndex.end();) {
        const auto& group = it->second;
        if (group && predicate(it->first, *group)) {
            it = layerGroupsByLayerIndex.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t RenderTarget::numLayerGroups() const noexcept {
    return layerGroupsByLayerIndex.size();
}

size_t RenderTarget::numDrawables() const noexcept {
    size_t count = 0;
    for (const auto& [_, layerGroup] : layerGroupsByLayerIndex) {
        if (layerGroup) {
            count += layerGroup->getDrawableCount();
        }
    }
    return count;
}

size_t RenderTarget::numContentLayerGroups() const noexcept {
    size_t count = 0;
    for (const auto& [layerIndex, layerGroup] : layerGroupsByLayerIndex) {
        if (layerIndex == std::numeric_limits<int32_t>::max() || !layerGroup || layerGroup->empty()) {
            continue;
        }
        count++;
    }
    return count;
}

void RenderTarget::beginDrapeSurfaceFrame(const uint64_t generation,
                                          const OverscaledTileID& tileID,
                                          const bool authorityExpected) {
    pendingDrapeGeneration = generation;
    drapeAuthorityExpected = authorityExpected;
    pendingDrapeTileID = tileID;
    pendingDrapeSourceCoverage.clear();
    pendingDrapeSourceAcknowledgements.clear();
    requiresRasterDrape = false;
    requiredRasterDrapeLayerIndex.reset();
    requiredRasterDrapeSourceID.clear();
}

void RenderTarget::requireRasterDrapeContent(const int32_t layerIndex, const std::string& sourceID) noexcept {
    requiresRasterDrape = true;
    requiredRasterDrapeLayerIndex = layerIndex;
    requiredRasterDrapeSourceID = sourceID;
    if (!sourceID.empty()) pendingDrapeSourceCoverage.emplace(sourceID, false);
}

void RenderTarget::declareDrapeSourceRequirement(const uint64_t generation, const std::string& sourceKey) {
    if (!generation || generation != pendingDrapeGeneration || sourceKey.empty()) return;
    pendingDrapeSourceCoverage.emplace(sourceKey, false);
}

void RenderTarget::noteDrapeSourceCoverage(const uint64_t generation,
                                           const std::string& sourceID,
                                           const bool complete) {
    if (!generation || generation != pendingDrapeGeneration || sourceID.empty()) return;
    const auto it = pendingDrapeSourceCoverage.find(sourceID);
    if (it != pendingDrapeSourceCoverage.end()) {
        it->second = complete;
        pendingDrapeSourceAcknowledgements.insert(sourceID);
    }
}

namespace {
bool klattraDrapeBakeSkipEnabled() {
    static const bool enabled = std::getenv("KLATTRA_DISABLE_DRAPE_BAKE_SKIP") == nullptr;
    return enabled;
}

// Unchanged-canvas frames between forced re-bakes. Bounds staleness from any
// input the fingerprint cannot see (in-place texture edits such as a pattern
// atlas update).
uint32_t klattraDrapeForcedRebakeFrames() {
    static const uint32_t frames = [] {
        const char* v = std::getenv("KLATTRA_DRAPE_FORCED_REBAKE_FRAMES");
        return v && *v ? static_cast<uint32_t>(std::max(0, std::atoi(v))) : 120u;
    }();
    return frames;
}

void klattraMix(uint64_t& seed, const uint64_t value) noexcept {
    seed ^= value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
}

bool drapeAttemptBlockedUntilExternalUpdate(const RenderTarget::DrapeRenderDiagnostic& diagnostic,
                                            const uint64_t pendingGeneration) noexcept {
    if (diagnostic.generation != pendingGeneration) {
        return false;
    }
    switch (diagnostic.reason) {
        case RenderTarget::DrapeRenderDiagnosticReason::NoAuthoritativeRequirement:
        case RenderTarget::DrapeRenderDiagnosticReason::WriterUnacknowledged:
        case RenderTarget::DrapeRenderDiagnosticReason::SourceIncomplete:
        case RenderTarget::DrapeRenderDiagnosticReason::MissingRasterFloor:
            return true;
        case RenderTarget::DrapeRenderDiagnosticReason::NeverAttempted:
        case RenderTarget::DrapeRenderDiagnosticReason::Unrenderable:
        case RenderTarget::DrapeRenderDiagnosticReason::Published:
            return false;
    }
    return false;
}
} // namespace

uint64_t RenderTarget::drapeContentFingerprint(const Color& canvasClearColor, const double cameraZoom) const {
    uint64_t seed = 0;
    const auto& texture = offscreenTexture->getTexture();
    klattraMix(seed, reinterpret_cast<uintptr_t>(texture.get()));
    klattraMix(seed, (uint64_t{texture->getSize().width} << 32) | texture->getSize().height);
    for (const float channel : {canvasClearColor.r, canvasClearColor.g, canvasClearColor.b, canvasClearColor.a}) {
        klattraMix(seed, static_cast<uint64_t>(std::lround(channel * 65535.0f)));
    }
    // Draped line widths follow the camera zoom; 1/8 zoom steps keep widths
    // current without re-baking every frame of a pinch.
    klattraMix(seed, static_cast<uint64_t>(std::llround(cameraZoom * 8.0)));
    for (const auto& [layerIndex, layerGroup] : layerGroupsByLayerIndex) {
        if (!layerGroup) continue;
        klattraMix(seed, static_cast<uint64_t>(static_cast<uint32_t>(layerIndex)));
        klattraMix(seed, reinterpret_cast<uintptr_t>(layerGroup.get()));
        klattraMix(seed, layerGroup->getEnabled() ? 1u : 2u);
        for (const auto& weakTweaker : layerGroup->getLayerTweakers()) {
            if (const auto tweaker = weakTweaker.lock()) {
                klattraMix(seed, reinterpret_cast<uintptr_t>(tweaker->getEvaluatedPropertiesIdentity()));
            }
        }
        // Order-independent over the group's drawables.
        uint64_t sum = 0, product = 1;
        std::size_t count = 0;
        visitLayerGroupDrawables(*layerGroup, [&](gfx::Drawable& drawable) {
            uint64_t item = static_cast<uint64_t>(drawable.getID().id());
            klattraMix(item, drawable.getEnabled() ? 1u : 2u);
            klattraMix(item, reinterpret_cast<uintptr_t>(drawable.getTexture(0).get()));
            klattraMix(item, reinterpret_cast<uintptr_t>(drawable.getTexture(1).get()));
            sum += item;
            product *= item | 1u;
            ++count;
        });
        klattraMix(seed, sum);
        klattraMix(seed, product);
        klattraMix(seed, count);
    }
    return seed;
}

bool RenderTarget::drapeVerticesModifiedSince(const std::chrono::duration<double> time) const {
    bool modified = false;
    for (const auto& [layerIndex, layerGroup] : layerGroupsByLayerIndex) {
        (void)layerIndex;
        if (!layerGroup || modified) continue;
        visitLayerGroupDrawables(*layerGroup, [&](gfx::Drawable& drawable) {
            if (!modified && drawable.getVertexAttributes() && drawable.getVertexAttributes()->isModifiedAfter(time)) {
                modified = true;
            }
        });
    }
    return modified;
}

bool RenderTarget::needsDrapeRenderAttempt(const uint64_t minimumCompletedRenders) const noexcept {
    return raster_drape::unbakedTargetNeedsAutomaticFrame(
        completedRenderCount,
        minimumCompletedRenders,
        pendingDrapeGeneration,
        lastDrapeRenderDiagnostic.generation,
        drapeAttemptBlockedUntilExternalUpdate(lastDrapeRenderDiagnostic, pendingDrapeGeneration));
}

bool RenderTarget::hasPendingDrapePublication() const noexcept {
    if (firstDrapePublicationNeedsConsumerFrame) {
        return true;
    }

    return raster_drape::publicationNeedsAutomaticFrameRetry(pendingDrapeGeneration,
                                                             publishedDrapeGeneration,
                                                             requiresRasterDrape,
                                                             pendingDrapeSourceCoverage.size(),
                                                             lastDrapeRenderDiagnostic.generation,
                                                             drapeAttemptBlockedUntilExternalUpdate(
                                                                 lastDrapeRenderDiagnostic, pendingDrapeGeneration));
}

bool RenderTarget::hasRasterFloorForTile(const OverscaledTileID& tileID,
                                         const std::optional<int32_t> layerIndexFilter) const noexcept {
    bool hasFloor = false;
    for (const auto& [layerIndex, layerGroup] : layerGroupsByLayerIndex) {
        if (hasFloor || (layerIndexFilter && layerIndex != *layerIndexFilter) ||
            layerIndex == std::numeric_limits<int32_t>::max() || !layerGroup || layerGroup->empty() ||
            layerGroup->getName().find("raster-drape") == std::string::npos) {
            continue;
        }
        visitLayerGroupDrawables(*layerGroup, [&](gfx::Drawable& drawable) {
            const auto& drawableTileID = drawable.getTileID();
            if (drawableTileID && klattraTileCoversWholeTarget(*drawableTileID, tileID)) {
                hasFloor = true;
            }
        });
    }
    return hasFloor;
}

bool RenderTarget::hasRasterDrawableCoveringTile(const OverscaledTileID& tileID) const noexcept {
    std::vector<OverscaledTileID> drawableTileIDs;
    uint8_t maxDrawableZoom = tileID.canonical.z;

    for (const auto& [layerIndex, layerGroup] : layerGroupsByLayerIndex) {
        if (layerIndex == std::numeric_limits<int32_t>::max() || !layerGroup || layerGroup->empty() ||
            layerGroup->getName().find("raster-drape") == std::string::npos) {
            continue;
        }

        visitLayerGroupDrawables(*layerGroup, [&](gfx::Drawable& drawable) {
            const auto& drawableTileID = drawable.getTileID();
            if (!drawableTileID) return;

            if (klattraTileCoversWholeTarget(*drawableTileID, tileID)) {
                drawableTileIDs.clear();
                drawableTileIDs.push_back(*drawableTileID);
                maxDrawableZoom = drawableTileID->canonical.z;
                return;
            }
            if (klattraTileIsStrictChildOf(*drawableTileID, tileID)) {
                drawableTileIDs.push_back(*drawableTileID);
                maxDrawableZoom = std::max(maxDrawableZoom, drawableTileID->canonical.z);
            }
        });

        if (drawableTileIDs.size() == 1 && klattraTileCoversWholeTarget(drawableTileIDs.front(), tileID)) {
            return true;
        }
    }

    if (drawableTileIDs.empty()) {
        return false;
    }

    // A satellite source can be one or more zoom levels sharper than the DEM
    // drape target. No single child tile covers the whole target, but a full
    // child set does. Treat that as ready so terrain can replace the flat
    // main pass once the complete colour surface exists.
    return klattraRasterDrawablesCoverTile(tileID, drawableTileIDs, maxDrawableZoom);
}

std::optional<uint8_t> RenderTarget::diagnosticRasterSurfaceZoomForTile(
    const OverscaledTileID& tileID,
    const std::optional<int32_t> layerIndexFilter) const noexcept {
    std::map<uint8_t, std::vector<OverscaledTileID>, std::greater<uint8_t>> drawableIDsByZoom;
    for (const auto& [layerIndex, layerGroup] : layerGroupsByLayerIndex) {
        if ((layerIndexFilter && layerIndex != *layerIndexFilter) ||
            layerIndex == std::numeric_limits<int32_t>::max() || !layerGroup || layerGroup->empty() ||
            layerGroup->getName().find("raster-drape") == std::string::npos) {
            continue;
        }
        visitLayerGroupDrawables(*layerGroup, [&](gfx::Drawable& drawable) {
            if (const auto& drawableTileID = drawable.getTileID()) {
                drawableIDsByZoom[drawableTileID->canonical.z].push_back(*drawableTileID);
            }
        });
    }

    for (const auto& [zoom, drawableTileIDs] : drawableIDsByZoom) {
        if (raster_drape::coversTargetAtAnyResolution(tileID, drawableTileIDs)) {
            return zoom;
        }
    }
    return std::nullopt;
}

static const LayerGroupBasePtr no_group;

const LayerGroupBasePtr& RenderTarget::getLayerGroup(const int32_t layerIndex) const {
    const auto hit = layerGroupsByLayerIndex.find(layerIndex);
    return (hit == layerGroupsByLayerIndex.end()) ? no_group : hit->second;
}

void RenderTarget::upload(gfx::UploadPass& uploadPass) {
    visitLayerGroups(([&](LayerGroupBase& layerGroup) { layerGroup.upload(uploadPass); }));
}

void RenderTarget::render(RenderOrchestrator& orchestrator, const RenderTree& renderTree, PaintParameters& parameters) {
    // Under memory pressure the target's MTLTexture allocation can fail;
    // encoding a pass with no valid attachments aborts the app. Skip the
    // pass — the target stays not-ready, consumers keep using fallbacks,
    // and the allocation is retried on a later frame.
    lastDrapeRenderDiagnostic = {};
    lastDrapeRenderDiagnostic.generation = pendingDrapeGeneration;
    lastDrapeRenderDiagnostic.declaredRequirements = pendingDrapeSourceCoverage.size();
    lastDrapeRenderDiagnostic.acknowledgedRequirements = pendingDrapeSourceAcknowledgements.size();
    for (const auto& [sourceID, complete] : pendingDrapeSourceCoverage) {
        if (!complete) {
            ++lastDrapeRenderDiagnostic.incompleteRequirements;
            if (lastDrapeRenderDiagnostic.blockedSourceKey.empty()) {
                lastDrapeRenderDiagnostic.blockedSourceKey = sourceID;
            }
        }
    }

    if (!offscreenTexture || !offscreenTexture->isRenderable()) {
        lastDrapeRenderDiagnostic.reason = DrapeRenderDiagnosticReason::Unrenderable;
        return;
    }

    // Terrain targets are mutable layer-group containers backed by a single
    // texture object. Holding a binding therefore does not preserve pixels if
    // this pass clears and rebakes that same texture with an incomplete source
    // cohort. Publish only coherent generations; skipped passes deliberately
    // leave the last presented texture untouched.
    if (pendingDrapeGeneration) {
        const bool hasAuthoritativeRequirement = raster_drape::hasAuthoritativePublicationRequirement(
            requiresRasterDrape, pendingDrapeSourceCoverage.size());
        // Only Swedish/Norwegian official Topo, Finnish OpenMapTiles and the
        // Esri terrain raster declare requirements that can prove coherence.
        // A style with none of them (Germany and its neighbours, the native
        // aerial blend) could never publish, which left Android terrain flat
        // (traska.100). Those generations publish immediately, as before
        // generations existed: a clear reads as ground, a hold as a hole.
        bool coherent = hasAuthoritativeRequirement || !drapeAuthorityExpected;
        for (const auto& [sourceID, complete] : pendingDrapeSourceCoverage) {
            (void)sourceID;
            coherent = coherent && complete;
        }
        if (requiresRasterDrape) {
            const auto sourceCoverage = pendingDrapeSourceCoverage.find(requiredRasterDrapeSourceID);
            coherent = coherent && !requiredRasterDrapeSourceID.empty() && requiredRasterDrapeLayerIndex &&
                       sourceCoverage != pendingDrapeSourceCoverage.end() && sourceCoverage->second &&
                       pendingDrapeTileID && hasRasterFloorForTile(*pendingDrapeTileID, requiredRasterDrapeLayerIndex);
        }
        if (!coherent) {
            if (!hasAuthoritativeRequirement) {
                lastDrapeRenderDiagnostic.reason = DrapeRenderDiagnosticReason::NoAuthoritativeRequirement;
            } else if (lastDrapeRenderDiagnostic.incompleteRequirements > 0) {
                lastDrapeRenderDiagnostic.reason =
                    lastDrapeRenderDiagnostic.acknowledgedRequirements < pendingDrapeSourceCoverage.size()
                        ? DrapeRenderDiagnosticReason::WriterUnacknowledged
                        : DrapeRenderDiagnosticReason::SourceIncomplete;
            } else {
                lastDrapeRenderDiagnostic.reason = DrapeRenderDiagnosticReason::MissingRasterFloor;
            }
            if (klattraLogDrapeTrace() && !debugName.empty()) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] target-render-hold target=" + debugName +
                              " pending=" + std::to_string(pendingDrapeGeneration) +
                              " published=" + std::to_string(publishedDrapeGeneration) +
                              " sources=" + std::to_string(pendingDrapeSourceCoverage.size()) +
                              " raster=" + std::to_string(requiresRasterDrape));
            }
            return;
        }
    }

    // KLATTRA diagnostics: label the colour attachment with the target's
    // debug name so sampled-before-rendered trace lines identify WHICH
    // target (hillshade-prep vs drape) was read too early.
    offscreenTexture->getTexture()->diagSetName(debugName);

    // Clear to the style's evaluated background colour (same value the main
    // pass clears with), NOT the member default black: a drape bake whose
    // background drawable hasn't been routed in yet — or never is, for some
    // far targets — otherwise produces an opaque BLACK texture, and terrain
    // meshes bound to it render as black rectangles until the tile's imagery
    // arrives or a cover churn recreates the target (2026-07-04 device
    // finding: mid-flight black rectangles that outlived pauses). With the
    // basemap colour as the floor, a contentless bake reads as unloaded
    // basemap instead of a void.
    Color drapeClearColor = renderTree.getParameters().backgroundColor;
    // .60 tint (opt-in since .61): yellow canvas-clear attribution.
    if (std::getenv("KLATTRA_TINT_WRITERS") != nullptr) {
        drapeClearColor = Color{1.0f, 1.0f, 0.0f, 1.0f};
    }

    // Re-bake a published drape canvas only when its inputs changed. Baking
    // all ~100 terrain canvases every frame queued ~0.6 s of GPU work per
    // interactive frame on the Android emulator, and a tap's synchronous
    // feature query waited behind it into an ANR. The fingerprint covers the
    // attachment, clear colour, camera zoom, layer groups, tweaker
    // properties and drawables; vertex edits are checked by time.
    std::optional<uint64_t> drapeFingerprint;
    const auto bakeStartTime = util::MonotonicTimer::now();
    if (pendingDrapeGeneration && klattraDrapeBakeSkipEnabled()) {
        drapeFingerprint = drapeContentFingerprint(drapeClearColor, parameters.state.getZoom());
        if (completedRenderCount > 0 && *drapeFingerprint == lastDrapeBakeFingerprint &&
            skippedDrapeBakes < klattraDrapeForcedRebakeFrames() && !drapeVerticesModifiedSince(lastDrapeBakeTime)) {
            ++skippedDrapeBakes;
            lastDrapeRenderDiagnostic.reason = DrapeRenderDiagnosticReason::Published;
            publishedDrapeGeneration = pendingDrapeGeneration;
            publishedRequiresRasterDrape = requiresRasterDrape;
            publishedHasRasterFloor = pendingDrapeTileID &&
                                      hasRasterFloorForTile(*pendingDrapeTileID, requiredRasterDrapeLayerIndex);
            return;
        }
    }

    // .46-diag: the first bakes are where flicker windows live — log the
    // ACTUAL clear colour + group population for every target's first few
    // bakes. A paper-beige clear during a satellite flight is the stale
    // sticky-background bug; groups=0 is a raw clear plate.
    if (klattraFlyDiag() && !debugName.empty() && completedRenderCount < 3 && klattraFlyDiagBudget()) {
        std::size_t diagDrawables = 0;
        visitLayerGroups([&](LayerGroupBase& layerGroup) { diagDrawables += layerGroup.getDrawableCount(); });
        klattraFlyDiagEmit(
            "[KLATTRA FLYDIAG] bake target=" + debugName + " completed=" + std::to_string(completedRenderCount) +
            " groups=" + std::to_string(numLayerGroups()) + " drawables=" + std::to_string(diagDrawables) +
                           " clear=" + klattraColorString(drapeClearColor));
    }

    if (klattraLogDrapeTrace() && !debugName.empty()) {
        const auto size = offscreenTexture->getSize();
        std::size_t drawableCount = 0;
        visitLayerGroups([&](LayerGroupBase& layerGroup) {
            drawableCount += layerGroup.getDrawableCount();
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] target-group target=" + debugName + " group=" + layerGroup.getName() +
                          " index=" + std::to_string(layerGroup.getLayerIndex()) +
                          " enabled=" + std::to_string(layerGroup.getEnabled()) +
                          " drawables=" + std::to_string(layerGroup.getDrawableCount()));
        });
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] target-render-begin target=" + debugName +
                      " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(this)) +
                      " size=" + std::to_string(size.width) + "x" + std::to_string(size.height) +
                      " groups=" + std::to_string(numLayerGroups()) + " drawables=" + std::to_string(drawableCount) +
                      " completedBefore=" + std::to_string(completedRenderCount) +
                      " clear=" + klattraColorString(drapeClearColor));
    }

    parameters.renderPass = parameters.encoder->createRenderPass(
        "render target",
        {.renderable = *offscreenTexture, .clearColor = drapeClearColor, .clearDepth = 1.0f, .clearStencil = {}});
    context.bindGlobalUniformBuffers(*parameters.renderPass);

    const gfx::ScissorRect prevScissorRect = parameters.scissorRect;
    const auto& size = getTexture()->getSize();
    parameters.scissorRect = {.x = 0, .y = 0, .width = size.width, .height = size.height};

    // Run layer tweakers to update any dynamic elements
    parameters.currentLayer = 0;
    visitLayerGroups([&](LayerGroupBase& layerGroup) {
        layerGroup.runTweakers(renderTree, parameters);
        parameters.currentLayer++;
    });

    // draw layer groups, opaque pass
    parameters.pass = RenderPass::Opaque;
    parameters.depthRangeSize = 1 -
                                (numLayerGroups() + 2) * PaintParameters::numSublayers * PaintParameters::depthEpsilon;

    parameters.currentLayer = 0;
    visitLayerGroupsReversed([&](LayerGroupBase& layerGroup) {
        layerGroup.render(orchestrator, parameters);
        parameters.currentLayer++;
    });

    // draw layer groups, translucent pass
    parameters.pass = RenderPass::Translucent;
    parameters.depthRangeSize = 1 -
                                (numLayerGroups() + 2) * PaintParameters::numSublayers * PaintParameters::depthEpsilon;

    parameters.currentLayer = static_cast<uint32_t>(numLayerGroups()) - 1;
    visitLayerGroups([&](LayerGroupBase& layerGroup) {
        layerGroup.render(orchestrator, parameters);
        if (parameters.currentLayer > 0) {
            parameters.currentLayer--;
        }
    });

    context.unbindGlobalUniformBuffers(*parameters.renderPass);
    parameters.renderPass.reset();
    parameters.encoder->present(*offscreenTexture);
    const bool firstDrapePublication = pendingDrapeGeneration > 0 && completedRenderCount == 0;
    completedRenderCount++;
    if (drapeFingerprint) {
        lastDrapeBakeFingerprint = *drapeFingerprint;
        lastDrapeBakeTime = bakeStartTime;
        skippedDrapeBakes = 0;
    }
    lastDrapeRenderDiagnostic.reason = DrapeRenderDiagnosticReason::Published;
    if (pendingDrapeGeneration) {
        publishedDrapeGeneration = pendingDrapeGeneration;
        publishedRequiresRasterDrape = requiresRasterDrape;
        publishedHasRasterFloor = pendingDrapeTileID &&
                                  hasRasterFloorForTile(*pendingDrapeTileID, requiredRasterDrapeLayerIndex);
        if (firstDrapePublication) {
            // The terrain update runs before this offscreen pass, so it could
            // not create a consumer drawable for the first published texture
            // yet. Request exactly one handoff frame; the next terrain update
            // acknowledges it even when DEM data still needs an external tile
            // update, avoiding another self-wake loop.
            firstDrapePublicationNeedsConsumerFrame = true;
        }
    }

    if (!debugName.empty()) {
        klattraMaybeInspectTarget(*offscreenTexture, debugName, completedRenderCount);
    }

    if (klattraLogDrapeTrace() && !debugName.empty()) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] target-render-end target=" + debugName +
                      " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(this)) +
                      " completedAfter=" + std::to_string(completedRenderCount));
    }

    parameters.scissorRect = prevScissorRect;
}

} // namespace mbgl
