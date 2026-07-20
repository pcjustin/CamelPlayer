#!/bin/bash
set -euo pipefail

# Usage: ./install_linux.sh [--debug|--release] [--install|--no-install]
#   --release      (default) optimized build; slower to compile
#   --debug        fast incremental build for local iteration
#   --install, -y  install to the system without prompting
#   --no-install   build only, skip the install prompt
# When run interactively without an install flag, the script asks whether to
# install after a successful build.
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
    echo "Building CamelPlayerGTK (release, optimized - this can take a few minutes)..."
else
    echo "Building CamelPlayerGTK (debug)..."
fi

swift build -c "$CONFIG" --product CamelPlayerGTK

# Locate the built binary instead of hardcoding the path
BIN_PATH="$(swift build -c "$CONFIG" --product CamelPlayerGTK --show-bin-path 2>/dev/null)"

echo "Build complete: $BIN_PATH/CamelPlayerGTK"

BIN_DEST="/usr/local/bin/camelplayer"
ICON_DEST="/usr/share/pixmaps/camelplayer.png"
DESKTOP_DEST="/usr/share/applications/camelplayer.desktop"

# Icon lookup (pixmaps fallback dir) only matches known raster formats, and
# every icon already installed there is .png/.xpm, never .jpg - convert with
# Pillow (already installed) so camelplayer.png follows the same convention.
ICON_PNG=".build/camelplayer.png"
if [ ! -f "$ICON_PNG" ] || [ "logo.jpg" -nt "$ICON_PNG" ]; then
    python3 -c "from PIL import Image; Image.open('logo.jpg').convert('RGB').save('$ICON_PNG')"
fi

do_install="$INSTALL_CHOICE"
if [ "$do_install" = "prompt" ]; then
    if [ ! -t 0 ]; then
        echo "Not running interactively; skipping system install."
        echo "Install manually by re-running with --install."
        do_install="no"
    else
        read -r -p "Install to the system (requires sudo)? [y/N] " ans || ans=""
        case "$ans" in
            y|Y|yes|YES) do_install="yes" ;;
            *) do_install="no" ;;
        esac
    fi
fi

if [ "$do_install" = "yes" ]; then
    echo "Installing..."
    sudo install -Dm755 "$BIN_PATH/CamelPlayerGTK" "$BIN_DEST"
    sudo install -Dm644 "$ICON_PNG" "$ICON_DEST"

    sudo install -d "$(dirname "$DESKTOP_DEST")"
    sudo tee "$DESKTOP_DEST" > /dev/null << EOF
[Desktop Entry]
Type=Application
Name=CamelPlayer
Comment=Bit-perfect audio player
Exec=camelplayer
Icon=camelplayer
Terminal=false
Categories=AudioVideo;Audio;Player;GTK;
EOF

    command -v update-desktop-database > /dev/null 2>&1 && \
        sudo update-desktop-database /usr/share/applications > /dev/null 2>&1

    echo "Installed: $BIN_DEST"
    echo "CamelPlayer should now appear in your application launcher."
else
    echo ""
    echo "To run without installing: $BIN_PATH/CamelPlayerGTK"
    echo "To install:                $0 --install"
fi
