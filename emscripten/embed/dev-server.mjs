#!/usr/bin/env node
// LibreSprite | Copyright (C)      2026  LibreSprite contributors
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

// Serves the site from stage.sh for local development, with the headers of the production host.
// No dependencies.
//
//   PARENT_ORIGINS=http://localhost:5173 node emscripten/embed/dev-server.mjs [--root DIR] [--port N]
//
// PARENT_ORIGINS (comma-separated) are the pages that can embed the editor.
// The root is emscripten/build/site by default, and the port is 8602 (or PORT).

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
const option = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
};

const root = resolve(option('--root') ?? fileURLToPath(new URL('../build/site', import.meta.url)));
const port = Number(option('--port') ?? process.env.PORT ?? 8602);

const origins = (process.env.PARENT_ORIGINS ?? 'http://localhost:5173')
  .split(',')
  .map((value) => value.trim())
  .filter(Boolean)
  .map((value) => {
    const url = new URL(value);
    if (!/^https?:$/.test(url.protocol) || url.origin !== value.replace(/\/$/, ''))
      throw new Error(`PARENT_ORIGINS: '${value}' is not an origin`);
    return url.origin;
  });

const types = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.data': 'application/octet-stream',
  '.txt': 'text/plain; charset=utf-8',
};

const configJs = `self.ART_EDITOR_CONFIG = ${JSON.stringify({ parentOrigins: origins })};\n`;

const send = (res, status, type, body, path) => {
  res.writeHead(status, {
    'Content-Type': type,
    'Cache-Control': status === 200 && path.startsWith('/v/') ? 'public, max-age=31536000, immutable' : 'no-cache',
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'cross-origin',
    'Content-Security-Policy': `frame-ancestors ${origins.join(' ')}`,
    'X-Content-Type-Options': 'nosniff',
  });
  res.end(body);
};

createServer(async (req, res) => {
  let path;
  try {
    path = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  } catch {
    return send(res, 400, 'text/plain', 'Bad request\n', '/');
  }
  if (req.method !== 'GET' && req.method !== 'HEAD')
    return send(res, 405, 'text/plain', 'Method not allowed\n', path);
  if (path === '/config.js')
    return send(res, 200, types['.js'], configJs, path);

  let file = resolve(root, '.' + path);
  if (file !== root && !file.startsWith(root + sep))
    return send(res, 404, 'text/plain', 'Not found\n', path);
  try {
    if ((await stat(file)).isDirectory()) {
      if (!path.endsWith('/')) {
        res.writeHead(301, { Location: path + '/' });
        return res.end();
      }
      file = join(file, 'index.html');
    }
    const body = await readFile(file);
    send(res, 200, types[extname(file)] ?? 'application/octet-stream', req.method === 'HEAD' ? undefined : body, path);
  } catch {
    send(res, 404, 'text/plain', 'Not found\n', path);
  }
}).listen(port, () => {
  console.log(`LibreSprite editor on http://localhost:${port}/ from ${root}`);
  console.log(`Embeddable by: ${origins.join(', ')}`);
  stat(join(root, 'index.html')).catch(() =>
    console.warn('No staged site there yet: build the web version and run emscripten/embed/stage.sh (see README.md).'));
});
