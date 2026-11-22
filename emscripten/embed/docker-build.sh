#!/bin/sh
# Makes the web build and the staged site in Docker, for local development without emsdk or CMake on the machine.
# It uses the toolchain stage of the Dockerfile, and keeps build/ and emscripten/build/ in the checkout, so a later run compiles only what changed.
# Do not mix it with a build made outside Docker in the same checkout: delete build/ and emscripten/build/ first.
#
# Usage: docker-build.sh [output dir inside the checkout]   (default: emscripten/build/site, which dev-server.mjs serves)
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
out=${1:-emscripten/build/site}
commit=$(git rev-parse HEAD)
image=libresprite-toolchain

# The toolchain stage copies nothing, so it needs no build context.
docker build --target toolchain -t "$image" - < emscripten/embed/Dockerfile

# The checkout is mounted at its own path, so the paths in the CMake caches stay the same between runs.
# emcc writes to its cache under /emsdk, so the build runs as root and gives the files back to the user at the end.
docker run --rm -v "$root:$root" -w "$root" \
  -e COMMIT="$commit" -e OUT="$out" -e OWNER="$(id -u):$(id -g)" \
  "$image" bash -c '
    (
      set -e
      cmake -S . -B build -G Ninja -DGEN_ONLY=ON
      ninja -C build
      ninja -C build copy_data
      source /emsdk/emsdk_env.sh > /dev/null
      emcmake cmake -S emscripten -B emscripten/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSLIM_DATA=ON
      ninja -C emscripten/build
      rm -rf "$OUT"
      sh emscripten/embed/stage.sh emscripten/build "$OUT" "$COMMIT"
    )
    status=$?
    chown -R "$OWNER" build emscripten/build "$OUT" 2> /dev/null || true
    exit $status
  '
