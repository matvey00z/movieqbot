#!/bin/bash

set -e

root="$(pwd)"
if [ "$1" = "--clean" ]; then
    rm -rf build/
fi
mkdir -p build
mkdir -p build/bin

# Build cutter
cp cutter/cutter.py build/bin
mkdir -p build/cutter
cd build/cutter
cmake ../../cutter -DCMAKE_BUILD_TYPE=Release
make -j $(nproc)
strip -s cutter
cd "$root"
cp build/cutter/cutter build/bin/

# Build bot
mkdir -p build/bot
cd build/bot
"$root"/bot/build.sh
strip -s bot
cd "$root"
cp build/bot/bot build/bin

# Build web
mkdir -p build/web
cd build/web
"$root"/web/build.sh
strip -s web
cd "$root"
cp build/web/web build/bin/movieq_web

echo "Build finished successfully"
