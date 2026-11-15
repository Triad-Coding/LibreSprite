// LibreSprite | Copyright (C)      2026  LibreSprite contributors
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

// Runs the editor inside a lesson page and speaks its postMessage protocol.
// The page loads us as /?locale=<en|pt>&parent=<its origin>.
// We arm only for a parent in config.js, post only to it and accept messages only from its window.
//
// Editor -> page:
//   art:ready {v: 1}                                  every 250 ms for 15 s, then every 2 s, until the page sends anything
//   art:loaded {width, height}                        the sprite is open and autosave is armed
//   art:load-error {reason}                           the art:load bytes did not open; never armed
//   art:dirty                                         first change after the last autosave was sent
//   art:autosave {seq, aseprite, png, width, height}  2 s after a change, 20 s at most
//   art:error {reason}                                fatal
// An autosave goes at once on Ctrl+S, tab hide, pagehide and art:flush-request, if there is a change or the last one has no ack.
//
// Page -> editor:
//   art:new {width, height} or art:load {aseprite}    once, after art:ready
//   art:save-ack {seq}
//   art:flush-request
//
// The C++ side is src/app/web_embed.cpp.
// It calls the Module.embed functions below and takes requests through Module._embedLoad.

(() => {
  'use strict';

  const DEBOUNCE_MS = 2000;
  const MAX_WAIT_MS = 20000;
  // The page can load after us (slow link), so art:ready never stops; it only slows down.
  const READY_FAST_MS = 250;
  const READY_FAST_FOR_MS = 15000;
  const READY_SLOW_MS = 2000;
  // src/cfg/cfg.cpp reads the settings from self.storage under this name.
  const INI_PATH = '/home/web_user/.config/libresprite/libresprite.ini';

  const base = document.currentScript.src.replace(/[^/]*$/, '');
  const params = new URLSearchParams(location.search);
  const locale = params.get('locale') === 'pt' ? 'pt' : 'en';
  const text = {
    en: {
      loading: 'Loading the editor…',
      outside: 'This editor opens inside a lesson.',
      stopped: 'The editor stopped. Reload the page to continue.',
      loadError: 'Your saved drawing could not be opened.',
      keyboard: 'Keyboard',
    },
    pt: {
      loading: 'Carregando o editor…',
      outside: 'Este editor abre dentro de uma aula.',
      stopped: 'O editor parou. Recarregue a página para continuar.',
      loadError: 'Não foi possível abrir o seu desenho salvo.',
      keyboard: 'Teclado',
    },
  }[locale];

  const status = document.getElementById('status');
  const show = (message) => {
    status.textContent = message;
    status.hidden = !message;
  };
  document.documentElement.lang = locale === 'pt' ? 'pt-BR' : 'en';
  document.getElementById('keyboard').textContent = text.keyboard;

  const toOrigin = (value) => {
    try {
      return new URL(value).origin;
    } catch {
      return null;
    }
  };
  const allowed = ((self.ART_EDITOR_CONFIG || {}).parentOrigins || []).map(toOrigin);
  const parentOrigin = toOrigin(params.get('parent'));
  if (window.parent === window || !parentOrigin || !allowed.includes(parentOrigin)) {
    show(text.outside);
    return;
  }

  const post = (message, transfer) => window.parent.postMessage(message, parentOrigin, transfer || []);

  let failed = false;
  const fatal = (reason) => {
    if (failed)
      return;
    failed = true;
    show(text.stopped);
    post({ type: 'art:error', reason: String(reason) });
  };

  // Threads need SharedArrayBuffer, which needs isolation of this frame and of the page around it.
  if (!self.crossOriginIsolated) {
    fatal('not cross-origin isolated');
    return;
  }

  show(text.loading);

  // Settings for this session only. The app reads them before it starts.
  const uiScale = innerWidth < 760 || innerHeight < 480 ? 1 : 2;
  self.storage = {
    data: {
      [INI_PATH]: `[general]\nlanguage = ${locale}\nscreen_scale = 1\ndata_recovery = false\n` +
                  `[experimental]\nui_scale = ${uiScale}\n`,
    },
    dirty: false,
    commit() {},
    getItem(key) { return this.data[key]; },
    setItem(key, value) { this.data[key] = value; },
  };

  let heard = false;      // the page sent something
  let request = null;     // its art:new or art:load
  let appReady = false;
  let requested = false;  // the request went to the app
  let armed = false;
  let latest = null;      // newest snapshot from the app
  let dirty = false;      // latest was not sent yet
  let seq = 0;
  let ackedSeq = 0;
  let debounce = 0;
  let maxWait = 0;

  const send = () => {
    clearTimeout(debounce);
    clearTimeout(maxWait);
    dirty = false;
    seq += 1;
    // Copies, so that latest stays whole for a resend.
    const aseprite = latest.aseprite.slice().buffer;
    const png = latest.png.slice().buffer;
    post({ type: 'art:autosave', seq, aseprite, png, width: latest.width, height: latest.height },
         [aseprite, png]);
  };

  // A hidden tab stops the app, so this sends only what we already hold.
  const flush = () => {
    if (armed && latest && (dirty || ackedSeq < seq))
      send();
  };

  const startApp = () => {
    if (!appReady || !request || requested || failed)
      return;
    requested = true;
    if (request.type === 'art:load') {
      Module.embed.bytes = request.aseprite;
      Module._embedLoad(0, 0);
    }
    else {
      Module._embedLoad(request.width, request.height);
    }
  };

  const isSize = (value) => Number.isInteger(value) && value >= 1 && value <= 4096;
  const isBytes = (value) => value instanceof ArrayBuffer || ArrayBuffer.isView(value);

  addEventListener('message', (event) => {
    if (event.source !== window.parent || event.origin !== parentOrigin)
      return;
    const message = event.data;
    if (!message || typeof message.type !== 'string')
      return;
    heard = true;
    switch (message.type) {
      case 'art:new':
      case 'art:load':
        if (request)
          return;  // one per boot: a repeated art:ready can cross the answer
        if (message.type === 'art:new' && !(isSize(message.width) && isSize(message.height)))
          return fatal('art:new needs integer width and height');
        if (message.type === 'art:load' && !isBytes(message.aseprite))
          return fatal('art:load needs the .aseprite bytes');
        request = message;
        startApp();
        break;
      case 'art:save-ack':
        if (Number.isInteger(message.seq))
          ackedSeq = Math.max(ackedSeq, message.seq);
        break;
      case 'art:flush-request':
        flush();
        break;
    }
  });

  const started = Date.now();
  const announce = () => {
    if (heard || failed)
      return;
    post({ type: 'art:ready', v: 1 });
    setTimeout(announce, Date.now() - started < READY_FAST_FOR_MS ? READY_FAST_MS : READY_SLOW_MS);
  };
  announce();

  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden')
      flush();
  });
  addEventListener('pagehide', flush);

  const canvas = document.getElementById('canvas');
  canvas.addEventListener('webglcontextlost', (event) => {
    event.preventDefault();
    fatal('WebGL context lost');
  });

  self.Module = {
    canvas,
    locateFile: (path) => base + path,
    setStatus: (message) => {
      const progress = /\((\d+(?:\.\d+)?)\/(\d+)\)/.exec(message || '');
      if (!armed && !failed && progress)
        show(`${text.loading} ${Math.floor(100 * progress[1] / progress[2])}%`);
    },
    onAbort: (what) => fatal(`wasm abort: ${what}`),
    // Called by src/app/web_embed.cpp, on this thread.
    embed: {
      bytes: null,
      onAppReady() {
        appReady = true;
        setTimeout(startApp);
      },
      onLoaded(width, height) {
        armed = true;
        show('');
        post({ type: 'art:loaded', width, height });
      },
      onLoadError(reason) {
        if (request.type !== 'art:load')
          return fatal(reason);
        show(text.loadError);
        post({ type: 'art:load-error', reason });
      },
      onSnapshot(aseprite, png, width, height) {
        if (!armed)
          return;
        latest = { aseprite, png, width, height };
        if (!dirty) {
          dirty = true;
          post({ type: 'art:dirty' });
          maxWait = setTimeout(send, MAX_WAIT_MS);
        }
        clearTimeout(debounce);
        debounce = setTimeout(send, DEBOUNCE_MS);
      },
      onFlush: flush,
    },
  };

  const script = document.createElement('script');
  script.src = base + 'libresprite.js';
  script.onerror = () => fatal('download failed');
  document.body.appendChild(script);
})();
