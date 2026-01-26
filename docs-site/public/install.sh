#!/bin/bash
#
# MystralNative Installer
# https://mystralengine.github.io/mystralnative/
#
# Usage: curl -fsSL https://mystralengine.github.io/mystralnative/install.sh | bash
#

set -e

REPO="mystralengine/mystralnative"
INSTALL_DIR="${MYSTRAL_INSTALL_DIR:-$HOME/.mystral}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_banner() {
    echo -e "${BLUE}"
    echo "  __  __           _             _ "
    echo " |  \/  |_   _ ___| |_ _ __ __ _| |"
    echo " | |\/| | | | / __| __| '__/ _\` | |"
    echo " | |  | | |_| \__ \ |_| | | (_| | |"
    echo " |_|  |_|\__, |___/\__|_|  \__,_|_|"
    echo "         |___/              Native"
    echo -e "${NC}"
}

info() {
    echo -e "${BLUE}==>${NC} $1"
}

success() {
    echo -e "${GREEN}==>${NC} $1"
}

warn() {
    echo -e "${YELLOW}Warning:${NC} $1"
}

error() {
    echo -e "${RED}Error:${NC} $1"
    exit 1
}

detect_platform() {
    local os=$(uname -s)
    local arch=$(uname -m)

    case "$os" in
        Darwin)
            OS="macOS"
            ;;
        Linux)
            OS="linux"
            ;;
        MINGW*|MSYS*|CYGWIN*)
            error "Please use PowerShell on Windows. See: https://mystralengine.github.io/mystralnative/docs/platforms/windows"
            ;;
        *)
            error "Unsupported operating system: $os"
            ;;
    esac

    case "$arch" in
        x86_64|amd64)
            ARCH="x64"
            ;;
        arm64|aarch64)
            ARCH="arm64"
            ;;
        *)
            error "Unsupported architecture: $arch"
            ;;
    esac

    PLATFORM="${OS}-${ARCH}"
    info "Detected platform: $PLATFORM"
}

get_latest_version() {
    info "Fetching latest release..."

    # Try to get the latest release tag
    if command -v curl &> /dev/null; then
        VERSION=$(curl -sL "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
    elif command -v wget &> /dev/null; then
        VERSION=$(wget -qO- "https://api.github.com/repos/$REPO/releases/latest" | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
    else
        error "Neither curl nor wget found. Please install one of them."
    fi

    if [ -z "$VERSION" ]; then
        error "Failed to fetch latest version. Please check your internet connection."
    fi

    info "Latest version: $VERSION"
}

select_build() {
    # Default to V8 + Dawn for best compatibility
    # Dawn is recommended for mystral-helmet and full Mystral Engine demos
    JS_ENGINE="v8"
    WEBGPU_BACKEND="dawn"

    BUILD_NAME="mystral-${PLATFORM}-${JS_ENGINE}-${WEBGPU_BACKEND}"
    DOWNLOAD_URL="https://github.com/$REPO/releases/download/$VERSION/${BUILD_NAME}.zip"

    info "Selected build: $BUILD_NAME"
}

download_and_install() {
    local tmpdir=$(mktemp -d)
    local zipfile="$tmpdir/mystral.zip"

    info "Downloading $BUILD_NAME..."

    if command -v curl &> /dev/null; then
        curl -fsSL "$DOWNLOAD_URL" -o "$zipfile" || error "Download failed. Check if the release exists: $DOWNLOAD_URL"
    else
        wget -q "$DOWNLOAD_URL" -O "$zipfile" || error "Download failed. Check if the release exists: $DOWNLOAD_URL"
    fi

    info "Extracting to $INSTALL_DIR..."

    # Create install directory
    mkdir -p "$INSTALL_DIR"

    # Extract
    if command -v unzip &> /dev/null; then
        unzip -q -o "$zipfile" -d "$tmpdir"
    else
        error "unzip is required but not installed."
    fi

    # Move files to install directory
    mv "$tmpdir/$BUILD_NAME"/* "$INSTALL_DIR/" 2>/dev/null || mv "$tmpdir"/*/* "$INSTALL_DIR/" 2>/dev/null || true

    # Make executable
    chmod +x "$INSTALL_DIR/mystral"

    # Cleanup
    rm -rf "$tmpdir"

    success "Installed to $INSTALL_DIR"
}

setup_path() {
    local shell_profile=""
    local shell_name=$(basename "$SHELL")

    case "$shell_name" in
        bash)
            if [ -f "$HOME/.bashrc" ]; then
                shell_profile="$HOME/.bashrc"
            elif [ -f "$HOME/.bash_profile" ]; then
                shell_profile="$HOME/.bash_profile"
            fi
            ;;
        zsh)
            shell_profile="$HOME/.zshrc"
            ;;
        fish)
            shell_profile="$HOME/.config/fish/config.fish"
            ;;
    esac

    if [ -n "$shell_profile" ]; then
        local path_line="export PATH=\"\$PATH:$INSTALL_DIR\""

        if [ "$shell_name" = "fish" ]; then
            path_line="set -gx PATH \$PATH $INSTALL_DIR"
        fi

        if ! grep -q "$INSTALL_DIR" "$shell_profile" 2>/dev/null; then
            echo "" >> "$shell_profile"
            echo "# MystralNative" >> "$shell_profile"
            echo "$path_line" >> "$shell_profile"
            info "Added $INSTALL_DIR to PATH in $shell_profile"
        else
            info "PATH already configured in $shell_profile"
        fi
    else
        warn "Could not detect shell profile. Add this to your shell config:"
        echo "  export PATH=\"\$PATH:$INSTALL_DIR\""
    fi
}

verify_installation() {
    if [ -x "$INSTALL_DIR/mystral" ]; then
        success "Installation complete!"
        echo ""
        echo "To use mystral in this terminal:"
        echo -e "  ${BLUE}export PATH=\"\$PATH:$INSTALL_DIR\"${NC}"
        echo ""
        echo "Or start a new terminal session."
        echo ""
        echo "Test your installation:"
        echo -e "  ${BLUE}mystral --version${NC}"
        echo ""
        echo "Run an example:"
        echo -e "  ${BLUE}cd $INSTALL_DIR && mystral run examples/triangle.js${NC}"
        echo ""
        echo -e "Documentation: ${BLUE}https://mystralengine.github.io/mystralnative/${NC}"
    else
        error "Installation failed. mystral binary not found."
    fi
}

main() {
    print_banner
    detect_platform
    get_latest_version
    select_build
    download_and_install
    setup_path
    verify_installation
}

main "$@"
