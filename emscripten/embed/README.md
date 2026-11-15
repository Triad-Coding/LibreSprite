# LibreSprite in a lesson page

This directory serves the web version of LibreSprite inside a cross-origin
iframe of a lesson page. The page and the editor talk through `postMessage`.
The protocol is the comment at the top of `bridge.js`; keep it and the
platform's copy (`docs/art-editor.md` in the platform repository) the same.

| File | What it does |
|---|---|
| `index.html` | The page in the iframe: the canvas, a status line, `config.js` and `bridge.js`. |
| `bridge.js` | The protocol, the session settings (language from `?locale=`, UI scale, no IndexedDB) and the engine start. |
| `source.html` | The `/source/` page: the license and the fork at the exact commit (GPL-2.0). |
| `stage.sh` | Lays out the site from a web build. |
| `dev-server.mjs` | Serves the staged site locally with the production headers. |
| `Dockerfile`, `sws.toml`, `entrypoint.sh` | The production host (Static Web Server). |

The C++ side is `src/app/web_embed.cpp`. It runs only when the page sets
`Module.embed`, so the normal web version is unchanged. It:

- opens a blank RGBA sprite of the requested size, or the saved `.aseprite`,
  zoomed to fit;
- cancels and hides the commands that open files or URLs, close the sprite or
  resize the canvas, plus the Scripts and Help menus and the tabs; Ctrl+S
  sends the autosave at once;
- after each change (undo history), writes the `.aseprite` and a PNG of the
  first frame and hands both to `bridge.js`. A hidden tab stops the engine,
  so `bridge.js` keeps the newest snapshot to send when the tab hides.

## Site layout

```
index.html            no-cache
config.js             no-cache; written at start from PARENT_ORIGINS
source/               no-cache
v/<content hash>/     immutable: libresprite.js, .wasm, .data, bridge.js (+ .br, .gz)
```

Every response has `Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp`,
`Cross-Origin-Resource-Policy: cross-origin` and
`Content-Security-Policy: frame-ancestors <PARENT_ORIGINS>`. The engine uses
threads, so it needs `SharedArrayBuffer`, which needs cross-origin
isolation. An iframe is isolated only when the page around it is isolated
too and the iframe has `allow="cross-origin-isolated"`.

The web build is made with `-DSLIM_DATA=ON`: no Chinese, Korean or Japanese
fonts and languages, and no bundled scripts. That takes the data package
from 24 MB to about 1.5 MB.

## Local development

The web build downloads Emscripten ports from github.com on its first link.

```sh
# Once: the native generated sources and data (the full native build also works)
cmake -S . -B build -G Ninja -DGEN_ONLY=ON && ninja -C build && ninja -C build copy_data

# The web build (emsdk 6.0.10, see .github/workflows/cmakeWasm.yml)
emcmake cmake -S emscripten -B emscripten/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSLIM_DATA=ON
ninja -C emscripten/build
sh emscripten/embed/stage.sh emscripten/build emscripten/build/site "$(git rev-parse HEAD)"

# Serve it on port 8602 (--port or PORT to change; --root for another site)
PARENT_ORIGINS=http://localhost:5173 node emscripten/embed/dev-server.mjs
```

`PARENT_ORIGINS` defaults to `http://localhost:5173` in the dev server. Opened
directly, the page only says that it runs inside a lesson.

## Deploy

Build the image from the repository root, with the submodules checked out:

```sh
docker build -f emscripten/embed/Dockerfile --build-arg SOURCE_COMMIT=$(git rev-parse HEAD) -t libresprite-embed .
docker run -p 8602:8602 -e PARENT_ORIGINS=https://triadcoding.com libresprite-embed
```

On Railway, set `RAILWAY_DOCKERFILE_PATH=emscripten/embed/Dockerfile` and
`PARENT_ORIGINS`. Railway passes `RAILWAY_GIT_COMMIT_SHA` and `PORT`. The
build context must contain the submodules (`third_party/simpleini`,
`src/flic`): the Dockerfile stops with a message when they are missing.

The build has not been timed. Expect about 20 to 40 minutes on a few cores:
emsdk and the ports, then about 700 sources at `-O3`. Later builds reuse the
emsdk and ports layers.

Behind Cloudflare, keep Browser Cache TTL at "Respect Existing Headers".
