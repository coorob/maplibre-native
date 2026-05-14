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
| 2 — Route 2D layer drawables into per-tile targets | ⏳ Pending | Currently blocks visible progress |
| 3 — Bind drape texture into terrain shader | ⏳ Pending | Trivial once Phase 2 lands |
| 4 — Proper depth + opaque pass + `setIs3D(true)` | ⏳ Pending | Mostly debugging once it "kinda works" |
| 5 — `getElevation()` for layer draping | ⏳ Pending | Optional for first ship |

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
even though the drape cache now allocates real targets. This is
deliberate — Phase 2 needs to actually render content into the targets
before binding their textures is meaningful, and we wanted Phase 1 to
land as a no-visual-change scaffolding commit so it's easy to bisect if
something downstream breaks.

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
