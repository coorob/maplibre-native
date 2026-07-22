#include <mbgl/mtl/upload_pass.hpp>

#include <mbgl/mtl/context.hpp>
#include <mbgl/mtl/command_encoder.hpp>
#include <mbgl/mtl/index_buffer_resource.hpp>
#include <mbgl/mtl/renderable_resource.hpp>
#include <mbgl/mtl/vertex_attribute.hpp>
#include <mbgl/mtl/vertex_buffer_resource.hpp>
#include <mbgl/util/instrumentation.hpp>
#include <mbgl/util/logging.hpp>

#include <Metal/Metal.hpp>

#include <algorithm>

namespace mbgl {
namespace mtl {

UploadPass::UploadPass(gfx::Renderable& renderable, CommandEncoder& commandEncoder_, const char* name)
    : commandEncoder(commandEncoder_) {
    auto& resource = renderable.getResource<RenderableResource>();

    resource.bind();

    if (const auto& buffer_ = resource.getCommandBuffer()) {
        buffer = buffer_;
        // blit encoder is not being used yet
        // if (auto upd = resource.getUploadPassDescriptor()) {
        //    encoder = NS::RetainPtr(buffer->blitCommandEncoder(upd.get()));
        //}
        // assert(encoder);
    }

    // Push the groups already accumulated by the encoder
    commandEncoder.visitDebugGroups([this](const auto& group) {
        debugGroups.emplace_back(gfx::DebugGroup<gfx::UploadPass>{*this, group.c_str()});
    });

    // Push the group for the name provided
    debugGroups.emplace_back(gfx::DebugGroup<gfx::UploadPass>{*this, name});

    // Let the encoder pass along any groups pushed to it after this
    commandEncoder.trackUploadPass(this);
}

UploadPass::~UploadPass() {
    commandEncoder.forgetUploadPass(this);
    endEncoding();
}

void UploadPass::endEncoding() {
    debugGroups.clear();

    if (encoder) {
        encoder->endEncoding();
        encoder.reset();
    }
}

std::unique_ptr<gfx::VertexBufferResource> UploadPass::createVertexBufferResource(const void* data,
                                                                                  const std::size_t size,
                                                                                  const gfx::BufferUsageType usage,
                                                                                  bool persistent) {
    return std::make_unique<VertexBufferResource>(
        commandEncoder.context.createBuffer(data, size, usage, /*isIndexBuffer=*/false, persistent));
}

void UploadPass::updateVertexBufferResource(gfx::VertexBufferResource& resource, const void* data, std::size_t size) {
    static_cast<VertexBufferResource&>(resource).get().update(data, size, /*offset=*/0);
}

std::unique_ptr<gfx::IndexBufferResource> UploadPass::createIndexBufferResource(const void* data,
                                                                                const std::size_t size,
                                                                                const gfx::BufferUsageType usage,
                                                                                bool persistent) {
    return std::make_unique<IndexBufferResource>(
        commandEncoder.context.createBuffer(data, size, usage, /*isIndexBuffer=*/true, persistent));
}

void UploadPass::updateIndexBufferResource(gfx::IndexBufferResource& resource, const void* data, std::size_t size) {
    static_cast<IndexBufferResource&>(resource).get().update(data, size, /*offset=*/0);
}

struct VertexBuffer : public gfx::VertexBufferBase {
    ~VertexBuffer() override = default;

    std::unique_ptr<gfx::VertexBufferResource> resource;
};

namespace {
const std::unique_ptr<gfx::VertexBufferResource> noBuffer;
}
const gfx::UniqueVertexBufferResource& UploadPass::getBuffer(const gfx::VertexVectorBasePtr& vec,
                                                             const gfx::BufferUsageType usage,
                                                             bool forceUpdate) {
    if (vec) {
        const auto* rawBufPtr = vec->getRawData();
        const auto rawBufSize = vec->getRawCount() * vec->getRawSize();

        // If we already have a buffer...
        if (auto* rawData = static_cast<VertexBuffer*>(vec->getBuffer()); rawData && rawData->resource) {
            auto& resource = static_cast<VertexBufferResource&>(*rawData->resource);

            // If the already-allocated buffer is large enough, we can re-use it
            if (rawBufSize <= resource.getSizeInBytes()) {
                // If the source changed, update the buffer contents
                if (forceUpdate || vec->isModifiedAfter(resource.getLastUpdated())) {
                    updateVertexBufferResource(resource, rawBufPtr, rawBufSize);
                    resource.setLastUpdated(vec->getLastModified());
                }
                return rawData->resource;
            }
        }
        // Otherwise, create a new one
        if (rawBufSize > 0) {
            auto buffer_ = std::make_unique<VertexBuffer>();
            buffer_->resource = createVertexBufferResource(rawBufPtr, rawBufSize, usage, /*persistent=*/false);
            vec->setBuffer(std::move(buffer_));
            return static_cast<VertexBuffer*>(vec->getBuffer())->resource;
        }
    }
    return noBuffer;
}

gfx::AttributeBindingArray UploadPass::buildAttributeBindings(
    const std::size_t vertexCount,
    [[maybe_unused]] const gfx::AttributeDataType vertexType,
    [[maybe_unused]] const std::size_t vertexAttributeIndex,
    [[maybe_unused]] const std::vector<std::uint8_t>& vertexData,
    const gfx::VertexAttributeArray& defaults,
    const gfx::VertexAttributeArray& overrides,
    const gfx::BufferUsageType usage,
    const std::optional<std::chrono::duration<double>> lastUpdate,
    /*out*/ std::vector<std::unique_ptr<gfx::VertexBufferResource>>&) {
    MLN_TRACE_FUNC();

    gfx::AttributeBindingArray bindings;
    bindings.resize(defaults.allocatedSize());

    // For each attribute in the program, with the corresponding default and optional override...
    const auto resolveAttr = [&]([[maybe_unused]] const size_t id, auto& default_, auto& override_) -> void {
        auto& effectiveAttr = override_ ? *override_ : default_;
        const auto& defaultAttr = static_cast<const VertexAttribute&>(default_);
        const auto index = static_cast<std::size_t>(defaultAttr.getIndex());
        const auto bufferIndex = static_cast<uint32_t>(defaultAttr.getBufferIndex());

        bindings.resize(std::max(bindings.size(), index + 1));

        if (!override_) {
            // No attribute was provided, this value will be used from a uniform, but we have to set
            // up a valid binding or the validation will complain when the shader code references the
            // attribute at compile time, regardless of whether it's ever used at runtime.
            bindings[index] = {
                .attribute = {.dataType = defaultAttr.getDataType(), /*offset=*/.offset = 0},
                .vertexStride = static_cast<uint32_t>(VertexAttribute::getStrideOf(defaultAttr.getDataType())),
                .vertexBufferResource = nullptr,
                .vertexOffset = 0,
                .bufferIndex = bufferIndex,
            };
            return;
        }

        // If the attribute references data shared with a bucket, get the corresponding buffer.
        const auto& sharedData = effectiveAttr.getSharedRawData();
        if (const auto& buffer_ = getBuffer(sharedData, usage, !lastUpdate)) {
            // A released VertexVector intentionally drops its CPU-side bytes
            // after upload while retaining the resident Metal buffer. Drape
            // drawables can legitimately outlive the bucket that released
            // that vector, so validating against getRawCount() incorrectly
            // rejected a still-valid buffer and bound a constant default. For
            // a position attribute that collapses the fill geometry to a
            // point, presenting as broad land-cover patches disappearing.
            //
            // Validate the complete attribute range against the resource that
            // Metal will actually read. Keep the default-binding fallback for
            // genuinely stale or malformed ranges so this remains crash-safe.
            const auto stride = static_cast<std::uint64_t>(effectiveAttr.getSharedStride());
            const auto vertexOffset = static_cast<std::uint64_t>(effectiveAttr.getSharedVertexOffset());
            const auto attributeOffset = static_cast<std::uint64_t>(effectiveAttr.getSharedOffset());
            const auto attributeSize = static_cast<std::uint64_t>(
                VertexAttribute::getStrideOf(effectiveAttr.getSharedType()));
            const auto residentBytes = static_cast<std::uint64_t>(
                static_cast<const VertexBufferResource&>(*buffer_).getSizeInBytes());
            const auto vertexSpan = vertexCount > 0 ? stride * static_cast<std::uint64_t>(vertexCount - 1) : 0;
            const auto requiredBytes = stride * vertexOffset + attributeOffset + vertexSpan + attributeSize;
            const bool validRange = stride > 0 && attributeSize > 0 && attributeOffset + attributeSize <= stride &&
                                    requiredBytes <= residentBytes;

            if (!validRange) {
                Log::Warning(Event::Render,
                             "Drape resident attribute range invalid; binding with default to avoid crash");
                bindings[index] = {
                    /*.attribute = */ {defaultAttr.getDataType(), /*offset=*/0},
                    /*.vertexStride = */ static_cast<uint32_t>(VertexAttribute::getStrideOf(defaultAttr.getDataType())),
                    /*.vertexBufferResource = */ nullptr,
                    /*.vertexOffset = */ 0,
                    /*.bufferIndex = */ bufferIndex,
                };
                return;
            }

            if (sharedData->isReleased() && sharedData->getRawCount() == 0) {
                static bool loggedReleasedBufferIdentity = false;
                if (!loggedReleasedBufferIdentity) {
                    Log::Info(Event::Render,
                              "[Klättra DRAPE_BUFFER] kind=drape-resident-buffer-range "
                              "action=bind-released-buffer");
                    loggedReleasedBufferIdentity = true;
                }
            }

            bindings[index] = {
                /*.attribute = */ {effectiveAttr.getSharedType(), effectiveAttr.getSharedOffset()},
                /*.vertexStride = */ effectiveAttr.getSharedStride(),
                /*.vertexBufferResource = */ buffer_.get(),
                /*.vertexOffset = */ effectiveAttr.getSharedVertexOffset(),
                /*.bufferIndex = */ bufferIndex,
            };
            return;
        }

        // Defensive: stride=0 means the override attribute is malformed
        // (e.g., a drape drawable whose source bucket got reparsed mid-
        // frame and left a paint-property attribute slot configured but
        // unpopulated). Don't crash; emit a placeholder binding so the
        // drawable renders with the shader's default for this attribute.
        if (effectiveAttr.getStride() == 0) {
            Log::Warning(Event::Render, "Terrain/drape attribute has 0 stride; skipping with default binding");
            bindings[index] = {
                /*.attribute = */ {defaultAttr.getDataType(), /*offset=*/0},
                /*.vertexStride = */ static_cast<uint32_t>(VertexAttribute::getStrideOf(defaultAttr.getDataType())),
                /*.vertexBufferResource = */ nullptr,
                /*.vertexOffset = */ 0,
                /*.bufferIndex = */ bufferIndex,
            };
            return;
        }

        // Otherwise, turn the data managed by the attribute into a buffer.
        if (const auto& buffer_ = VertexAttribute::getBuffer(
                effectiveAttr, *this, gfx::BufferUsageType::StaticDraw, !lastUpdate)) {
            bindings[index] = {
                /*.attribute = */ {effectiveAttr.getDataType(), /*offset=*/0},
                /*.vertexStride = */ static_cast<uint32_t>(effectiveAttr.getStride()),
                /*.vertexBufferResource = */ buffer_.get(),
                /*.vertexOffset = */ 0,
                /*.bufferIndex = */ bufferIndex,
            };
            return;
        }

        // Same defensive fallback: per-attribute buffer creation failed.
        // Bind a placeholder with no buffer so the drawable renders with
        // the shader's default value for this attribute rather than
        // aborting. Most common cause is a drape drawable referencing a
        // shared vertex vector that was emptied by an upstream tile
        // reparse before the upload pass ran.
        Log::Warning(Event::Render, "Terrain/drape attribute has no buffer; skipping with default binding");
        bindings[index] = {
            /*.attribute = */ {defaultAttr.getDataType(), /*offset=*/0},
            /*.vertexStride = */ static_cast<uint32_t>(VertexAttribute::getStrideOf(defaultAttr.getDataType())),
            /*.vertexBufferResource = */ nullptr,
            /*.vertexOffset = */ 0,
            /*.bufferIndex = */ bufferIndex,
        };
    };
    // This version is called when the attribute is available, but isn't being used by the shader
    const auto missingAttr = [&](const size_t, auto& missingAttr) -> void {
        missingAttr->setDirty(false);
    };
    defaults.resolve(overrides, resolveAttr, missingAttr);

    return bindings;
}

namespace {
constexpr auto missing = "<none>";
NS::String* toNSString(const char* str) {
    return NS::String::string(str ? str : missing, NS::UTF8StringEncoding);
}
} // namespace

void UploadPass::pushDebugGroup(const char* name) {
    if (encoder) {
        encoder->pushDebugGroup(toNSString(name));
    }
}

void UploadPass::popDebugGroup() {
    if (encoder) {
        encoder->popDebugGroup();
    }
}

gfx::Context& UploadPass::getContext() {
    return commandEncoder.context;
}

const gfx::Context& UploadPass::getContext() const {
    return commandEncoder.context;
}

} // namespace mtl
} // namespace mbgl
