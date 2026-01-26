#!/usr/bin/env bash
#
# build-production.sh — Local production build for MystralNative demos
#
# Mirrors the GitHub Actions sponza.yml workflow so you can test production
# builds locally before pushing.
#
# Usage:
#   ./scripts/build-production.sh                     # Build Sponza demo (default)
#   ./scripts/build-production.sh --demo sponza       # Same as above
#   ./scripts/build-production.sh --demo helmet       # Build DamagedHelmet demo
#   ./scripts/build-production.sh --skip-build        # Skip cmake, use existing binary
#   ./scripts/build-production.sh --skip-strip        # Don't strip binary (keep debug symbols)
#   ./scripts/build-production.sh --no-app            # Don't create .app bundle (just compile)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

# --- Defaults ---
DEMO="sponza"
SKIP_BUILD=false
SKIP_STRIP=false
NO_APP=false
BUILD_TYPE="Release"
OUTPUT_DIR="dist"

# --- Parse arguments ---
while [[ $# -gt 0 ]]; do
    case "$1" in
        --demo)         DEMO="$2"; shift 2 ;;
        --skip-build)   SKIP_BUILD=true; shift ;;
        --skip-strip)   SKIP_STRIP=true; shift ;;
        --no-app)       NO_APP=true; shift ;;
        --output)       OUTPUT_DIR="$2"; shift 2 ;;
        --debug)        BUILD_TYPE="Debug"; SKIP_STRIP=true; shift ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --demo <name>     Demo to build: sponza (default), helmet"
            echo "  --skip-build      Skip cmake build, use existing binary in build/"
            echo "  --skip-strip      Don't strip the binary (preserves debug symbols)"
            echo "  --no-app          Don't create .app bundle, just compile the demo"
            echo "  --output <dir>    Output directory (default: dist)"
            echo "  --debug           Use Debug build type (implies --skip-strip)"
            echo ""
            echo "Examples:"
            echo "  $0                                  # Full production build of Sponza"
            echo "  $0 --skip-build                     # Repackage with existing binary"
            echo "  $0 --demo helmet                    # Build DamagedHelmet demo"
            echo "  $0 --no-app                         # Just compile, don't package .app"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Run $0 --help for usage"
            exit 1
            ;;
    esac
done

# --- Demo configuration ---
# Each demo defines: entry script, required assets, display name, and assets to REMOVE
ALL_ASSETS=("DamagedHelmet.glb" "environment.hdr" "Sponza.glb")
DRACO_WASM_FILES=("draco_decoder.js" "draco_decoder.wasm" "draco-worker.js")

case "$DEMO" in
    sponza)
        DEMO_NAME="Sponza"
        DEMO_ENTRY="examples/sponza.js"
        KEEP_ASSETS=("Sponza.glb")
        BUNDLE_ID="com.mystralengine.sponza"
        ;;
    helmet)
        DEMO_NAME="DamagedHelmet"
        DEMO_ENTRY="examples/mystral-helmet.js"
        KEEP_ASSETS=("DamagedHelmet.glb" "environment.hdr")
        BUNDLE_ID="com.mystralengine.damagedhelmet"
        ;;
    *)
        echo "Error: Unknown demo '$DEMO'. Available: sponza, helmet"
        exit 1
        ;;
esac

echo "=== MystralNative Production Build ==="
echo "Demo:       $DEMO_NAME"
echo "Entry:      $DEMO_ENTRY"
echo "Build type: $BUILD_TYPE"
echo ""

# --- Step 1: Build ---
if [ "$SKIP_BUILD" = false ]; then
    echo "--- Step 1: CMake Build ($BUILD_TYPE) ---"

    # Detect available features
    CMAKE_EXTRA_ARGS=""
    if [ -d "third_party/v8" ]; then
        CMAKE_EXTRA_ARGS="$CMAKE_EXTRA_ARGS -DMYSTRAL_USE_V8=ON -DMYSTRAL_USE_QUICKJS=OFF"
        echo "  V8: ON"
    else
        echo "  V8: OFF (using QuickJS)"
    fi
    if [ -d "third_party/dawn" ]; then
        CMAKE_EXTRA_ARGS="$CMAKE_EXTRA_ARGS -DMYSTRAL_USE_DAWN=ON -DMYSTRAL_USE_WGPU=OFF"
        echo "  Dawn: ON"
    fi
    if [ -d "third_party/draco" ]; then
        CMAKE_EXTRA_ARGS="$CMAKE_EXTRA_ARGS -DMYSTRAL_USE_DRACO=ON"
        echo "  Draco (native): ON"
    fi

    cmake -B build -DCMAKE_BUILD_TYPE=$BUILD_TYPE $CMAKE_EXTRA_ARGS
    cmake --build build --config $BUILD_TYPE -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

    echo "  Binary: $(du -h build/mystral | awk '{print $1}')"
else
    echo "--- Step 1: Build (skipped) ---"
    if [ ! -f "build/mystral" ]; then
        echo "Error: build/mystral not found. Run without --skip-build first."
        exit 1
    fi
fi

# Verify build
./build/mystral --version || echo "(version check not supported)"

# --- Step 2: Strip binary ---
if [ "$SKIP_STRIP" = false ]; then
    echo ""
    echo "--- Step 2: Strip binary ---"
    BINARY_SIZE_BEFORE=$(du -h build/mystral | awk '{print $1}')
    cp build/mystral build/mystral-debug  # Keep debug copy
    strip build/mystral
    BINARY_SIZE_AFTER=$(du -h build/mystral | awk '{print $1}')
    echo "  Before: $BINARY_SIZE_BEFORE -> After: $BINARY_SIZE_AFTER"
    echo "  Debug copy saved as build/mystral-debug"
else
    echo ""
    echo "--- Step 2: Strip (skipped) ---"
fi

# --- Step 3: Prepare assets (in-place, restored after compile) ---
echo ""
echo "--- Step 3: Prepare assets ---"

# Track files we remove so we can restore them
REMOVED_FILES=()

# Remove assets NOT needed for this demo
for asset in "${ALL_ASSETS[@]}"; do
    KEEP=false
    for keep_asset in "${KEEP_ASSETS[@]}"; do
        if [ "$asset" = "$keep_asset" ]; then
            KEEP=true
            break
        fi
    done
    if [ "$KEEP" = false ] && [ -f "examples/assets/$asset" ]; then
        echo "  - examples/assets/$asset (not needed)"
        REMOVED_FILES+=("examples/assets/$asset")
        rm -f "examples/assets/$asset"
    fi
done

# Check if native Draco decoder is compiled in
HAS_NATIVE_DRACO=false
if [ -f "build/CMakeCache.txt" ] && grep -q "MYSTRAL_USE_DRACO:BOOL=ON" "build/CMakeCache.txt"; then
    HAS_NATIVE_DRACO=true
fi

# Remove WASM Draco decoder files if native decoder is available
if [ "$HAS_NATIVE_DRACO" = true ]; then
    echo ""
    echo "  Native Draco decoder compiled in — removing WASM decoder files:"
    for f in "${DRACO_WASM_FILES[@]}"; do
        if [ -f "examples/assets/$f" ]; then
            echo "  - examples/assets/$f"
            REMOVED_FILES+=("examples/assets/$f")
            rm -f "examples/assets/$f"
        fi
    done
else
    echo ""
    echo "  No native Draco decoder — keeping WASM decoder fallback"
fi

# Show what remains
echo ""
echo "  Remaining assets:"
ls -lh examples/assets/ 2>/dev/null || echo "  (empty)"

# Restore removed files on exit (even on failure)
restore_assets() {
    if [ ${#REMOVED_FILES[@]} -gt 0 ]; then
        echo ""
        echo "--- Restoring removed assets ---"
        # Use git checkout to restore files from HEAD
        for f in "${REMOVED_FILES[@]}"; do
            git checkout HEAD -- "$f" 2>/dev/null || true
        done
        echo "  Restored ${#REMOVED_FILES[@]} files"
    fi
}
trap restore_assets EXIT

# --- Step 4: Compile bundle ---
echo ""
echo "--- Step 4: Compile standalone bundle ---"

# Build the compile command with only the necessary --include flags
COMPILE_ARGS=("compile" "$DEMO_ENTRY"
    "--include" "examples/assets"
    "--bundle-only"
    "--output" "build/${DEMO}.bundle")

# Include draco WASM decoder only if native decoder is NOT available
if [ "$HAS_NATIVE_DRACO" = false ] && [ -d "draco" ]; then
    COMPILE_ARGS+=("--include" "draco")
fi

./build/mystral "${COMPILE_ARGS[@]}"

BUNDLE_SIZE=$(du -h "build/${DEMO}.bundle" | awk '{print $1}')
echo "  Bundle: build/${DEMO}.bundle ($BUNDLE_SIZE)"

# --- Step 5: Package .app ---
if [ "$NO_APP" = false ] && [ "$(uname)" = "Darwin" ]; then
    echo ""
    echo "--- Step 5: Create macOS .app bundle ---"

    mkdir -p "$OUTPUT_DIR"

    ./scripts/package-app.sh \
        --binary build/mystral \
        --name "$DEMO_NAME" \
        --bundle "build/${DEMO}.bundle" \
        --output "$OUTPUT_DIR" \
        --bundle-id "$BUNDLE_ID" \
        --version "1.0.0"

    APP_SIZE=$(du -sh "$OUTPUT_DIR/$DEMO_NAME.app" | awk '{print $1}')

    # Also create a zip for distribution
    echo ""
    echo "--- Step 6: Create distributable zip ---"
    (cd "$OUTPUT_DIR" && zip -r "${DEMO_NAME}-macOS-$(uname -m).zip" "${DEMO_NAME}.app")
    ZIP_SIZE=$(du -h "$OUTPUT_DIR/${DEMO_NAME}-macOS-$(uname -m).zip" | awk '{print $1}')

    echo ""
    echo "=== Production Build Complete ==="
    echo ""
    echo "  App:     $OUTPUT_DIR/$DEMO_NAME.app ($APP_SIZE)"
    echo "  Zip:     $OUTPUT_DIR/${DEMO_NAME}-macOS-$(uname -m).zip ($ZIP_SIZE)"
    echo "  Binary:  $(du -h build/mystral | awk '{print $1}') (stripped)"
    echo "  Bundle:  $BUNDLE_SIZE"
    echo ""
    echo "Run:  open $OUTPUT_DIR/$DEMO_NAME.app"
elif [ "$NO_APP" = true ]; then
    echo ""
    echo "=== Production Build Complete (no .app) ==="
    echo ""
    echo "  Binary: $(du -h build/mystral | awk '{print $1}')"
    echo "  Bundle: build/${DEMO}.bundle ($BUNDLE_SIZE)"
else
    echo ""
    echo "=== Production Build Complete (non-macOS) ==="
    echo ""
    echo "  Binary: $(du -h build/mystral | awk '{print $1}')"
    echo "  Bundle: build/${DEMO}.bundle ($BUNDLE_SIZE)"
    echo ""
    echo "Run:  ./build/mystral run-bundle build/${DEMO}.bundle"
fi
