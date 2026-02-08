#!/bin/bash
set -e

# Build the GUI in release mode
echo "Building CamelPlayerGUI..."
swift build -c release --product CamelPlayerGUI

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
cp .build/release/CamelPlayerGUI "$MACOS_DIR/CamelPlayer"

# Generate app icon from logo.png
if [ -f "logo.png" ]; then
    echo "Generating app icon..."
    ICONSET="AppIcon.iconset"
    rm -rf "$ICONSET"
    mkdir -p "$ICONSET"

    # Generate different icon sizes
    sips -z 16 16     logo.png --out "$ICONSET/icon_16x16.png" > /dev/null 2>&1
    sips -z 32 32     logo.png --out "$ICONSET/icon_16x16@2x.png" > /dev/null 2>&1
    sips -z 32 32     logo.png --out "$ICONSET/icon_32x32.png" > /dev/null 2>&1
    sips -z 64 64     logo.png --out "$ICONSET/icon_32x32@2x.png" > /dev/null 2>&1
    sips -z 128 128   logo.png --out "$ICONSET/icon_128x128.png" > /dev/null 2>&1
    sips -z 256 256   logo.png --out "$ICONSET/icon_128x128@2x.png" > /dev/null 2>&1
    sips -z 256 256   logo.png --out "$ICONSET/icon_256x256.png" > /dev/null 2>&1
    sips -z 512 512   logo.png --out "$ICONSET/icon_256x256@2x.png" > /dev/null 2>&1
    sips -z 512 512   logo.png --out "$ICONSET/icon_512x512.png" > /dev/null 2>&1
    sips -z 1024 1024 logo.png --out "$ICONSET/icon_512x512@2x.png" > /dev/null 2>&1

    # Convert to .icns
    iconutil -c icns "$ICONSET" -o "$RESOURCES_DIR/AppIcon.icns"
    rm -rf "$ICONSET"
    echo "✅ App icon created"
else
    echo "⚠️  logo.png not found, skipping icon generation"
fi

# Create Info.plist
echo "Creating Info.plist..."
cat > "$APP_DIR/Info.plist" << 'EOF'
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
    <string>1.0.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>12.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSHumanReadableCopyright</key>
    <string>Copyright © 2025</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.music</string>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
EOF

# Make executable
chmod +x "$MACOS_DIR/CamelPlayer"

echo "✅ App bundle created successfully: $APP_NAME"
echo ""
echo "To run the app:"
echo "  open $APP_NAME"
echo ""
echo "To install to Applications:"
echo "  cp -r $APP_NAME /Applications/"
