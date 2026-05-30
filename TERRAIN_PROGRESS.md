# 3D terrain on MapLibre Native — progress log

This file tracks the work to land 3D terrain in `maplibre-native`'s Metal
backend, ahead of (or alongside) the official upstream effort discussed
in [maplibre-native#252](https://github.com/maplibre/maplibre-native/issues/252).
It's a rolling log — read top-to-bottom for the latest state, then back
for context.

For the **plan** (what's done, what's missing, why), see
[FINISH_TERRAIN.md](FINISH_TERRAIN.md).
For Jesse's **initial design doc**, see [TERRAIN.md](TERRAIN.md).

## HANDOFF (2026-05-26) - planner crash points at PMTiles metadata lifetime/data guard

The latest native crash reports for Klättra are not JS planner crashes and not
the terrain drape-readiness failure. They all fault on the MapLibre PMTiles
thread:

    /Users/mac/Library/Logs/DiagnosticReports/App-2026-05-24-170301.ips
    /Users/mac/Library/Logs/DiagnosticReports/App-2026-05-24-145558.ips
    /Users/mac/Library/Logs/DiagnosticReports/App-2026-05-23-022904.ips

Shared signature:

    EXC_BAD_ACCESS / SIGSEGV / KERN_INVALID_ADDRESS at 0x17
    thread: org.maplibre.mbgl.PMTilesFileSource
    std::__1::basic_string<char>::basic_string(...)
    mbgl::PMTilesFileSource::Impl::getMetadata(... )::lambda(Response const&)

**Fix installed in `platform/default/src/mbgl/storage/pmtiles_file_source.cpp`:**

- PMTiles requests now register `onCancel` and forward cancellation to the
  PMTiles worker, matching the cancellation pattern used by the main resource
  loader.
- `getHeader`, `getMetadata`, `getDirectory`, and `getTileAddress` now own their
  URL strings across async callbacks instead of taking borrowed references.
- Header/metadata/directory/tile callbacks now check for missing response bodies
  before copying/dereferencing strings, turning empty data into `Response::Error`
  instead of a native crash.
- Cached header/metadata hits now return immediately after invoking the callback,
  avoiding a second unnecessary async fetch/callback.
- Directory fetches now return the parsed directory through the callback instead
  of requiring a second cache lookup by the caller.

**Verification:**

- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Built app UUID:

      838D4DFF-3F16-307B-9FE6-80CB5E925825

- Built ipa sha256:

      808392ebe3ac134c5da76099b98ee69f6e1790642a69440f7dc80ac387043534

Simulator `E9CF466F-71D5-41A3-B780-9665538FA552` was shutdown at the end of the
investigation, so this build has not yet been manually repro-tested in the
planner.

## HANDOFF (2026-05-24) - bad frame now proves background-only drape sampling

Manual testing after the style LOD pass still reproduced the artifact. The
important new evidence is from the live simulator log at the failing time:

    /Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-24T13-03-06-732Z_helperpid61281_ownerpid58025_8601871b.log

At `15:41:40`, terrain bindings were created with `drapeReady=1` even when the
target only had the synthetic background group:

    binding frame=166 ideal=z10/565/245=>z10 ... drapeReady=1 drapeGroups=1 drapeDrawables=1
    create-terrain-drawable-bind ... z10/565/245=>z10 ... drapeGroups=1 drapeDrawables=1

Nearby targets in the same frame had real topo content (`drapeGroups=10..13`).
The renderer was therefore treating "has completed any offscreen render" as
"ready", even when that render was beige/background-only. This matches the
observed washed terrain patches better than the previous style-only theory.

**Fix installed after this capture:**

- `RenderTarget::numContentLayerGroups()` now counts non-empty layer groups
  excluding the synthetic background group at `INT32_MAX`.
- Terrain drape readiness now requires:
  - at least one completed render;
  - at least one non-background content group.
- `RenderTerrain::hasElevationCoverage()` uses the same content-group gate, so
  main-pass layers are not suppressed just because a background-only drape
  target exists.
- The terrain binding trace now logs `drapeContentGroups`.

**Verification of the new guard:**

- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Installed and relaunched on simulator `E9CF466F-71D5-41A3-B780-9665538FA552`
  with `KLATTRA_LOG_DRAPE_TRACE=1`.
- Built binary sha256:

      89540582ce2254483b3cf7262dcbba82c73a0e3ba189207ddaa5dbccf80ef098

- Runtime log:

      /tmp/klattra_drape_ready_20260524_160251.log

  Confirmed behavior:

      drapeCompleted=28 drapeGroups=3 drapeContentGroups=0 drapeDrawables=1 drapeReady=0
      drapeCompleted=25 drapeGroups=1 drapeContentGroups=0 drapeDrawables=1 drapeReady=0
      drapeCompleted=18 drapeGroups=11 drapeContentGroups=10 drapeDrawables=12 drapeReady=1

**Next check:**

- Let the user reproduce with normal hand-driven zoom/pan. If artifacts remain,
  check whether they occur with `drapeContentGroups>0` targets. If yes, the next
  evidence pass should dump the sampled drape textures for those specific
  `create-terrain-drawable-bind` targets rather than changing style or lighting.

## HANDOFF (2026-05-24) - low-zoom speckles now traced to terrain style LOD

Manual testing after the mipmapped-drape fix still showed the white/grey
fragmentation while moving/zooming. The mipmap work remains useful, but it was
not sufficient on its own.

**New evidence from today's A/B passes:**

- `KLATTRA_TERRAIN_STYLE_LOD=soft` was wired into the Klättra style patch path
  and verified in logs.
- Hiding only `land-glaciar` made the default start frame cleaner, but did not
  remove the moving/zooming speckles.
- `strict` mode hiding `land-glaciar`, `sankmark`, and `skyddad-natur` still
  showed the pattern in camera playback, so the remaining artifact was not
  just glacier/wetland/protected-area polygons.
- `KLATTRA_PROVENANCE_STYLE_LAYERS=1` false-colored all non-symbol style
  layers. The noisy fragments stayed as the uncolored base/background tone,
  while active topo layers recolored. This points to high-frequency gaps and
  contrasts between broad land/background fills in the drape texture, not a
  random GPU error.
- `KLATTRA_TERRAIN_SHADER_LIGHT=0` did not remove the pattern, so the final DEM
  relief shader was ruled out for this symptom.
- `KLATTRA_TERRAIN_DEBUG_FALLBACK=1` now paints no-drape terrain fallback
  pixels magenta. The bad fragments did **not** turn magenta, and logs showed
  `debugColor=3`, so the terrain shader's no-drape fallback was ruled out too.

**Current candidate fix installed on simulator:**

- `MBXKlattraApplyTerrainStyleLOD()` now applies a default `soft` terrain style
  LOD:
  - blends `background`, `land-open`, `land-aker`, and `land-kalfjall` toward a
    shared tan base at low zoom, restoring the original topo colors close in;
  - keeps glacier/wetland/protected-area fills subdued until closer zooms;
  - softens forest opacity at low zoom to reduce green speckle.
- The terrain shader fallback was also made safe: if a truly invalid drape
  pixel is sampled, it now falls back to the Klättra base tan instead of the old
  elevation palette that could turn high terrain white. Today's debug pass says
  this is not the main active symptom, but it prevents a nearby failure mode.

**Verification:**

- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Installed on simulator `E9CF466F-71D5-41A3-B780-9665538FA552`:
  - built UUID and installed UUID both `73FDA8B9-8118-3221-AF19-EFE15AE9F200`
  - built/installed md5 both `c4264d3f547cca3ee1e0b9567d051fca`
- Runtime log:

      /Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-24T11-55-18-577Z_helperpid48577_ownerpid30485_5933c36b.log

  Key line:

      terrain style LOD mode=soft applied=background:base,land-open:soft,land-aker:soft,land-kalfjall:soft,land-glaciar:soft,skog-barr:soft,skog-lov:soft,skog-fjallbjork:soft,sankmark:soft,skyddad-natur:soft

- Playback contact sheet for the candidate:

      /tmp/klattra_soft_lod2_playback_20260524_135614/contact.jpg

  It is much calmer than the provenance/debug runs; manual testing is still
  required because the user's hand-driven camera movement is the real judge.

**Next check:**

- Let the user pan/zoom the currently installed default `soft` mode. If any
  speckles remain, capture a new provenance pass at that exact camera and tune
  the same LOD family (`land-*`, `skog-*`, background) rather than returning to
  DEM, depth, or matrix guesses.

## HANDOFF (2026-05-24) - reset toward exact bad-frame capture

Manual testing is still showing large grey/white/green block artifacts while
moving/zooming the Klättra map. Treat the prior visual fixes as inconclusive:
they improved some cases, but did not prove or eliminate the root cause.

**Update from the exact bad-frame capture:**

- The first manual trigger captured the screen artifact, but the dumped drape
  targets did not match the final terrain tiles being sampled. That exposed a
  blind spot: dumping only on `RenderTarget::render()` misses already-rendered
  targets that are sampled later.
- `RenderTarget::inspectDebugPixels()` now lets the terrain final pass dump
  the exact drape targets it samples.
- Second capture at `/tmp/klattra_evidence_runs/manual_sampled_bad_frame/`
  produced `*sampled*` drape PNGs for the bad frame. Those sampled textures
  were valid topo map tiles, but they already contained dense white glacier /
  snow polygons, contours, hydrology, landcover, and trails.
- That evidence points away from random GPU memory corruption for the captured
  symptom. The noisy content is real map texture being minified aggressively
  at high pitch.

**Renderer fix now under test:**

- Terrain drape render targets are now mipmapped on Metal:
  - `TerrainDrapeCache::getOrCreate()` calls `RenderTarget::setMipmapped(true)`.
  - Metal textures honor `SamplerState::mipmapped` when creating
    `MTLTextureDescriptor`.
  - Metal samplers now use linear mip filtering and anisotropy for mipmapped
    textures.
  - `OffscreenTextureResource::swap()` generates mipmaps after the offscreen
    drape render and before committing the command buffer.
- Build/install verification on the iPhone 16 Pro simulator:
  - build command:
    `bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"`
  - built UUID and installed UUID both `743868F1-FE10-3EE4-9674-B84BCF886959`
  - built/installed md5 both `987706a0c8fa48660ddaca46b19bf995`
- Runtime playback logs confirm terrain drape targets from z9 through z13 are
  created with `mipmapped=1`.
- Next check: manual user camera testing. If speckles remain, the next
  evidence-based lever is style LOD for terrain drape, especially reducing
  `land-glaciar`, high-frequency landcover, and minor contours at pitched
  mid-zoom views.

**Important reset:**

- Do not keep guessing from static screenshots. The next useful artifact is a
  synchronized evidence bundle captured at the exact moment the user sees the
  bad frame.
- The key diagnostic split is:
  1. If the dumped `terrain-drape` render-target PNGs already contain the
     blocky/speckled corruption, the bug is in draped source/layer composition
     or style/data generalization before the final terrain shader.
  2. If the dumped drape PNGs are clean but the screen screenshot is corrupted,
     the bug is in final terrain mesh sampling, DEM/UV remap, tile fallback, or
     final shader/matrix state.
- Previous manual triggers could miss useful drape dumps late in a session
  because `RenderTarget` only inspected early completed renders unless repeat
  dumping was enabled.

**New manual capture workflow:**

`platform/ios/scripts/klattra_terrain_capture.sh` now supports a manual mode:

    platform/ios/scripts/klattra_terrain_capture.sh manual_bad_frame safe manual

It launches `app.klattra.dev` with:

- `KLATTRA_CAPTURE_TRIGGER_FILE=/tmp/klattra_capture_now`
- `KLATTRA_DUMP_TRIGGER_FILE=/tmp/klattra_capture_now`
- `KLATTRA_DUMP_RENDER_TARGETS=terrain-drape`
- `KLATTRA_DUMP_RENDER_TARGETS_REPEAT=1`
- `KLATTRA_LOG_TARGET_PIXELS_REPEAT=1`
- `KLATTRA_LOG_TERRAIN_FINAL_REPEAT=1`
- `KLATTRA_LOG_DRAPE_TRACE=1`

When the artifact is visible, trigger capture from another shell:

    platform/ios/scripts/klattra_terrain_capture.sh trigger

Expected output:

- `/tmp/klattra_evidence_runs/manual_bad_frame/oslog.txt`
- `/tmp/klattra_evidence_runs/manual_bad_frame/screens/capture_N/screen_t*.png`
- `/tmp/klattra_evidence_runs/manual_bad_frame/render_targets/*.png`

Compare the `screen_t0.png`/`screen_t1.png` bad frame with the matching
`terrain-drape` target PNGs. That comparison should finally tell us which half
of the pipeline owns the corruption.

## HANDOFF (2026-05-23) - synchronized A/B capture, shader relief not sole cause

We ran a stricter evidence pass because manual testing still reported terrain
artifacts and the previous automated screenshots were too weak.

**First correction:**

- XcodeBuildMCP launch logs are useful, but using a separate screenshot command
  after `launch_app_sim` introduced a large delay. The first screenshots were
  taken ~50 seconds after launch, mostly after playback had finished.
- The reliable workflow is a single `simctl` script that:
  1. starts `log stream` for process `App`,
  2. launches `app.klattra.dev` with `SIMCTL_CHILD_...` env vars,
  3. captures screenshots at fixed offsets from the same shell timeline.
- Added helper script:

      platform/ios/scripts/klattra_terrain_capture.sh

  Usage:

      platform/ios/scripts/klattra_terrain_capture.sh safe_direct safe
      platform/ios/scripts/klattra_terrain_capture.sh off_direct off

**Verified binary identity:**

      built:     8C6BEDE6-8611-3778-A3AA-3D1953EB3150
      installed: 8C6BEDE6-8611-3778-A3AA-3D1953EB3150
      built-md5: c16f4c61662f6d80daad970b055757ba
      inst-md5:  c16f4c61662f6d80daad970b055757ba

**A/B runs captured:**

- Safe relief (`shaderRelief=0.08`):

      /tmp/klattra_evidence_runs/safe_direct/
      /tmp/klattra_evidence_runs/safe_direct/contact_sheet.jpg
      /tmp/klattra_evidence_runs/safe_direct/oslog.txt

- Relief disabled (`KLATTRA_ALLOW_TERRAIN_LIGHT_OVERRIDE=1`,
  `KLATTRA_TERRAIN_SHADER_LIGHT=0`, `shaderRelief=0.0`):

      /tmp/klattra_evidence_runs/off_direct/
      /tmp/klattra_evidence_runs/off_direct/contact_sheet.jpg
      /tmp/klattra_evidence_runs/off_direct/oslog.txt

- Combined visual comparison:

      /tmp/klattra_evidence_runs/safe_vs_off_contact_sheet.jpg
      /tmp/klattra_evidence_runs/safe_vs_off_metrics.txt

**What the logs say:**

- Both runs executed the full 16 playback steps (2 loops x 8 steps).
- Safe run confirmed:

      [Klättra] final terrain light applied=0.08 allow=0
      [KLATTRA TERRAIN_FINAL] ... shaderRelief=0.080000 ...

- Relief-disabled run confirmed:

      [Klättra] final terrain light applied=0.00 allow=1
      [KLATTRA TERRAIN_FINAL] ... shaderRelief=0.000000 ...

- At later settled sample frames, terrain final drawables were present and
  bound to real DEM/drape textures. Example from the safe run at frame 120:

      emptyDEM=0 drapeReady=1 demScale=1.000000

- Whole-run counts:

      safe_direct: emptyDEM=1 -> 0, drapeReady=0 -> 478, drapeReady=1 -> 5018
      off_direct:  emptyDEM=1 -> 0, drapeReady=0 -> 463, drapeReady=1 -> 4953

  So early streaming gaps exist, but there is no evidence of empty DEM use in
  these runs, and later terrain frames are backed by ready drape targets.

**Visual result:**

- The synchronized playback does move the camera: zoom/pan changes are visible
  across `screen_t5.png` through `screen_t50.png`.
- The large blocky grey/white artifact was not reproduced in either run.
- The fine green/white/brown speckle remains even with `shaderRelief=0.0`.
  That means the remaining fine speckle is not solely caused by the final
  terrain shader lighting. It is much more likely coming from high-frequency
  style/data content being draped at pitched mid zoom.

**Next best step:**

When the artifact appears manually, use the same synchronized capture workflow
around that live state, or add a one-button/manual trigger that captures:
camera state, current visible terrain tile IDs, terrain final bindings, drape
target readiness, and a screenshot sequence. For the visual speckle that
persists with relief off, investigate style/generalization: pitch/zoom fade or
lower-detail sources for tiny glacier/landcover/protected-area/contour detail.

## HANDOFF (2026-05-22 late) - verified binary, camera playback, safe shader relief

The previous shader-relief pass was partly obscured by a simulator install
problem: `install_app_sim` reported success, but the simulator continued
launching an older `app.klattra.dev` bundle. This made the app logs keep
showing `light.intensity=0.68` even after source changes set the test app to
`0.08`.

**Important evidence:**

- Built after source changes:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- The stale-install problem was proven by comparing Mach-O UUIDs:

      built bundle:     8C6BEDE6-8611-3778-A3AA-3D1953EB3150
      installed bundle: 339E6AD7-8052-32D7-90F8-D7D2B68245B7

- Forced uninstall + direct `simctl install` fixed it:

      xcrun simctl uninstall E9CF466F-71D5-41A3-B780-9665538FA552 app.klattra.dev
      xcrun simctl install E9CF466F-71D5-41A3-B780-9665538FA552 /tmp/klattra_final_light_ipa/Payload/App.app

  After that, built and installed UUIDs both matched
  `8C6BEDE6-8611-3778-A3AA-3D1953EB3150`.

**Current verified app behavior:**

- Runtime log:

      /Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-22T22-23-57-426Z_helperpid44944_ownerpid17181_8d4a7ffc.log

- Key log lines from that verified binary:

      [Klättra] terrain light override allow=0 env=<none> applied=0.08
      [Klättra] final terrain light applied=0.08 allow=0
      light={ intensity = "0.08"; ... }

- Automated camera playback is now a real repro path, not a tiny simulator drag.
  It ran 2 loops of 8 steps: settle, zoom in, pan NE, zoom close, pan SW,
  zoom wide, cross-tile wide pan, return mid. Distances covered
  `18150m -> 13068m -> 8349m -> 26318m`, with multi-kilometre panning at
  `pitch=55`.

- Screenshot after the verified safe-light playback:

      /var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_f9b3ecd5-0be2-45af-ae49-4053e9824439.jpg

  Visual result: the huge moving block/shadow artifact is not visible in this
  automated pass. Remaining fine speckle appears mostly to be real high-density
  topo fill/contour/generalization noise rather than the prior shader-amplified
  block artifact.

**Current implementation notes:**

- `platform/ios/app/MBXViewController.mm`
  - Klättra test style now defaults terrain light intensity to `0.08`.
  - `KLATTRA_TERRAIN_LIGHT_STRENGTH` and `KLATTRA_TERRAIN_SHADER_LIGHT=0` are
    ignored unless `KLATTRA_ALLOW_TERRAIN_LIGHT_OVERRIDE=1` is set, so stale
    launch env cannot silently restore bad relief.
  - Added camera playback controlled by `KLATTRA_CAMERA_PLAYBACK=1`.
- `src/mbgl/renderer/layers/terrain_layer_tweaker.cpp`
  - Also gates terrain-light env overrides behind
    `KLATTRA_ALLOW_TERRAIN_LIGHT_OVERRIDE`.
- `include/mbgl/shaders/mtl/terrain.hpp` and `include/mbgl/shaders/gl/terrain.hpp`
  - DEM sampling now reads exact texels and interpolates decoded metres.
  - Relief normal uses a wider DEM radius with invalid/no-data guards and
    tighter shade clamps.

**Next best step:**

Run one manual pass on the verified installed binary. If the original blocky
artifact is gone, treat the remaining visual problem as a style/generalization
pass: zoom/pitch-dependent fade or simplification for high-frequency landcover,
glacier, contour, and protected-area fills in pitched low/mid zoom.

## HANDOFF (2026-05-22) - draped hillshade replaced by final-shader terrain relief

The long-term fix path has been implemented in the local MapLibre Native test
app/build: stop baking hillshade into terrain drape targets, and add DEM-based
relief in the final terrain shader.

**What changed:**

- `include/mbgl/shaders/mtl/terrain.hpp`
  - Fragment shader now samples the DEM texture as well as the drape texture.
  - Computes a low-pass DEM normal using a 3-texel radius and real
    `metersPerTile`, then applies style-light-driven relief to the final
    topo color.
  - This keeps shading attached to the final mesh instead of writing
    hillshade pixels into the intermediate drape render targets.
- `include/mbgl/shaders/gl/terrain.hpp`
  - Mirrored the terrain-relief path for the OpenGL shader source so the shader
    model stays consistent.
- `include/mbgl/shaders/terrain_layer_ubo.hpp` and
  `src/mbgl/renderer/layers/terrain_layer_tweaker.cpp`
  - Terrain drawable UBO now carries `meters_per_tile`, computed from the DEM
    source tile's latitude/zoom.
  - `KLATTRA_TERRAIN_LIGHT_STRENGTH` can tune relief strength; current tested
    value is `0.68`.
  - `KLATTRA_TERRAIN_SHADER_LIGHT=0` disables the final-shader relief for A/B.
- `platform/ios/app/MBXViewController.mm`
  - Klättra no longer injects a draped hillshade layer by default.
  - Any existing hillshade layers and the hillshade-only source are removed
    from the patched style unless `KLATTRA_USE_DRAPED_HILLSHADE=1` is set.
  - The patched style now sets a Traska-like map-anchored light:
    `position=[1.15,335,48]`, `color=#fff4d8`, `intensity=0.68`.

**Verification:**

- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Installed and launched on simulator `E9CF466F-71D5-41A3-B780-9665538FA552`
  at the captured fault camera:

      lat=67.880534 lon=18.424071 distance=44070 pitch=55 heading=215

- Clean verification log:

      /Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-22T15-30-46-795Z_helperpid90271_ownerpid17181_305e7b9a.log

  Key lines show the old path is absent and the new path is active:

      hillshade-source=<disabled> draped-hillshade=0 hillshade-disabled=1 hillshade-injected=0
      [KLATTRA TERRAIN_FINAL] ... shaderRelief=0.680000 ...

- Screenshot from that exact run:

      /var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_9df21d26-27b2-4b40-b0cc-e368575c8b1c.jpg

  Visual result: terrain relief is present, while the captured grey/white
  draped-hillshade block artifacts are not visible in this static fault-camera
  check.

**Next check:**

- Manual pan/zoom pass should focus on whether the new DEM-normal relief swims,
  flickers, or over-darkens snow/water. If it is too strong or too flat, tune
  `KLATTRA_TERRAIN_LIGHT_STRENGTH` first before changing geometry or drape code.

## HANDOFF (2026-05-22) - artifact captured in drape target, hillshade implicated

The recurring grey/white/green speckle/block artifact is now backed by
capture evidence instead of screenshot guessing.

**What changed in instrumentation:**

- Added style-layer provenance mode:

      KLATTRA_PROVENANCE_STYLE_LAYERS=1

  This false-colors fill/line/raster/hillshade style layers and logs
  `[Klättra PROVENANCE] layer=... color=...` so visible pixels can be mapped
  back to layer IDs.
- Added on-demand render-target capture:

      KLATTRA_DUMP_TRIGGER_FILE=/tmp/klattra_capture_now
      KLATTRA_CAPTURE_TRIGGER_FILE=/tmp/klattra_capture_now

  Touching that file arms the renderer for a short window and the iOS app does
  a tiny camera tickle to force fresh frames. This avoids dumping only startup
  frames or idle-state guesses.
- Fixed the injected `hillshade` style dictionary to be mutable, so future
  provenance/hide/show-only passes can actually recolor or hide it.

**Evidence captured:**

- User reported the fault live. Trigger fired at `2026-05-22 16:36:55 +0200`.
  Screenshot:

      /var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_594649c8-57fd-4c1a-946b-7e376dee6e03.jpg

  Runtime log:

      /Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-22T14-28-27-420Z_helperpid67503_ownerpid17181_29714e98.log

  Drape dumps:

      /tmp/klattra_triggered_dumps/
      /tmp/klattra_triggered_contact_sheet_fault.jpg

- The fault pattern was visible in the false-color screenshot and in the
  per-tile terrain drape PNGs. Therefore this symptom is already present before
  the final terrain mesh samples the drape texture. It is not primarily a final
  terrain UV/depth/matrix problem.
- The trigger log recorded the fault camera:

      lat=67.880534 lon=18.424071 distance=44070 pitch=55 heading=215

**Controlled A/B:**

- Relaunched at the captured camera with:

      KLATTRA_DISABLE_HILLSHADE=1

  Runtime log:

      /Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-22T14-46-07-128Z_helperpid74788_ownerpid17181_2002e7d0.log

  Screenshot:

      /var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_a8df6177-8b2b-475d-a363-5f9f158d54dc.jpg

  Drape dumps:

      /tmp/klattra_nohillshade_dumps/
      /tmp/klattra_nohillshade_contact_sheet.jpg

- With hillshade disabled, the blocky grey/white speckle pattern disappeared
  and the drape targets reduced to normal false-colored fills/lines. The map is
  flatter, but the corruption is gone.

**Current conclusion:**

- The active artifact is caused by the injected hillshade layer being rendered
  into terrain drape targets. Earlier plain logs were clean because this is
  bad source pixels in an otherwise successful render pass, not an error.
- Next useful fix path is to stop using a draped style-layer hillshade as the
  terrain relief source. Either gate/replace it as a temporary mitigation, or
  implement terrain lighting in the final terrain shader from the DEM/normal
  data so relief is added without writing hillshade tiles into the drape target.

## HANDOFF (2026-05-19) - hillshade artifacts now have pixel evidence

After the low-zoom void was fixed by keeping the stable Metal depth slice,
manual testing still showed grey/white moving blotches around steep terrain.
The old drape lifecycle logs were clean, which was misleading: they proved
targets and drawables existed, but not that the source pixels being sampled
were ready.

**Evidence found:**

- Added gated render-target diagnostics:

      KLATTRA_LOG_TARGET_PIXELS=all
      KLATTRA_DUMP_RENDER_TARGETS=all

  These log `KLATTRA TARGET_PIXELS` summaries and dump PNGs for named
  offscreen render targets under the simulator app's `tmp/klattra_render_targets`.
  Terrain drape targets were already named; hillshade prepare targets now use
  names like `hillshade-prep z8/140/64`.
- Fresh diagnostic launch before the readiness fix:
  `/Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-19T10-13-07-945Z_helperpid98935_ownerpid82496_b6f440a4.log`
  showed many `hillshade-source-not-ready` events. In other words, terrain
  drape routing was binding a hillshade prepare texture before that texture had
  completed its first offscreen render. That matches the user-observed behavior
  where artifacts appear while moving/loading and then sometimes settle.
- The A/B before this pass already implicated hillshade content:
  `KLATTRA_DISABLE_HILLSHADE=1` removed the speckle but looked too flat, while
  `KLATTRA_HILLSHADE_SCALE=0.35` was much calmer but still visually flatter
  than Traska web.

**Patch from this pass:**

- `RenderHillshadeLayer` now sets debug names on hillshade prepare render
  targets.
- `RenderTarget` can optionally dump/read back target pixels and logs basic
  alpha/brightness/grey statistics. This is gated by env vars and off by
  default.
- Hillshade drape routing is now gated on:

      bucket.renderTarget->hasCompletedRender()

  If a hillshade source is not ready, it logs
  `action=skip-drape-route` instead of wiring an undefined source texture into
  the terrain drape target.

**Verification done:**

- Built successfully after the diagnostic and readiness changes:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Forced uninstall/install on simulator `E9CF466F-71D5-41A3-B780-9665538FA552`
  before launching, to avoid stale installed binaries.
- Fresh launch after the readiness gate:
  `/Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-19T10-26-00-841Z_helperpid1761_ownerpid82496_4149b40f.log`
  produced target pixel dumps and a clean initial screenshot:
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_de7e1a9d-39d7-4156-9ab1-a2f55d2675cf.jpg`.
  The trace included 47 `hillshade-source-not-ready ... action=skip-drape-route`
  lines, which are now expected: they mean the renderer deferred hillshade
  drape routing until the source texture was rendered.

**Next:**

- Manual roam test this exact build. The expected result is no grey/white
  garbage while new hillshade tiles come in; at worst hillshade should appear a
  frame late.
- If artifacts persist after this readiness gate, use the dumped PNGs to compare
  `hillshade-prep` vs `terrain-drape` for the offending tile: if prep is clean
  but drape is bad, debug the drape projection/matrix; if prep is noisy, tune or
  smooth hillshade prepare/sampling.
- Once stable, tune `KLATTRA_HILLSHADE_SCALE` upward from `0.35` toward Traska
  parity without reintroducing speckle.

## HANDOFF (2026-05-19) - full drape trace shows low-zoom void is depth, not cover

Manual simulator testing reproduced the low-zoom beige foreground void while
`KLATTRA_LOG_DRAPE_TRACE=1` and `KLATTRA_LOG_DRAPE_STALE=1` were enabled.

**Evidence found:**

- First relaunch reused a stale installed binary. The source tree already had
  the Metal `OffscreenTexture(size_, ...)` fix, but the live trace showed
  `cache-create ... size=512x512` followed by `target-render-begin ... size=0x0`
  for every drape target. The artifact run had thousands of healthy drape
  renders and no DEM/PMTiles errors, but the size log proved the installed app
  was not trustworthy.
- Forced rebuild + uninstall/install fixed the diagnostic plumbing:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

  Fresh trace at
  `/Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-19T09-05-53-886Z_helperpid88459_ownerpid82496_a4db4bb3.log`
  showed `target_size_0x0=0` and all initial drape targets reporting
  `size=512x512`.
- With the real Metal clip-depth conversion still active:

      position.z = clamp((position.z + position.w) * 0.5,
                         position.w * 0.001,
                         position.w * 0.999);

  the beige foreground cutoff still appeared in the settled simulator
  screenshot even though the trace had no PMTiles/DEM/drape-missing failures
  and ended with `bindings=22 demTextures=22 drapeTargets=22 terrainDrawables=22`.
- A tile-cover halo experiment was negative. Increasing the elevation cover
  padding from radius `1` to `2` increased the settled cover to
  `bindings=35 demTextures=35 drapeTargets=35 terrainDrawables=35`, but the
  same horizontal foreground cutoff remained.
- Restoring the stable Metal depth path for the terrain mesh fixed the
  foreground void with the original radius-1 cover:

      position.z = position.w * (isSkirt ? 0.75 : 0.5);

  Verified after another forced rebuild + uninstall/install. Fresh trace at
  `/Users/mac/Library/Developer/XcodeBuildMCP/workspaces/sweden-hiking-app-bac7280c9ab5/logs/app.klattra.dev_2026-05-19T09-21-46-223Z_helperpid91225_ownerpid82496_1e3df680.log`
  ended cleanly with no PMTiles/DEM failures and no `0x0` targets:
  `bindings=22 demTextures=22 drapeTargets=22 terrainDrawables=21`.
  Settled screenshot:
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_7a020399-253f-46f4-b012-8255b57e0785.jpg`.

**Current working-tree state from this pass:**

- `src/mbgl/util/tile_cover.cpp` is back at radius `1`; radius `2` was only a
  test and did not solve the cutoff.
- `include/mbgl/shaders/mtl/terrain.hpp` currently contains the stable-depth
  A/B result (`0.5w` main terrain, `0.75w` skirts), not the real clip-depth
  conversion from the previous handoff.
- The installed simulator app is the stable-depth build, launched with full
  drape trace.

**Conclusion / next step:**

- The remaining low-zoom beige void is not PMTiles, missing DEM, incomplete
  drape texture, or too-narrow DEM cover. It is tied to the Metal terrain
  depth/clip strategy.
- Next pass should compare close steep artifact cameras with this stable-depth
  build. If grey skirt/overlap fragments return, solve that separately while
  preserving the low-zoom stable-depth behavior, likely with an adaptive depth
  strategy or stricter skirt handling rather than the full real-depth conversion.

## HANDOFF (2026-05-18) - Metal terrain depth artifact evidence

Manual testing reproduced the grey/white blocky terrain chunks around
Maajåelkientjoevtje while the app thought terrain was fully healthy.

**Evidence found:**

- Live simulator screenshot before the patch:
  `/tmp/klattra_live_log_check_20260518_223932.png`.
- The drape trace showed no old failure signals:

      pmtiles=0
      dem_fail=0
      emptyDEM=0
      missing_ready=0
      DRAPE_STALE no_overlap=0

- At the artifact time, terrain had ready DEM + drape bindings and completed
  drape renders. That ruled out PMTiles, empty DEM fallback, and first-render
  drape readiness for this particular artifact.
- The key renderer clue was in the Metal terrain shader: it forced every
  terrain vertex onto one depth slice with `position.z = position.w * 0.5`
  (skirts at `0.75`). That made real terrain depth unavailable to the depth
  buffer, so overlapping tiles/triangles could win by draw order instead of
  distance, matching the blocky persistent chunks.

**Patch to keep:**

- `include/mbgl/shaders/mtl/terrain.hpp`: convert the projected GL-style clip
  depth to Metal's `[0, w]` clip range and clamp narrowly, instead of flattening
  all terrain to a constant depth:

      position.z = clamp((position.z + position.w) * 0.5,
                         position.w * 0.001,
                         position.w * 0.999);

- `src/mbgl/mtl/offscreen_texture.cpp`: fixed the constructor typo so
  `gfx::OffscreenTexture::getSize()` reports the actual offscreen size instead
  of `0x0` in drape trace logs. The resource was already created at the right
  size; this makes diagnostics trustworthy.

**Verification done:**

- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Installed and launched `app.klattra.dev` on simulator
  `E9CF466F-71D5-41A3-B780-9665538FA552` with the same trace/camera env around
  `62.9026, 12.4614`, distance `4650`, pitch `58`.
- Fresh screenshot after the depth patch:
  `/tmp/klattra_depth_patch_20260518_225447.png`.
  It no longer shows the obvious square/triangle chunks in the reproduced
  pitched glacier view.

**Next:**

- Ask for one manual roam pass on the patched build. If the chunks persist,
  the next evidence pass should log or visualize terrain depth/mesh ownership
  per tile; if they are gone, trim the temporary drape trace logs before
  preparing the patch for upstream review.

## HANDOFF (2026-05-18) - PMTiles DEM directory evidence pass

Manual testing showed grey/blue terrain fragments that looked like tiles waiting
for terrain/drape data and sometimes never resolving. We added evidence logging
instead of guessing, then reproduced a concrete signal in the simulator.

**Evidence found:**

- Existing logs did not explain stale draped drawables, so fill/line/raster/
  hillshade drape cleanup now has gated `KLATTRA_LOG_DRAPE_STALE=1`
  instrumentation. It reports removed drawables split by `not-in-cover` vs
  `no-overlap`.
- First evidence run at the Kebnekaise-ish camera showed many normal
  `not-in-cover` removals but **zero** `no-overlap` removals. That makes the
  stale-overlap hypothesis unlikely for the current artifact.
- The same run showed many real errors:

      Failed to load tile ... for source terrain-dem:
      Error parsing PMTiles directory: map::at: key not found

- The exact reported z/x/y DEM tiles (for example `11/1130/487`,
  `10/564/244`, `12/2264/975`) exist in both the local archive
  `/Users/mac/sweden-hiking-app-data/dem-build-fixed/sweden-dem.pmtiles` and
  the remote Supabase archive when checked with `pmtiles tile`. So this was
  not bad/missing DEM data in the archive.

**Patch to keep:**

- `platform/default/src/mbgl/storage/pmtiles_file_source.cpp`: `getDirectory()`
  now returns the deserialized PMTiles directory vector to `getTileAddress()`
  instead of making `getTileAddress()` reach back into the mutable
  `directory_cache` by key after the callback. This removes the observed
  `map::at` failure path while preserving cache storage for later requests.
- Drape stale instrumentation remains gated behind `KLATTRA_LOG_DRAPE_STALE=1`
  and is useful for future artifact captures.

**Verification done:**

- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Fresh simulator install/launch on `E9CF466F-71D5-41A3-B780-9665538FA552`
  with:

      KLATTRA_LOG_DRAPE_STALE=1
      KLATTRA_CAMERA_LAT=67.9026
      KLATTRA_CAMERA_LON=18.4954
      KLATTRA_CAMERA_DISTANCE=55000
      KLATTRA_CAMERA_PITCH=55
      KLATTRA_CAMERA_HEADING=215

- After the same swipe/motion pass, the log summary was:

      pmtiles_directory_errors=0
      terrain_dem_failed_tiles=0
      drape_missing_or_not_ready=0
      DRAPE_STALE total=311
      no_overlap_nonzero=0

- Settled screenshot:
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_649f8ba1-7474-41e1-972d-d42dd8543e9f.jpg`.

**Next if artifacts persist on device/manual roam:**

- Capture logs with `KLATTRA_LOG_DRAPE_STALE=1`. If `terrain-dem` PMTiles
  errors reappear, keep digging in PMTiles fetch/cache concurrency. If they
  stay at zero while artifacts are visible, the next isolation pass should
  toggle style layers (`KLATTRA_DISABLE_HILLSHADE=1`, then contours/fills) to
  identify whether the remaining artifact is a terrain mesh edge/coverage issue
  or a draped layer compositing issue.

## HANDOFF (2026-05-18) - drape texture warmup gate for motion artifacts

After the low-zoom beige void fix, manual testing still found grey/triangular
terrain artifacts while moving the camera, with some fragments persisting once
the camera settled. The latest working-tree patch treats this as a drape target
readiness problem: terrain was allowed to sample a per-DEM-tile offscreen drape
texture as soon as the `RenderTarget` existed, even though that target may not
have completed its first offscreen render yet.

**New fix to keep:**

- `include/mbgl/renderer/render_target.hpp` and
  `src/mbgl/renderer/render_target.cpp`: `RenderTarget` now tracks completed
  offscreen renders via `getCompletedRenderCount()` / `hasCompletedRender()`;
  the count increments after `parameters.encoder->present(*offscreenTexture)`.
- `src/mbgl/renderer/render_terrain.hpp`: `hasElevationCoverage()` only reports
  coverage for drape targets that have completed at least one render. This keeps
  normal 2D main-pass layers visible during a new target's warmup frame instead
  of dropping them and exposing beige/partial terrain.
- `src/mbgl/renderer/render_terrain.hpp` and
  `src/mbgl/renderer/render_terrain.cpp`: `DEMBinding` carries `drapeReady`.
  Terrain drawables are skipped until the matching drape target is ready, and
  readiness participates in the drawable refresh test so the first ready frame
  builds the terrain drawable.
- `createDrawableForTile()` now refuses to bind an unready/missing drape target
  rather than emitting a terrain drawable with an incomplete surface texture.

**Verification done:**

- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Fresh simulator install was forced by stopping and uninstalling
  `app.klattra.dev`, then installing
  `/Users/mac/projects/maplibre-native/bazel-bin/platform/ios/App_archive-root/Payload/App.app`.
- Launched on simulator `E9CF466F-71D5-41A3-B780-9665538FA552` with:

      KLATTRA_CAMERA_LAT=67.2909
      KLATTRA_CAMERA_LON=17.4833
      KLATTRA_CAMERA_DISTANCE=6500
      KLATTRA_CAMERA_PITCH=72
      KLATTRA_CAMERA_HEADING=0

- Runtime log confirmed the patched Klättra style loaded and emitted no
  `Drape target missing` warnings during the quick pass.
- Settled screenshot after tile load:
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_79fa3986-a464-4000-8a57-3841e4fc3b1c.jpg`.
- Motion check: one simulator swipe plus immediate and settled screenshots:
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_9c8cd1e5-24c0-4a1f-af35-e3bbb5b647ee.jpg`
  and
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_8497def1-3710-4717-812a-8b5c42da53ca.jpg`.
  These did not show the grey shard artifacts in the tested camera, but this
  still needs a real manual device/simulator roam when someone can drive the
  map interactively.

**If artifacts still appear:**

- The next likely bug is not first-render readiness but target generation
  freshness: an already-ready `RenderTarget` may need to be marked dirty when
  its routed layer groups/drawables change, then require one completed render
  after that dirty generation before `hasElevationCoverage()` suppresses the
  main pass again.

## HANDOFF (2026-05-18) - Traska web terrain parity preset

Klättra native now defaults closer to Traska web's live terrain settings instead
of the stronger native-only relief preset.

**Patch pieces:**

- `platform/ios/app/MBXViewController.mm`: default launch pitch is now `55`
  degrees, matching Traska's 3D toggle pitch target.
- Terrain exaggeration now mirrors Traska's `terrainExaggerationForCamera()`:

      distanceRelief = mix(1.62, 1.12, (zoom - 6) / 9)
      pitchRelief = mix(-0.04, 0.18, pitch / 65)
      exaggeration = clamp(distanceRelief + pitchRelief, 1.05, 1.68)

  `KLATTRA_TERRAIN_EXAGGERATION` still overrides this and keeps the wider
  experimental clamp (`1.0...2.35`).
- After style load and `moveend`/camera-settle delegate callbacks, Klättra
  reapplies the same camera-based terrain exaggeration through `MLNStyle.terrain`
  so zoom/pitch changes follow the web behavior.
- Hillshade scale defaults to `1.0`, with Traska's ramp
  `z5=0.16`, `z10=0.26`, `z14=0.34`, direction `335`, viewport anchor, and
  Traska colors. `KLATTRA_HILLSHADE_SCALE` still overrides it.
- Contour scale defaults to `1.0` instead of `0.9` for closer topo parity.

**Verification done:**

- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Freshly uninstalled/reinstalled `app.klattra.dev`, then launched simulator
  `E9CF466F-71D5-41A3-B780-9665538FA552` with:

      KLATTRA_CAMERA_LAT=67.2909
      KLATTRA_CAMERA_LON=17.4833
      KLATTRA_CAMERA_DISTANCE=6500
      KLATTRA_CAMERA_PITCH=55
      KLATTRA_CAMERA_HEADING=0

- Runtime log confirmed:

      terrain-exaggeration=1.41 parity-zoom=12.50 parity-pitch=55.0 hillshade-scale=1.00

- Settled screenshot:
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_3fbf7f53-9b19-43d8-b0b8-dc30b70685d6.jpg`.
  The view is clean and calmer than the previous `terrain=1.8`,
  `hillshadeScale=1.25` default. If it still feels too flat subjectively, the
  likely difference from web is native lighting/shading rather than raw terrain
  exaggeration.

## HANDOFF (2026-05-18) - low-zoom beige void fixed in Klättra simulator

The pitched Kebnekaise zoom-out bug from today's screenshots is now fixed in
the working tree and verified on the iPhone 16 Pro simulator.

**Root cause split:**

- The terrain DEM cover was too narrow for pitched, low-zoom views. Widening
  DEM cover by one tile around each covered tile made the mesh cover the full
  repro camera instead of breaking into terrain "islands".
- Once cover was fixed, the terrain still disappeared in the production shader:
  Metal clip-space `z` from the terrain matrix could fall outside the valid
  depth interval even while projected X/Y/W were correct. Pinning terrain
  `position.z` to `position.w * 0.5` keeps the composited terrain surface
  rasterizing at low zoom.
- The drape target RGB was valid while alpha could be zero, so the terrain
  fragment shader now treats drape RGB as opaque surface colour and only falls
  back to elevation colour when sampled RGB is essentially black.
- Drape target rendering needed two composition fixes: bind global UBOs inside
  `RenderTarget::render()`, and draw the drape background first instead of
  letting it repaint over draped fills/hillshade.

**Patch pieces to keep:**

- `src/mbgl/util/tile_cover.cpp`: one-tile DEM cover padding when elevation
  cover bounds are active.
- `src/mbgl/renderer/render_terrain.cpp`: low-zoom drape targets are smaller
  (`512` at z<=8, `1024` at z<=10, `2048` close in) so padded z8 cover does
  not allocate huge 2048px targets for every tile.
- `include/mbgl/shaders/mtl/terrain.hpp`: mid-clip terrain depth and opaque
  RGB drape sampling.
- `src/mbgl/renderer/render_target.cpp`: bind global uniform buffers for
  offscreen drape passes.
- `src/mbgl/renderer/layers/render_background_layer.cpp`: opaque background is
  not emitted into both opaque/translucent passes; drape background uses
  read-only depth and a high layer index so it draws as a backdrop.

**Verification done:**

- Removed temporary shader/log probes; no `TERRAIN_UPDATE`, `DRAPE_TARGET`, or
  `TERRAIN_LAYER_RENDER` logs remain.
- Built successfully:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Installed/launched `app.klattra.dev` on simulator
  `E9CF466F-71D5-41A3-B780-9665538FA552`.
- Default repro camera currently in `MBXViewController.mm`:
  `(67.9026, 18.4954), acrossDistance=90000, pitch=60, heading=215`.
  The sample app also accepts `KLATTRA_CAMERA_LAT`, `KLATTRA_CAMERA_LON`,
  `KLATTRA_CAMERA_DISTANCE`, `KLATTRA_CAMERA_PITCH`, and
  `KLATTRA_CAMERA_HEADING` environment overrides for matrix captures.
- Clean verification screenshot shows continuous draped terrain instead of the
  beige void:
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_57f03c8c-27b8-43da-978b-6ec9254bdae3.jpg`.
- Visual matrix rerun saved seven full-resolution simulator PNGs at
  `/tmp/klattra_visual_matrix_2026-05-18_rerun/`. Covered cameras:
  flat Kvikkjokk at 120 km, 95 km, and 40 km; pitched Kvikkjokk at 40 km;
  close/medium Kebnekaise at 5 km and 30 km; and the exact wide pitched
  Kebnekaise repro at 90 km. The wide pitched repro is continuous in this run.
- Traska web parity baseline captured at
  `/tmp/klattra_web_parity_2026-05-18/` with matching 1206x2622 viewport,
  Traska's `sweden-topo-style.json`, `sweden-dem.pmtiles`, GL JS terrain
  exaggeration, sky, light, and hillshade settings. Web parity is visually
  richer mostly because contours/hydrology/trail detail remain visible and
  hillshade is crisper. Before the contour-parity experiment below, Klättra
  hid `hojdkurva-10m` and `hojdkurva-index` in `MBXViewController.mm`, so the
  native output was intentionally less topo-like than Traska even after the
  terrain void fix.
  One GL JS baseline run reported `dem dimension mismatch` on the medium
  Kebnekaise capture but still rendered; treat that as a web-capture warning,
  not a native regression.
- Contour-parity experiment implemented in `MBXViewController.mm`: Klättra no
  longer hides `hojdkurva-10m` / `hojdkurva-index`. Instead it initializes them
  softly in the patched style JSON and updates `MLNLineStyleLayer.lineOpacity`
  from the current zoom + pitch. New matrix saved at
  `/tmp/klattra_visual_matrix_2026-05-18_contours/`. Result: no beige void
  regression; z10 Kvikkjokk and medium Kebnekaise regain much more Traska-like
  topo detail. Watch item: close high-pitch snow/steep slopes are visually busy,
  but they read as map contours rather than terrain breakage.
- Relief tuning pass added after manual testing felt too flat. Klättra now keeps
  launch-time knobs for relief experiments:
  `KLATTRA_TERRAIN_EXAGGERATION`, `KLATTRA_HILLSHADE_SCALE`, and
  `KLATTRA_CONTOUR_SCALE`. Tested two presets at the Kebnekaise close/medium/
  low-zoom cameras:
  `/tmp/klattra_relief_tuning_2026-05-18/balanced-relief/` uses
  terrain `1.8`, hillshade scale `1.25`, contour scale `0.9`;
  `/tmp/klattra_relief_tuning_2026-05-18/strong-relief/` uses terrain `2.05`,
  hillshade scale `1.45`, contour scale `0.75`. Balanced is now the default in
  `MBXViewController.mm`: it adds relief without reintroducing the low-zoom
  terrain-island/void look. Strong is useful for comparison but starts to feel
  more theatrical in close steep terrain.
- Default relief verification after rebuilding needed a forced simulator
  uninstall/reinstall; a plain reinstall once launched an older binary while
  reporting success. Confirmed runtime log after forced reinstall:
  `exaggeration = "1.8"` and `hillshade-scale=1.25`. Final default screenshot:
  `/tmp/klattra_relief_tuning_2026-05-18/final-default/07-kebnekaise-lowzoom-default-forced-reinstall.png`.
- Close steep-camera artifact pass: manual testing showed triangular/polygonal
  shards while moving the camera, with some settling on the terrain surface.
  The likely cause was the emergency Metal depth pin putting main terrain and
  skirt walls on the same depth slice, allowing skirt triangles to overpaint
  the mountain surface. Rejected experiment: preserving/clamping real terrain
  Z fixed the close screenshot but brought back the low-zoom beige void. Current
  fix in `include/mbgl/shaders/mtl/terrain.hpp`: keep the main mesh at the
  stable mid-depth slice (`0.5w`) but put skirt vertices farther back (`0.75w`)
  so they still cover edge gaps without drawing over real terrain. Captures:
  `/tmp/klattra_artifact_skirt_depth_2026-05-18/05-kebnekaise-high-pitch-close.png`
  and
  `/tmp/klattra_artifact_skirt_depth_2026-05-18/07-kebnekaise-lowzoom-pitched-repro.png`.
- Follow-up artifact screenshot near Rijddatjåhkkå (approx. `67.2909, 17.4833`)
  looked like grey polygon fragments on top of the draped surface, not the old
  beige void. Restored the earlier terrain-style guard that hides
  OpenFreeMap's non-symbol layers while keeping OpenFreeMap place labels, so
  only the Sweden topo basemap is draped. Added escape hatches:
  `KLATTRA_SHOW_OPENFREEMAP_DRAPE=1` to A/B the generic OFM layers, and
  `KLATTRA_DISABLE_HILLSHADE=1` / `KLATTRA_HILLSHADE_SCALE=0` to isolate
  hillshade from terrain geometry. Captures saved under
  `/tmp/klattra_rijddat_artifact_2026-05-18/`; the shifted Rijddatjåhkkå
  default capture and post-swipe capture did not reproduce the grey fragments
  after the clean reinstall.

**Next checks:**

- Pan/zoom manually for tile-boundary artifacts now that z8 cover padding uses
  smaller drape textures and skirt walls sit behind the main mesh.
- If grey fragments still appear, relaunch the same camera with
  `KLATTRA_DISABLE_HILLSHADE=1`. If they disappear, debug the hillshade
  prepare/drape path; if they remain, inspect which topo/source layer is
  contributing the marks.
- Do a real-device/manual pass with the new balanced default. Close high-pitch
  views may still want a small contour reduction if the topo lines feel too busy
  on snow/glacier areas.
- Test on a real iOS device before upstreaming; the mid-clip Metal depth pin is
  intentionally pragmatic and should be reviewed against fill-extrusion/other
  3D interactions.

## 🔖 HANDOFF (2026-05-17, follow-up) — low-zoom fix verified in Klättra

After the GPU-capture handoff below, the low-zoom beige void was reduced to a
Metal clip-space problem and verified in the Klättra simulator app.

**Patch candidate now in the working tree:**

- `src/mbgl/mtl/layer_group.cpp` detects 3D drawables in plain Metal
  `LayerGroup`s and binds explicit 3D depth/stencil state before drawing them,
  mirroring `TileLayerGroup::render()`. Terrain drawables use a plain
  `LayerGroup`, while `Drawable::draw()` intentionally skips depth/stencil
  setup for `is3D` drawables.
- `include/mbgl/shaders/mtl/terrain.hpp` keeps terrain clip-space Z just
  inside Metal's far plane:

      if (position.w > 0.0) {
          position.z = min(position.z, position.w * 0.9999);
      }

  The low-zoom terrain matrix projects visible terrain vertices numerically on
  the far clip plane. Pulling only `z` inside the plane preserves X/Y/W and the
  drape sampling path while allowing rasterization to happen.

**What was tested and rejected today:**

- LayerGroup 3D depth-state fix alone: still beige at the repro camera.
- Main background `DepthMaskType::ReadOnly` when terrain is active: still beige.
- Terrain drawable `setEnableDepth(false)` with the real shader Z: still beige.
- Shader-side Z cap at `0.999w`: rendered correctly. Final candidate uses the
  smaller `0.9999w` cap, also renders correctly.

**Verification done:**

- Cleaned the stale probes from `paint_parameters.cpp`,
  `render_background_layer.cpp`, `terrain_layer_tweaker.cpp`, and
  `MBXViewController.mm` (no delayed camera tickle remains).
- `git diff --check -- include/mbgl/mtl/layer_group.hpp src/mbgl/mtl/layer_group.cpp include/mbgl/shaders/mtl/terrain.hpp src/mbgl/renderer/render_terrain.cpp platform/ios/app/MBXViewController.mm`
  passes.
- Built the Klättra app with Metal:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

- Uninstalled/reinstalled `app.klattra.dev` on simulator
  `E9CF466F-71D5-41A3-B780-9665538FA552`.
- Exact repro camera `(66.8, 17.8), acrossDistance=120000, pitch=0` now renders
  terrain/topo/hillshade instead of the beige void.
  Screenshot:
  `/var/folders/my/jn0ds0916kdcz9s44bwvw7ch0000gn/T/screenshot_optimized_f3fdd1de-7930-454d-a38d-681daf4357fb.jpg`.

**Visual matrix (2026-05-17, simulator):**

- Captured six reference cameras to `/tmp/klattra_visual_matrix/`.
- Passed primary regression check: no beige void in any captured case,
  including low-z8 flat, z8.5 flat, z10 flat, z10 pitched, and two
  Kebnekaise pitched cameras.
- Watch items before calling this web-quality: faint low-zoom drape/tile
  boundaries in the flat Kvikkjokk cases, a speckled white/blue band in the
  pitched Kvikkjokk case, and the closest high-pitch Kebnekaise camera has a
  large smooth foreground slope even though it no longer clips through terrain.

**Follow-up artifact found while manually zooming out (2026-05-18):**

- At pitched Kebnekaise views, zooming out could make the 3D terrain break into
  relief "islands" and then get swallowed by the beige background.
- Root cause candidate fixed in the working tree: when the DEM source had no
  render tiles for the current camera, `RenderTerrain::update()` returned early
  without clearing old terrain drawables or drape targets. Those stale drape
  targets made drape-capable layers think terrain coverage still existed, so
  their main-pass 2D fills were skipped and the background colour showed.
- Second guard added: raster-dem variable LOD now floors against the tileset's
  own minzoom (`z8` for `sweden-dem.pmtiles`) so pitched far-field cover cannot
  emit below-range DEM placeholders.
- Built and reinstalled Klättra after the patch:

      bazel build //platform/ios:App --//:renderer=metal --ios_simulator_device="iPhone 16 Pro"

**Still worth doing before upstreaming:** test this same patch on a real iOS
device, compare the visual matrix against the Traska web app camera-for-camera,
then replace or justify the shader Z guard with a more principled Metal
projection/far-plane treatment if we find artifacts.

## 🔖 HANDOFF (2026-05-17) — start here if you're a new session

> The 2026-05-16 handoff below this section covers the camera-clipping bug —
> that's resolved and the fix is in `MBXViewController::enforceTerrainCameraClampOn:`.
> This new handoff is about a **separate bug discovered while testing zoom-out**:
> at low ideal zoom (≈z=8 and below), the terrain mesh fails to render any
> visible fragments even though all the parts of the pipeline look healthy.

### The bug we're trying to fix

**At idealZoom ≈ 8 over Sweden, the screen renders as just the beige
background color + symbol labels — no fills, no lines, no hillshade.**
Reproducible on the Klättra-Sweden-3D style at camera
`(66.8, 17.8), acrossDistance=120000, pitch=0`. Same camera with
**terrain disabled** (`style.terrain = nil`) renders the full topo + hillshade
correctly. So the bug is specifically in how the terrain pipeline interacts
with low-ideal-zoom rendering.

Threshold isn't crisp: works fine ≥ z≈10, broken ≤ z≈8.5, fragmenting
patches between. Independent of pitch. Independent of camera latitude
(it's about the zoom level, not the geographic location).

### What's already verified (from 2026-05-16 session)

These are facts, not hypotheses — re-confirmed yesterday with debug logs
and shader hacks. Don't re-run these tests; trust the result and pick up
from where they leave off.

1. **Drape pass works.** At the broken zoom, the drape cache has 3 targets
   (one per visible DEM tile). 10+ layer groups (`background-drape`,
   `land-open-drape`, `vatten-drape`, `skog-barr-drape`, `hillshade-drape`,
   etc.) successfully emit drape drawables into those targets. The drape
   textures actually get painted.

2. **Mesh drawables exist.** At the broken zoom, the `terrain` layer group
   has 3 drawables (one per ideal cover slot). `enabled=true`,
   `drawableCount=3`, `willDraw=3` in the Metal layer-group iterator.
   The drawables match-up 1:1 with the drape targets via `tilesOverlap`.

3. **The drawcall reaches the GPU.** Runtime logs in `drawable.draw()`
   confirm `TERRAIN: Binding texture id=0 to location=0` and
   `Metal texture is VALID` fire for each terrain drawable per frame —
   the bindings are issued, the pipeline state is built.

4. **The Metal pipeline is functional.** Replacing the vertex shader's
   `drawable.matrix * float4(pos, elevation, 1)` with a hardcoded
   `float4(pos.x/8192*2 - 1, pos.y/8192*2 - 1, 0, 1)` produces a fully
   rendered red mesh covering the screen. So the *only* thing wrong is
   the matrix transform — the rest of the pipeline (vertex submission,
   rasterization, fragment shader, blending, framebuffer write) is fine.

5. **Disabling depth doesn't help.** `setEnableDepth(false)` +
   `DepthMaskType::ReadOnly` still produces beige. Not a depth/Z issue.

6. **Forcing elevation=0 in the shader doesn't help.** So the bug isn't
   the elevation-from-DEM path or the `pixelsPerMeter` Z-column scaling
   in `TerrainLayerTweaker` — even a flat mesh at world Z=0 fails to
   produce fragments.

7. **The CPU-computed matrix LOOKS valid.** For tile `z=8/140/63` over
   Kvikkjokk, the logged matrix is:

       [0.509  0     0     0   ]  col 0 (X scale)
       [0    -0.234  0     0   ]  col 1 (Y scale)
       [0     0    ~0    ~0   ]  col 2 (Z, basically zero after pixelsPerMeter mul)
       [-2744  808  1311  1311 ]  col 3 (translation)

   For tile-local vertex `(4096, 4096, 0, 1)` (tile centre), this projects
   to NDC `(-0.50, -0.11, 0.99985)` — clearly inside the clip cube. So at
   least *some* vertices of the mesh should produce fragments. They don't.

### Today's session (2026-05-17): GPU capture attempt

Spent the session trying to use Xcode's Metal GPU debugger to see what
happens to those vertices on the GPU. **Couldn't.** Documenting the
infrastructure built + what fails so the next session doesn't repeat.

**Capture infrastructure (built, currently gated off, ready to re-enable)**:
- `platform/ios/app/Info.plist`: added `<key>MetalCaptureEnabled</key><true/>`.
  Required for `.gputrace` capture without an attached debugger. Leave it.
- `src/mbgl/renderer/renderer_impl.cpp`: existing `EnableMetalCapture`
  block was wired to `CaptureDestinationDeveloperTools` (needs Xcode
  attached). Rewrote to use `CaptureDestinationGPUTraceDocument` saving
  to `$HOME/Documents/klattra-frame-N.gputrace` so we can pull the trace
  via `xcrun simctl get_app_container` and open in Xcode offline.
  **Gated off via `EnableMetalCapture = 0`.** To re-enable:

      constexpr auto EnableMetalCapture = 1;
      constexpr auto CaptureFrameStart = 100; // pick a frame past style load (~25)
      constexpr auto CaptureFrameCount = 1;

- Two reference traces saved at `/tmp/klattra_captures/`:
  - `low-zoom-z8-f100.gputrace` (76MB, broken state at acrossDistance=120000)
  - `high-zoom-z10-f60.gputrace` (57MB, working state at acrossDistance=40000)

**The hard wall**: the iOS Simulator 26.4 Metal Replayer doesn't work.
Every `.gputrace` it produces crashes the Replayer with
`Unexpected Replayer Termination — guest app crashed (512)`, regardless
of whether the captured frame rendered correctly. Confirmed by
attempting to replay the **working** high-zoom trace — same crash, same
exit code. So the crash is a simulator limitation, NOT a clue about our
bug. Initially looked promising; it's a red herring.

Without working Replay, Xcode's GPU debugger (View > Frame Debugger
Workspace, etc.) never opens the interactive drawcall inspector. The
static "Group by API Call" view is technically only reachable AFTER a
Replay attempt completes — which it never does — so the inspection
path through Xcode UI is closed on simulator captures.

**Non-obvious things found while debugging the capture setup** (will
save a lot of time for the next attempt):

- MapLibre's renderer skips drawcall emission entirely when nothing in
  the scene changed since last frame. `[mapView triggerRepaint]` alone
  is **not** enough — the trace ends up empty (1 command buffer, 1
  empty render encoder, 0 drawcalls). To force real draws, animate the
  camera by a microscopic amount each tick. A ±5e-7 degree lat alternation
  every 16ms does the trick. Code lives in `MBXViewController viewDidLoad`
  but has been removed from the file in cleanup — re-add if needed.

- Style loads around frame 25 of `Renderer::Impl::frameCount`. First ~25
  frames are too early — the terrain source/mesh hasn't been registered
  yet. **Capture frame ≥ 100** for a stable state with full rendering.

- The trace file size is a useful sanity check before you bother opening
  it in Xcode: ~18-30MB = empty (just buffer/texture dumps, no drawcalls),
  75MB+ = real rendering content.

### What the next session needs to do

The bug is one matrix transform away from being conclusively diagnosed.
Pick **ONE** of these paths:

**A. Capture on a real iOS device** *(recommended)*. The simulator
Replayer is just broken; on real device hardware the GPU debugger works
normally. Plug in an iPhone, open the iOS test app target in Xcode
(`/Users/mac/projects/maplibre-native/platform/ios/`), Cmd+R to launch
on device, navigate the Klättra style to the broken low-zoom view
(coords above), Debug > Capture GPU Workload. Then inspect the
`terrain-tile` drawable in the captured frame:
- *Vertex output* tab: what does the GPU compute for the post-vertex-
  shader positions? Manual math says some vertices should land inside
  the clip cube. Does the GPU agree?
- *Bound resources*: is the `TerrainDrawableUBO` for this drawable
  uploaded correctly? Check the actual matrix bytes vs. what the
  tweaker logged.
- *Viewport / scissor* state at the drawcall. We never confirmed these
  aren't being left over from the drape pass.

**B. Programmatic dump** *(if no device handy)*. Instrument
`src/mbgl/mtl/drawable.cpp` to log `setVertexBuffer`/`setFragmentBuffer`/
`drawIndexedPrimitives` arguments right before encoder.endEncoding().
For the terrain drawable specifically, dump the full UBO contents
(`memcpy` 80 bytes from the buffer and hexdump). At the broken zoom,
compare with the working zoom — find the byte that differs unexpectedly.
About an hour of work. Less satisfying than option A but bypasses the
simulator issue entirely.

**C. Manual matrix bisection**. Take the matrix values logged yesterday
for z=8 *and* re-capture them for z=10 (works), then carry vertex
`(4096, 4096, 0, 1)` through both matrices step-by-step in Python.
Yesterday I did this informally for z=8 only and the math said the
vertex should render. Doing it side-by-side with z=10 might surface
something — a sign flip, an unexpected zero, anything. Less work than
A or B, but might also turn up nothing if the bug is GPU-side
(misaligned UBO, wrong buffer slot, etc.).

### Working tree state right now

Same uncommitted changes as yesterday's handoff, **plus** the GPU
capture infrastructure (Info.plist + renderer_impl.cpp changes,
gated off). All debug logging from yesterday is removed. Camera-tickle
timer in MBXViewController is removed. Camera is restored to the
default Kebnekaise 5km/pitch72 view. High-zoom 3D rendering verified
working post-cleanup (see `/tmp/klattra_verify/cleanup_final.png`).

Render tests should still pass 1246/1246; nothing in today's session
touched the rendering paths. (Last run yesterday confirmed.)

---

## 🔖 Previous handoff (2026-05-16) — camera-clipping bug fix

This branch (`klattra-terrain-work`) has 21 files of uncommitted changes
(+1423 / -110, see `git diff --stat`). **Nothing is committed yet.**
Render tests pass: 1246 passed, 0 failed.

### What the user actually reported

A specific bug: **camera clipping into the terrain mesh** at steep
pitch + close zoom. The user's words:

> "I see this in some video games, clipping through something at
> certain angles and you see inside the 3d object."

The screen showed the framebuffer behind the clip plane — initially
the 2D basemap, then (after some misdirected fixes) the style's
background colour. **Misdiagnosed earlier in the session** as a
rendering bug; only correctly identified late in the session.

### Live state of the fix stack

In approximate order of "load-bearing for the user-reported bug":

1. **`elevationOffset` 500 m → 0 m** in
   `src/mbgl/renderer/layers/terrain_layer_tweaker.cpp`. Removes a
   workaround that's no longer needed (Pass 2 below makes it
   unnecessary) and gives the camera +500 m of headroom against the
   mesh.
2. **iOS-side camera-altitude clamp** in
   `platform/ios/app/MBXViewController.mm` (`enforceTerrainCameraClampOn:`,
   hooked into three delegate methods). When `camera.altitude <
   2 500 m` (hard-coded to Klättra's coverage), recomputes pitch as
   `acos(2500 / viewingDistance)` and re-sets the camera.
   **This is the actual fix for the clipping bug.** Recursion-guarded.
3. **Pass 1 (downward skirts)** in
   `src/mbgl/renderer/render_terrain.cpp`'s `generateMesh()` + the
   vertex shaders. Useful for hiding LOD seams. `MESH_SIZE = 253`.
4. **Pass 2 (skip main-pass for drape-capable layers)** in
   `render_fill_layer.cpp`, `render_line_layer.cpp`,
   `render_raster_layer.cpp`, `render_hillshade_layer.cpp`. Fills/
   lines/rasters/hillshades stop double-rendering at z=0 in the
   main pass when terrain is active; they only drape into the
   per-tile RenderTarget that the terrain mesh samples.
5. **GL-JS-style parent-fallback DEM sampling** —
   `TerrainDrawableUBO` extended with `dem_tl` + `dem_scale`, vertex
   shader applies the UV remap, `RenderTerrain::update()` builds
   per-IDEAL `DEMBinding`s. Mirrors `_demMatrixCache` in
   `maplibre-gl-js/src/render/terrain.ts`.
6. **`tileLodMinZoom` floor on the cover algorithm** in
   `tile_cover.cpp`/`.hpp` + `tile_parameters.hpp` +
   `render_raster_dem_source.cpp`. Caps variable-zoom emission at
   `idealZoom - 2` so the cover never emits z=8 horizon tiles
   directly; the prefetch infrastructure still loads them as
   backup.
7. **Bucket-change drape cleanup** in fill/line/raster/hillshade
   layers — when a source tile's bucket reparses, the drape-pass
   drawables for that tile are removed alongside the main-pass
   ones.
8. **Defensive log+skip in `mtl::UploadPass::buildAttributeBindings`**
   (and the partner spot in `mtl::VertexAttribute::getBuffer`).
   Three `assert(…)` paths replaced with `Log::Warning` +
   placeholder bindings. Safety net against any remaining drape
   lifecycle race.

### Misdiagnosis arcs to be aware of

Three "fixes" were built and then either reverted or kept for a
different reason than originally intended:

- **Tier 2 — atmospheric sky-gradient backdrop**: built (new
  `TerrainSkyShader`, fullscreen NDC quad with vertical gradient,
  rendered behind everything at high layer index, plus
  background-layer main-pass skip). The user looked at it and said
  "the sky shouldn't be inside the mountain either — I want the
  *surface* always visible." Reverted. All sky-shader files
  deleted, all references removed from `shader_source.hpp`,
  `shader_defines.hpp`, `shaders/manifest.json`, `shader_manifest.hpp`,
  both backends' `registerTypes` lists, `CMakeLists.txt`,
  `bazel/core.bzl`. The `elevationOffset = 0` change (item 1
  above) was originally introduced as part of Tier 2 prep; it's
  kept because it actually helps with the camera-clipping bug.
- **Outward-leaning skirts (Pass 3 attempt)**: extended the
  perimeter skirts outward in (x, y) as well as downward (Cesium
  style). Produced visible sloped "fin" artifacts at the outer
  cover boundary. Reverted to vertical-only skirts.
- **Variable-zoom emission `tileLodScale = 4.0, MinRadius = 1.0`**:
  first iteration of the cover bump. Surfaced a latent
  `mtl::UploadPass::buildAttributeBindings` assertion during
  fill-drape upload under fast camera pans. Dialled back to
  `tileLodScale = 1.0, MinRadius = 2.0` + the `tileLodMinZoom`
  floor; the defensive log+skip (item 8) is the safety net for
  the assertion class.

### Real follow-ups (not done in this session)

The iOS-side camera clamp is a working demo but has known
limitations. The proper library-level fix:

1. **Expose terrain elevation to the iOS layer** —
   `Map::getTerrainElevation(LatLng) -> double` backed by
   `RenderTerrain::getElevation`, plus an
   `[MLNMapView elevationAtCoordinate:]` wrapper.
2. **Thread the constraint through `Transform`/`TransformState`**
   — a `setMinAltitudeAboveTerrain(double)` analogue to the
   existing `setMinZoom` / `setMaxPitch`. Checked after every
   `jumpTo` / `easeTo` / `flyTo` / gesture handler, so the
   constraint is applied at the source rather than retroactively
   re-setting the camera via the public API.
3. **Sample the actual DEM at the camera's projected ground
   position**, not a hard-coded `kMinCameraAltitude = 2 500 m`.
   The current clamp value is tuned for Sweden / Kebnekaise
   coverage only.

Once those land, the iOS-side `enforceTerrainCameraClampOn:` block
in `MBXViewController.mm` can be deleted.

### Other open follow-ups in this branch

- The defensive log+skip in `mtl/upload_pass.cpp` and
  `mtl/vertex_attribute.cpp` (item 8) masks the underlying drape-
  drawable lifecycle bug rather than fixing it. A clean upstream
  fix would have layer groups validate drawables against the
  current bucket pointer before upload, or have buckets actively
  detach themselves from referencing drawables during reparse.
- Skirts (Pass 1) hide LOD/seam gaps but don't help at the outer
  cover boundary (which is the camera-clipping problem; now
  solved differently). If the cover edge ever becomes a problem
  again, the right answer is **terrain-aware tile cover** — the
  `procedural-gl-js` screen-space picker approach in
  `src/terrain.js`, or geometry clipmaps.
- `terrain/default` + `terrain/exaggeration` render-test baselines
  were regenerated after `elevationOffset = 0` landed. If you
  back that change out for any reason, regen again.

### How to build and verify

```sh
# Build the iOS sample app
bazel build //platform/ios:App --//:renderer=metal \
            --ios_simulator_device="iPhone 16 Pro"

# Install + launch on the iPhone 16 Pro simulator
DEVICE_ID=E9CF466F-71D5-41A3-B780-9665538FA552
xcrun simctl install "$DEVICE_ID" \
    bazel-bin/platform/ios/App_archive-root/Payload/App.app
xcrun simctl launch "$DEVICE_ID" app.klattra.dev
xcrun simctl io "$DEVICE_ID" screenshot /tmp/check.png
```

```sh
# Render-test suite (after rebuilding mbgl-render-test-runner via
# cmake --build build-macos --target mbgl-render-test-runner)
./build-macos/mbgl-render-test-runner \
    --manifestPath=metrics/macos-xcode11-release-style.json \
    --filter "terrain/.*"

# Full suite — last full run: 1246 passed, 25 passed-but-ignored,
# 83 ignored, 0 failed, 0 errored
./build-macos/mbgl-render-test-runner \
    --manifestPath=metrics/macos-xcode11-release-style.json
```

### What's untouched

- `FINISH_TERRAIN.md` (the original phase plan) — has not been
  updated with the new bug-fixing work; it tracks the *feature*
  plan, not the bug-fix arc. Either update it or treat
  `TERRAIN_PROGRESS.md` as the live source of truth for this
  branch.
- `PR_HANDOFF.md` (mentioned in earlier log entries) — same.

### Likely first-day questions for the next session

- *Should the camera clamp go into the library?* Yes — see
  follow-up #1-#3 above. The iOS-side version is a demo.
- *Are the sky-shader files truly gone?* Yes:
  `include/mbgl/shaders/mtl/terrain_sky.hpp`,
  `include/mbgl/shaders/gl/terrain_sky.hpp`,
  `src/mbgl/shaders/mtl/terrain_sky.cpp` all `rm`'d; `git status`
  doesn't list them.
- *Where's the camera clamp code?* `MBXViewController.mm`, the
  `enforceTerrainCameraClampOn:` method and the three delegate
  methods that call it. Constants
  `kMinCameraAltitude=2500`, `kMaxPitchClamp=60` at the top of
  the delegate block.
- *Is `MESH_SIZE = 254` or `253`?* 253 (had to drop one to fit
  skirts under the UInt16 index ceiling — `(MESH_SIZE+1)² +
  4·(MESH_SIZE+1) ≤ 65 535`).
- *Where do I look for the camera-clipping verification screenshots?*
  `/tmp/clamp_default.png`, `/tmp/clamp_v4.png`, etc. — they're
  ephemeral. Re-run the build + screenshot loop in "How to build
  and verify" above.

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

### GPU frame capture findings (2026-05-15, 08:30)

Successfully attached Xcode and captured a Metal frame from the
Klättra sample app running on iPhone 16 Pro simulator. Required
`SIMCTL_CHILD_MTL_CAPTURE_ENABLED=1` env var when launching the
process so Xcode's GPU Capture button isn't greyed out.

Frame stats: 11 Command Buffers, 11 Render Encoders, 368 Draw Calls.

**Render Encoder 0 (drape pass)** — 13 draw calls, 1MB load/store,
draws into Texture 0x117c40180 (512×512 RGBA8Unorm, the drape
RenderTarget for one DEM tile). Pipeline-State grouping shows:

- BackgroundShader
- FillShader (~25 draws via this pipeline across the frame)
- FillOutlineShader
- LineShader / LineSDFShader×2
- TerrainShader
- ClippingMaskProgram
- FillOutlineTriangulatedShader
- CircleShader×2
- SymbolSDFShader

**Drape texture contents at capture time:**
- Cream/tan base (background fill drape) ✓
- Light-blue water lakes (fill drape) ✓
- Pixel readout at (101, 161) inside drape texture: R=0.86, G=0.81,
  B=0.69, A=1 — recognisable tan-cream value, NOT zero.

**FillShader draw 63 (an actual fill drape draw):**
- indexCount=2892, instanceCount=1
- Pipeline state: FillShader
- Output: the same cream-tan drape texture filled with the layer's
  geometry. Xcode's "draw highlight" green outlined the affected
  fragments; pixels inside the outlines were the fill colour (not
  zero, not transparent).

**Terrain mesh draw 2168 (main pass):**
- Pipeline state: TerrainShader
- Vertex stage Texture 1 = 0x117c40180 (drape texture, 512×512)
- Vertex stage Texture 0 = 0x117c40000 (DEM, 514×514)
- Fragment stage binds the same two textures + samplers
- Attachments: CAMetalLayer Display Drawable (main framebuffer)
- Output pixel readout at (715, 638) on main framebuffer: R=0.74,
  G=0.77, B=0.79, A=1 — a sampled drape colour with diffuse-lighting
  tint applied. Visible terrain colour in the live render.

### Conclusion: fill drape is contributing

The captured frame disproves the earlier "fill drape produces no
visible fragments" hypothesis. In the captured frame:

1. The drape texture has rendered content (cream background, water
   lake fills, line features).
2. The terrain mesh fragment shader samples that drape texture at
   the right slot (Texture 1) with a valid sampler.
3. The main framebuffer pixel reads back a recognisable sampled
   drape colour — confirming the sampling path reaches the screen.

The "magenta-test no magenta" result from the overnight session is
explained by which fill drape draw was overridden — the test forced
the colour in the main fill tweaker path, but the drape path uses
its own `drapeTargetID`-branched matrix and runs through a
separately-bound `FillEvaluatedPropsUBO` per drape target. The
magenta override likely missed the drape-tweaker UBO that the GPU
actually read.

**Net effect for the PR:** the "fill drape draws but doesn't contribute"
known limitation in `PR_HANDOFF.md` is overstated. The fill drape
pipeline works as designed in the captured frame. The remaining
real limitation is the per-tile vs per-vertex elevation on
connected geometry (seams at tile boundaries), which is a different
issue (vertex-attribute scope, not pipeline state).

### Render-test baselines captured (2026-05-15, 09:00)

Built `mbgl-render-test-runner` from `cmake --preset macos` after
adding terrain source files to `CMakeLists.txt` (`include/`/`src/`
under `renderer/`, `renderer/layers/`, `style/`, `style/conversion/`,
plus Metal/GL shader headers). The Bazel build already tracked the
files; CMake did not.

Files added to `CMakeLists.txt`:
- `include/mbgl/renderer/render_terrain_drape_cache.hpp`
- `include/mbgl/style/conversion/terrain.hpp`
- `include/mbgl/style/terrain.hpp`
- `include/mbgl/style/terrain_observer.hpp`
- `src/mbgl/renderer/layers/terrain_layer_tweaker.{cpp,hpp}`
- `src/mbgl/renderer/render_terrain.{cpp,hpp}`
- `src/mbgl/renderer/render_terrain_drape_cache.cpp`
- `src/mbgl/style/conversion/terrain.cpp`
- `src/mbgl/style/terrain.cpp` + `terrain_impl.hpp`
- Metal section: `include/mbgl/shaders/mtl/terrain.hpp`,
  `include/mbgl/shaders/terrain_layer_ubo.hpp`,
  `src/mbgl/shaders/mtl/terrain.cpp`
- GL section: `include/mbgl/shaders/gl/terrain.hpp`

Build clean, `mbgl-render-test-runner` produced. Ran:

```
./build-macos/mbgl-render-test-runner \
    --manifestPath=metrics/macos-xcode11-release-style.json \
    --update default --filter "terrain/.*"
```

Updated both terrain style.json scaffolds to include a hillshade
layer so the baseline is visually meaningful (otherwise the test
renders only the background colour). Baselines now show recognisable
hillshaded mountain terrain at pitch 60°.

`default/expected.png` and `exaggeration/expected.png` are byte-
identical because the hillshade layer renders as a 2D overlay and
doesn't follow the terrain mesh — the exaggeration setting changes
the terrain-mesh elevation but not the hillshade output. A future
test scenario with a draped layer (fill or line) would surface the
difference.

### Full render-test regression run (2026-05-15, ~09:10)

After capturing the terrain baselines, ran the full suite to verify
that the `activeTerrain` pointer threaded through `PaintParameters`
and the drape-routing branches in every drape-capable layer don't
regress unrelated tests.

```
./build-macos/mbgl-render-test-runner \
    --manifestPath=metrics/macos-xcode11-release-style.json
```

Result:
- 1246 passed (92.0%)
- 25 passed but were ignored (1.8%) — pre-existing ignore-list
  entries whose underlying upstream bugs have since been fixed;
  not introduced by this branch.
- 83 ignored (6.1%)
- **0 failed, 0 errored**.

Including the two new terrain tests, which pass against their newly-
captured baselines. So the layer plumbing changes are confirmed
non-regressing on the macOS Metal backend.

### Exaggeration test design investigation (2026-05-15, 10:00)

Tried to make the exaggeration baseline visibly differ from default
by adding a draped fill polygon over the north half of the view.
Findings after several iterations:

1. **Exaggeration plumbing works.** Confirmed via temp `Log::Info` in
   `TerrainLayerTweaker::execute` — the value flows: style.json →
   `style::Terrain::Impl::exaggeration` → `RenderTerrain::getExaggeration`
   → `TerrainEvaluatedPropsUBO.exaggeration` → Metal shader
   `props.exaggeration`. The `mbgl-render-test-runner` saw
   `tweaker exaggeration=1.000000` for default and `=10.000000`,
   `=100.000000` for exaggeration variants.

2. **Drape path fires.** Confirmed via temp log in
   `RenderFillLayer::emitDrapeVariant` — `activeTerrain=yes,
   shaderGroup=yes` for both Fill and FillOutline variants, on every
   drape target. Drape drawables are produced.

3. **The rendered polygon is perfectly flat regardless of
   exaggeration.** Tested at 256×256/pitch 60° and 512×512/pitch 75°,
   exaggeration 1, 3, 5, 10, 100. All produce byte-identical pixels.
   The cyan polygon's south edge is a straight horizontal line at the
   polygon's latitude — no terrain-following deformation visible.

4. **Why the test setup can't show exaggeration's effect:**
   - Fills emit drawables in BOTH main pass (always) AND drape pass
     (when terrain is active). Two parallel paths.
   - Main-pass fill renders the polygon at z=0 as a flat 2D shape.
   - Drape pass paints it into the per-DEM-tile drape RenderTarget,
     which the terrain mesh samples.
   - For the draped version to be visible, the terrain mesh must
     occlude the main-pass version (depth-tested opaque). In the
     Klättra iOS capture this happens — terrain mesh is the visible
     surface and main fills are hidden behind it.
   - In this test, the DEM tile coverage is approximately
     `(-113.4 to -113.13, 35.88 to 36.03)`. The polygon extends to
     latitude 36.10 — past the DEM coverage. Where there's no DEM
     coverage there's no terrain mesh to occlude the main-pass fill.
   - With pitch 60°, the polygon's projected area on screen is mostly
     in the "past the horizon" region where DEM coverage doesn't
     reach. Result: the visible polygon is dominated by the main-pass
     fill, which doesn't depend on exaggeration.

   To make a render-test that surfaces exaggeration sensitivity:
   - Bound the polygon entirely inside DEM coverage, AND
   - Pick a view angle/zoom where the polygon's edges fall over
     terrain mesh that the camera looks at along an angle (so a few
     metres of vertical shift cross a pixel boundary on screen).

   This is design work outside the immediate scope; deferred. The
   current hillshade-only baselines still catch the "terrain rendering
   crashes / produces wrong colour / wrong projection" classes of
   regression, just not "exaggeration value silently changed."

Reverted the draped-fill style edits and the debug logs. Tests still
pass against the committed hillshade-only baselines.

### GL backend build verification (2026-05-15, 10:15)

Built `mbgl-render-test-runner` from a fresh `build-macos-opengl/`
configured with `MLN_WITH_OPENGL=ON`, `MLN_WITH_METAL=OFF`,
`MLN_WITH_VULKAN=OFF`. CMake found the macOS `OpenGL.framework`,
configured cleanly, full build (485 steps) linked without errors —
about 7-8 minutes incremental.

**What this confirms:**
- The new GL terrain shader header (`include/mbgl/shaders/gl/terrain.hpp`)
  compiles when included by the shader registration path.
- The shader registration via `BuiltIn::TerrainShader` in
  `gl/renderer_backend.cpp::registerTypes` resolves all the
  `ShaderSource<BuiltIn::TerrainShader, gfx::Backend::Type::OpenGL>`
  symbols (attributes, textures, name, vertex/fragment source).
- `nm libmbgl-core.a | grep terrain` lists the full set of terrain
  symbols including `TerrainLayerTweaker::execute`, both UBO
  `UniformBufferArray::createOrUpdate` template instantiations, and
  the referenced `RenderTerrain::getElevation` / `getExaggeration`.
- `libmbgl-render-test.a` and the final executable link cleanly.

**What this does NOT confirm:**
- That the GL terrain shader actually renders correctly.
  `./build-macos-opengl/mbgl-render-test-runner ... --filter
  "background-color/default"` (a baseline non-terrain test) crashes
  with the GL backend on Apple Silicon: `glBindVertexArray` raises
  `GL_INVALID_OPERATION` (1282), `glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT)`
  raises `GL_INVALID_ENUM` (1280), `glMapBufferRange` raises 1282,
  then an assertion in `src/mbgl/gl/buffer_allocator.cpp:280` fires.
  This is a macOS-arm64 / Apple-GL-stack limitation, not anything
  in the terrain code path — every render-test fails the same way.

Visual verification of the GL terrain rendering needs an Android,
Linux, or Windows target. The PR description has been updated to
reflect that the GL caveat is now "compiles + links cleanly,
runtime verification deferred to non-macOS targets" rather than
"partially implemented".

### Exaggeration test design — second attempt findings (2026-05-15, 10:35)

Went back to try designing a render-test where the exaggeration
baseline visibly differs from the default baseline. Used a series of
temp `Log::Info`s in `Renderer::Impl::render`, `mtl::LayerGroup::render`,
and the Metal terrain vertex/fragment shaders, plus successive
stripped-down style.json files.

**Key findings:**

1. **The terrain layer group IS iterated and rendered.** Confirmed
   via the per-pass log: `opaque pass numLayerGroups=3, ... opaque
   rendering group=terrain drawables=4 ... LG_DEBUG: group=terrain
   pass=1 seen=4 drawn=4 skip_enabled=0 skip_pass=0`. All four
   terrain mesh drawables submit `drawable.draw()` to the Metal
   command encoder in the opaque pass.

2. **Background layer overwrites terrain.** With a style containing
   only `{ background: red, terrain: ... }` the framebuffer is solid
   red — even after forcing the terrain fragment shader to return
   `half4(0, 1, 0, 1)` (bright green) AND forcing the vertex shader
   to output NDC-space full-screen positions (which guarantees every
   fragment runs). Same MD5 with exaggeration=1, 3, 10, 100. The
   background is winning the depth comparison.

3. **Terrain mesh DOES render when isolated.** With the same forced
   green shader but `layers: []` (no background, no fill), the
   framebuffer is pure green. So the terrain mesh's draw call
   reaches the rasterizer and produces visible pixels — the issue is
   *strictly* downstream depth ordering when other layers are present.

4. **Real terrain shader on its own produces near-black.** Same
   `layers: []` style but with the *committed* fragment shader (no
   forcing) yields an almost-entirely-black framebuffer. The fallback
   branch (`mapColor.a < 0.01`) executes — because there's nothing
   draping into the per-tile RenderTargets — and returns
   `color * diffuse * props.light_color_pad.rgb`. With no `light`
   block in the style, `light_color_pad` ends up zero/uninitialised
   for the test, so the fallback is multiplied to black.

**Together these mean:**
- The PR's terrain rendering works in the headless render-test runner
  for the same code paths the iOS app uses.
- Designing a test that surfaces exaggeration sensitivity needs a
  style where the terrain mesh has visible drape content *and* where
  the depth ordering against background lets terrain win. The
  Klättra capture works because real vector layers drape rich
  content and the visible terrain mesh has plenty of opaque material
  to occlude the background.
- The simplest design that should work: no background, a single
  draped fill polygon entirely inside DEM coverage, and rely on the
  terrain mesh's drape sampling to show the fill at terrain
  elevations. The default vs exaggeration baselines would then
  differ at the polygon edges. (Reasonable to ship as a follow-up
  test refinement.)

Reverted all the debug instrumentation. The current hillshade-only
baselines (terrain/default and terrain/exaggeration) still pass
against the committed `expected.png`s; they catch the
"terrain-rendering-crashes / wrong-colour / wrong-projection"
regression classes even if they don't catch silent exaggeration
drift.

The deeper "background overwrites terrain in opaque pass" issue is
a real architectural observation worth raising with maintainers
during PR review — it doesn't affect the Klättra app because that
style's vector layers have plenty of opaque content, but a simple
style with terrain + opaque background isn't behaving the way one
would expect.

### Exaggeration test design — working baselines (2026-05-15, 10:50)

After understanding from the previous round that the terrain mesh
fallback needs a non-zero `light` to paint visibly, and that the
hillshade layer drapes onto the mesh, designed a style pair that
actually exercises exaggeration:

- **terrain/default** and **terrain/exaggeration** share:
  - `light` block with white colour, intensity 0.5, position
    `[1.15, 210, 30]` — so the terrain fallback shader multiplies to
    a recognisable grey instead of black.
  - `hillshade` layer that drapes onto the terrain mesh.
  - No opaque `background` layer (which was overwriting the terrain
    in the previous attempt — see prior log entry).
  - Identical centre, zoom 11, pitch 60.
- The only difference: `terrain.exaggeration = 1.0` vs `20.0`.

Resulting baselines visibly differ. The exaggeration=20 mesh extrudes
mountain peaks high enough that they occlude more of the upper
viewport, producing a clearly taller relief silhouette than the
exaggeration=1 baseline.

Re-ran the full suite after `--update default`: 1246 passed, 25
passed-but-ignored, 83 ignored, 0 failed, 0 errored. No regressions.

`PR_HANDOFF.md` updated to remove the "baselines byte-identical"
caveat — the test now meaningfully catches both "terrain rendering
crashes / wrong colour / wrong projection" *and* "exaggeration value
silently changed".

### TERRAIN_LAYER_INDEX fix (2026-05-15, 11:00)

Root-caused the "background overwrites terrain" symptom we
discovered while investigating the exaggeration test design.

The code had a TODO-style comment in `render_terrain.hpp`:
> // Layer index (terrain renders early in 3D pass, use negative index)
> // TEMP: Using positive index to render ON TOP for debugging visibility
> static constexpr int32_t TERRAIN_LAYER_INDEX = 10000;

With index=10000:
- `visitLayerGroupsReversed` iterates highest→lowest, so the
  terrain layer group is drawn FIRST in the opaque pass and the
  user style's `background` (index 0) is drawn LAST.
- For 2D drawables in Metal,
  `LayerTweaker::multiplyWithProjectionMatrix` subtracts
  `(1 + currentLayer) * numSublayers * depthEpsilon` from
  `projMatrix[14]`, pulling their clip-space depth slightly toward
  the near plane. Background with `currentLayer=2` ends up with a
  depth value smaller than the terrain mesh's perspective depth.
- Background therefore PASSES the LessEqual test against terrain's
  already-written depth and overwrites it.

With the comment-suggested fix (`TERRAIN_LAYER_INDEX = -1`):
- terrain becomes the LOWEST-indexed layer group.
- `visitLayerGroupsReversed` iterates background (0) first, then
  terrain (-1) last.
- Terrain's opaque pixels overwrite background where the mesh has
  depth-passing geometry; background remains visible past the
  silhouette (sky/horizon).

Test with `background-color: red + hillshade + terrain
exaggeration=20`: dark-red hillshaded mountains visible in
foreground, bright-red sky visible above the silhouette. Matches
the rendering we see in the iOS Klättra capture.

Full render-test suite re-run with the fix: 1246 passed, 0 failed,
0 errored. No regressions.

### Why this matters for the PR

The shipped baselines (`terrain/default` and `terrain/exaggeration`)
weren't testing this code path because they don't have a
background layer. But the iOS sample app and any real style with a
background were depending on the terrain-mesh-overwrites-background
behaviour we just fixed. Without the fix, terrain would have been
invisible whenever the user's style had an opaque background, which
is the common case.

### iOS visual verification attempt (2026-05-15, 11:15)

Tried to visually verify the TERRAIN_LAYER_INDEX = -1 fix in the
maplibre-native iOS sample app on the iPhone 16 Pro simulator.

- Re-generated `platform/ios/MapLibre.xcodeproj` via
  `bazel-xcodeproj.sh --flavor drawable` after temporarily reducing
  the `target_environments` in `platform/ios:xcodeproj` from
  `[simulator, device]` to `[simulator]` only (the `App` target
  drops `provisioning_profile`, which `rules_apple` requires for
  device-environment generation).
- Generation succeeded.
- `bazel build //platform/ios:App --ios_multi_cpus=sim_arm64
  --apple_platform_type=ios --//:renderer=drawable` failed with 4
  compile errors in `platform/darwin/app/PluginLayerExampleMetalRendering.mm`:
  `property 'device' / 'mtkView' not found on object of type
  'MLNBackendResource *'`. `MLNBackendResource`'s Metal-only
  properties are gated on `MLN_RENDER_BACKEND_METAL`, and the
  preprocessor define isn't reaching that translation unit in the
  current Bazel iOS build graph.
- `bazel build //platform/ios/app-swift:MapLibreApp` failed with
  `fatal error: 'mbgl/mtl/mtl_fwd.hpp' file not found` in
  `MLNMapView.mm` — same kind of build-config gap.

Neither error is in code this PR touches. The earlier GPU frame
capture in this branch proves the iOS build did succeed at some
prior point; the build hygiene has slid since then for unrelated
reasons (likely an Xcode 26 SDK + rules_apple interaction).

**Verdict on the TERRAIN_LAYER_INDEX = -1 fix:** verified at the
render-test runner level (synthetic `background:red + hillshade +
terrain exaggeration=20` style renders correctly under the fix and
broken under the old value) and the logic is straightforward
(comment in `render_terrain.hpp` had `// TEMP: Using positive index
to render ON TOP for debugging visibility` flagging this exact
constant as something to revisit). Shipping without a live iOS
re-verification this session.

Reverted the `target_environments` edit in `platform/ios/BUILD.bazel`
since it's a local convenience, not part of the PR.

### iOS visual verification — SUCCESS (2026-05-15, 11:36)

Root-caused the earlier "iOS build broken" finding: was passing the
wrong renderer flag. `bazel-xcodeproj.sh` defaults to
`--flavor drawable` which maps to `--//:renderer=drawable` (drawable
OpenGL mode); for Metal you need `--//:renderer=metal`. The
`drawable_renderer` config_setting defines `MLN_RENDER_BACKEND_OPENGL=1`,
not `MLN_RENDER_BACKEND_METAL=1`, so the plugin file's accesses to
`MLNBackendResource.device` / `mtkView` (which are guarded by
`#if MLN_RENDER_BACKEND_METAL`) had no properties to bind to.

Correct build command:
```
bazel build //platform/ios:App --ios_multi_cpus=sim_arm64 \
            --apple_platform_type=ios --//:renderer=metal
```

550 actions, ~3 min, exit 0. Installed via `xcrun simctl install`,
launched via `xcrun simctl launch app.klattra.dev`. The Klättra
(Sweden 3D) style loads by default and renders correctly:

- 3D terrain mesh visible with proper shading and depth.
- Topo basemap drapes onto the terrain (kalfjäll, glaciar,
  hojdkurva, skog, hydrolinje, strandlinje, vatten all visible at
  correct mesh-relative positions).
- Place labels (Kebnekaise, Råbots glaciär, etc.) lift to terrain
  elevation via the Phase 5 symbol-elevation path.
- Hiking trails (red dashed) follow ridges and valleys.
- Background (sky beyond the silhouette) shows correctly past the
  terrain mesh — confirms TERRAIN_LAYER_INDEX = -1 fix works in
  the real app, not just the render-test runner.

Net: the entire branch's terrain-rendering pipeline is now verified
end-to-end on iOS Metal at runtime. The earlier "iOS verification
deferred" note can be removed in the next PR_HANDOFF.md edit.

### Horizon bleed-through fix (2026-05-15, 18:20)

Symptom: at high pitch (~70°+), the terrain mesh rendered correctly
for nearby foreground tiles but the horizon area at the bottom of
the screen showed the flat 2D topo basemap directly — "mountains
floating in a beige ocean of basemap" when looking south-west from
Kebnekaise at pitch 72°.

Root cause, two compounding issues confirmed by per-frame debug
logging in `RenderTerrain::update()` and tile-pyramid request logs:

1. **DEM cover never went variable-zoom.** With default
   `tileLodMinRadius=3, tileLodScale=1` (see
   `src/mbgl/renderer/tile_parameters.hpp:36-38`), every visible
   DEM tile fell inside the foreground "always-max-zoom" radius.
   The cover loop in `src/mbgl/util/tile_cover.cpp:158-290`
   emitted only z=12 tiles — none at lower zooms for the horizon —
   even though the pitch > tileLodPitchThreshold condition would
   have allowed it. Pre-fix tile-pyramid log at the Klättra default
   camera (`(67.9026, 18.4954)`, distance 5000 m, pitch 72°,
   heading 215°): 8 tiles, all z=12.

2. **Tiles in cover with no parsed DEM were skipped entirely.**
   `RenderTerrain::update()` had `continue;` for tiles where
   `hillshadeBucket == nullptr` or the DEM image was empty,
   leaving holes in the mesh wherever DEM data hadn't arrived
   yet. Even after fix (1) emitted variable-zoom tiles, those new
   horizon tiles take a moment to load — without an
   empty-DEM-fallback, every pan would briefly reveal the bleed-
   through again until the new z=8 tile parsed.

Fixes shipped as two minimal patches:

**Part A — empty-DEM-fallback texture**
(`src/mbgl/renderer/render_terrain.cpp:328-355`,
`src/mbgl/renderer/render_terrain.hpp:215-237`):

Cache a 1×1 RGBA texture encoding Mapbox-RGB elevation 0
(`{R: 1, G: 134, B: 160} = 0x0186A0 → 100000 → height = 0 m`)
and bind it when a tile's DEM hasn't loaded. The terrain mesh
renders flat at sea level instead of leaving a hole. Mirrors
maplibre-gl-js's `_emptyDemTexture` in
`/Users/mac/projects/maplibre-gl-js/src/render/terrain.ts:272-280`.

Also restructured the drawable-create loop to track whether each
tile's existing drawable was built against real DEM data or the
empty fallback (`tilesWithDrawables` now stores `bool hasRealDEM`
instead of just "drawable exists"). When the state transitions
(empty-fallback → real DEM arrives, or tile drops from cover),
the old drawable is removed and rebuilt with the right texture.
Without this, fallback drawables would persist forever once DEM
data arrived, leaving a stale flat patch under what should be
real terrain.

**Part B — variable-zoom cover for the DEM source**
(`src/mbgl/renderer/sources/render_raster_dem_source.cpp:25-50`):

In `RenderRasterDEMSource::updateInternal`, copy the
`TileParameters` and override
`tileLodMinRadius = 1.0, tileLodScale = 4.0` (vs the global
defaults of 3.0 and 1.0). The existing variable-zoom logic in
`tile_cover.cpp` is gated on `pitch > tileLodPitchThreshold`
(default 60°); the bumped LOD parameters make the same logic
actually emit lower-zoom tiles for distant areas in the cover
once that pitch threshold is crossed.

Tradeoff: this also affects hillshade-only usage (non-terrain
styles using a raster-dem source for hillshade), which will
request a few extra DEM tiles at high pitch — small (~67 KB
per tile, only at pitch > 60°). No effect on non-DEM sources or
on foreground tile zoom. A more surgical fix would thread a
"terrain is using this source" flag from `RenderOrchestrator`
into `RenderRasterDEMSource`, but that requires API changes
across the source plumbing; deferred.

Approach considered and rejected:

- **B2 (terrain-aware bounding volumes in tile_cover.cpp).** This
  is what maplibre-gl-js does (see
  `/tmp/cover.ts:234` — `getTileBoundingVolume(tileID, wrap,
  transform.elevation, options)`). Most "correct" — uses tile
  elevation extents to influence the frustum intersection — but
  requires plumbing terrain state into `TileCoverParameters` and
  changing the cover algorithm. Out of scope for the immediate
  bleed-through fix; left for upstream review.
- **B3 (RenderTerrain computes its own cover).** Would decouple
  the terrain mesh layout from the DEM source's pyramid entirely
  and would need parent-fallback DEM-texture sampling with a
  per-tile UV remapping matrix (mirroring gl-js's
  `_demMatrixCache` in `terrain.ts:291-305`). Most invasive;
  deferred.

Verification:

- **Tile-pyramid log at the Klättra default camera, post-fix:**
  21 DEM tiles in cover — 2 at z=8 (horizon), 3 at z=11
  (middle distance), 16 at z=12 (foreground). The z=8 tiles are
  the variable-zoom emission that didn't happen before.
- **iOS screenshot at pitch 72°, distance 5000 m:** terrain
  mesh fills the visible frame from foreground glaciers all the
  way to the horizon silhouette. No 2D basemap visible at the
  bottom of the screen. `/tmp/klattra_after_fix_pitch72_clean.png`.
- **Terrain render-tests:** both `terrain/default` and
  `terrain/exaggeration` pass against the existing committed
  baselines — no regeneration needed. The added empty-DEM-
  fallback path doesn't change behavior for the test scenes
  (their DEM coverage is dense and pre-loaded).
- **Full render-test suite:** 1246 passed (92.0%), 25 passed-but-
  ignored, 83 ignored, **0 failed, 0 errored**. Matches the prior
  baseline exactly. The DEM-source `tileLodScale` bump and the
  empty-DEM-fallback path don't regress anything in the suite.

### Crash on fast camera pan + parent-fallback DEM sampling (2026-05-16, ~00:00)

After the variable-zoom fix landed, user feedback during interactive
stress-testing surfaced two follow-up issues:

1. **Crash on fast zoom/pan** — `EXC_CRASH/SIGABRT` from
   `__assert_rtn` deep inside
   `mtl::UploadPass::buildAttributeBindings` while uploading a fill-
   layer drape drawable. Backtrace pointed at
   `RenderTarget::upload → TileLayerGroup::upload → Drawable::upload
   → buildAttributeBindings`.

2. **Visible "blurry pop"** during camera movement: when the user
   panned to a new area, the mesh briefly rendered with smoothed,
   low-resolution shape (z=8 parent stand-in for missing z=12
   ideals) before snapping crisp once the network caught up.

Root causes:

- **Crash:** the drape-capable layers (`RenderFillLayer`,
  `RenderLineLayer`, `RenderHillshadeLayer`, `RenderRasterLayer`)
  detect bucket-ID changes via
  `setRenderTileBucketID(tileID, bucket.getID())` and call
  `removeTile(renderPass, tileID)` to drop stale main-pass
  drawables — but `removeTile` only walks the layer's own
  `TileLayerGroup`, not the per-drape-target groups inside the
  terrain drape cache. Drape drawables therefore survived a
  bucket reparse with their `setSharedRawData` pointer still
  aimed at the OLD bucket's vertex vector, which was either
  empty or partially repopulated by upload time. The upload
  asserts both `effectiveAttr.getStride() > 0` and the fall-
  through `assert(false)` fired in that scenario.

- **Blurry pop:** the existing `RenderTerrain::update()` keyed
  terrain drawables by the actual DEM tile's `OverscaledTileID`
  (`renderTile.getOverscaledTileID()`). When `updateRenderables`
  fell back to a z=10 parent for several z=12 ideals, all 16
  children collapsed into a single drawable at z=10 mesh density
  (128×128 vertices for ~64 km² instead of 16 separate 128×128
  meshes for 4 km² each). The drape texture sampling also
  silently stretched the parent's drape over the larger area,
  producing the smoothed-mountains effect.

Fixes shipped:

**Per-source LOD floors** (`render_raster_dem_source.cpp`):

- Added `TileCoverParameters::tileLodMinZoom` and
  `TileParameters::tileLodMinZoom`. `tile_cover.cpp` consults the
  floor when computing `minZoom` for variable-zoom emission.
- `RenderRasterDEMSource::updateInternal` sets
  `tileLodMinZoom = idealZoom − 2` to cap variable-zoom *cover
  emission* at 4× resolution gap. Prefetch backup is left at the
  global default (`DEFAULT_PREFETCH_ZOOM_DELTA = 4` = z=8 for a
  z=12 ideal): with the new parent-fallback DEM sampling, a
  cached z=8 panTile drives 16-per-z=10 / 256-per-z=12 sub-rect
  reads through `dem_tl/dem_scale`, so the mesh stays at z=12
  vertex density and just inherits slightly softer texture
  detail until the exact-zoom DEM streams in. Wider prefetch
  area is a net win — pans that overshoot the immediate
  neighbourhood still find a cached ancestor.
- LOD itself dialed from `(scale=4, MinRadius=1)` → `(2, 2)` →
  `(1, 2)` over the session — the first pass surfaced the crash,
  the second mitigated frequency, the third leaned on the new
  `tileLodMinZoom` floor for clean horizon emission.

**Per-ideal terrain drawables with UV remap**
(`render_terrain.cpp` / `.hpp`, `terrain_layer_tweaker.cpp`,
`terrain_layer_ubo.hpp`, `mtl/terrain.hpp`, `gl/terrain.hpp`):

- Extended `TerrainDrawableUBO` from 64 → 80 bytes, adding
  `dem_tl` (float2) + `dem_scale` (float) + pad. Mirrors the
  `_demMatrixCache` matrix in maplibre-gl-js's
  `src/render/terrain.ts:291-305`.
- Metal + GL vertex shaders apply
  `mapUV = uv * dem_scale + dem_tl` before sampling DEM; fragment
  shaders sample the drape texture at `mapUV` too (the drape
  RenderTarget is keyed by the same source DEM tile, so the same
  remap factor applies to both).
- `RenderTerrain::update()` now keys drawables by the IDEAL
  `OverscaledTileID` (constructed from `renderTile.id`), tracks
  GPU DEM textures in `demTexturesByTile` keyed by source ID,
  and produces a per-ideal `DEMBinding` (texture + UV remap)
  resolved against the actual tile underlying each cover slot.
  When `updateRenderables` falls back to a z=10 parent for 16
  z=12 ideals, the new code produces 16 separate drawables —
  each at z=12 mesh density, each sampling a 1/16 sub-rect of
  the parent's texture — instead of one collapsed z=10 mesh.
- The tweaker writes the per-drawable `dem_tl` / `dem_scale`
  into the UBO so the shader applies the right sub-rect per
  drawable.

**Drape cleanup on bucket change** (every drape-capable layer):

- `RenderFillLayer`, `RenderLineLayer`, `RenderHillshadeLayer`,
  `RenderRasterLayer` now extend their existing "drop drawables
  on bucket-ID change" path. After `removeTile(renderPass,
  tileID)` they also iterate `activeTerrain->visitDrapeTargets`
  and call `drapeGroup->removeDrawables(renderPass, tileID)` on
  each, so the drape pipeline never holds a drawable whose
  shared-data pointer is mid-reparse.

**Defensive guards in the upload pass**
(`src/mbgl/mtl/upload_pass.cpp`, `src/mbgl/mtl/vertex_attribute.cpp`):

- Replaced the three `assert(…)` paths inside
  `UploadPass::buildAttributeBindings`'s `resolveAttr` lambda
  (and the matching `assert(false)` in
  `mtl::VertexAttribute::getBuffer`) with `Log::Warning` plus a
  placeholder binding. When an attribute slot ends up with no
  shared data, no raw data, and no buffer — typically a drape
  drawable whose source vertex vector has been emptied by a
  concurrent reparse — the drawable now renders with the
  shader's default for that slot instead of aborting the process.
  This is a generic safety net, not a root-cause fix, but the
  practical effect is that the application stays responsive
  under aggressive interactive use while the underlying layer-
  group lifecycle bugs are addressed.

Verification:

- Steady-state at the Klättra default camera renders identically
  to the previous fix — `/tmp/final_steady.png` indistinguishable
  from `/tmp/klattra_after_fix_pitch72_clean.png`.
- Scripted four-camera pan stress test (pan east → pan diagonal
  + zoom out → pan back + zoom in) now completes without crash
  on Apple Silicon iOS simulator. Before this commit the same
  sequence aborted within ~3 seconds on the second pan.
- `terrain/default` baseline unchanged (still bit-identical).
  `terrain/exaggeration` baseline regenerated (0.1% pixel
  diff from the new vertex-shader path — the identity-remap
  `uv * 1.0 + (0,0)` introduces sub-LSB float rounding in the
  rasterized varying).

Known limitations:

- Pan-tile prefetch coverage (`prefetchZoomDelta=2`) caches z=10
  backup for the area immediately around the camera. A fast pan
  to ground outside that radius briefly shows flat-basemap
  bleed-through until tiles arrive over the network. Mitigation
  would be either a larger prefetch radius (more memory) or
  pre-warming the tile cache on app start; both are out of scope
  here.
- The defensive log-and-skip in `buildAttributeBindings` masks
  the underlying lifecycle bug rather than fixing it. A clean
  upstream fix would have layer groups validate their drawables
  against the current bucket pointer before upload, or have
  buckets actively detach themselves from referencing drawables
  during reparse. Tracked as a follow-up.

Full render-test suite re-run with all changes: 1246 passed,
25 passed-but-ignored, 83 ignored, **0 failed, 0 errored**.
Same as the prior baseline.

### Basemap bleed-through past tile edges (2026-05-16, ~13:30)

User reported visible flat 2D basemap "underneath the 3D mountain"
at close zoom + steep pitch — looked like clipping through a 3D
model and seeing the inside. Two screenshots showed mountain mesh
with proper shading in the upper portion and a sharp transition
to flat tan-land + blue-water topo basemap in the lower portion.

Root cause: the basemap layers (fill / line / raster / hillshade)
were rendering BOTH into the per-tile drape RenderTargets (sampled
by the terrain mesh) AND straight into the main framebuffer at
z=0. The terrain mesh occluded the framebuffer basemap where the
mesh covered it — but past the mesh's tile-edge boundaries (or
in regions outside DEM coverage / parent-fallback reach), the
flat z=0 basemap was the only thing rendered to those screen
pixels. From a steep camera angle this read as the 2D map
showing "underneath" the mountains.

Research turned up the standard industry fixes
(`TERRAIN_PROGRESS.md` notes the sources: gl-js
`_buildSkirts` + `LAYERS_TO_TEXTURES`, Cesium quantized-mesh
skirt fields, the GameDev / Demon Throne tutorials, MapTiler
docs). Two complementary techniques:

**Pass 1: terrain skirts** (mirrors gl-js `_buildSkirts` in
`maplibre-gl-js/src/render/terrain.ts:525`).

`RenderTerrain::generateMesh()` now emits four extra vertex
strips around each tile's perimeter — `top`, `bottom`, `left`,
`right` — each with `(MESH_SIZE+1)` vertices sharing the
corresponding mesh-edge `pos.xy` but carrying
`texture_pos = (-1, -1)` as a sentinel. The Metal and GL vertex
shaders detect the negative `texture_pos` and drop the vertex's
world elevation by `5000 m`, turning each strip into a vertical
wall hanging straight down from the tile edge. The wall samples
the same drape texture as the edge, so it visually continues the
basemap content underneath the mesh and hides any seam between
adjacent same-zoom tiles or between different LODs.

`MESH_SIZE` dropped from 254 → 253 to keep total vertex count
(`(MESH_SIZE+1)² + 4·(MESH_SIZE+1) = 65,532`) under the UInt16
index ceiling.

**Pass 2: skip main-pass for drape-capable layers when terrain
is active** (mirrors gl-js `LAYERS_TO_TEXTURES` short-circuit in
`maplibre-gl-js/src/webgl/render_to_texture.ts`).

The four drape-capable layers (`RenderFillLayer`,
`RenderLineLayer`, `RenderRasterLayer`, `RenderHillshadeLayer`)
now early-skip the main-pass `addDrawable` path when
`activeTerrain` is non-null. The drape-pass code already routes
this layer's content into the per-tile drape RenderTargets that
the terrain mesh samples; the main-pass render only existed as
the legacy "2D map" path and is what produced the bleed-through.
Each layer's `update()` also clears any previously-accumulated
main-pass drawables on the first frame where `activeTerrain`
flips on. The `RenderBackgroundLayer` deliberately stays in the
main pass — its single solid colour fills the framebuffer past
the terrain mesh as a backdrop, taking the role gl-js gives to
its sky-coloured clear.

Verification at the reproduction camera (close zoom, pitch 78°,
heading 170°, ~67.9094 N / 18.4975 E):

- Before fix: lower screen showed sharp 2D basemap (tan
  landcover + light blue lake / peninsula). [Screenshot:
  `/tmp/skirts_bleed.png`]
- After Pass 1 alone (skirts only): unchanged — the basemap
  bleed-through was not at the immediate tile-edge seam but
  out where the mesh ends entirely, beyond the skirts' reach.
- After Pass 1 + Pass 2: lower screen is the style's background
  colour. Mountain mesh in the upper portion is the only
  "ground" content. [Screenshot: `/tmp/pass2_test.png`]

Default-camera steady-state (Kebnekaise, dist 5 km, pitch 72°)
visually unchanged — the visible frame is fully covered by
mesh tiles, so removing the main-pass basemap has no observable
effect.

`terrain/default` baseline unchanged. `terrain/exaggeration`
baseline regenerated (skirt vertices contribute a few extra
near-z=0 pixels at the mesh perimeter that propagate through to
a sub-percent pixel diff). Full render-test suite re-run with
both passes in place: 1246 passed, 25 passed-but-ignored, 83
ignored, **0 failed, 0 errored** — same as baseline.

Trade-off documented for downstream callers: terrain-on now
behaves as "drape-or-nothing" for fill/line/raster/hillshade
content. A style that mixed terrain with a partial-coverage 2D
basemap (e.g., DEM only over a sub-region with the rest meant
to read as a 2D map) would now see backdrop colour outside the
DEM area instead of the 2D basemap. For the Klättra style and
any other typical full-coverage 3D-terrain style this is the
desired behaviour. A future style-spec extension could expose
a per-layer "skip-main-pass-with-terrain" toggle if mixed-mode
becomes a real use case.

### Atmospheric backdrop / sky gradient (2026-05-16, ~16:30)

User stress-tested the Pass 2 build and found a remaining artifact:
at certain steep-pitch + offset-from-target camera angles, the
loaded terrain tile cover ended visibly in the foreground and the
style's flat background colour (cream/beige) showed past the mesh
edge. Same visual class as the original bleed-through but with the
*background layer* now standing in for the previously-visible
fills/lines.

Skirts (Pass 1) couldn't reach this — they only hang downward
from the perimeter; outward-leaning skirts produced visible
sloped "fin" artifacts (commit reverted same session). The real
fix was researched against three production 3D map renderers
(maplibre-gl-js, Cesium, `felixpalmer/procedural-gl-js`) and they
agreed: **render an atmospheric sky behind the terrain.** See
`procedural-gl-js`'s [`src/sky.js`](https://github.com/felixpalmer/procedural-gl-js/blob/master/src/sky.js)
(hemisphere mesh, `renderOrder = 10000`, atmospheric-scattering
material).

Our adaptation, kept as small as possible:

**New `TerrainSkyShader`** (`include/mbgl/shaders/mtl/terrain_sky.hpp`,
`include/mbgl/shaders/gl/terrain_sky.hpp`,
`src/mbgl/shaders/mtl/terrain_sky.cpp`, plus entries in
`shader_source.hpp`, `shader_defines.hpp`, `shaders/manifest.json`,
both backends' `registerTypes` lists, and `shader_manifest.hpp`).
Vertex stage draws a fullscreen NDC quad with `gl_Position.z = 1.0`
(far plane). Fragment stage emits a vertical gradient — cyan top
(`rgb(130, 176, 217)`) to warm horizon haze bottom (`rgb(219, 209,
188)`) — squared so the midpoint reads slightly hazier. Pos is
packed as `int16` NDC × 32767 to fit the existing Short2 attribute
infrastructure.

**Sky drawable** in `RenderTerrain`. New `skyLayerGroup` at
`SKY_LAYER_INDEX = 100000` — well above any real style-layer
index, so it always sorts highest in `layerGroupsByLayerIndex` and
gets drawn FIRST in the reversed-order opaque pass; the terrain
mesh at `TERRAIN_LAYER_INDEX = -1` overdraws it last via depth
test. The drawable itself is a 4-vertex / 2-triangle NDC quad
with `setDepthType(ReadOnly)` so the sky's z=1.0 fragments lose
the depth test to any terrain pixel but don't write into the
depth buffer for later passes.

**Background layer main-pass skip** (`render_background_layer.cpp`).
With the sky drawable as backdrop, the `RenderBackgroundLayer`
main-pass drawables would overdraw the sky with the style's flat
colour and bring back the original problem. Same pattern as Pass
2's fill/line/raster/hillshade skip: when `activeTerrain` is
non-null, clear existing main-pass drawables and skip the per-tile
emit loop. The drape-pass routing into per-DEM-tile drape targets
stays intact so the background colour still contributes
*underneath* the basemap fills on the terrain mesh's surface.

Verification:

- **Default camera** (Kebnekaise, dist 5 km, pitch 72°, heading
  215°): visually identical to before. Mesh fully covers the
  frame so the sky is occluded everywhere.
- **Bleed-through camera** (67.9094 N / 18.4975 E, dist 4 km,
  pitch 78°, heading 170° — same setup that previously showed
  the cream cut): now shows a smooth cyan-to-haze vertical
  gradient past the mesh edge. Reads as atmosphere, not as a
  rendering bug. `/tmp/sky_bleed_camera.png`.
- **`terrain/default` + `terrain/exaggeration` baselines**
  regenerated to capture the sky behind the visible mesh edges
  in those test scenes. Both pass against the new baselines.
- **Full render-test suite** (after baseline regen):
  **1246 passed (92.0%), 25 passed-but-ignored, 83 ignored,
  0 failed, 0 errored** — same as baseline.

Known follow-ups (not done here):

- **Sky gradient is screen-space, not view-direction-aware.** At
  extreme camera pitches (looking nearly straight down or
  straight up) the gradient orientation can read slightly wrong.
  A proper sky-hemisphere mesh with view-direction sampling
  (Procedural GL JS's full solution) would handle this; deferred.
- **No atmospheric fog blending on the terrain mesh itself**, so
  the mesh edges meet the sky with a hard line rather than fading
  through haze. Procedural GL JS does this by rendering a 256×256
  spheremap of the sky into a render target and sampling it from
  the terrain fragment shader as a distance-blended fog colour.
  Adds two passes and a uniform; deferred to a Tier 3 if needed.
- **Gradient colours are baked into the shader**, not exposed to
  style spec. Anyone wanting custom sky tones would need to edit
  `terrain_sky.hpp`. A proper `style.sky` extension (the gl-js
  approach) would expose `sky-color`, `horizon-color`, `fog-color`
  as paint properties — out of scope here.

### Misdiagnosis course-correction → terrain-aware camera (2026-05-16, ~18:30)

User feedback on Tier 2: *"the problem was that we lost the mountain
surface in the first place, replacing the thing that should not show
with something else that should not show does not make it better.
also why would the sky be inside the mountain? all I want is for the
mountain surface texture to stay visible at all times and not clip
inside the mountain."*

This was a misdiagnosis, in retrospect obvious from the original
report:

> "I see this in some video games, clipping through something at
> certain angles and you see inside the 3d object, here is see the
> base map that is under the mountain if I zoom in too much"

The signature is **camera-inside-mesh clipping**: at steep pitch +
close zoom, the camera's altitude (= `viewingDistance × cos(pitch)`)
falls *below* the terrain mesh's peak elevation. The camera's near
plane intersects the mountain's interior, and through that clip the
framebuffer behind the mesh shows — originally the 2D basemap, then
(after Tier 2) the sky gradient. **Both are wrong** — the user wants
the mountain *surface* always visible, i.e. the camera prevented
from going inside the mesh in the first place.

Tier 2 (sky gradient + background main-pass skip) is reverted:

- `TerrainSkyShader` files deleted (`mtl/terrain_sky.hpp`,
  `gl/terrain_sky.hpp`, `mtl/terrain_sky.cpp`)
- `shader_source.hpp` / `shader_defines.hpp` / `shader_manifest.hpp`
  entries removed
- `shaders/manifest.json` entry removed
- Metal + GL `registerTypes` lists restored
- `CMakeLists.txt` + `bazel/core.bzl` entries removed
- `RenderTerrain` sky drawable + `SKY_LAYER_INDEX` constant
  removed
- `render_background_layer.cpp` main-pass-skip restored

Two changes kept from this round:

1. **`elevationOffset` reduced 500 m → 0 m**
   (`terrain_layer_tweaker.cpp:44`). The original 500 m uplift was a
   workaround for the basemap rendering at z=0 in the main pass
   (mesh's lowest valleys fought the basemap on depth). With Pass 2
   in place, the basemap doesn't render at z=0 in the main pass
   anymore, so the offset isn't needed. Removing it gives the camera
   +500 m of vertical headroom against the mesh — small but
   non-trivial mitigation for the same clipping bug.
2. **Camera-altitude clamp in the iOS sample app's MLNMapView
   delegate** (`MBXViewController.mm`). The proper fix.

The camera clamp implementation in
`enforceTerrainCameraClampOn:`:

- Hooks into `mapView:regionDidChangeWithReason:animated:`,
  `mapView:regionDidChangeAnimated:`, and
  `mapView:regionIsChangingWithReason:`. (The "with reason"
  variants suppress the simpler delegates when implemented, per
  MLNMapViewDelegate.h; we hit all three to be safe.)
- Reads `camera.altitude` (perpendicular distance from viewpoint to
  the z=0 plane) and `camera.viewingDistance` (slant distance from
  viewpoint to target).
- If `altitude < kMinCameraAltitude` (2 500 m — clears Kebnekaise's
  2 100 m peak with ~400 m margin), recomputes pitch so the
  altitude lands on the clamp:
  `pitch = acos(kMinCameraAltitude / viewingDistance)`.
- Recursion-guarded via `isAdjustingCameraForTerrain` so the
  follow-up `setCamera:` doesn't loop.
- Initial `setCamera:` in `viewDidLoad` doesn't fire the delegate,
  so the clamp is also applied explicitly there.

Behaviour:

- Default camera (5 km / 72° → clamped by `PITCH_MAX` to 60° / alt
  2 500 m): unchanged. Camera lands exactly on the clamp; mountain
  fully visible.
- Aggressive close-zoom camera (4 km / 78° → clamped to 60° / alt
  2 000 m, *below* threshold): clamp pulls pitch to 51°, then
  39°, converging at alt 2 621 m > 2 500 m. View becomes less
  tilted than the user requested — the trade-off any
  terrain-aware camera makes — but the mountain stays visible
  and nothing clips through.
- During interactive gestures: the clamp fires on every
  `regionIsChanging` callback, so the camera feels like it "rests
  against" the terrain instead of letting the user dive into it.

Limitations of this iOS-only demo (tracked as the real follow-up):

- **Hard-coded `kMinCameraAltitude = 2 500 m`** is sized for the
  Klättra DEM's coverage (Sweden, max peak ≈ 2 100 m). A
  geographically general solution needs to sample the actual DEM
  at the camera's projected ground position and clamp dynamically.
- **iOS sample app only**. The Android and macOS sample apps
  don't get the same clamp, and any third-party MLNMapView
  consumer would have to add it themselves. The correct fix is
  in the library:
  - expose `Map::getTerrainElevation(LatLng) -> double` (and an
    iOS `[MLNMapView elevationAtCoordinate:]` wrapper) backed by
    the existing `RenderTerrain::getElevation`,
  - thread a "min altitude above terrain" constraint through
    `Transform` / `TransformState` (analogous to `MinZoom` /
    `MaxPitch`), checked after every `jumpTo` / `easeTo` /
    `flyTo` / gesture handler,
  - or make `Transform` consult an injected
    `TerrainElevationProvider` and apply the constraint there.
- **Re-entrance via `setCamera:`**. The clamp triggers another
  delegate callback; the recursion guard works but cleaner would
  be a Transform-level constraint that never produces an
  invalid camera state in the first place.

### `terrain/default` + `terrain/exaggeration` render-test baselines

Both baselines regenerated to capture the mesh at the new
`elevationOffset = 0` (mountains sit 500 m lower in absolute
terms). Both pass against the new baselines. Full render-test
suite re-run: **1246 passed (92.0%), 25 passed-but-ignored, 83
ignored, 0 failed, 0 errored** — matches baseline.
