# The lesson editor host: builds the web version and serves it with Static Web Server.
# Build from the repository root with the submodules checked out:
#
#   docker build -f emscripten/embed/Dockerfile --build-arg SOURCE_COMMIT=$(git rev-parse HEAD) .
#
# On Railway, RAILWAY_GIT_COMMIT_SHA takes the place of SOURCE_COMMIT.
# Run it with PARENT_ORIGINS set (see README.md).

FROM ubuntu:24.04 AS build
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ARG EMSDK_VERSION=6.0.10
ARG CMAKE_VERSION=4.4.3

# The native step only generates sources (GEN_ONLY), as for Android
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl git python3 xz-utils brotli g++ ninja-build \
      libtinyxml2-dev libfreetype6-dev libgif-dev libjpeg-dev libpixman-1-dev libpng-dev \
      libsdl2-dev libsdl2-image-dev libx11-dev libxcursor-dev zlib1g-dev libarchive-dev \
 && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz \
    | tar -xz -C /usr/local --strip-components=1
RUN git clone --depth 1 https://github.com/emscripten-core/emsdk.git /emsdk \
 && /emsdk/emsdk install ${EMSDK_VERSION} \
 && /emsdk/emsdk activate ${EMSDK_VERSION}

# Builds the Emscripten ports in their own layer.
# Freetype goes first: a fresh port cache asserts on the first link (emscripten-core/emscripten#20204).
RUN source /emsdk/emsdk_env.sh \
 && echo 'int main(){}' > /tmp/dummy.c \
 && emcc -O2 -pthread -sUSE_FREETYPE /tmp/dummy.c -o /tmp/dummy.js \
 && emcc -O2 -pthread -sUSE_SDL=2 -sUSE_SDL_IMAGE=2 -sUSE_SDL_TTF=2 -sUSE_ZLIB=1 -sUSE_GIFLIB=1 \
      -sUSE_LIBJPEG=1 -sUSE_LIBPNG -sSDL2_IMAGE_FORMATS='["png","jpg"]' /tmp/dummy.c -o /tmp/dummy.js

WORKDIR /src
COPY . .
RUN test -f third_party/simpleini/SimpleIni.h && test -f src/flic/flic.h \
 || { echo "The submodules are missing from the build context (see README.md)" >&2; exit 1; }

ARG SOURCE_COMMIT
ARG RAILWAY_GIT_COMMIT_SHA
RUN commit=${SOURCE_COMMIT:-$RAILWAY_GIT_COMMIT_SHA} \
 && { [ -n "$commit" ] || { echo "Pass --build-arg SOURCE_COMMIT=<full git hash>" >&2; exit 1; }; } \
 && sed -i "s/\"local build\"/\"${commit:0:7}\"/" src/config.h \
 && cmake -S . -B build -G Ninja -DGEN_ONLY=ON \
 && ninja -C build \
 && ninja -C build copy_data \
 && source /emsdk/emsdk_env.sh \
 && emcmake cmake -S emscripten -B emscripten/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSLIM_DATA=ON \
 && ninja -C emscripten/build \
 && sh emscripten/embed/stage.sh emscripten/build /site "$commit"

FROM joseluisq/static-web-server:2-alpine
COPY --from=build /site /public
# The base image runs as sws, and entrypoint.sh writes config.js when the container starts.
# Only that file is the server user's; the rest of /public stays read-only to it.
USER root
RUN install -o sws -g sws -m 644 /dev/null /public/config.js
USER sws:sws
COPY emscripten/embed/sws.toml /etc/sws/sws.toml.template
COPY emscripten/embed/entrypoint.sh /embed-entrypoint.sh
ENTRYPOINT ["/bin/sh", "/embed-entrypoint.sh"]
CMD []
