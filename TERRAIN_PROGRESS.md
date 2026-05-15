# 3D terrain on MapLibre Native — progress log

This file tracks the work to land 3D terrain in `maplibre-native`'s Metal
backend, ahead of (or alongside) the official upstream effort discussed
in [maplibre-native#252](https://github.com/maplibre/maplibre-native/issues/252).
It's a rolling log — read top-to-bottom for the latest state, then back
for context.

For the **plan** (what's done, what's missing, why), see
[FINISH_TERRAIN.md](FINISH_TERRAIN.md).
For Jesse's **initial design doc**, see [TERRAIN.md](TERRAIN.md).

## Lineage and credit

This branch builds on
[JesseCrocker/maplibre-native@ios-terrain-claude](https://github.com/JesseCrocker/maplibre-native/pull/1),
which contributed the bulk of the scaffolding:

- `style::Terrain` and JSON parsing
- `RenderTerrain` class + orchestrator integration
- DEM source lookup
- 128×128 mesh generation
- Per-tile drawable creation with DEM texture upload
- Metal vertex shader that actually decodes Mapbox-RGB elevations and
  displaces the mesh

Without that work this branch wouldn't exist. The remaining gap was
exactly what TERRAIN.md called out under "Current Limitations": no
drawable creation for the drape pass, no DEM source binding for layers,
no draping of other layer types. This work picks up there.

## Status

| Phase | State | Result |
|------|-------|--------|
| 0 — Xcode 26 / clang strictness | ✅ Done | Build passes again on Xcode 26.4 |
| 0 — Klättra rebrand of sample app | ✅ Done | Iteration target with our own DEM data |
| 1 — Per-tile drape RenderTarget cache | ✅ Done | Targets allocated, registered, pruned |
| 3 — Bind drape texture into terrain shader | ✅ Done | Terrain mesh samples drape target instead of checkerboard |
| 2 — Scaffolding: `RenderLayer::activeTerrain` hook | ✅ Done | Layers can access `RenderTerrain` during `update()` |
| 2 — `RenderTerrain::getDrapeTarget(tileID)` accessor | ✅ Done | Public lookup for drape RenderTarget per tile |
| End-to-end pipeline verified | ✅ Done | Terrain mesh visibly samples drape target's clear colour — every step from cache allocation → GPU render pass → texture binding → fragment sampling confirmed |
| 2 — `RenderBackgroundLayer` drape routing | ✅ Done | 7 drape drawables emitted across 6 visible tiles; terrain mesh visibly samples the basemap's background colour |
| 2 — Other drapeable layers (fill, line, raster) | ⏳ Pending | Same pattern as background, repeated per layer type |
| 4 — Proper depth + opaque pass + `setIs3D(true)` | ⏳ Pending | Mostly debugging once it "kinda works" |
| 5 — `getElevation()` for layer draping | ⏳ Pending | Optional for first ship |

Note: Phase 3 wiring landed before Phase 2 because it's a 50-line change
that's safe to merge — the visual state goes from "checkerboard on
displaced mesh" to "transparent black on displaced mesh", both incorrect,
but the latter proves the texture-binding path. Phase 2 fills the
targets and the mesh comes alive without further changes to the
terrain drawable code.

## How to build and run

### Prerequisites

- macOS with **Xcode 26.4** (older versions also work but expect different
  diagnostics; see Phase 0 notes below)
- Bazel via `bazelisk`:
  ```sh
  brew install bazelisk
  ```
  The repo pins Bazel 8.4.1 in `.bazelversion`; bazelisk grabs it on first
  invocation.
- All submodules initialised:
  ```sh
  git submodule update --init --recursive --depth 1
  ```
  This pulls ~500 MB of vendor code (boost, harfbuzz, freetype, etc.).

### Build the static xcframework

```sh
bazel build //platform/ios:MapLibre.static --//:renderer=metal
```

First build takes ~7 minutes on an M-series Mac and produces
`bazel-bin/platform/ios/MapLibre.static.xcframework.zip` (~71 MB).
Incremental rebuilds are sub-30s.

### Build and run the Klättra sample app

```sh
bazel build //platform/ios:App --//:renderer=metal \
  --ios_simulator_device="iPhone 16 Pro"
```

Then install + launch in the simulator:

```sh
DEVICE_ID=$(xcrun simctl create "iPhone 16 Pro" \
  "com.apple.CoreSimulator.SimDeviceType.iPhone-16-Pro" \
  "com.apple.CoreSimulator.SimRuntime.iOS-26-4")
xcrun simctl boot "$DEVICE_ID"
open -a Simulator
xcrun simctl install "$DEVICE_ID" \
  bazel-bin/platform/ios/App_archive-root/Payload/App.app
xcrun simctl privacy "$DEVICE_ID" grant location app.klattra.dev
xcrun simctl launch "$DEVICE_ID" app.klattra.dev
```

The app launches centred on Kebnekaise (Sweden's highest peak) with the
"Klättra (Sweden 3D)" style as default. The terrain mesh is displaced
correctly against `sweden-dem.pmtiles`, but until Phase 2 lands you'll
see Jesse's checkerboard placeholder surface texture instead of the
real basemap. That's expected.

### Iteration loop while editing rendering code

Almost everything is cached after the first build, so the cycle is:

```sh
bazel build //platform/ios:App --//:renderer=metal \
  --ios_simulator_device="iPhone 16 Pro"
xcrun simctl terminate "$DEVICE_ID" app.klattra.dev
xcrun simctl install "$DEVICE_ID" \
  bazel-bin/platform/ios/App_archive-root/Payload/App.app
xcrun simctl launch --terminate-running-process "$DEVICE_ID" app.klattra.dev
xcrun simctl io "$DEVICE_ID" screenshot /tmp/run.png
```

End-to-end ~30 seconds per change. To see RenderTerrain's verbose
logging, dump the system log filtered to the app:

```sh
xcrun simctl spawn "$DEVICE_ID" log show \
  --predicate 'processImagePath CONTAINS "/App.app/App"' \
  --info --last 1m | grep -iE "terrain|drape|dem"
```

## Test data

The sample app's "Klättra (Sweden 3D)" style points at three public
Supabase URLs:

```
sweden-topo.pmtiles          # vector basemap (Lantmäteriet Topografi 10/50)
sweden-dem.pmtiles           # raster-dem (Lantmäteriet 1m, resampled to 10m,
                             #   Mapbox-RGB encoded, z8-z13, Sweden coverage)
sweden-terrain-test-style.json  # the style that wires it all together
```

These are bytes-on-disk in the `maps/` bucket of a Supabase project, served
over HTTP Range requests via the standard pmtiles:// protocol. Anyone
can use this style as a real-data terrain test — no API key, no auth.

## Architecture decisions

### Why per-tile RenderTargets (Phase 1)

We could have tried a single fullscreen RenderTarget, but the gl-js
implementation uses per-tile targets and there are concrete reasons:

- **Texture aliasing matches the mesh.** Each terrain drawable has its
  own 128×128 mesh covering one tile. If a single big target were used,
  every drawable would have to do extra math to clip the sample into
  its tile's region. Per-tile targets keep `uv = pos / EXTENT` valid as
  Jesse already wrote it.
- **Caching across frames.** A drape target whose contents haven't
  changed (no new tiles loaded, no layer paint changes, camera moving
  but not crossing tile boundaries) doesn't need re-rendering. Per-tile
  granularity is the natural unit of invalidation.
- **Memory bound on visible tiles.** Worst-case ~64 visible tiles
  × 512² × RGBA = ~67 MB of offscreen textures. Comfortable on iOS.

### Why we didn't refactor `createTestMapTexture` away yet (Phase 1 vs 3)

The Phase 1 commit leaves Jesse's checkerboard placeholder in place
even though the drape cache now allocates real targets. This was
deliberate so Phase 1 could land as a no-visual-change scaffolding
commit. Phase 3 swapped the binding (commit `5a6ff27`) — now the
terrain shader samples the drape target's texture, which is currently
empty, so the mesh renders dark instead of checkerboard. The
checkerboard fallback is kept only for the defensive case where the
cache somehow doesn't have a target for a tile.

### Why the new `TerrainDrapeTargetPtr` alias

`include/mbgl/renderer/render_target.hpp` declares `class RenderTarget`
but does **not** export a `RenderTargetPtr` typedef. The alias is
duplicated independently in `change_request.hpp`, `gfx/context.hpp`,
`vulkan/context.cpp`, and `gl/context.cpp`. That's a long-standing
inconsistency we didn't want to widen, so we use a locally-named alias
(`TerrainDrapeTargetPtr`) in the cache header and stay self-contained.
The right cleanup, if we upstream this, is to add `using
RenderTargetPtr = std::shared_ptr<RenderTarget>;` to `render_target.hpp`
itself and use that everywhere. Out of scope here.

## Phase 2 design notes

Phase 2 is "route 2D layer drawables into per-tile drape RenderTargets
when terrain is active". It's the heavy lift — the other phases are all
plumbing around it. Documenting the entry points and design tradeoffs so
the next contributor can pick up cleanly.

### The orchestrator hook

`src/mbgl/renderer/render_orchestrator.cpp:1002`:

```cpp
for (const auto& item : items) {
    auto& renderLayer = item.layer.get();
    renderLayer.update(shaders, context, state, updateParameters,
                       renderTree, changes);
}

if (renderTerrain && renderTerrain->isEnabled()) {
    renderTerrain->update(*this, shaders, context, state,
                          updateParameters, renderTree, changes);
}
```

Each `renderLayer.update()` is what causes a `RenderBackgroundLayer`,
`RenderFillLayer`, etc. to emit `AddDrawableRequest`s for the main
layer-group hierarchy. This is the natural intercept point. Terrain
runs AFTER the per-layer loop, which is correct for Phase 1+3 (we need
visible-tile data to know what targets to allocate), but Phase 2 needs
to influence the loop itself.

### Three design approaches and tradeoffs

1. **Per-layer terrain-awareness.** Add a `RenderTarget*` (or tile set)
   argument to `RenderLayer::update()`. Each layer type knows how to
   redirect its drawables. Highest fidelity, but touches every layer
   class.

2. **Change-request interception.** After each `layer.update()` call,
   walk the new `AddDrawableRequest`s. If terrain is on and the layer
   is drapeable, clone the request to also target each visible tile's
   drape target with the tile's projection matrix. Mid-blast-radius;
   doesn't require modifying layer classes, but the per-tile cloning
   has to happen at the request level which is awkward (drawables
   aren't cheaply clone-able).

3. **Drawable shim layer.** Introduce a new abstraction — a
   `RoutingLayerGroup` that wraps the main layer group and, when
   terrain is on, splays each `addDrawable()` call across the visible
   tiles' drape-target layer groups. Layers stay unaware; the
   orchestrator hands them this routing wrapper instead of the main
   layer group when terrain is on. Cleanest in terms of layer code,
   but requires careful matrix arithmetic in the wrapper.

`maplibre-gl-js` uses **approach 1** via the
`IRenderToTexture.renderLayer(layer, renderOptions)` interface (see
`src/render/render_to_texture_interface.ts` and `src/render/painter.ts:530+`).
Each layer is given a chance to "render itself into the texture"
before the painter renders it normally. The texture binding stays in
the layer, which keeps its matrix handling intact.

Recommendation for MapLibre Native: **mirror approach 1.** It matches
the JS architecture (easier to port the gl-js logic file-by-file), it
keeps layer-specific matrix math inside the layer (so we don't have
to re-implement projection math in a wrapper), and the existing
`RenderLayer::update()` signature already takes a `changes` vector
which the layer can populate differently.

### Concrete first step (when picking this up)

Scaffolding has landed in commits `bfa03ef` and `2ce7ce7`:

- `RenderTerrain::getDrapeTarget(const OverscaledTileID&)` returns
  the per-tile drape RenderTarget (or nullptr).
- `RenderLayer` has an `activeTerrain` field that the orchestrator
  sets to the current `RenderTerrain*` right before each layer's
  `update()` runs (and clears immediately after). The orchestrator
  also reordered so `renderTerrain->update()` runs FIRST, ensuring
  the cache is populated by the time layers look it up.

Now the actual routing. Start with **background layer only** —
simplest case (flat colour, no per-tile geometry):

1. In `src/mbgl/renderer/layers/render_background_layer.cpp`,
   inside `RenderBackgroundLayer::update()`, after the existing
   `tileLayerGroup` setup:
   ```cpp
   if (activeTerrain) {
       // For each visible tile in tileCover, look up its drape target
       // and add a background drawable to its layer group at this
       // layer's index, in addition to (or instead of) the main
       // tileLayerGroup.
       for (const auto& tileID : tileCover) {
           if (auto drape = activeTerrain->getDrapeTarget(
                   tileID.toOverscaledTileID())) {
               // ... build drawable, add to drape->layerGroup ...
           }
       }
   }
   ```
2. The drape RenderTarget's layer-group slot for this layer index
   may not exist on first use — create a `TileLayerGroup` for it
   and call `drape->addLayerGroup(...)`. Subsequent calls reuse.
3. Verify visually: terrain mesh now renders the background colour
   (typically white or a subtle base color) draped over the
   displaced surface instead of black.
4. Once that works, repeat the pattern for `RenderFillLayer`,
   `RenderLineLayer`, `RenderRasterLayer` in order of complexity.

`RenderSymbolLayer` is special — symbols (text + icons) typically
should NOT be draped (they should float above the terrain at their
unprojected positions). Per gl-js, symbol layers skip the drape pass
and render to the main framebuffer last, on top of the terrain.

### Working precedent in the codebase

`src/mbgl/renderer/layers/render_hillshade_layer.cpp` lines 244-296 already
implement exactly the pattern Phase 2 needs:

```cpp
// 1. Allocate a RenderTarget for the tile
auto renderTarget = context.createRenderTarget({tilesize, tilesize},
                                               gfx::TextureChannelDataType::UnsignedByte);
addRenderTarget(renderTarget, changes);   // AddRenderTargetRequest

// 2. Create a single-tile LayerGroup inside the target
auto singleTileLayerGroup = context.createTileLayerGroup(
    /*layerIndex=*/0, /*initialCapacity=*/1, getID());
renderTarget->addLayerGroup(singleTileLayerGroup, /*replace=*/true);

// 3. Set up a tweaker specifically for the offscreen pass (different
//    matrix arithmetic than the main framebuffer)
auto prepareLayerTweaker = std::make_shared<HillshadePrepareLayerTweaker>(...);
singleTileLayerGroup->addLayerTweaker(prepareLayerTweaker);

// 4. Build a drawable with the prepare-pass shader + texture bindings
auto builder = context.createDrawableBuilder("hillshadePrepare");
builder->setShader(hillshadePrepareShader);
builder->setTexture(demTexture, idHillshadeImageTexture);
// ...
builder->flush(context);

// 5. Add the drawable into the target's layer group
for (auto& drawable : builder->clearDrawables()) {
    drawable->setTileID(tileID);
    drawable->setLayerTweaker(prepareLayerTweaker);
    singleTileLayerGroup->addDrawable(renderPass, tileID, std::move(drawable));
}
```

For Phase 2 background routing, swap:
- `createRenderTarget(...)` → `activeTerrain->getDrapeTarget(tileID)`
  (we already have it from Phase 1)
- `HillshadePrepareLayerTweaker` → `BackgroundLayerTweaker` (existing)
  or a new `BackgroundDrapeLayerTweaker` if matrix math differs
- `hillshadePrepareShader` → background's `plainShader` / `patternShader`
- Texture bindings → none for plain background, pattern texture for
  patterned background
- Vertex attributes → background's existing fullscreen-tile quad

The matrix-arithmetic difference is what makes the drape tweaker
separate: a drape target renders the tile content at tile-local coords
(0..EXTENT) into a tile-sized texture (with NDC -1..1), no camera
transform. The main-framebuffer tweaker applies camera + projection.
Hillshade's `HillshadePrepareLayerTweaker` is the reference for how
the drape-pass matrix is computed; copy that pattern for background.

### Open question for Phase 2 — "both or neither?"

When a layer routes to a drape target, should it ALSO add to the
main `tileLayerGroup`?

- gl-js: **neither** — `renderToTexture.renderLayer()` returns true
  meaning "I handled this; skip the main framebuffer pass entirely."
- Initial port for safety: **both** — simpler, but causes flat
  basemap to render to main framebuffer underneath the terrain mesh.
  Visible only if the terrain mesh doesn't fully cover the viewport
  (e.g., low-zoom views with sky around the edges).
- Long term: match gl-js. Either swap which group a drawable goes to
  based on `activeTerrain`, or render both but suppress main-pass
  drawing of drapeable-layers when terrain is on.

### Files that will need touching (Phase 2)

- `include/mbgl/renderer/render_layer.hpp` — add the drape-target hook
  to `RenderLayer::update()`
- `src/mbgl/renderer/render_orchestrator.cpp` — line ~1002, pass the
  drape-target lookup through to each layer's update
- `src/mbgl/renderer/render_terrain.hpp/.cpp` — add a public
  `getDrapeTarget(tileID)` accessor (or a "for each visible tile,
  drape target" callable)
- `src/mbgl/renderer/layers/render_background_layer.cpp` (start here)
- `src/mbgl/renderer/layers/render_fill_layer.cpp`
- `src/mbgl/renderer/layers/render_line_layer.cpp`
- `src/mbgl/renderer/layers/render_raster_layer.cpp`
- Skip `render_symbol_layer.cpp` for first ship (see above)

## Known issues / things to revisit

- **Drape targets aren't deregistered.** `pruneIf` evicts entries from
  the cache, but we don't emit a corresponding `RemoveRenderTargetRequest`
  to the orchestrator. The orchestrator therefore keeps a stale reference
  until the next style change. Tiny memory creep on map pans; harmless
  for testing but should be fixed before upstreaming. Approach: change
  `pruneIf` to return the evicted `(id, target)` pairs and emit the
  removal request in `RenderTerrain::update()`.

- **Verbose logging in `render_terrain.cpp` left in place.** Jesse added
  `Log::Info` calls at every step (`Created terrain layer group`,
  `Terrain examining tile ...`, etc.). Useful for early debugging; noisy
  for production. Strip before upstreaming.

- **`createTestMapTexture` is dead code once Phase 3 lands.** Remove
  along with the `mapTexture` slot-1 binding in `createDrawableForTile`.

- **`getElevation()` returns `0.0f`.** Layer draping (Phase 5) needs
  bilinear DEM sampling. Port from `terrain.ts:_getElevationForLngLatZoom`.

- **Renderer is Metal-only.** OpenGL backend is a separate port; same
  algorithm, different shader syntax + framebuffer plumbing. Vulkan is
  also separate.

- **`Klättra` rebrand should be reverted before upstreaming.** Bundle
  id, display name, default camera, "Traska Sweden Terrain" style entry,
  and the location-permission ornament default are all our test
  configuration, not appropriate for the official sample app. The
  Xcode 26 fixes and the terrain code itself are upstream-safe.

## Upstreaming path

When this branch is ready (Phase 1–4 landed, working terrain on Metal):

1. Squash all changes into discrete, reviewable commits:
   - Three Xcode 26 fixes → one commit each, each landable on
     `maplibre/maplibre-native:main` independently
   - Phase 1 (drape cache) → one commit
   - Phase 2 (drawable routing) → potentially several
   - Phase 3 (texture binding) → one commit
   - Phase 4 (depth/opaque cleanup) → one commit
   - Revert the Klättra-branding diff
2. Open PRs against `JesseCrocker/maplibre-native:ios-terrain-claude`
   first (his branch is the natural integration point).
3. Once Jesse signs off, work with him to retarget the combined branch
   at `maplibre/maplibre-native:main`. The maintainers
   ([louwers](https://github.com/louwers),
   [birkskyum](https://github.com/birkskyum),
   [sjg-wdw](https://github.com/sjg-wdw)) all said in issue #252 that
   the policy isn't the blocker — funding for someone to do the work
   was, and that work is what's in this branch.

## Phase 0 fixes (Xcode 26 / clang)

Newer Xcode SDKs caught three latent issues in `maplibre-native:main`
itself, not specific to the terrain work. These need to be in any branch
that builds on Xcode 26.4. All three are tiny and should be PR-able
upstream independently.

### `platform/darwin/src/MLNReachability.m`

`#import <netinet6/in6.h>` directly is now an `#error` in the SDK per
RFC 2553. Dropped the explicit import; `<netinet/in.h>` transitively
provides the IPv6 definitions.

### `platform/darwin/src/MLNMapSnapshotter.mm`

```objc
auto localFontFamilyName = config.localFontFamilyName
    ? std::string(config.localFontFamilyName.UTF8String)
    : nullptr;
```

The ternary can't unify `std::string` and `nullptr` in newer clang.
`mbgl::MapSnapshotter`'s constructor takes
`std::optional<std::string>`, so construct that explicitly with
`std::nullopt` for the absent case.

### `platform/ios/src/MLNMapView.mm` (around line 6479)

```objc
if (strongSelf.userTrackingState == MLNUserTrackingStateBegan ||
    strongSelf.userTrackingState == MLNDistanceThresholdForCameraPause)
```

The second clause compares an `NSUInteger` enum value against a
`CLLocationDistance` (`= 500.0`). It was always false (enum values are
`0..3`), so the OR was effectively just the first clause. Newer clang
errors on the type mismatch. Dropped the dead clause.

## Session log

### 2026-05-14

- Cloned `JesseCrocker/maplibre-native:ios-terrain-claude` into
  `/Users/mac/projects/maplibre-native`
- Sparse-cloned `maplibre-gl-js` (just `src/render`, `src/style`) into
  `/Users/mac/projects/maplibre-gl-js` for reference
- Read TERRAIN.md and the relevant files; wrote FINISH_TERRAIN.md plan
- Three Xcode 26 fixes (commit `1107449`)
- Klättra rebrand of the sample app (commit `e8b07e8`)
- Built and ran Klättra in the iPhone 16 Pro simulator
- Confirmed the terrain mesh is rendered correctly displaced against
  our Sweden DEM data — the checkerboard placeholder warps to match
  Kebnekaise topography exactly as predicted, confirming
  `RenderTerrain::createDrawableForTile` + the Metal vertex shader work
  end-to-end with real production DEM tiles
- Phase 1: TerrainDrapeCache (commit `d453783`)
- Wrote TERRAIN_PROGRESS.md (commit `bd3924e`)
- Phase 3 wiring: bind drape texture into terrain drawables (commit
  `5a6ff27`). Visual check confirmed pipeline: terrain mesh now
  samples the (currently empty) drape target rather than the
  checkerboard, so the mesh renders dark on Kebnekaise. Once Phase 2
  populates the drape targets with real layer content, the same
  drawable configuration will display the basemap draped over the
  displaced mesh — no further changes in terrain drawable code.
- TERRAIN_PROGRESS Phase 2 design notes (commit `60b6279`)
- `RenderTerrain::getDrapeTarget(tileID)` public accessor
  (commit `bfa03ef`)
- Phase 2 scaffolding: `RenderLayer::activeTerrain` hook + orchestrator
  reordering (commit `2ce7ce7`). No runtime change yet — runtime path
  identical because no layer subclass uses `activeTerrain`. Next step
  is implementing the routing in `RenderBackgroundLayer::update()`.
- End-to-end visual proof (commit `a876439`). Added a configurable
  clear colour on `RenderTarget` (default unchanged) and set drape
  targets to a muted forest-green debug colour. Terrain mesh now
  renders that green — confirming **every step of the drape pipeline
  is wired correctly** from cache allocation through GPU clear pass
  through texture binding through fragment sampling. The faint warp
  seam at the displaced mesh edge confirms DEM displacement is still
  happening alongside the new colour. This is the architectural
  proof point — Phase 2 routing now just has to overwrite the green
  with real basemap pixels.
- Phase 2 structural framework (commit `2af2def`). Background layer
  emits drape drawables alongside the main path. Compiles and runs,
  but visual unchanged at this point because of tile-ID mismatch
  + matrix-arithmetic issues, both documented.
- Phase 2 drape tweaker (commits `584d433`, `8a6d4a0`). Added a
  `drapeMode` flag on `BackgroundLayerTweaker` that swaps the
  per-drawable matrix from `getTileMatrix(...)` (camera-aware) to
  `matrix::ortho(0, EXTENT, -EXTENT, 0, -1, 1)` + translate — exact
  template from `HillshadePrepareLayerTweaker`. Background layer
  instantiates a dedicated drape tweaker and attaches it to the
  per-target layer group.
- Phase 2 lands visually (commit `93d3970`). Two fixes turned the
  routing from "code path runs but does nothing" into "drawables
  actually emit into drape targets":
    1. `RenderOrchestrator::update()` now bypasses the
       `backgroundLayerAsColor` optimisation when terrain is active.
       That optimisation normally skips `RenderBackgroundLayer::update()`
       entirely (sets framebuffer clear colour instead), which
       starved the drape pass — no update() → no drape drawables.
    2. `RenderBackgroundLayer` iterates the drape cache directly
       via the new `RenderTerrain::visitDrapeTargets(...)` rather
       than its own `tileCover`. The two were at different zooms
       (basemap at z=10, DEM source at z=8 in our test), so
       `getDrapeTarget(tileID)` by tileCover IDs always returned
       nullptr.
  After these two fixes, log evidence shows 7 drape drawables
  emitted across the 6 visible DEM tiles in the first ~150 ms after
  the cache populated. Visual: the terrain mesh now renders in the
  topo style's actual background colour (#F4EAD0 beige) instead of
  the debug forest-green clear. **First confirmed working drape
  pass on MapLibre Native iOS.**
- Diffuse lighting added to terrain fragment shader (commit
  `a341c6a`). The fragment reconstructs a screen-space surface normal
  from `dfdx`/`dfdy` of the elevation varying and applies Lambertian
  shading (ambient 0.55, directional 0.45). With this alone the screen
  was still uniform beige — diagnosis was correct: mesh flat in clip
  space.
- **Phase 4: terrain matrix Z-scale fix (commit `1853d53`)** — 5-line
  change in `TerrainLayerTweaker`. The vertex shader passes elevation
  in metres on the Z axis, but `matrixForTile()`'s base tile matrix
  scales X/Y from tile units to mercator world-pixels and leaves Z
  scale at 1. So metres were going into clip space at the wrong scale
  and the mesh stayed essentially flat. Mirrored the trick the camera
  uses in `Camera::getWorldToCamera`: multiply the matrix's Z column
  (indices 8..11) by `pixelsPerMeter` at the current latitude/zoom.
  Result: **first 3D terrain rendering on MapLibre Native iOS.** Real
  Kebnekaise extrusion, with the diffuse lighting from the previous
  commit producing visible hillshade across the whole mesh. The drape
  pipeline (per-tile RenderTarget cache → background layer routed
  into drape → texture bound to mesh fragment) and the 3D extrusion
  pipeline (DEM-sampled elevation → exaggerated → matrix-projected
  in metres) are both verified end-to-end. Phase 2 still needs to
  expand from background to fill/line/raster/symbol; Phase 4 still
  needs proper depth-write + opaque-pass + `setIs3D(true)` before
  this is fit for upstream review. But the visual proof is in.

## Known bug discovered during skip-main-pass experiment

When `RenderFillLayer`'s main `TileLayerGroup` is disabled
(`setEnabled(false)`) while terrain is active, the visible glaciers /
water on the terrain mesh disappear. This means the fills we *thought*
were appearing on the terrain mesh via drape were actually the
main-pass fill drawables rendering on top of the terrain mesh (depth
test apparently doesn't reject them in translucent pass) — the drape
routing emits fill drawables into the drape target's TileLayerGroup,
but those drawables don't visibly contribute to the terrain mesh's
sampled texture.

Things that might be wrong:
- Drape target's depth buffer never clears between frames (RenderTarget
  passes `.clearDepth = {}`). Each frame's drape drawables write z=0;
  next frame's identical z=0 fail the LESS depth test.
- Fill drape builder uses `DepthMaskType::ReadWrite` (writes depth);
  line drape uses `ReadOnly` (no write). The line drape visibly works,
  the fill drape doesn't — consistent with the depth-buffer-not-clearing
  hypothesis since lines don't disturb it.
- Easy fix to try first: set fill drape `DepthMaskType::ReadOnly`. If
  that works it's evidence for the depth-not-clearing root cause.

This explains why the visual was so close to baseline even on the
phases where drape was supposedly "the source of truth": main pass
content was always on top. The drape pipeline is structurally correct
but the per-target render needs depth clearing (or depth-disabled
drape drawables) before "skip main pass when terrain is active" can
be safely turned on.

## Overnight debug session findings (2026-05-15)

Dug deeper into the fill-drape-not-visible bug. Added per-frame
instrumentation in `mtl::TileLayerGroup::render` to log every drape
group's `drew=N` count. Output for one frame at Kebnekaise (with main
fill enabled):

```
[drape-trace] background-drape drew=1            (pass=1 opaque)
[drape-trace] background-drape drew=1            (pass=2 translucent)
[drape-trace] land-open-drape drew=1             (pass=2)
[drape-trace] land-kalfjall-drape drew=1         (pass=2)
[drape-trace] land-glaciar-drape drew=2          (pass=2)
[drape-trace] skog-barr-drape drew=1             (pass=2)
[drape-trace] vatten-drape drew=2                (pass=2)
[drape-trace] ralstrafik-drape drew=1            (pass=2)
[drape-trace] vag-huvud-casing-drape drew=1      (pass=2)
```

So fill drape `drawable.draw(parameters)` IS being called — twice for
land-glaciar (glacier) and twice for vatten (water). The drawables
exist, the render path reaches them, the Metal command encoder gets
their draw calls. Yet they don't visibly contribute to the drape
texture (verified by setting terrain shader to return raw mapColor —
texture only has BG cream + line content, no fill colours).

Things ruled out by repeated A/B testing:

- **Depth-buffer stale**: Switched `RenderTarget::render` to clear
  depth to 1.0 each frame. No change. (Kept the fix — it's correct.)
- **Depth state**: Tried `DepthMaskType::ReadOnly`,
  `setEnableDepth(false)`. No change.
- **Stencil**: `setEnableStencil(false)` explicit. No change.
- **Shared vertex-attrs ownership**: Switched drape FillBuilder to own
  its own `VertexAttributeArray` (don't share with main fillBuilder
  which std::moves them later). No change. (Kept the fix — it's
  correct.)
- **Missing position vertex attribute**: Explicitly bound
  `idFillPosVertexAttribute` from `bucket.sharedVertices` on the drape
  vertexAttrs. No change. (Kept the fix — it's correct.)
- **Skip main pass**: Disabling `fillTileLayerGroup` via setEnabled —
  removes main fill overlay, confirms drape doesn't contribute.
- **Per-tweaker propagation**: Drape FillLayerTweaker still iterates
  its drawables and computes drape matrix correctly.

What still differs between line drape (works) and fill drape (doesn't):

- Different bucket type and vertex layout (FillBucket vs LineBucket).
- Different paint property binders (different `idLineColorVertexAttribute`
  vs `idFillColorVertexAttribute` setups).
- Different fragment shader (line.hpp vs fill.hpp).

Likely next steps for whoever picks this up:

1. **GPU frame capture** via Xcode Metal debugger — inspect the drape
   target's texture content between BG draw and fill draw to confirm
   whether fill fragments are reaching the framebuffer.
2. **Compare bucket vertex layouts** — does FillBucket need extra
   attributes (e.g. `attributes::pos` only vs Line's pos + normal +
   data)?
3. **Inspect the actual UBO/uniform values** passed to the fill
   fragment shader on the drape path. Maybe `props.opacity` ends up
   0 or `props.color` ends up transparent for the per-drape-target
   FillLayerTweaker.
4. **Try copying the exact shape of RenderHillshadeLayer's prepare-
   builder setup**, which uses a RenderTarget with a single full-tile
   quad and visibly works upstream. The structural similarity might
   reveal a missing call.

### Phase 5 symbol elevation (2026-05-15, morning)

Commit `2db2590` lifts symbol drawables off z=0 to the per-tile centre
elevation when terrain is active. Three small changes:

1. `PaintParameters` gains a `const RenderTerrain* activeTerrain`.
2. `RendererImpl` sets it after the orchestrator's update.
3. `SymbolLayerTweaker` calls `activeTerrain->getElevation(tileID,
   0.5, 0.5)` and translates the symbol matrix's Z column by
   `elevation * pixelsPerMeter` (same scale fix the terrain mesh uses).

Tried extending the same per-tile elevation to fill and line tweakers
too. **That produced visible discontinuities at tile boundaries** —
two adjacent tiles sampled different centre elevations, so connected
polygons / lines got a vertical step between them. Reverted both
back to z=0 for connected geometry. Per-symbol elevation works fine
because symbols are point features (no inter-tile connectivity).

For proper terrain-aware fill / line rendering we'd need per-vertex
elevation: a vertex attribute populated when the bucket is built,
sampled at each vertex's actual position. That's a bucket-layout
change and deferred for now — main-pass fill / line at z=0 is fine
because the terrain mesh occludes them via depth anyway.

### Overnight push 2 (2026-05-15, ~01:00)

Continued working after the first overnight session reached a natural
stopping point. Eight more commits:

1. **GL backend port** (`24efce5`) — terrain shader in GLSL ES,
   ShaderInfo registration, registerTypes inclusion. Core lib builds
   clean with `--//:renderer=drawable`. Documented under "GL backend
   port" below.

2. **Fill drape uses its own shader pointer** (`468df43`) — instead
   of borrowing the main builder's. Matches line / hillshade pattern.
   Architectural correctness; didn't fix the visibility bug.

3. **LineGradient drape** (`bedfb37`) — extends `emitDrapeLineVariant`
   with an optional `configureExtras` callback for variant-specific
   texture binding. Color-ramp texture attaches via the callback.

4. **LinePattern drape** (`50b30a3`) — atlas tweaker on drape builder
   via the same configureExtras hook. After this, **all four line
   variants drape**.

5. **FillPattern + FillOutlinePattern drape** (`7ff5c77`) — same
   pattern, atlas tweaker via configureExtras on the fill version of
   the lambda. After this, **four of five fill variants drape**.
   (FillOutlineTriangulated is Metal-only niche, left for later.)

6. **Render-test scaffolds** (`f444e8b`) — two style.json files under
   `metrics/integration/render-tests/terrain/` for `default` and
   `exaggeration`. Baseline `expected.png` files need to be generated
   on a known-good build.

7. **PR_HANDOFF updates** (`a88acca`, `92f9a85`) — cherry-pick list
   kept in sync with the new commits.

Branch ends overnight at **52 commits** with clean working tree.

### GL backend port (2026-05-15)

Commit `24efce5` ports the terrain shader to the OpenGL backend:

- `include/mbgl/shaders/gl/terrain.hpp` — vertex + fragment shaders in
  GLSL ES with `std140` uniform blocks. Mirrors the Metal shader
  structurally: DEM-sampled elevation in the vertex stage, drape-texture
  sample + dfdx/dfdy diffuse hillshading + style light in the fragment
  stage. Falls back to elevation gradient when the drape is empty,
  same as Metal.
- `include/mbgl/shaders/gl/shader_info.hpp` — declares the
  `ShaderInfo<TerrainShader, OpenGL>` specialisation alongside the
  others.
- `src/mbgl/shaders/gl/shader_info.cpp` — defines the attribute /
  uniform block / texture registration so the linker knows about
  `TerrainDrawableUBO`, `TerrainEvaluatedPropsUBO`, the two attributes
  and the two textures.
- `src/mbgl/gl/renderer_backend.cpp` — adds `BuiltIn::TerrainShader`
  to the GL backend's `initShaders` registration list.

Core lib (`//:mbgl-core`) builds clean with `--//:renderer=drawable`
(GL/drawable mode). Cannot visually verify on iOS — MapLibre Native
iOS is Metal-only (`MLNMapView.mm` hardcodes `mbgl/mtl` headers), so
the GL shader will exercise on Android / Linux / macOS native targets.
Metal still builds clean (no regression).

### Overnight session summary (2026-05-15, 00:00–00:30)

Three commits added while the user slept (`de288b0`, `c859228`,
`fcb5b8d`, `7ea37a8`):

1. **Two architectural fixes shipped** (commit `de288b0`):
   - `RenderTarget::render` now clears depth to 1.0 per frame.
     Previously empty optional left stale depth from prior frames.
   - `RenderFillLayer` fill drape now owns its own
     `VertexAttributeArray` (instead of sharing with the main builder
     that std::moves it) and explicitly binds
     `idFillPosVertexAttribute` from `bucket.sharedVertices`. Mirrors
     the pattern the line drape already uses.

2. **Diagnostic infrastructure** added in `mtl::TileLayerGroup::render`
   (temporary, removed): logged `drew=N` per drape group per pass to
   confirm where draws happen. Showed every fill drape group reaches
   `drawable.draw(parameters)` — drew=2 for glacier, water, etc.

3. **Magenta-test** (temporary, removed): forced fill drape
   `FillEvaluatedPropsUBO.color` to bright magenta, disabled main fill
   layer group. Result: **no magenta anywhere on terrain**. The drape
   FillBuilder's `draw()` runs, but its fragments don't reach the
   drape target's colour attachment.

### Conclusion: the bug is at Metal pipeline level

Combined evidence narrows the bug to between `drawable.draw(parameters)`
issuing the Metal command and the GPU writing a fragment. The most
likely candidates (couldn't be confirmed without Xcode frame capture):

- The vertex shader outputs positions that all sit outside clip space
  (silent clip rejection of every triangle).
- A Metal pipeline-state mismatch between what was set on the
  DrawableBuilder and what the FillShader expects, leading to the
  fragment shader writing nothing.
- An attribute layout mismatch — the fill vertex shader expects
  certain vertex attributes at certain buffer slots that the drape
  builder is not providing (despite our explicit position binding).

Whoever picks this up next should attach an Xcode Metal frame capture
to a drape pass, inspect:
- The drape target's colour attachment texture content after each
  draw call.
- The pipeline-state objects for the drape FillBuilder vs the main
  FillBuilder — are they actually identical? Different MTLVertexDescriptor?
- The actual GPU buffers bound to vertex attribute slots.

### Magenta-test confirmation

Last diagnostic run before stopping: forced the drape FillLayerTweaker
to write `Color{1, 0, 1, 1}` (bright magenta) into the FillEvaluatedPropsUBO
when `drapeTargetID` is set. With main fill disabled (so drape is the
only source of fill colour on the terrain), the terrain mesh shows
the same cream-shaded hillshade — **no magenta anywhere**. The drape
trace still says `drew=2` for `land-glaciar-drape`.

Combined with the trace log, this proves: the fill drape drawable
gets fully through `drawable.draw(parameters)`, but the resulting
fragments produce zero visible output in the drape target's colour
attachment. Either the vertex shader transforms vertices outside
clip space (silent), the fragment shader writes alpha=0, or the
Metal pipeline state is mis-configured in a way that drops fragments.

The same fragment shader visibly works for main-pass fill drawables
with otherwise-identical setup (only the matrix differs via the drape
tweaker's `drapeTargetID` branch). The most suspect remaining surface
is the vertex shader path through `FillBinders` and per-vertex
attribute setup. Worth attaching a Metal frame capture next session
to inspect the drape pipeline state and the actual fragments emitted.
