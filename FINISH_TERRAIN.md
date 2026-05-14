# Finishing 3D terrain on MapLibre Native (Metal)

This is the work plan for completing JesseCrocker's `ios-terrain-claude`
draft PR (https://github.com/JesseCrocker/maplibre-native/pull/1) so that
3D terrain actually renders the map content over the displaced mesh,
instead of a checkerboard test pattern.

## What's already done

Per [TERRAIN.md](TERRAIN.md) and a code review of Jesse's branch, the
following is in place and working:

- **Style parsing**: `style::Terrain` parses the `"terrain"` block out
  of the style JSON and stores `source` + `exaggeration`. Conversion
  code in `src/mbgl/style/conversion/terrain.cpp` handles JSON → struct.
- **Render class**: `RenderTerrain` is constructed by the orchestrator
  for each terrain config. Holds an `Immutable<Terrain::Impl>` and a
  layer group.
- **DEM source lookup**: `update()` calls `orchestrator.getRenderSource()`
  to resolve the raster-dem source by ID and caches the pointer.
- **Mesh generation**: 128×128 vertex grid, indexed as ~32k triangles,
  using tile-space coords 0–EXTENT (8192). Stored as `int16_t` short4
  vertices to match the Metal attribute layout. Mesh is shared across
  all tiles.
- **Per-tile drawables**: For each `RasterDEMTile` with a loaded
  `HillshadeBucket`, `createDrawableForTile()` builds a `gfx::Drawable`
  bound to the shared mesh, a per-tile DEM texture, and the
  `TerrainShader`.
- **Metal shader (`include/mbgl/shaders/mtl/terrain.hpp`)**: vertex
  stage samples the DEM, decodes Mapbox-RGB to meters, multiplies by
  exaggeration, and projects to clip space with `drawable.matrix`.
  Fragment stage samples a `mapTexture` and outputs the color.
- **UBOs**: `TerrainDrawableUBO` (matrix), `TerrainTilePropsUBO` (dem_tl,
  dem_scale), `TerrainEvaluatedPropsUBO` (exaggeration). Tweaker
  populates them per-frame.

The pipeline up to "vertex displacement against a DEM texture" is wired
end-to-end. **What it lacks is the surface color.** Jesse's
`createTestMapTexture()` plants a checkerboard at texture slot 1; that's
what the user sees when they enable 3D terrain.

## The actual gap: a per-tile drape pass

In `maplibre-gl-js`, terrain rendering works like this:

1. The painter has a `renderToTexture: IRenderToTexture` member.
2. When terrain is active, for each layer in render order, painter
   calls `renderToTexture.renderLayer(layer, options)`. If terrain owns
   the layer, the call diverts the draw into a tile-sized FBO and
   returns `true` (signalling "I handled it; skip the on-screen draw").
3. Once all draped layers have rendered into their per-tile FBOs, the
   painter draws the terrain mesh for each visible tile, sampling that
   tile's FBO texture as the surface color.

MapLibre Native already has the offscreen primitive — `RenderTarget`,
declared in `include/mbgl/renderer/render_target.hpp`. A `RenderTarget`
owns an `OffscreenTexture` plus a set of `LayerGroup`s that render into
it. The orchestrator visits all registered render targets each frame
and runs their upload + render passes (`renderer_impl.cpp:278, :338`).

**Nothing currently routes layer drawables into per-tile render
targets when terrain is enabled.** That's the missing link.

## Plan

### Phase 1 — Tile-keyed RenderTarget cache (the drape sources)

Add a structure inside `RenderTerrain` that maps `OverscaledTileID →
RenderTargetPtr`, lifecycle-managed alongside `tilesWithDrawables`.

- **New file**: `src/mbgl/renderer/render_terrain_drape_cache.hpp/.cpp`
  - `class TerrainDrapeCache` — keyed by `OverscaledTileID`
  - `getOrCreate(context, tileID, size)` returns the `RenderTargetPtr`
  - On every frame's `update()`, evict entries for tiles that are no
    longer in `demSource->getRawRenderTiles()`
- **Modify `RenderTerrain::update()`** in
  `src/mbgl/renderer/render_terrain.cpp`:
  - For each visible DEM tile, ensure a `RenderTarget` exists in the
    cache
  - Register the cache's render targets with the orchestrator via
    `AddRenderTargetRequest` (just like Jesse already does for the
    main terrain layer group)

Tile size choice: match the DEM source tile size (256 for terrarium, 512
for Mapbox-RGB). Don't go bigger; it doesn't add detail and wastes GPU.

### Phase 2 — Route 2D layer drawables into per-tile drape targets

This is the heaviest change. Today every layer's drawables go into the
main on-screen layer groups. With terrain active, we need the same
drawables to be rendered into per-tile RenderTargets.

Approach A — clone drawables per tile (the JS approach):

- During `RenderOrchestrator::update()`, when terrain is enabled, for
  each layer that's "drapeable" (background, fill, line, raster — see
  `renderToTexture.shouldRenderLayer()` in gl-js for the exact list),
  also create a copy of its drawables targeted at each visible tile's
  RenderTarget, with the projection matrix replaced by that tile's
  local matrix.

Approach B — single-pass with viewport switching (simpler, smaller
diff): render the same drawable N times into N viewports, binding the
correct projection matrix each time.

Recommend **Approach A** — matches the JS architecture, easier to
reason about, gives the existing render loop a clean visit per target.
The cost is more drawable objects in memory; bounded by visible-tile
count (~16–64 typical) × drapeable-layer-count (~5–20).

Files to touch:
- `src/mbgl/renderer/render_orchestrator.cpp` — extend the per-frame
  loop that builds layer groups so it also builds per-tile clones for
  drape targets when terrain is active.
- `src/mbgl/renderer/layers/render_*_layer.cpp` for each drapeable
  layer type — most already produce drawables; we need them to accept
  an optional "render to this RenderTarget with this tile matrix"
  parameter.
- `src/mbgl/renderer/render_terrain.hpp/.cpp` — expose
  `getOrCreateDrapeTarget(tileID)` to the orchestrator.

### Phase 3 — Wire the drape texture into terrain drawables

In `RenderTerrain::createDrawableForTile()`:

- **Remove the `createTestMapTexture()` call** and the checkerboard
  helper entirely.
- For texture slot 1 (the `mapTexture` the fragment shader samples),
  fetch `drapeCache.get(tileID)->getTexture()` and bind that.
- If no drape target exists yet (first frame for a new tile), skip
  drawable creation — wait for the next frame. The DEM texture is
  already gated this way; same pattern.

### Phase 4 — Render order, depth, and 3D mode cleanup

Jesse currently has:

```cpp
builder->setRenderPass(RenderPass::Translucent);
builder->setDepthType(gfx::DepthMaskType::ReadOnly);
builder->setColorMode(gfx::ColorMode::unblended());
builder->setEnableDepth(false);
builder->setIs3D(false);
```

All marked `TEMP`. The correct config for 3D terrain:

```cpp
builder->setRenderPass(RenderPass::Opaque);
builder->setDepthType(gfx::DepthMaskType::ReadWrite);
builder->setColorMode(gfx::ColorMode::unblended());
builder->setEnableDepth(true);
builder->setIs3D(true);
```

Plus the orchestrator needs to know that when terrain is enabled,
**drape targets render before** the main framebuffer pass (already true
via the order in `renderer_impl.cpp:278/338` — drape targets are
`RenderTarget`s, they render first).

Set `staticData->has3D = true` when terrain is enabled so the
depth/stencil framebuffer attachments are configured correctly.
`renderer_impl.cpp:180` already has the hook (`renderTreeParameters.
has3D`) — confirm the render tree carries the bit when terrain is set.

### Phase 5 — `getElevation()` for layer-draping (optional, ship without)

`RenderTerrain::getElevation()` currently returns 0. Layer draping
(making symbol/line layers sit on the terrain surface instead of at
sea level) requires it to actually sample the DEM and interpolate.

This is **not blocking the basic ship** — without it, raster basemap
draping over terrain works, but symbols float. We can land Phase 1–4
first and treat layer-draping as a follow-up.

The JS reference is `terrain.ts:getElevation()` /
`terrain.ts:_getElevationForLngLatZoom()` — bilinear-interpolate four
neighboring DEM pixels at the requested tile coordinate.

## Build + test plan

1. **Local build environment**: requires Bazel 7.x + Xcode 16+ + a
   recent macOS. See `platform/ios/README.md`. Expect ~30–60 min first
   build, faster incrementals after.
2. **Test harness**: `platform/ios/app/MBXViewController.mm` already
   has terrain demo wiring (Jesse added it). Build the iOS sample app
   target, point it at a style with `"terrain": {...}`, observe.
3. **Logging**: Jesse left verbose `Log::Info` calls in the render
   path. Useful for early debugging. Strip before upstreaming.
4. **Render-test corpus**: `metrics/` contains pixel-diff render
   tests; add a `terrain-basic` test once the implementation is stable.

## Estimated effort

| Phase | Effort | Notes |
|-------|--------|-------|
| 1 — drape cache | 1–2 days | small, mostly mechanical |
| 2 — drawable cloning | 1–2 weeks | the real work; touches many layer types |
| 3 — wire drape texture | half a day | trivial once Phase 1+2 land |
| 4 — depth + render order | 1–2 days | mostly debugging once it "kinda works" |
| 5 — `getElevation()` | 2–3 days | optional for first ship |

Plus 2–5 days of iOS build/install/iterate friction. Total: 3–4 weeks
of focused single-engineer time to land a usable v1 on Metal/iOS.

OpenGL backend is a separate port — same algorithm, different shader
syntax + framebuffer plumbing. Add 1–2 weeks. Vulkan another 1–2.

## Upstreaming path

1. Land Phase 1–4 cleanly in this fork.
2. Open follow-up PR(s) against `JesseCrocker/maplibre-native:ios-terrain-claude`
   so the diff is small and reviewable.
3. Once Jesse signs off, work with him to retarget the combined branch
   at `maplibre/maplibre-native:main`.
4. The MapLibre Native maintainers (`louwers`, `birkskyum`,
   `sjg-wdw`) have all explicitly said they'd accept this work in
   issue #252 — funding has been their constraint, not policy.

## Working state

- Fork checkout: `/Users/mac/projects/maplibre-native`
  (branch `ios-terrain-claude`)
- Reference: `/Users/mac/projects/maplibre-gl-js`
  (sparse checkout of `src/render`, `src/style`)
- GitHub PR for reference: https://github.com/JesseCrocker/maplibre-native/pull/1
- Upstream issue: https://github.com/maplibre/maplibre-native/issues/252
