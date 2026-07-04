#include <mbgl/mtl/offscreen_texture.hpp>
#include <mbgl/mtl/context.hpp>
#include <mbgl/mtl/renderable_resource.hpp>
#include <mbgl/mtl/renderer_backend.hpp>
#include <mbgl/mtl/texture2d.hpp>

#include <Metal/Metal.hpp>

namespace mbgl {
namespace mtl {

class OffscreenTextureResource final : public RenderableResource {
public:
    OffscreenTextureResource(Context& context_,
                             const Size size_,
                             const gfx::TextureChannelDataType type_,
                             bool depth,
                             [[maybe_unused]] bool stencil)
        : context(context_),
          size(size_),
          type(type_) {
        assert(!size.isEmpty());
        colorTexture = context.createTexture2D();
        colorTexture->setSize(size);
        colorTexture->setFormat(gfx::TexturePixelType::RGBA, type);
        colorTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                               .wrapU = gfx::TextureWrapType::Clamp,
                                               .wrapV = gfx::TextureWrapType::Clamp});
        static_cast<Texture2D*>(colorTexture.get())
            ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);

        if (depth) {
            depthTexture = context.createTexture2D();
            depthTexture->setSize(size);
            depthTexture->setFormat(gfx::TexturePixelType::Depth, gfx::TextureChannelDataType::Float);
            depthTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                   .wrapU = gfx::TextureWrapType::Clamp,
                                                   .wrapV = gfx::TextureWrapType::Clamp});
            static_cast<Texture2D*>(depthTexture.get())
                ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);
        }

        // On iOS simulator, the depth target is PixelFormatDepth32Float_Stencil8
#if !TARGET_OS_SIMULATOR
        if (stencil) {
            stencilTexture = context.createTexture2D();
            stencilTexture->setSize(size);
            stencilTexture->setFormat(gfx::TexturePixelType::Stencil, gfx::TextureChannelDataType::UnsignedByte);
            stencilTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                     .wrapU = gfx::TextureWrapType::Clamp,
                                                     .wrapV = gfx::TextureWrapType::Clamp});
            static_cast<Texture2D*>(stencilTexture.get())
                ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);
        }
#endif

        context.renderingStats().numFrameBuffers++;
    }

    ~OffscreenTextureResource() noexcept override { context.renderingStats().numFrameBuffers--; }

    // Whether the color target's MTLTexture actually exists. Allocation can
    // fail under memory pressure (newTextureWithDescriptor returns nil);
    // rendering into a descriptor with zero valid attachments raises
    // NSInvalidArgumentException. Callers skip the pass instead; create()
    // retries on a later frame once memory frees up (textureDirty stays set).
    bool isRenderable() {
        colorTexture->create();
        return static_cast<Texture2D*>(colorTexture.get())->getMetalTexture() != nullptr;
    }

    void bind() override {
        assert(context.getBackend().getCommandQueue());
        // All offscreen passes of a frame share one command buffer (owned by
        // the Context) so the drape bakes pipeline on the GPU instead of one
        // committed-and-waited buffer per target.
        commandBuffer = context.offscreenCommandBuffer();
        colorTexture->create();

        renderPassDescriptor = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
        if (auto* colorTarget = renderPassDescriptor->colorAttachments()->object(0)) {
            colorTarget->setTexture(static_cast<Texture2D*>(colorTexture.get())->getMetalTexture());
        }

        if (depthTexture) {
            depthTexture->create();
            if (auto* depthTarget = renderPassDescriptor->depthAttachment()) {
                depthTarget->setTexture(static_cast<Texture2D*>(depthTexture.get())->getMetalTexture());
            }
        }
        if (stencilTexture) {
            stencilTexture->create();
            if (auto* stencilTarget = renderPassDescriptor->stencilAttachment()) {
                stencilTarget->setTexture(static_cast<Texture2D*>(stencilTexture.get())->getMetalTexture());
            }
        }
    }

    void swap() override {
        assert(commandBuffer);
        encodeMipmaps(commandBuffer.get());
        // No commit and no wait here: the shared buffer is committed once per
        // frame (Context::flushOffscreenRenderWork) after all targets have
        // encoded. Same-queue commit order makes every bake visible to the
        // main pass, which is committed later; only CPU readback needs an
        // explicit wait (see readStillImage).
        commandBuffer.reset();
        renderPassDescriptor.reset();
    }

    PremultipliedImage readStillImage() {
        assert(static_cast<Texture2D*>(colorTexture.get())->getMetalTexture());

        // CPU readback: the pass that rendered this texture may still be
        // pending in the shared offscreen command buffer, or in flight.
        context.waitOffscreenRenderWork();

        auto data = std::make_unique<uint8_t[]>(colorTexture->getDataSize());
        MTL::Region region = MTL::Region::Make2D(0, 0, size.width, size.height);
        NS::UInteger bytesPerRow = size.width * colorTexture->getPixelStride();

        static_cast<Texture2D*>(colorTexture.get())->getMetalTexture()->getBytes(data.get(), bytesPerRow, region, 0);

        return {size, std::move(data)};
    }

    gfx::Texture2DPtr& getTexture() {
        assert(colorTexture);
        return colorTexture;
    }

    void setMipmapped(bool enabled) {
        mipmapped = enabled;
        colorTexture->setSamplerConfiguration({gfx::TextureFilterType::Linear,
                                               gfx::TextureWrapType::Clamp,
                                               gfx::TextureWrapType::Clamp,
                                               static_cast<uint8_t>(enabled ? 8 : 1),
                                               enabled});
    }

    void generateMipmaps() {
        if (!mipmapped) {
            return;
        }

        const auto& commandQueue = context.getBackend().getCommandQueue();
        if (!commandQueue) {
            return;
        }

        auto commandBuffer = NS::RetainPtr(commandQueue->commandBuffer());
        if (!commandBuffer) {
            return;
        }

        encodeMipmaps(commandBuffer.get());
        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();
    }

    const RendererBackend& getBackend() const override { return context.getBackend(); }

    const MTLCommandBufferPtr& getCommandBuffer() const override { return commandBuffer; }

    MTLBlitPassDescriptorPtr getUploadPassDescriptor() const override {
        return NS::TransferPtr(MTL::BlitPassDescriptor::alloc()->init());
    }

    const MTLRenderPassDescriptorPtr& getRenderPassDescriptor() const override {
        assert(renderPassDescriptor);
        return renderPassDescriptor;
    }

private:
    Context& context;
    const Size size;
    const gfx::TextureChannelDataType type;
    gfx::Texture2DPtr colorTexture;
    gfx::Texture2DPtr depthTexture;
    gfx::Texture2DPtr stencilTexture;
    MTLCommandBufferPtr commandBuffer;
    MTLRenderPassDescriptorPtr renderPassDescriptor;
    bool mipmapped = false;

    void encodeMipmaps(MTL::CommandBuffer* targetCommandBuffer) {
        if (!mipmapped || !targetCommandBuffer) {
            return;
        }

        colorTexture->create();
        auto* texture = static_cast<Texture2D*>(colorTexture.get())->getMetalTexture();
        if (!texture || texture->mipmapLevelCount() <= 1) {
            return;
        }

        auto blitEncoder = NS::RetainPtr(targetCommandBuffer->blitCommandEncoder());
        if (!blitEncoder) {
            return;
        }

        blitEncoder->generateMipmaps(texture);
        blitEncoder->endEncoding();
    }
};

OffscreenTexture::OffscreenTexture(
    Context& context, const Size size_, const gfx::TextureChannelDataType type, bool depth, bool stencil)
    : gfx::OffscreenTexture(size_, std::make_unique<OffscreenTextureResource>(context, size_, type, depth, stencil)) {}

bool OffscreenTexture::isRenderable() {
    return getResource<OffscreenTextureResource>().isRenderable();
}

PremultipliedImage OffscreenTexture::readStillImage() {
    return getResource<OffscreenTextureResource>().readStillImage();
}

const gfx::Texture2DPtr& OffscreenTexture::getTexture() {
    return getResource<OffscreenTextureResource>().getTexture();
}

void OffscreenTexture::setMipmapped(bool enabled) {
    getResource<OffscreenTextureResource>().setMipmapped(enabled);
}

void OffscreenTexture::generateMipmaps() {
    getResource<OffscreenTextureResource>().generateMipmaps();
}

} // namespace mtl
} // namespace mbgl
