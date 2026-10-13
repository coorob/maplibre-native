#include "jni.hpp"
#include "jni_native.hpp"
#include <jni/jni.hpp>

#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <thread>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// KLATTRA_* renderer knobs are read with getenv(), but an Android app cannot be
// launched with a custom environment. Mirror `adb shell setprop
// debug.klattra.envN "NAME=VALUE,NAME2=VALUE2"` (N = 0..9, 91 chars each) into
// this process before any renderer code caches a knob. debug.* properties are
// writable only from adb/root, so release users never reach this path.
void klattraImportEnvList(const std::string& list, const char separator) {
    std::size_t start = 0;
    while (start < list.size()) {
        std::size_t end = list.find(separator, start);
        if (end == std::string::npos) end = list.size();
        std::string item = list.substr(start, end - start);
        while (!item.empty() && (item.back() == '\r' || item.back() == ' ')) item.pop_back();
        const std::size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) {
            setenv(item.substr(0, eq).c_str(), item.substr(eq + 1).c_str(), 1);
            __android_log_print(ANDROID_LOG_WARN, "Mbgl", "[KLATTRA ENV] set %s", item.c_str());
        }
        start = end + 1;
    }
}

void klattraImportEnvFromSystemProperties() {
    for (int n = 0; n < 10; ++n) {
        char name[32];
        std::snprintf(name, sizeof name, "debug.klattra.env%d", n);
        char value[PROP_VALUE_MAX] = {0};
        const int length = __system_property_get(name, value);
        if (n == 0) __android_log_print(ANDROID_LOG_WARN, "Mbgl", "[KLATTRA ENV] %s length=%d", name, length);
        if (length <= 0) continue;
        klattraImportEnvList(value, ',');
    }
    // Same knobs from <app data>/files/klattra.env (one NAME=VALUE per line),
    // writable with adb root on an emulator.
    char cmdline[256] = {0};
    if (FILE* f = std::fopen("/proc/self/cmdline", "r")) {
        std::fread(cmdline, 1, sizeof cmdline - 1, f);
        std::fclose(f);
    }
    const std::string path = std::string("/data/data/") + cmdline + "/files/klattra.env";
    if (FILE* f = std::fopen(path.c_str(), "r")) {
        std::string contents;
        char buffer[512];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof buffer, f)) > 0) contents.append(buffer, read);
        std::fclose(f);
        __android_log_print(ANDROID_LOG_WARN, "Mbgl", "[KLATTRA ENV] file %s bytes=%zu", path.c_str(), contents.size());
        klattraImportEnvList(contents, '\n');
    }
}

// Android discards native stderr. The renderer's KLATTRA_TRACE_STDERR probes
// (raster pyramids, drape routing) write there, so mirror it into logcat
// under the MbglTrace tag while that knob is set.
void klattraForwardStderrToLogcat() {
    if (!std::getenv("KLATTRA_TRACE_STDERR")) return;
    int fds[2];
    if (pipe(fds) != 0) return;
    if (dup2(fds[1], STDERR_FILENO) < 0) return;
    close(fds[1]);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([fd = fds[0]] {
        std::string pending;
        char buffer[1024];
        ssize_t count = 0;
        while ((count = read(fd, buffer, sizeof buffer)) > 0) {
            pending.append(buffer, static_cast<std::size_t>(count));
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                __android_log_write(ANDROID_LOG_WARN, "MbglTrace", pending.substr(0, newline).c_str());
                pending.erase(0, newline + 1);
            }
        }
    }).detach();
}

} // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    assert(vm != nullptr);
    klattraImportEnvFromSystemProperties();
    klattraForwardStderrToLogcat();
    mbgl::android::registerNatives(vm);
    return JNI_VERSION_1_6;
}
