#import <Foundation/Foundation.h>

#import "MLNTerrain.h"

#include <memory>

namespace mbgl {
namespace style {
class Terrain;
}
}  // namespace mbgl

@interface MLNTerrain (Private)

/**
 Initializes and returns an ``MLNTerrain`` from an `mbgl::style::Terrain`.
 */
- (instancetype)initWithMBGLTerrain:(const mbgl::style::Terrain *)mbglTerrain;

/**
 Returns an `mbgl::style::Terrain` representation of the ``MLNTerrain``.
 */
- (std::unique_ptr<mbgl::style::Terrain>)mbglTerrain;

@end
