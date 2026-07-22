#!/bin/sh
# Build examples/raytracer.simp, the fixed-point ray tracer.
set -e
cd "$(dirname "$0")/.."

SDL_PREFIX=${SDL_PREFIX:-/opt/homebrew}
if [ ! -d "$SDL_PREFIX/include/SDL2" ]; then
    echo "SDL2 not found under $SDL_PREFIX — try: brew install sdl2" >&2
    echo "(Intel Homebrew: SDL_PREFIX=/usr/local sh tools/build_raytracer.sh)" >&2
    exit 1
fi

mkdir -p build
./simplec examples/raytracer.simp --link SDL2 --libdir "$SDL_PREFIX/lib" \
    -o build/raytracer
echo "built: build/raytracer   (writes render.ppm on exit)"
