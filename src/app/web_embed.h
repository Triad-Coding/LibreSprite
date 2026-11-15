// LibreSprite | Copyright (C)      2026  LibreSprite contributors
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#pragma once

namespace app {
namespace web_embed {

  // Web build only.
  // If the page sets Module.embed (emscripten/embed/), locks the UI to one sprite and gives the page a snapshot after each change.
  // Call it from the UI thread before the message loop starts.
  void start();

} // namespace web_embed
} // namespace app
