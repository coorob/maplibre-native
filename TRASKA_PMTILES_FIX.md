# PMTiles `FileSource` teardown use-after-free fix (`crashfix-6.26.0`)

This branch is upstream MapLibre Native **[`ios-v6.26.0`](https://github.com/maplibre/maplibre-native/releases/tag/ios-v6.26.0)**
plus one commit that fixes a use-after-free crash in `PMTilesFileSource`, so iOS apps can use
native `pmtiles://` without crashing during map teardown/transitions.

- **Commit:** `9a74bb23` — fix + regression tests
- **Files:** `platform/default/src/mbgl/storage/pmtiles_file_source.cpp`,
  `test/storage/pmtiles_file_source.test.cpp`
- **Prebuilt distribution:** [coorob/maplibre-gl-native-distribution `6.26.0-traska.1`](https://github.com/coorob/maplibre-gl-native-distribution)
  — drop-in SwiftPM package, ready to consume.

## The bug

`EXC_BAD_ACCESS` on the `org.maplibre.mbgl.PMTilesFileSource` thread during map
teardown/transitions (e.g. leaving a map screen while topo tiles are still loading).

`PMTilesFileSource::Impl` services one tile request by chaining sub-requests on its worker
thread — **header → directory → tile** — and stored each handle in a single per-request slot:

```cpp
std::map<AsyncRequest*, std::unique_ptr<AsyncRequest>> tasks;   // ONE slot per logical request
...
tasks[req] = getFileSource()->request(resource, [=](const Response&){ ... });  // at EVERY hop
```

Because every hop *reassigned* `tasks[req]`, the `unique_ptr` destroyed the **previous,
still-executing** sub-request — from inside that sub-request's own completion callback. That is a
use-after-free; during teardown (when the worker thread is also being torn down) it became a hard
crash. The same single-slot pattern is present **unchanged in current upstream `main`**.

## The fix

Hold the whole chain alive for the lifetime of the logical request, and release it together on
cancel/teardown:

```cpp
std::map<AsyncRequest*, std::vector<std::unique_ptr<AsyncRequest>>> tasks;  // a chain, not a slot
...
tasks[req].push_back(getFileSource()->request(resource, [=](const Response&){ ... }));  // append
...
void cancel(AsyncRequest* req) { tasks.erase(req); }   // release the chain together
```

and wire cancellation in `PMTilesFileSource::request`:

```cpp
req->onCancel([actorRef = thread->actor(), req = req.get()]{ actorRef.invoke(&Impl::cancel, req); });
```

Bundled hardening in the same commit:
- pass the parsed directory **through** the callback (new `AsyncDirectoryCallback`) instead of
  re-`.at()`-ing a cache entry that may have been evicted;
- null/short-data guards on every sub-response before dereferencing;
- by-value `url` capture in the async lambdas;
- `return` after cache-hit callbacks in `getHeader`/`getMetadata` (the old code fell through and
  issued a redundant network request).

## Regression tests

`test/storage/pmtiles_file_source.test.cpp`:

- **`ConcurrentTileRequests`** — fire several overlapping tile-request chains; all must complete cleanly.
- **`DestroyDuringRequest`** — destroy the file source while requests are in flight; must not crash.

A use-after-free is undefined behaviour and may not crash a plain build, so run these under
**AddressSanitizer** to make the regression deterministic.

## Build the iOS xcframework

Toolchain: Bazel (auto-fetched per `.bazelversion`, currently 8.5.0) + Xcode 16+. ~7 min on Apple silicon.

```bash
# 1. Submodules ARE required — analysis fails with
#    "no such package 'vendor/maplibre-tile-spec/cpp'" otherwise.
#    (Most deps come via bzlmod, but a few are git submodules with their own BUILD files.)
git submodule update --init --recursive --depth 1 --jobs 4 vendor
npm install --ignore-scripts          # node-driven codegen the Bazel build invokes

# 2. Build (--//:renderer=metal is REQUIRED for iOS).
bazel build //platform/ios:MapLibre.dynamic \
  --//:renderer=metal --compilation_mode=opt --copt=-g --copt=-Oz --strip=never

# 3. Output is a zipped xcframework (ios-arm64 + ios-arm64_x86_64-simulator):
ls bazel-bin/platform/ios/MapLibre.dynamic.xcframework.zip
swift package compute-checksum bazel-bin/platform/ios/MapLibre.dynamic.xcframework.zip
```

Publish it as a SwiftPM distribution (a GitHub release asset whose URL + checksum go into a
`Package.swift` `.binaryTarget`) — see the distribution repo's README.

## Upstreaming

The fix is a small, self-contained commit meant for upstream. To open a PR against
maplibre/maplibre-native, branch off `main` and `git cherry-pick 9a74bb23` (re-resolve against
the current `pmtiles_file_source.cpp` if it has drifted — the logic maps directly). This
Traska-specific doc is a separate commit and should be left out of the upstream PR.
