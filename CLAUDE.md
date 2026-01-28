# MystralNative - Claude Code Instructions

This document contains important instructions for AI assistants working on the MystralNative codebase.

## Critical Rules

### DO NOT Use Native GLTF/GLB Loaders

**NEVER** use or reference the C++ native GLTF/GLB loading code in this repository. The files exist but are deprecated and will cause compile errors if included:

- `src/gltf/gltf_loader.cpp` - DEPRECATED
- `src/utils/cgltf_impl.cpp` - DEPRECATED
- `include/mystral/gltf/gltf_loader.h` - DEPRECATED
- `third_party/cgltf/` - DEPRECATED

**Why?** MystralNative should use the **JavaScript GLBLoader/GLTFLoader** from the Mystral web engine (same code that runs in browsers). This ensures:

1. **Feature parity** with the web engine
2. **Consistent behavior** across platforms (web, desktop, mobile)
3. **Extension support** like Draco mesh compression (via WASM)
4. **Easier maintenance** - one codebase for all platforms

### How to Load GLTF/GLB Files

In your JavaScript/TypeScript code running on MystralNative, use:

```javascript
// For GLB files (binary GLTF)
import { loadGLBModel } from 'mystral';
const model = await loadGLBModel(device, './path/to/model.glb');

// For GLTF files (JSON + external resources)
import { loadGLTFModel } from 'mystral';
const model = await loadGLTFModel(device, './path/to/model.gltf');
```

These loaders are implemented in:
- `src/loaders/GLBLoader.ts` (main Mystral engine repo) - Supports both .glb and .gltf files
- `src/loaders/GLTFLoader.ts` - **DEPRECATED** (uses loaders.gl, adds ~500KB to bundle)

## Draco Compression Support

GLBLoader supports Draco-compressed meshes (`KHR_draco_mesh_compression`) with two decoder paths:

### Native C++ Decoder (MystralNative — Recommended)

When built with `-DMYSTRAL_USE_DRACO=ON` (the default when `third_party/draco/` exists), the native C++ Draco decoder runs on a **libuv thread pool thread** and bypasses WASM/Worker entirely. GLBLoader auto-detects this via `typeof __mystralNativeDecodeDracoAsync === 'function'`.

```bash
# Download prebuilt Draco library
node scripts/download-deps.mjs --only draco
```

No WASM files needed — the native decoder is compiled into the binary.

### WASM Fallback (Browser / No Native Draco)

For browser builds or MystralNative builds without Draco, copy the WASM decoder:
```bash
mkdir -p public/draco
curl -o public/draco/draco_decoder.js https://www.gstatic.com/draco/versioned/decoders/1.5.6/draco_decoder.js
curl -o public/draco/draco_decoder.wasm https://www.gstatic.com/draco/versioned/decoders/1.5.6/draco_decoder.wasm
```

Or use CDN (no setup):
```javascript
import { GLBLoader } from 'mystral';
const loader = new GLBLoader(device, { dracoDecoderPath: 'cdn' });
const result = await loader.load('./model.glb');
```

## Known Limitations

**loaders.gl removed**: We removed loaders.gl dependency to reduce bundle size by ~500KB. The GLBLoader now handles all GLTF/GLB loading natively.

## Build Configuration

The project uses CMake with several options:

```bash
# Recommended: V8 + Dawn for full shader compatibility
cmake -B build \
  -DMYSTRAL_USE_V8=ON \
  -DMYSTRAL_USE_DAWN=ON \
  -DMYSTRAL_USE_QUICKJS=OFF \
  -DMYSTRAL_USE_WGPU=OFF

cmake --build build --parallel
```

See `README.md` for full build options.

## Production Builds

Use the local production build script to build, strip, filter assets, and package a macOS `.app` bundle. This mirrors what the GitHub Actions `sponza.yml` workflow does in CI.

```bash
# Full production build of Sponza demo (default)
./scripts/build-production.sh

# Build DamagedHelmet demo instead
./scripts/build-production.sh --demo helmet

# Skip cmake build (repackage with existing binary)
./scripts/build-production.sh --skip-build

# Skip stripping (keep debug symbols)
./scripts/build-production.sh --skip-strip

# Just compile the bundle, no .app packaging
./scripts/build-production.sh --no-app
```

The script automatically:
- Detects V8/Dawn/Draco from `third_party/` and sets CMake flags
- Strips the binary (saves debug copy as `build/mystral-debug`)
- Filters assets: includes only files needed for the target demo
- Excludes WASM Draco decoder files when native C++ Draco is compiled in
- Restores all removed files via `git checkout` on exit (safe for working tree)
- Creates `.app` bundle + distributable `.zip` on macOS

## Releases

Releases are managed via GitHub Actions workflows. The `.github/` directory is synced from `internal_packages/mystralnative/.github/` to the public repo `mystralengine/mystralnative`. Edit workflows here and sync them.

### MystralNative Release (all platforms)

Triggered by pushing a `v*` tag to the public repo:

```bash
cd ~/Projects/github/mystralengine/mystralnative

# Create and push a version tag
git tag v0.1.0
git push origin v0.1.0
```

This triggers `release.yml` which builds all platform/engine/backend combinations (macOS, Linux, Windows x V8, QuickJS, JSC x Dawn, wgpu) and creates a GitHub Release with download artifacts.

Tags containing `alpha`, `beta`, or `rc` are marked as prerelease.

Alternatively, use the **CD workflow** (`cd.yml`) for auto-versioning:
- Go to Actions > CD > Run workflow
- Select version bump type: `patch`, `minor`, or `major`
- The workflow auto-increments `package.json`, creates the tag, builds, and releases

### Sponza Demo Release

Triggered by pushing a `sponza-v*` tag or manual dispatch:

```bash
cd ~/Projects/github/mystralengine/mystralnative

# Create and push a sponza release tag
git tag sponza-v1.1.0
git push origin sponza-v1.1.0
```

This triggers `sponza.yml` which builds the Sponza demo for macOS (arm64), Linux (x64), and Windows (x64) with stripped binaries and filtered assets.

Manual dispatch is also available via Actions > Sponza Demo Build > Run workflow.

### Release Checklist

1. Sync latest changes from private repo to public repo (see Repository Sync below)
2. Push tags to the public repo to trigger release workflows
3. Verify the GitHub Actions run succeeds and artifacts are published

## Repository Sync

This code syncs to the public repository `mystralengine/mystralnative`. See the main repo's `CLAUDE.md` for sync instructions.
