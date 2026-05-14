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
