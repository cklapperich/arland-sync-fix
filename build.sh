#!/bin/bash

# build.sh - Cross-compile to Windows DLL and deploy

# Game directory
GAME_DIR="/home/klappec/.steam/debian-installation/steamapps/common/Atelier Meruru ~The Apprentice of Arland~ DX/"

# Compile to Windows DLL
echo "Compiling d3d11.cpp to Windows DLL..."
x86_64-w64-mingw32-g++ -shared -o d3d11.dll code/d3d11.cpp \
    -static-libgcc -static-libstdc++ -ld3d11

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "Compilation successful!"

# Copy to game directory
echo "Copying to game directory..."
mv d3d11.dll "$GAME_DIR"

echo "Done! d3d11.dll deployed to game directory"
ls -lh "$GAME_DIR/d3d11.dll"