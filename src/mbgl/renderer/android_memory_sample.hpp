#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string_view>
#include <algorithm>

namespace mbgl::android_memory {

// VmRSS includes native/Java resident pages. Add swapped pages so reclaim does
// not hide a growing process. This is a cheap, approximate Linux pressure
// signal, not iOS phys_footprint or a measurement of all GPU allocations.
inline std::optional<uint64_t> residentAndSwapKiB(std::string_view status) {
    std::optional<uint64_t> resident;
    std::optional<uint64_t> swap;
    while (!status.empty()) {
        const auto end = status.find('\n');
        auto line = status.substr(0, end);
        status = end == std::string_view::npos ? std::string_view{} : status.substr(end + 1);
        std::optional<uint64_t>* field = nullptr;
        if (line.starts_with("VmRSS:")) {
            field = &resident;
            line.remove_prefix(6);
        } else if (line.starts_with("VmSwap:")) {
            field = &swap;
            line.remove_prefix(7);
        } else {
            continue;
        }
        if (field->has_value()) return std::nullopt;
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string_view::npos) return std::nullopt;
        line.remove_prefix(start);
        uint64_t value = 0;
        const auto result = std::from_chars(line.data(), line.data() + line.size(), value);
        if (result.ec != std::errc{}) return std::nullopt;
        auto unit = std::string_view(result.ptr, line.data() + line.size() - result.ptr);
        const auto unitStart = unit.find_first_not_of(" \t");
        if (unitStart == std::string_view::npos) return std::nullopt;
        unit.remove_prefix(unitStart);
        const auto unitEnd = unit.find_last_not_of(" \t\r");
        unit = unit.substr(0, unitEnd + 1);
        if (unit != "kB") return std::nullopt;
        *field = value;
    }
    if (!resident || !swap || *swap > std::numeric_limits<uint64_t>::max() - *resident) {
        return std::nullopt;
    }
    return *resident + *swap;
}

struct Sample {
    int64_t residentAndSwapMiB = -1;
    int64_t peakMiB = -1;
};

inline Sample sample() {
    // TilePyramid may call this per source and frame. At most one small proc
    // read per second per renderer thread; never walk smaps on the render path.
    struct Cache {
        std::chrono::steady_clock::time_point nextRead{};
        Sample value;
    };
    thread_local Cache cache;
    const auto now = std::chrono::steady_clock::now();
    if (now < cache.nextRead) return cache.value;
    cache.nextRead = now + std::chrono::seconds(1);
    cache.value.residentAndSwapMiB = -1;
    auto* file = std::fopen("/proc/self/status", "r");
    if (!file) return cache.value;
    std::array<char, 16384> buffer{};
    const auto size = std::fread(buffer.data(), 1, buffer.size(), file);
    const bool complete = std::feof(file) && !std::ferror(file);
    std::fclose(file);
    if (!complete) return cache.value;
    if (const auto kib = residentAndSwapKiB({buffer.data(), size})) {
        cache.value.residentAndSwapMiB = static_cast<int64_t>(*kib / 1024);
        cache.value.peakMiB = std::max(cache.value.peakMiB, cache.value.residentAndSwapMiB);
    }
    return cache.value;
}

} // namespace mbgl::android_memory
