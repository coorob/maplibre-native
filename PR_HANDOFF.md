# Upstream PR handoff

This file documents the state of `klattra-terrain-work` and how to
prepare a clean PR branch for upstream MapLibre Native review.

## Branch state

42 commits on `klattra-terrain-work` covering:

- Jesse Crocker's terrain scaffolding (the `ios-terrain-claude` base —
  preserved as-is).
- Phase 0: Xcode 26 SDK / clang strictness fixes.
- Phase 1: Per-tile drape RenderTarget cache (`TerrainDrapeCache`).
- Phase 2: Drape routing for background, fill (Fill + FillOutline),
  line (Simple + SDF), raster.
- Phase 3: Drape texture binding into terrain mesh fragment shader.
- Phase 4: Elevation matrix Z-scale fix, depth correctness, opaque
  pass, `setIs3D(true)`.
- Phase 5 foundation: `RenderTerrain::getElevation()` with bilinear
  DEM sampling.
- Drape eviction (RemoveRenderTargetRequest).
- Per-layer drape group teardown on pan.
- Property-change propagation to per-drape-target tweakers.
- Style light wired through terrain UBO + shader.
- Source-to-drape tile-relative matrix helper.

## Cherry-pick commit list (in order)

Apply these to a fresh branch from `maplibre/main` (the upstream main).
They depend on Jesse's `ios-terrain-claude` series being applied first
(commits up to `6e687eb`):

```
# Phase 0 (could be its own precursor PR)
1107449   Fix three Xcode 26 SDK + clang strictness breakages

# Phase 1
d453783   Terrain Phase 1: per-tile drape RenderTarget cache

# Phase 3
5a6ff27   Terrain Phase 3: bind drape target texture into terrain drawables

# Phase 2 scaffolding
bfa03ef   Expose RenderTerrain::getDrapeTarget() for Phase 2 layer routing
2ce7ce7   Phase 2 scaffolding: expose RenderTerrain to layer update() calls
a876439   End-to-end visual proof: configurable RenderTarget clear color

# Phase 2 background
2af2def   Phase 2 (background, structural): route drawables into drape targets
584d433   Phase 2 (background tweaker): add drapeMode flag for terrain drape
8a6d4a0   Phase 2 (background): wire dedicated drape tweaker into routing
93d3970   Phase 2: bypass background-as-color, iterate drape cache directly
b97a5e9   Phase 2 cleanup: remove verbose drape-debug logging

# Phase 4 — terrain mesh as 3D opaque
a341c6a   Terrain shader: gradient-based diffuse lighting
1853d53   Phase 4: scale terrain matrix Z column by pixelsPerMeter so elevation is in metres
0a844ad   Phase 4: terrain mesh is opaque + depth-tested + truly 3D

# Shared helper
008a43d   Extract LayerTweaker::getDrapeMatrix helper for source→drape tile transforms

# Phase 2 expansion
6f8170c   Phase 2: route fill drawables into terrain drape pass
39ca9fc   Phase 2: route line drawables into terrain drape pass
198cf59   Phase 2: route raster drawables into terrain drape pass

# Cleanup / maintenance
a3bda61   Strip diagnostic logs and dead test scaffolding from terrain code
7626a50   Drape eviction: emit RemoveRenderTargetRequest
35f15b6   Per-layer drape group teardown
5567141   Phase 2 fill: extend drape routing to FillOutline + refactor into helper
c85b1a2   Property-change propagation
30a647e   Wire style light into terrain diffuse shading
3a8097a   Phase 2 line: extend drape routing to LineSDF + refactor into helper
23e798f   Implement RenderTerrain::getElevation with bilinear DEM sampling
de288b0   Fill drape: own vertex attrs with explicit position; clear drape depth

# GL backend port
24efce5   GL backend: port terrain shader + register attribute/texture info

# Architectural improvements
468df43   Fill drape: emitDrapeVariant fetches its own shader, like line/hillshade
bedfb37   Phase 2 line: extend drape routing to LineGradient
50b30a3   Phase 2 line: extend drape routing to LinePattern (atlas binding)
7ff5c77   Phase 2 fill: extend drape routing to FillPattern + FillOutlinePattern

# Render tests
f444e8b   Add render-test scaffolds for terrain (default + exaggeration)
9f5eff1   Terrain render-tests: capture baselines

# Phase 5: symbol elevation
2db2590   Phase 5: per-tile symbol elevation via getElevation()

# Reviewer-facing architecture documentation
67cb86f   Add docs/terrain-architecture.md as a reviewer-facing reference

# CMake wiring (so non-Bazel builds find the new terrain files)
2145ded   CMake: wire terrain source files into mbgl-core
```

## Commits to SKIP

These are local-only and shouldn't go upstream:

```
e8b07e8   Klättra: rebrand sample app + add Sweden 3D test style
592e417   Klättra: default view pitch 55°
# All TERRAIN_PROGRESS doc updates — internal log, not for upstream
bd3924e, 5f9e821, 4d6e2fd, c433721, 60b6279, d499db4, 7f70908,
204c11b, 869e869, c859228, fcb5b8d, 7ea37a8, 68a34a7, e1cff3f,
c2b2bbc, f3cc678
# This PR handoff doc
d9ad3f3, a88acca
```

## Cherry-pick procedure

```bash
cd /Users/mac/projects/maplibre-native

# Add upstream remote if not present
git remote add upstream https://github.com/maplibre/maplibre-native.git
git fetch upstream main

# Create the PR branch
git checkout -b terrain-drape-metal upstream/main

# Apply Jesse's series first (if not already in upstream main)
git cherry-pick d1704c2..6e687eb  # adjust range as needed

# Apply our commits (use the list above). NOTE: this is the early
# pre-Phase-5 batch — append the later commits (24efce5 GL backend,
# 468df43 Fill drape emitDrapeVariant, bedfb37/50b30a3/7ff5c77 line
# and fill variants, f444e8b render-test scaffolds, 9f5eff1 render-
# test baselines, 2db2590 Phase 5 symbol elevation, 67cb86f
# architecture doc, 2145ded CMake wiring) before pushing.
git cherry-pick 1107449 d453783 5a6ff27 bfa03ef 2ce7ce7 a876439 \
                2af2def 584d433 8a6d4a0 93d3970 b97a5e9 \
                a341c6a 1853d53 0a844ad 008a43d \
                6f8170c 39ca9fc 198cf59 \
                a3bda61 7626a50 35f15b6 5567141 c85b1a2 30a647e 3a8097a 23e798f de288b0 \
                24efce5 468df43 bedfb37 50b30a3 7ff5c77 \
                f444e8b 9f5eff1 2db2590 67cb86f 2145ded
```

Expect conflicts at:
- `platform/ios/app/MBXViewController.mm` — Klättra style entry. Either
  drop the entry or rename to a generic "Terrain demo" style with a
  public-DEM tileset.
- `platform/ios/BUILD.bazel` — bundle ID. Restore the upstream
  `com.mapbox.app` (or whatever the demo app uses).
- `platform/ios/app/Info.plist` — CFBundleDisplayName/CFBundleName.
  Restore.

## PR description draft

> **Adds Metal-backend 3D terrain support to MapLibre Native iOS.**
>
> Builds on [#252](https://github.com/maplibre/maplibre-native/issues/252)
> and Jesse Crocker's [draft branch](https://github.com/JesseCrocker/maplibre-native/tree/ios-terrain-claude).
>
> This is a **draft** — landing it lets us collaborate on the remaining
> work in-tree.
>
> ## What's in
>
> - `TerrainDrapeCache`: per-DEM-tile RenderTarget cache, lifecycle-
>   managed with Add/RemoveRenderTargetRequest.
> - Terrain mesh: 3D opaque, depth-tested, elevation matrix-projected
>   in metres via Camera-style `pixelsPerMeter` scale on the Z column.
> - Drape pass routing for background, fill (Fill + FillOutline), line
>   (Simple + SDF), raster. Layers emit drape drawables alongside main
>   drawables when terrain is active.
> - `LayerTweaker::getDrapeMatrix(sourceID, drapeID)` shared helper for
>   the source-to-drape ortho + tile-relative transform.
> - Style light (`light-color`, `light-intensity`, `light-position`)
>   wired into the terrain fragment shader, consistent with
>   fill-extrusion.
> - `RenderTerrain::getElevation(tileID, x, y)` with bilinear DEM
>   sampling — foundation for symbol/line elevation-aware placement.
>
> ## Known limitations
>
> - **GL backend: code compiles + links, visual verification deferred
>   to non-macOS targets.** GLSL terrain shader, shader registration,
>   and `BuiltIn::TerrainShader` symbol resolution all build cleanly
>   under `MLN_WITH_OPENGL=ON` / `MLN_WITH_METAL=OFF` on macOS. The
>   `libmbgl-core.a` from that build contains every terrain symbol
>   (TerrainLayerTweaker, TerrainDrawableUBO/TerrainEvaluatedPropsUBO
>   UniformBufferArray::createOrUpdate instantiations,
>   RenderTerrain::getElevation/getExaggeration references).
>   The resulting `mbgl-render-test-runner` cannot be exercised at
>   runtime on macOS-arm64 because Apple's deprecated OpenGL stack on
>   M-series chips is missing modern GL features the renderer needs —
>   `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT` returns `GL_INVALID_ENUM`,
>   `glMapBufferRange` errors, then an assertion in
>   `gl/buffer_allocator.cpp` fires — and this affects every render
>   test, not just terrain. Visual verification of the GL terrain
>   path therefore needs an Android, Linux, or Windows target.
> - **Fill drape verified working via Xcode Metal frame capture.**
>   The drape RenderTarget texture readback shows the expected cream
>   background, water lake fills, and line features; the terrain mesh
>   fragment shader samples this texture and the main framebuffer
>   pixel readback confirms the sampled drape colour reaches the
>   screen. The earlier "draws but no visible fragments" hypothesis
>   (from trace logs + forced-magenta test) is superseded by the
>   captured frame — the magenta override likely missed the
>   drape-tweaker UBO. See `TERRAIN_PROGRESS.md` for the capture
>   details and pixel readouts.
> - **Symbol elevation: per-tile only.** Labels are lifted to the
>   centre elevation of their tile via `getElevation()`. Visible
>   correctness improvement at zoom levels where tiles are small,
>   but per-symbol elevation (vertex attribute) would be more
>   accurate.
> - **Fill / line connected-geometry elevation: deferred.** Per-tile
>   elevation produced visible seams at tile boundaries (see
>   `TERRAIN_PROGRESS.md`). Proper fix needs a per-vertex elevation
>   attribute populated when the bucket is built; not critical
>   because the depth-tested terrain mesh occludes main-pass fills /
>   lines and the drape pass handles the visible content.
> - **Fill/line variants fully covered.** Fill: Fill, FillOutline,
>   FillPattern, FillOutlinePattern all emit drape drawables.
>   FillOutlineTriangulated (Metal-only niche) is the one exception.
>   Line: Simple, SDF, Gradient, Pattern all drape.
> - **Render tests with baselines that differ on exaggeration.**
>   `metrics/integration/render-tests/terrain/{default,exaggeration}`
>   ship captured `expected.png` baselines (256×256, pitch 60°). The
>   default style uses `exaggeration: 1.0`; the exaggeration variant
>   uses `exaggeration: 20.0`. Both styles include a `light` block
>   so the terrain mesh fallback fragment shader paints visibly, and
>   the `hillshade` layer drapes onto the terrain mesh. The two
>   baselines visibly differ: exaggeration=20 produces dramatically
>   taller relief that occludes more of the upper viewport than the
>   exaggeration=1 baseline. Captured via `mbgl-render-test-runner
>   --update default` against `macos-xcode11-release-style.json`.
> - **Full render-test suite passes.** Ran the entire macOS Metal
>   manifest after the branch's drape-routing and `activeTerrain`
>   plumbing landed — 1246 passed, 0 failed, 0 errored (plus 25
>   passed-but-ignored and 83 ignored from the pre-existing ignore
>   list). Confirms the layer changes don't regress unrelated tests.
>
> Total ≈1700 lines added across ~25 commits, mostly Phase 2 layer
> routing. Happy to split into smaller PRs if maintainers prefer.

## Phase 0 as separate PR

The three Xcode 26 strictness fixes in `1107449` are independent of
terrain work and would land cleanly as their own small PR.
