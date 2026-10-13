#pragma once

#include <mbgl/gfx/texture2d.hpp>
#include <mbgl/util/size.hpp>

#include <memory>

namespace mbgl {
namespace gl {

class Context;

// Renderer-owned, same-frame depth copy for terrain label anchor visibility.
// Destroy within a BackendScope; unsupported captures return no texture.
class TerrainSymbolDepthSnapshot {
public:
    struct Capture {
        std::shared_ptr<gfx::Texture2D> texture;
        float depthEpsilon = 0;
    };

    explicit TerrainSymbolDepthSnapshot(Context&);
    ~TerrainSymbolDepthSnapshot();
    Capture capture(Size framebufferSize);

private:
    struct Impl;
    Context& context;
    std::unique_ptr<Impl> impl;
};

} // namespace gl
} // namespace mbgl
