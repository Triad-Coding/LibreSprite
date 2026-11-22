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

# With a terminal, ninja shows its progress line and Ctrl+C goes to the build in the container.
tty=
if [ -t 0 ] && [ -t 1 ]; then
  tty=-it
fi

# The build runs as you, so the files in the checkout are always yours, also when the build stops part-way.
# The checkout is mounted at its own path, so the paths in the CMake caches stay the same between runs.
# --init with TINI_KILL_PROCESS_GROUP sends a signal (Ctrl+C without a terminal, docker stop) to all the build processes.
# IMAGE_ID is the list of layers: the image ID can change at each docker build (the containerd image store), also when all layers come from the cache.
docker run --rm $tty --init -e TINI_KILL_PROCESS_GROUP=1 \
  -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$root:$root" -w "$root" \
  -e COMMIT="$commit" -e OUT="$out" -e IMAGE_ID="$(docker image inspect -f '{{range .RootFS.Layers}}{{.}} {{end}}' "$image")" \
  "$image" bash -c '
    set -e

    # Older versions of this script built as root.
    if [ -n "$(find build emscripten/build ! -user "$(id -u)" -print -quit 2> /dev/null)" ]; then
      echo "build/ or emscripten/build/ has files that are not yours. Delete them through the image:" >&2
      echo "  docker run --rm -v \"\$PWD:\$PWD\" -w \"\$PWD\" libresprite-toolchain rm -rf build emscripten/build" >&2
      exit 1
    fi

    # A cache made by a build outside Docker names tools that are not in the image.
    for cache in build/CMakeCache.txt emscripten/build/CMakeCache.txt; do
      [ -f "$cache" ] || continue
      for tool in $(sed -n "s/^\(CMAKE_COMMAND\|CMAKE_CXX_COMPILER\|CMAKE_TOOLCHAIN_FILE\):[A-Z]*=//p" "$cache"); do
        if [ ! -e "$tool" ]; then
          echo "$cache is from a build outside Docker: $tool is not in the image." >&2
          echo "Delete build/ and emscripten/build/, then run this script again." >&2
          exit 1
        fi
      done
    done

    # emcc writes to its cache. The cache in the image (with the ports) belongs to root, so the build uses a copy in emscripten/build.
    # IMAGE_ID (the layers of the image) shows when the image changed and the copy is old.
    em_cache=$PWD/emscripten/build/emcache
    if [ "$(cat "$em_cache/image-id" 2> /dev/null)" != "$IMAGE_ID" ]; then
      echo "Copying the Emscripten cache from the image to $em_cache"
      rm -rf "$em_cache"
      mkdir -p "$em_cache"
      cp -R /emsdk/upstream/emscripten/cache/. "$em_cache"
      echo "$IMAGE_ID" > "$em_cache/image-id"
    fi

    cmake -S . -B build -G Ninja -DGEN_ONLY=ON
    ninja -C build
    ninja -C build copy_data
    source /emsdk/emsdk_env.sh > /dev/null
    # emsdk_env.sh clears EM_CACHE, so set it after.
    export EM_CACHE=$em_cache
    emcmake cmake -S emscripten -B emscripten/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSLIM_DATA=ON
    ninja -C emscripten/build
    rm -rf "$OUT"
    sh emscripten/embed/stage.sh emscripten/build "$OUT" "$COMMIT"
  '
