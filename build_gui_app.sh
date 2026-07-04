#!/bin/bash
set -euo pipefail

# Usage: ./build_gui_app.sh [--debug|--release] [--install|--no-install]
#   --release      (default) optimized build; slower to compile
#   --debug        fast incremental build for local iteration
#   --install, -y  install to /Applications without prompting
#   --no-install   skip the /Applications install prompt
# When run interactively without an install flag, the script asks whether to
# install to /Applications after a successful build.
CONFIG="release"
INSTALL_CHOICE="prompt"
for arg in "$@"; do
    case "$arg" in
        --debug) CONFIG="debug" ;;
        --release) CONFIG="release" ;;
        --install|-y) INSTALL_CHOICE="yes" ;;
        --no-install) INSTALL_CHOICE="no" ;;
        -h|--help)
            echo "Usage: $0 [--debug|--release] [--install|--no-install]"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

if [ "$CONFIG" = "release" ]; then
    echo "Building CamelPlayerGUI (release, optimized — this can take a few minutes)..."
else
    echo "Building CamelPlayerGUI (debug)..."
fi

swift build -c "$CONFIG" --product CamelPlayerGUI

# Locate the built binary instead of hardcoding the path
BIN_PATH="$(swift build -c "$CONFIG" --product CamelPlayerGUI --show-bin-path 2>/dev/null)"

# Create .app bundle structure
APP_NAME="CamelPlayer.app"
APP_DIR="$APP_NAME/Contents"
MACOS_DIR="$APP_DIR/MacOS"
RESOURCES_DIR="$APP_DIR/Resources"

echo "Creating app bundle structure..."
rm -rf "$APP_NAME"
mkdir -p "$MACOS_DIR"
mkdir -p "$RESOURCES_DIR"

# Copy the executable
echo "Copying executable..."
cp "$BIN_PATH/CamelPlayerGUI" "$MACOS_DIR/CamelPlayer"

# Generate app icon from logo.jpg, caching the .icns so it is only regenerated
# when logo.jpg changes (the bundle is recreated on every run).
ICON_CACHE=".build/AppIcon.icns"
if [ -f "logo.jpg" ]; then
    if [ ! -f "$ICON_CACHE" ] || [ "logo.jpg" -nt "$ICON_CACHE" ]; then
        echo "Generating app icon..."
        ICONSET="$(mktemp -d)/AppIcon.iconset"
        mkdir -p "$ICONSET"

        # -s format png: iconutil needs real PNGs, not JPEG data in .png names.
        sips -s format png -z 16 16     logo.jpg --out "$ICONSET/icon_16x16.png" > /dev/null 2>&1
        sips -s format png -z 32 32     logo.jpg --out "$ICONSET/icon_16x16@2x.png" > /dev/null 2>&1
        sips -s format png -z 32 32     logo.jpg --out "$ICONSET/icon_32x32.png" > /dev/null 2>&1
        sips -s format png -z 64 64     logo.jpg --out "$ICONSET/icon_32x32@2x.png" > /dev/null 2>&1
        sips -s format png -z 128 128   logo.jpg --out "$ICONSET/icon_128x128.png" > /dev/null 2>&1
        sips -s format png -z 256 256   logo.jpg --out "$ICONSET/icon_128x128@2x.png" > /dev/null 2>&1
        sips -s format png -z 256 256   logo.jpg --out "$ICONSET/icon_256x256.png" > /dev/null 2>&1
        sips -s format png -z 512 512   logo.jpg --out "$ICONSET/icon_256x256@2x.png" > /dev/null 2>&1
        sips -s format png -z 512 512   logo.jpg --out "$ICONSET/icon_512x512.png" > /dev/null 2>&1
        sips -s format png -z 1024 1024 logo.jpg --out "$ICONSET/icon_512x512@2x.png" > /dev/null 2>&1

        iconutil -c icns "$ICONSET" -o "$ICON_CACHE"
        rm -rf "$ICONSET"
    else
        echo "Reusing cached app icon..."
    fi
    cp "$ICON_CACHE" "$RESOURCES_DIR/AppIcon.icns"
    echo "✅ App icon ready"
else
    echo "⚠️  logo.jpg not found, skipping icon generation"
fi

# Derive version metadata from git when available
SHORT_VERSION="$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || true)"
SHORT_VERSION="${SHORT_VERSION:-1.0.0}"
BUILD_NUMBER="$(git rev-list --count HEAD 2>/dev/null || echo 1)"
YEAR="$(date +%Y)"

# Create Info.plist
echo "Creating Info.plist..."
cat > "$APP_DIR/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>CamelPlayer</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>CFBundleIdentifier</key>
    <string>com.camelplayer.gui</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>CamelPlayer</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$SHORT_VERSION</string>
    <key>CFBundleVersion</key>
    <string>$BUILD_NUMBER</string>
    <key>LSMinimumSystemVersion</key>
    <string>12.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSHumanReadableCopyright</key>
    <string>Copyright © $YEAR</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.music</string>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
EOF

# Make executable
chmod +x "$MACOS_DIR/CamelPlayer"

# Re-sign the assembled bundle (ad-hoc). Modifying the bundle invalidates the
# binary's original signature, and its identifier did not match the bundle id.
echo "Signing app bundle..."
codesign --force --sign - "$APP_NAME"

echo "✅ App bundle created successfully: $APP_NAME"

# Optionally install to /Applications, prompting first (with an extra warning
# when an existing copy would be replaced).
DEST="/Applications/$APP_NAME"
do_install="$INSTALL_CHOICE"

if [ "$do_install" = "prompt" ]; then
    if [ ! -t 0 ]; then
        # Non-interactive: don't hang waiting for input.
        echo "ℹ️  Not running interactively; skipping /Applications install."
        echo "    Install manually with: cp -R $APP_NAME /Applications/"
        do_install="no"
    else
        if [ -d "$DEST" ]; then
            echo "⚠️  $DEST already exists and will be replaced."
            read -r -p "Replace the existing app in /Applications? [y/N] " ans || ans=""
        else
            read -r -p "Install to /Applications? [y/N] " ans || ans=""
        fi
        case "$ans" in
            y|Y|yes|YES) do_install="yes" ;;
            *) do_install="no" ;;
        esac
    fi
fi

if [ "$do_install" = "yes" ]; then
    echo "Installing to /Applications..."
    if rm -rf "$DEST" 2>/dev/null && cp -R "$APP_NAME" "$DEST" 2>/dev/null; then
        echo "✅ Installed: $DEST"
    else
        echo "⚠️  Could not write to /Applications (permission denied?)."
        echo "    Try: sudo cp -R $APP_NAME /Applications/"
    fi
else
    echo ""
    echo "To run the app:        open $APP_NAME"
    echo "To install manually:   cp -R $APP_NAME /Applications/"
fi
