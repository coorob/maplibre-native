#include <mbgl/renderer/terrain_perf_diagnostic.hpp>
#include <mbgl/gl/terrain_symbol_depth.hpp>
#include <mbgl/gl/context.hpp>
#include <mbgl/gl/defines.hpp>
#include <mbgl/gl/texture2d.hpp>
#include <mbgl/util/logging.hpp>

#include <algorithm>
#include <new>
#include <cstdlib>
#include <string>

namespace mbgl {
namespace gl {

using namespace platform;

namespace {

// BEGIN standalone GL capture operations (also exercised by the host GPU test).
// These operations deliberately bypass Context's combined-framebuffer cache:
// GLES exposes separate read/draw bindings, but the cache stores only one.
struct DepthCaptureGLState {
    GLint readFramebuffer = 0, drawFramebuffer = 0;
    GLint activeTexture = 0, texture0 = 0;
    GLboolean scissor = GL_FALSE;

    DepthCaptureGLState() {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        glActiveTexture(activeTexture);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
    }
    ~DepthCaptureGLState() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture0);
        glActiveTexture(activeTexture);
        if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    }
};

DepthTextureFormat sourceDepthFormat(GLint sourceFramebuffer, GLint* reportedBits = nullptr) {
    GLint samples = 0, depthBits = 0, stencilBits = 0, depthType = GL_NONE, stencilType = GL_NONE;
    glGetIntegerv(GL_SAMPLES, &samples);
    if (samples != 0) return DepthTextureFormat::None;
    const GLenum depthAttachment = sourceFramebuffer ? GL_DEPTH_ATTACHMENT : GL_DEPTH;
    const GLenum stencilAttachment = sourceFramebuffer ? GL_STENCIL_ATTACHMENT : GL_STENCIL;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, depthAttachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &depthType);
    if (depthType == GL_NONE) return DepthTextureFormat::None;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, depthAttachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depthBits);
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, stencilAttachment,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilType);
    if (stencilType != GL_NONE) {
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, stencilAttachment,
                                              GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencilBits);
    }
    if (reportedBits) *reportedBits = depthBits;
    if (glGetError() != GL_NO_ERROR) return DepthTextureFormat::None;
    if (depthBits == 16) return DepthTextureFormat::Depth16;
    if (depthBits != 24) return DepthTextureFormat::None;
    if (stencilBits == 8) {
        // Android's single-sample EGL depth24/stencil8 surface is packed. For
        // user FBOs, distinguish packed from independent depth/stencil objects.
        GLint depthName = 0, stencilName = 0;
        if (sourceFramebuffer) {
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, depthAttachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &depthName);
            glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, stencilAttachment,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &stencilName);
        }
        if (depthType == stencilType && depthName == stencilName) return DepthTextureFormat::Depth24Stencil8;
    }
    return DepthTextureFormat::Depth24;
}

bool copyTerrainDepth(GLuint source, GLuint destination, GLuint depthTexture,
                      DepthTextureFormat format, GLsizei width, GLsizei height) {
    terrain_perf::Scope setupPerf(terrain_perf::Phase::DepthSetup);
    // Set both destination bindings before altering its read/draw buffer state.
    glBindFramebuffer(GL_FRAMEBUFFER, destination);
    const GLenum attachment = format == DepthTextureFormat::Depth24Stencil8
                                  ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, depthTexture, 0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    setupPerf.stop();
    terrain_perf::Scope blitPerf(terrain_perf::Phase::DepthBlit);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, destination);
    glDisable(GL_SCISSOR_TEST); // Blits honor scissor, regardless of the depth write mask.
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    return glGetError() == GL_NO_ERROR;
}
// END standalone GL capture operations.

template <typename Value>
struct SavedCacheState {
    State<Value>& state;
    typename Value::Type value;
    bool dirty;
    explicit SavedCacheState(State<Value>& state_) : state(state_), value(state.getCurrentValue()), dirty(state.isDirty()) {}
    ~SavedCacheState() {
        state.setCurrentValue(value);
        if (dirty) state.setDirty();
    }
};

} // namespace

struct TerrainSymbolDepthSnapshot::Impl {
    std::shared_ptr<Texture2D> texture;
    std::unique_ptr<UniqueFramebuffer> framebuffer;
    Size size{0, 0};
    DepthTextureFormat format = DepthTextureFormat::None;
    bool failed = false;
    bool reported = false;
};

TerrainSymbolDepthSnapshot::TerrainSymbolDepthSnapshot(Context& context_)
    : context(context_), impl(std::make_unique<Impl>()) {}
TerrainSymbolDepthSnapshot::~TerrainSymbolDepthSnapshot() = default;

TerrainSymbolDepthSnapshot::Capture TerrainSymbolDepthSnapshot::capture(Size framebufferSize) {
    terrain_perf::Scope capturePerf(terrain_perf::Phase::DepthCapture);
    terrain_perf::count(terrain_perf::Counter::DepthCaptures);
    terrain_perf::Scope queryPerf(terrain_perf::Phase::DepthQuery);
    SavedCacheState<value::ActiveTextureUnit> activeCache(context.activeTextureUnit);
    SavedCacheState<value::BindTexture> textureCache(context.texture[0]);
    DepthCaptureGLState state;
    GLint depthBits = 0;
    const auto format = sourceDepthFormat(state.drawFramebuffer, &depthBits);
    queryPerf.stop();
    if (impl->size != framebufferSize || impl->format != format) {
        impl->texture.reset();
        impl->framebuffer.reset();
        impl->size = framebufferSize;
        impl->format = format;
        impl->failed = false;
        impl->reported = false;
    }
    const auto report = [&](const char* status) {
        const char* setting = std::getenv("KLATTRA_STAGEDIAG");
        if (setting && *setting && !(*setting == '0' || *setting == 'f' || *setting == 'F')) {
            Log::Info(Event::General, std::string("[TERRAIN SYMBOL DEPTH] ") + status +
                      " size=" + std::to_string(framebufferSize.width) + "x" + std::to_string(framebufferSize.height) +
                      " depthBits=" + std::to_string(depthBits) + " format=" + std::to_string(int(format)));
        }
        impl->reported = true;
    };
    if (format == DepthTextureFormat::None || !framebufferSize.width || !framebufferSize.height) {
        terrain_perf::count(terrain_perf::Counter::DepthFailures);
        if (!impl->reported) report("fallback=unsupported");
        return {};
    }
    if (impl->failed) {
        terrain_perf::count(terrain_perf::Counter::DepthFailures);
        return {};
    }
    try {
        if (!impl->texture) {
            terrain_perf::Scope allocationPerf(terrain_perf::Phase::DepthSetup);
            auto texture = std::make_shared<Texture2D>(context);
            texture->setDepthFormat(format).setSize(framebufferSize).setSamplerConfiguration({});
            // Allocation uses TU0 and the Context cache; force both caches dirty
            // because external renderables can enter with independently bound GL state.
            context.activeTextureUnit.setDirty();
            context.texture[0].setDirty();
            texture->create();
            impl->texture = std::move(texture);
            impl->framebuffer = std::make_unique<UniqueFramebuffer>(context.createFramebuffer());
        }
        if (copyTerrainDepth(state.drawFramebuffer, impl->framebuffer->get(), impl->texture->getTextureID(),
                             format, framebufferSize.width, framebufferSize.height)) {
            if (!impl->reported) report("active");
            const float maximumDepth = format == DepthTextureFormat::Depth16 ? 65535.0f : 16777215.0f;
            return {impl->texture, std::max(2.0f / maximumDepth, 2e-7f)};
        }
    } catch (const std::bad_alloc&) {
        // Optional occlusion must never turn allocation pressure into a frame failure.
    }
    impl->texture.reset();
    impl->framebuffer.reset();
    terrain_perf::count(terrain_perf::Counter::DepthFailures);
    report("fallback=capture-failed");
    impl->failed = true; // Retry only after resize/format change or terrain re-enable.
    return {};
}

} // namespace gl
} // namespace mbgl
