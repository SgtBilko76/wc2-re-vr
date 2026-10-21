#!/bin/sh
# Fetches the third-party sources the Quest build needs but the repo does not
# vendor: SDL2 (source + Android Java glue).  miniLZO and the OpenXR headers /
# loader are checked in under quest/third_party.
set -e
cd "$(dirname "$0")/../third_party"
SDL_VER=2.32.10
if [ ! -d "SDL2-$SDL_VER" ]; then
    curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" | tar xz
fi
JAVA_DST=../android/app/src/main/java/org/libsdl/app
mkdir -p "$JAVA_DST"
cp "SDL2-$SDL_VER"/android-project/app/src/main/java/org/libsdl/app/*.java "$JAVA_DST"/
echo "third-party sources ready"
