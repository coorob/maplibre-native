# 3D Terrain Architecture

Targeted reference for reviewers of the terrain-drape draft PR. Covers
the load-bearing pieces of the implementation in one place so the
commit-by-commit history is easier to navigate.

## Pipeline overview

When a style has a `terrain` block and a matching `raster-dem` source,
the renderer goes through five conceptual phases per frame:

```
[DEM tiles arrive]
       |
       v
  Phase 1 ── allocate per-DEM-tile RenderTarget (drape texture)
       |
       v
  Phase 2 ── 2D layers (background, fill, line, raster) emit drape
             drawables into each visible drape target's TileLayerGroup
       |
       v
  Phase 3 ── terrain mesh drawable binds the drape target as
             its `mapTexture` sampler
       |
       v
  Phase 4 ── per-frame:
               1. RenderTarget.render(...) populates each drape texture
                  with the basemap content for that DEM tile
               2. drawableOpaquePass renders the terrain mesh in the
                  main framebuffer; the mesh vertex shader samples DEM
                  for elevation, the fragment shader samples the drape
                  texture for surface colour
       |
       v
  Phase 5 ── symbol drawables sample terrain elevation at their
             tile's centre and translate their matrix so labels
             follow ridges and valleys instead of floating at z=0
```

## Key classes

| Class | Purpose | File |
|------|--------|------|
| `RenderTerrain` | Owns drape cache, mesh generation, drape-target lifecycle, `getElevation()` | `src/mbgl/renderer/render_terrain.{hpp,cpp}` |
| `TerrainDrapeCache` | `tileID → shared_ptr<RenderTarget>` cache with eviction via `RemoveRenderTargetRequest` | `include/mbgl/renderer/render_terrain_drape_cache.hpp` |
| `TerrainLayerTweaker` | Per-frame UBO update: matrix Z-scaled by `pixelsPerMeter`; style light fed in | `src/mbgl/renderer/layers/terrain_layer_tweaker.{hpp,cpp}` |
| `LayerTweaker::getDrapeMatrix(source, drape)` | Shared helper: ortho + source-to-drape tile-relative translate/scale | `src/mbgl/renderer/layer_tweaker.{hpp,cpp}` |
| `LayerTweaker::tilesOverlap(a, b)` | Decides whether a source tile should drape into a given drape target | same |
| `RenderLayer::activeTerrain` | Pointer set by the orchestrator per-frame to give layers a way to ask "is terrain on?" | `src/mbgl/renderer/render_layer.hpp` |
| `PaintParameters::activeTerrain` | Same pointer plumbed via paint parameters so tweakers can read it without reaching into the orchestrator | `src/mbgl/renderer/paint_parameters.hpp` |

## Drape routing pattern (per layer type)

Every draped layer follows the same shape inside its `update()`:

```cpp
if (activeTerrain) {
    activeTerrain->visitDrapeTargets(
        [&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& target) {
            if (!target || !LayerTweaker::tilesOverlap(tileID, drapeID)) return;

            // Per-drape-target tweaker, lazily built, knows its drape tile ID
            auto& tw = drapeLayerTweakers[drapeID];
            if (!tw) tw = std::make_shared<LayerTweaker>(..., drapeID);

            // Per-drape-target TileLayerGroup inside the drape target
            auto* drapeGroup = ...;

            // Per-source-tile drape drawable, deduped by (tileID, variant)
            auto drapeBuilder = ...;
            drapeBuilder->setShader(shaderGroup->getOrCreateShader(...));
            drapeBuilder->setVertexAttributes(freshAttrs);
            drapeBuilder->setSegments(...);
            // Optional: configureExtras for texture binding
            drapeBuilder->flush(context);

            for (auto& drawable : drapeBuilder->clearDrawables()) {
                drawable->setTileID(sourceTileID);  // source, not drape
                drawable->setLayerTweaker(tw);
                drawable->setBinders(bucket, &binders);
                drapeGroup->addDrawable(renderPass, sourceTileID,
                                        std::move(drawable));
            }
        });
}
```

Layers that need texture binding (FillPattern, LinePattern, LineGradient)
provide a `configureExtras` callback that attaches the texture or atlas
tweaker to the drape builder before flush.

## Elevation matrix Z-scale fix

The terrain mesh's vertex shader takes elevation in metres on its Z
axis. The base tile matrix `parameters.matrixForTile(tileID)` scales
X/Y from tile units to mercator world-pixels but leaves Z scale at 1,
so metres would feed into clip space at the wrong magnitude and the
mesh would collapse to a near-flat plane.

`TerrainLayerTweaker` mirrors the trick `Camera::getWorldToCamera`
uses for camera-space transforms: multiply the matrix's Z column
(indices 8..11) by `pixelsPerMeter` at the current latitude/zoom.

```cpp
const double pixelsPerMeter = 1.0 /
    Projection::getMetersPerPixelAtLatitude(state.getLatLng().latitude(),
                                            state.getZoom());
matrix[8]  *= pixelsPerMeter;
matrix[9]  *= pixelsPerMeter;
matrix[10] *= pixelsPerMeter;
matrix[11] *= pixelsPerMeter;
```

`SymbolLayerTweaker` applies the same scale when translating the
symbol matrix by the tile's elevation.

## Backend coverage

- **Metal**: complete shader, attribute info, registration.
- **GL**: shader ported (GLSL ES with std140 uniform blocks),
  ShaderInfo declared and defined, registerTypes inclusion in
  `gl/renderer_backend.cpp`. Core library builds. Cannot exercise
  on iOS sample app (MapLibre Native iOS is Metal-only — `MLNMapView.mm`
  hardcodes `mbgl/mtl` headers); visual verification needs an Android /
  Linux / macOS-native target.

## Known limitations

See `TERRAIN_PROGRESS.md` for the rolling notes and bug investigations.
Highlights:

- **Fill drape draws but doesn't visibly contribute.** Confirmed via
  per-frame trace + forced-magenta test: `drawable.draw()` runs, Metal
  command is issued, but the resulting fragments don't reach the drape
  target's colour attachment. Needs Xcode Metal frame capture to
  diagnose pipeline state. Lines drape correctly with the same
  architecture, so the bug is fill-shader-specific.
- **Fill / line connected-geometry elevation.** Per-tile centre
  sampling produced visible seams at tile boundaries; needs per-vertex
  elevation via a bucket attribute change.
- **No FillOutlineTriangulated drape.** Metal-only niche.
- **No render-test baselines yet.** `style.json` scaffolds are in
  `metrics/integration/render-tests/terrain/`.
