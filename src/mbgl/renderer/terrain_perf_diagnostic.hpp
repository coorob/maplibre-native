#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
#include <mbgl/util/logging.hpp>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <time.h>
#endif

namespace mbgl::terrain_perf {

enum class Phase : size_t {
    RenderFrame, CreateTree, TerrainUpdate, PrepareSymbols, HeightStreams, LayerUpdate,
    InitialUpload, LayerUpload, ProcessChanges, OffscreenFlush, Draw2D, TerrainDraw,
    Post3D, DepthCapture, DepthQuery, DepthSetup, DepthBlit, SymbolUBO, PostRender,
    Present, EndFrame, GLCleanup, DEMClone, OffscreenDraw, Draw3D, BeginFrame, RenderJobs, UBODefragment, Count
};
enum class Counter : size_t {
    Frames, TerrainFrames, SymbolBuckets, CacheReset, CacheHit, CacheMiss, AnchorSamples,
    BindingCandidates, HeightVertices, HeightRebuilds, RawDEMRevision, MeshRevision,
    DEMClones, DEMCloneBytes, UBOCreate, UBOUpdate, DepthCaptures, DepthFailures,
    GoldfishLayerRebuilds, Count
};

#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
// Invocation-only diagnostic. Read before the first map render; absent/other
// values perform no clock reads, allocations or logging. Never enable by default.
inline bool enabled() noexcept {
    static const bool value = [] {
        const char* setting = std::getenv("KLATTRA_TERRAIN_PERFDIAG");
        return setting && setting[0] == '1' && setting[1] == '\0';
    }();
    return value;
}

struct SpanStats {
    uint64_t calls = 0, wallNs = 0, cpuNs = 0, maxWallNs = 0;
};
struct State {
    std::array<SpanStats, static_cast<size_t>(Phase::Count)> phases{};
    std::array<uint64_t, static_cast<size_t>(Counter::Count)> counts{};
    uint64_t windowStart = 0, cpuUnavailable = 0;
    size_t activeScopes = 0;
};
inline State& state() noexcept {
    static thread_local State value;
    return value;
}
inline uint64_t wallTime() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline int64_t threadCPUTime() noexcept {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return -1;
    return static_cast<int64_t>(value.tv_sec) * 1000000000LL + value.tv_nsec;
}
inline void count(Counter counter, uint64_t amount = 1) noexcept {
    if (enabled()) state().counts[static_cast<size_t>(counter)] += amount;
}

// Inclusive spans may nest: do not sum parent and child measurements. Thread
// CPU excludes background workers; wall time includes scheduling/GL waits.
class Scope {
public:
    explicit Scope(Phase phase_) noexcept : phase(phase_), active(enabled()) {
        if (!active) return;
        startWall = wallTime();
        startCPU = threadCPUTime();
        auto& data = state();
        if (!data.windowStart) data.windowStart = startWall;
        ++data.activeScopes;
    }
    ~Scope() noexcept { stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void stop() noexcept {
        if (!active) return;
        const int64_t endCPU = threadCPUTime();
        const uint64_t elapsed = wallTime() - startWall;
        auto& data = state();
        auto& span = data.phases[static_cast<size_t>(phase)];
        ++span.calls;
        span.wallNs += elapsed;
        span.maxWallNs = span.maxWallNs < elapsed ? elapsed : span.maxWallNs;
        if (startCPU >= 0 && endCPU >= startCPU) span.cpuNs += endCPU - startCPU;
        else ++data.cpuUnavailable;
        --data.activeScopes;
        active = false;
    }
private:
    Phase phase;
    bool active;
    uint64_t startWall = 0;
    int64_t startCPU = -1;
};

inline void report() noexcept {
    if (!enabled()) return;
    auto& data = state();
    const uint64_t now = wallTime();
    if (!data.windowStart || data.activeScopes || now - data.windowStart < 1000000000ULL) return;
    static constexpr std::array<const char*, static_cast<size_t>(Phase::Count)> phaseNames{
        "frame", "tree", "terrain", "symbols", "heights", "layers", "upload0", "upload",
        "changes", "flush", "draw2d", "drawterrain", "post3d", "depth", "depthquery", "depthsetup",
        "depthblit", "symbolubo", "postrender", "present", "endframe", "glcleanup", "demclone",
        "targets", "draw3d", "beginframe", "renderjobs", "ubodefrag"};
    static constexpr std::array<const char*, static_cast<size_t>(Counter::Count)> counterNames{
        "frames", "terrainframes", "buckets", "cachereset", "cachehit", "cachemiss", "samples",
        "candidates", "heightverts", "heightrebuilds", "rawrevision", "meshrevision", "demcopies",
        "demcopybytes", "ubocreate", "uboupdate", "captures", "capturefail", "goldfishlayers"};
    try {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "[TERRAIN PERF] mono_end_ms=%.3f window_ms=%.3f cpu_missing=%llu phases(n/wall_ms/cpu_ms/max_wall_ms)=",
                      now / 1e6, (now - data.windowStart) / 1e6, static_cast<unsigned long long>(data.cpuUnavailable));
        std::string line(buffer);
        for (size_t i = 0; i < data.phases.size(); ++i) {
            const auto& span = data.phases[i];
            if (!span.calls) continue;
            std::snprintf(buffer, sizeof(buffer), "%s:%llu/%.3f/%.3f/%.3f;", phaseNames[i],
                          static_cast<unsigned long long>(span.calls), span.wallNs / 1e6,
                          span.cpuNs / 1e6, span.maxWallNs / 1e6);
            line += buffer;
        }
        line += " counts=";
        for (size_t i = 0; i < data.counts.size(); ++i) {
            if (!data.counts[i]) continue;
            line += counterNames[i];
            line += '=';
            line += std::to_string(data.counts[i]);
            line += ';';
        }
        Log::Info(Event::General, line);
    } catch (...) {
        // A diagnostic allocation/log failure must not interrupt rendering.
    }
    data.phases = {};
    data.counts = {};
    data.cpuUnavailable = 0;
    // Space reports at least one second apart, including logging overhead.
    data.windowStart = wallTime();
}

class FrameScope {
public:
    FrameScope() noexcept : scope(Phase::RenderFrame) { count(Counter::Frames); }
    ~FrameScope() noexcept { scope.stop(); report(); }
private:
    Scope scope;
};
#else
inline constexpr bool enabled() noexcept { return false; }
inline void count(Counter, uint64_t = 1) noexcept {}
class Scope {
public:
    explicit Scope(Phase) noexcept {}
    void stop() noexcept {}
};
class FrameScope {};
#endif

} // namespace mbgl::terrain_perf
