#include <mbgl/mtl/vertex_attribute.hpp>

#include <mbgl/gfx/vertex_vector.hpp>
#include <mbgl/mtl/buffer_resource.hpp>
#include <mbgl/mtl/upload_pass.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/convert.hpp>

#include <cstring>
#include <sstream>

namespace mbgl {
namespace mtl {

const gfx::UniqueVertexBufferResource& VertexAttribute::getBuffer(gfx::VertexAttribute& attrib_,
                                                                  UploadPass& uploadPass,
                                                                  const gfx::BufferUsageType usage,
                                                                  bool forceUpdate) {
    if (!attrib_.getBuffer()) {
        auto& attrib = static_cast<VertexAttribute&>(attrib_);
        if (attrib.sharedRawData) {
            return uploadPass.getBuffer(attrib.sharedRawData, usage, forceUpdate);
        } else {
            if (!attrib.rawData.empty()) {
                auto buffer = uploadPass.createVertexBufferResource(
                    attrib.rawData.data(), attrib.rawData.size(), usage, false);
                attrib.setBuffer(std::move(buffer));
                attrib.setRawData({});
                attrib_.setDirty(false);
            }
            // else: attribute exists but has no shared data and no raw
            // data. Return the (null) buffer; the caller in
            // `UploadPass::buildAttributeBindings` now handles this with
            // a placeholder binding instead of asserting. Previously hit
            // during fast camera pans when a drape drawable's source
            // bucket got reparsed and emptied its paint-property
            // vertex vectors.
        }
    }
    return attrib_.getBuffer();
}

const std::unique_ptr<gfx::VertexAttribute>& VertexAttributeArray::set(const size_t id,
                                                                       int index,
                                                                       gfx::AttributeDataType dataType,
                                                                       int bufferIndex) {
    auto& attrib = gfx::VertexAttributeArray::set(id, index, dataType, 1);
    if (attrib) {
        static_cast<VertexAttribute*>(attrib.get())->setBufferIndex(bufferIndex);
    }
    return attrib;
}

} // namespace mtl
} // namespace mbgl
