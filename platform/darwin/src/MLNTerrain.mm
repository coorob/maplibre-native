#import "MLNTerrain.h"
#import "MLNTerrain_Private.h"

#include <mbgl/style/terrain.hpp>

@implementation MLNTerrain

- (instancetype)initWithSourceIdentifier:(NSString *)sourceIdentifier
                            exaggeration:(float)exaggeration {
    if ((self = [super init])) {
        _sourceIdentifier = [sourceIdentifier copy];
        _exaggeration = exaggeration;
    }
    return self;
}

- (instancetype)initWithMBGLTerrain:(const mbgl::style::Terrain *)mbglTerrain {
    NSString *sourceID = @(mbglTerrain->getSource().c_str());
    float exaggeration = mbglTerrain->getExaggeration();
    return [self initWithSourceIdentifier:sourceID exaggeration:exaggeration];
}

- (std::unique_ptr<mbgl::style::Terrain>)mbglTerrain {
    return std::make_unique<mbgl::style::Terrain>(
        std::string([self.sourceIdentifier UTF8String]),
        self.exaggeration);
}

@end
