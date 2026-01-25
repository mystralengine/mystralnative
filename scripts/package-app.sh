#!/usr/bin/env bash
#
# package-app.sh — Create a macOS .app bundle from a MystralNative binary
#
# Two modes:
#
# 1. Compiled binary mode (default):
#    ./scripts/package-app.sh --binary build/sponza --name Sponza --output dist
#    Note: Compiled binaries have appended bundle data that prevents code signing.
#
# 2. Resources mode (recommended for signed .app):
#    ./scripts/package-app.sh --binary build/mystral --name Sponza \
#      --script examples/sponza.js --resources examples/assets --output dist
#    The regular mystral binary is clean Mach-O and can be properly signed.
#
# Code Signing:
#   By default, uses ad-hoc signing (works on your own machine, triggers Gatekeeper).
#   For distribution, provide an Apple Developer Team ID:
#     --team-id <id>           Apple Developer Team ID (or set APPLE_TEAM_ID env var)
#     --sign-identity <id>     Full signing identity (or set APPLE_SIGNING_IDENTITY env var)
#                              If only --team-id is given, identity is auto-detected from keychain.
#     --entitlements <plist>   Optional entitlements file
#     --no-sign                Skip code signing entirely
#
set -euo pipefail

# Defaults
BINARY_PATH=""
APP_NAME=""
OUTPUT_DIR="."
BUNDLE_ID=""
VERSION="1.0.0"
ICON_PATH=""
TEAM_ID="${APPLE_TEAM_ID:-}"
SIGN_IDENTITY="${APPLE_SIGNING_IDENTITY:-}"
ENTITLEMENTS=""
DO_SIGN="true"
SCRIPT_PATH=""
RESOURCE_DIRS=()

usage() {
    echo "Usage: $0 --binary <path> --name <name> [options]"
    echo ""
    echo "Required:"
    echo "  --binary <path>         Path to MystralNative binary"
    echo "  --name <name>           App display name (e.g. 'Sponza')"
    echo ""
    echo "Resources Mode (recommended for signed .app):"
    echo "  --script <path>         Entry point JS file (enables resources mode)"
    echo "  --resources <dir>       Directory to include in Resources (repeatable)"
    echo ""
    echo "Options:"
    echo "  --output <dir>          Output directory (default: .)"
    echo "  --bundle-id <id>        Bundle identifier (default: com.mystralengine.<name>)"
    echo "  --version <ver>         Version string (default: 1.0.0)"
    echo "  --icon <icns>           Path to .icns icon file"
    echo ""
    echo "Code Signing:"
    echo "  --team-id <id>          Apple Developer Team ID (or APPLE_TEAM_ID env var)"
    echo "  --sign-identity <id>    Signing identity (or APPLE_SIGNING_IDENTITY env var)"
    echo "  --entitlements <plist>  Entitlements file for signing"
    echo "  --no-sign               Skip code signing"
    echo ""
    echo "Environment Variables:"
    echo "  APPLE_TEAM_ID           Apple Developer Team ID"
    echo "  APPLE_SIGNING_IDENTITY  Full signing identity string"
    echo ""
    echo "Examples:"
    echo "  # Compiled binary (no signing):"
    echo "  $0 --binary build/sponza --name Sponza --output dist"
    echo ""
    echo "  # Resources mode (proper signing):"
    echo "  $0 --binary build/mystral --name Sponza \\"
    echo "     --script examples/sponza.js --resources examples/assets --output dist"
    exit 1
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)       BINARY_PATH="$2"; shift 2 ;;
        --name)         APP_NAME="$2"; shift 2 ;;
        --output)       OUTPUT_DIR="$2"; shift 2 ;;
        --bundle-id)    BUNDLE_ID="$2"; shift 2 ;;
        --version)      VERSION="$2"; shift 2 ;;
        --icon)         ICON_PATH="$2"; shift 2 ;;
        --team-id)      TEAM_ID="$2"; shift 2 ;;
        --sign-identity) SIGN_IDENTITY="$2"; shift 2 ;;
        --entitlements) ENTITLEMENTS="$2"; shift 2 ;;
        --no-sign)      DO_SIGN="false"; shift ;;
        --script)       SCRIPT_PATH="$2"; shift 2 ;;
        --resources)    RESOURCE_DIRS+=("$2"); shift 2 ;;
        --help|-h)      usage ;;
        *)              echo "Unknown option: $1"; usage ;;
    esac
done

# Validate required args
if [ -z "$BINARY_PATH" ] || [ -z "$APP_NAME" ]; then
    echo "Error: --binary and --name are required"
    usage
fi

if [ ! -f "$BINARY_PATH" ]; then
    echo "Error: Binary not found: $BINARY_PATH"
    exit 1
fi

# Determine mode
RESOURCES_MODE="false"
if [ -n "$SCRIPT_PATH" ]; then
    RESOURCES_MODE="true"
    if [ ! -f "$SCRIPT_PATH" ]; then
        echo "Error: Script not found: $SCRIPT_PATH"
        exit 1
    fi
fi

# Derive lowercase name for executable and bundle ID
APP_NAME_LOWER=$(echo "$APP_NAME" | tr '[:upper:]' '[:lower:]')

if [ -z "$BUNDLE_ID" ]; then
    BUNDLE_ID="com.mystralengine.${APP_NAME_LOWER}"
fi

# Resolve signing identity
resolve_signing_identity() {
    # If explicit identity is set, use it
    if [ -n "$SIGN_IDENTITY" ]; then
        echo "$SIGN_IDENTITY"
        return
    fi

    # If team ID is set, find the Developer ID Application certificate
    if [ -n "$TEAM_ID" ]; then
        # Search keychain for Developer ID Application certificate matching team ID
        local found_identity
        found_identity=$(security find-identity -v -p codesigning 2>/dev/null | \
            grep "Developer ID Application" | \
            grep "$TEAM_ID" | \
            head -1 | \
            sed 's/.*"\(.*\)".*/\1/' || true)

        if [ -n "$found_identity" ]; then
            echo "$found_identity"
            return
        fi

        # Try Apple Development certificate (for local testing)
        found_identity=$(security find-identity -v -p codesigning 2>/dev/null | \
            grep "Apple Development" | \
            grep "$TEAM_ID" | \
            head -1 | \
            sed 's/.*"\(.*\)".*/\1/' || true)

        if [ -n "$found_identity" ]; then
            echo "$found_identity"
            return
        fi

        echo "Warning: No signing certificate found for team ID: $TEAM_ID" >&2
        echo "Falling back to ad-hoc signing" >&2
    fi

    # Fallback: ad-hoc
    echo "-"
}

# Create .app structure
APP_DIR="${OUTPUT_DIR}/${APP_NAME}.app"
rm -rf "$APP_DIR"
mkdir -p "${APP_DIR}/Contents/MacOS"
mkdir -p "${APP_DIR}/Contents/Resources"

if [ "$RESOURCES_MODE" = "true" ]; then
    echo "Creating ${APP_NAME}.app (resources mode)..."

    # Copy the mystral runtime binary
    cp "$BINARY_PATH" "${APP_DIR}/Contents/MacOS/mystral"
    chmod +x "${APP_DIR}/Contents/MacOS/mystral"
    echo "  Runtime: mystral ($(du -sh "${APP_DIR}/Contents/MacOS/mystral" | awk '{print $1}'))"

    # Copy script to Resources, preserving directory structure
    SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
    mkdir -p "${APP_DIR}/Contents/Resources/${SCRIPT_DIR}"
    cp "$SCRIPT_PATH" "${APP_DIR}/Contents/Resources/${SCRIPT_PATH}"
    echo "  Script: ${SCRIPT_PATH}"

    # Copy resource directories to Resources, preserving directory structure
    for RES_DIR in "${RESOURCE_DIRS[@]}"; do
        if [ -d "$RES_DIR" ]; then
            # Preserve the directory structure relative to project root
            mkdir -p "${APP_DIR}/Contents/Resources/${RES_DIR}"
            cp -R "${RES_DIR}/." "${APP_DIR}/Contents/Resources/${RES_DIR}/"
            RES_SIZE=$(du -sh "$RES_DIR" | awk '{print $1}')
            echo "  Resources: ${RES_DIR} (${RES_SIZE})"
        elif [ -f "$RES_DIR" ]; then
            RES_PARENT=$(dirname "$RES_DIR")
            mkdir -p "${APP_DIR}/Contents/Resources/${RES_PARENT}"
            cp "$RES_DIR" "${APP_DIR}/Contents/Resources/${RES_DIR}"
            echo "  Resource: ${RES_DIR}"
        else
            echo "  Warning: Resource not found: ${RES_DIR}"
        fi
    done

    # Create launcher script
    # This sets CWD to Resources/ so relative paths in the JS code resolve correctly
    cat > "${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}" << 'LAUNCHER_HEADER'
#!/bin/bash
# MystralNative App Launcher
# Sets working directory to Resources for correct asset path resolution
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR/../Resources"
LAUNCHER_HEADER
    echo "exec \"\$DIR/mystral\" run \"${SCRIPT_PATH}\" \"\$@\"" >> "${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}"
    chmod +x "${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}"
    echo "  Launcher: ${APP_NAME_LOWER} -> mystral run ${SCRIPT_PATH}"

else
    echo "Creating ${APP_NAME}.app (compiled binary mode)..."

    # Copy compiled binary directly
    cp "$BINARY_PATH" "${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}"
    chmod +x "${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}"
    echo "  Binary: ${APP_NAME_LOWER} ($(du -sh "${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}" | awk '{print $1}'))"
fi

# Generate Info.plist
ICON_ENTRY=""
if [ -n "$ICON_PATH" ] && [ -f "$ICON_PATH" ]; then
    ICON_ENTRY="    <key>CFBundleIconFile</key>
    <string>AppIcon</string>"
fi

cat > "${APP_DIR}/Contents/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME_LOWER}</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${APP_NAME}</string>
    <key>CFBundleVersion</key>
    <string>${VERSION}</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleSignature</key>
    <string>????</string>
${ICON_ENTRY}
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
</dict>
</plist>
PLIST

echo "  Info.plist generated (bundle-id: ${BUNDLE_ID})"

# Copy icon if provided
if [ -n "$ICON_PATH" ] && [ -f "$ICON_PATH" ]; then
    cp "$ICON_PATH" "${APP_DIR}/Contents/Resources/AppIcon.icns"
    echo "  Icon: ${ICON_PATH}"
fi

# Check for dynamic library dependencies that need bundling
# In resources mode, check the mystral binary; in compiled mode, check the app binary
if [ "$RESOURCES_MODE" = "true" ]; then
    CHECK_BINARY="${APP_DIR}/Contents/MacOS/mystral"
else
    CHECK_BINARY="${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}"
fi

DYLIBS=$(otool -L "$CHECK_BINARY" 2>/dev/null | \
    tail -n +2 | \
    awk '{print $1}' | \
    grep -v '/usr/lib/' | \
    grep -v '/System/' | \
    grep -v '@rpath' | \
    grep -v '@executable_path' || true)

if [ -n "$DYLIBS" ]; then
    mkdir -p "${APP_DIR}/Contents/Frameworks"
    echo "  Bundling dynamic libraries:"
    while IFS= read -r dylib; do
        if [ -f "$dylib" ]; then
            echo "    - $(basename "$dylib")"
            cp "$dylib" "${APP_DIR}/Contents/Frameworks/"
            install_name_tool -change "$dylib" \
                "@executable_path/../Frameworks/$(basename "$dylib")" \
                "$CHECK_BINARY"
        fi
    done <<< "$DYLIBS"
fi

# Code signing
if [ "$DO_SIGN" = "true" ]; then
    IDENTITY=$(resolve_signing_identity)

    if [ "$RESOURCES_MODE" = "true" ]; then
        # Resources mode: clean Mach-O binary, proper signing works
        if [ "$IDENTITY" = "-" ]; then
            echo "  Signing: ad-hoc"
            codesign --force --sign - "${APP_DIR}/Contents/MacOS/mystral"
            codesign --force --sign - "$APP_DIR"
            echo "  Ad-hoc signature applied"
        else
            echo "  Signing: ${IDENTITY}"
            SIGN_ARGS=(--force --sign "$IDENTITY" --options runtime --timestamp)

            if [ -n "$ENTITLEMENTS" ] && [ -f "$ENTITLEMENTS" ]; then
                SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
                echo "  Entitlements: ${ENTITLEMENTS}"
            fi

            codesign "${SIGN_ARGS[@]}" "${APP_DIR}/Contents/MacOS/mystral"
            codesign "${SIGN_ARGS[@]}" "$APP_DIR"
            echo "  Developer ID signature applied"
        fi

        # Verify signature
        if codesign --verify --deep --strict "$APP_DIR" 2>/dev/null; then
            echo "  Signature verified (strict)"
        elif codesign --verify --deep "$APP_DIR" 2>/dev/null; then
            echo "  Signature verified"
        else
            echo "  Warning: Signature verification failed"
        fi
    else
        # Compiled binary mode: appended data breaks codesign
        # Try signing but handle failures gracefully
        if [ "$IDENTITY" = "-" ]; then
            echo "  Signing: ad-hoc"
            if codesign --force --sign - "${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}" 2>&1; then
                codesign --force --sign - "$APP_DIR" 2>&1 || true
                echo "  Ad-hoc signature applied"
            else
                echo "  Warning: Ad-hoc signing failed (compiled binaries have appended data)"
                echo "  The .app will still work - bypass Gatekeeper with right-click > Open"
            fi
        else
            echo "  Signing: ${IDENTITY}"
            SIGN_ARGS=(--force --sign "$IDENTITY" --options runtime --timestamp)

            if [ -n "$ENTITLEMENTS" ] && [ -f "$ENTITLEMENTS" ]; then
                SIGN_ARGS+=(--entitlements "$ENTITLEMENTS")
                echo "  Entitlements: ${ENTITLEMENTS}"
            fi

            if codesign "${SIGN_ARGS[@]}" "${APP_DIR}/Contents/MacOS/${APP_NAME_LOWER}" 2>&1; then
                codesign "${SIGN_ARGS[@]}" "$APP_DIR" 2>&1 || true
                echo "  Developer ID signature applied"
            else
                echo "  Warning: Developer ID signing failed (compiled binaries have appended data)"
                echo "  The .app will still work - bypass Gatekeeper with right-click > Open"
            fi
        fi

        # Verify
        if codesign --verify --deep "$APP_DIR" 2>/dev/null; then
            echo "  Signature verified"
        else
            echo "  Note: Signature verification skipped (compiled binary with embedded bundle)"
        fi
    fi
else
    echo "  Signing: skipped"
fi

# Print summary
APP_SIZE=$(du -sh "$APP_DIR" | awk '{print $1}')
echo ""
echo "Created: ${APP_DIR} (${APP_SIZE})"
