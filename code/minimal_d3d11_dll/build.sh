#!/bin/bash

# build.sh - Cross-compile to Windows DLL using Meson and deploy

set -e  # Exit on error

# Game directory
GAME_DIR="/home/klappec/.steam/debian-installation/steamapps/common/Atelier Meruru ~The Apprentice of Arland~ DX/"

# Change to code directory where meson.build is located
cd code

# Setup meson build directory if needed
if [ ! -d "build" ]; then
    echo "Setting up meson build directory..."
    meson setup build --cross-file ../build-win64.txt
fi

# Build with meson
echo "Building with meson..."
meson compile -C build

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo "Build successful!"

# Copy to game directory
echo "Copying to game directory..."
cp build/d3d11.dll "$GAME_DIR"

echo "Done! d3d11.dll deployed to game directory"
ls -lh "$GAME_DIR/d3d11.dll"