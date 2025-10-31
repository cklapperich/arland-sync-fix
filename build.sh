#!/bin/bash
set -e

# Ensure we're using posix-threaded MinGW for proper C++11 stdlib support
echo "Setting MinGW to posix threading model..."
sudo update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
sudo update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix

# Build
echo "Building..."
cd code
rm -rf build
meson setup --cross-file ../build-win64.txt build
ninja -C build

# Deploy
echo "Deploying to game directory..."
GAME_DIR=$(find ~/.steam/debian-installation/steamapps/common -name "Atelier Meruru*" -type d | head -1)

if [ -z "$GAME_DIR" ]; then
    echo "ERROR: Could not find Atelier Meruru game directory"
    echo "Please copy code/build/d3d11.dll manually to your game directory"
    exit 1
fi

echo "Found game at: $GAME_DIR"
cp build/d3d11.dll "$GAME_DIR/"
echo "Done! d3d11.dll deployed to $GAME_DIR"
