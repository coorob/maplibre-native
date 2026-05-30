#import <Foundation/Foundation.h>

#import "MLNFoundation.h"

NS_ASSUME_NONNULL_BEGIN

/**
 An ``MLNTerrain`` object configures 3D terrain rendering for an ``MLNStyle``.

 Terrain drapes the map's drape-capable layers (fills, lines, raster, hillshade)
 over a digital elevation model. Assign an ``MLNTerrain`` to ``MLNStyle.terrain``
 to enable terrain, or assign `nil` to disable it.

 The source identifier must refer to a raster-DEM source that exists in the
 style. If the source does not exist, terrain is ignored.
 */
MLN_EXPORT
@interface MLNTerrain : NSObject

/**
 Returns a terrain configuration that draws elevation from the given source.

 @param sourceIdentifier The identifier of a raster-DEM source in the style.
 @param exaggeration A multiplier applied to elevation values. Default: `1.0`.
 */
- (instancetype)initWithSourceIdentifier:(NSString *)sourceIdentifier
                            exaggeration:(float)exaggeration NS_DESIGNATED_INITIALIZER;

- (instancetype)init NS_UNAVAILABLE;

/**
 The identifier of the raster-DEM source providing elevation data.
 */
@property (nonatomic, copy) NSString *sourceIdentifier;

/**
 A multiplier applied to elevation values. Values greater than `1.0` exaggerate
 terrain relief; values less than `1.0` flatten it.
 */
@property (nonatomic) float exaggeration;

@end

NS_ASSUME_NONNULL_END
