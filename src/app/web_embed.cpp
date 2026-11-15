// LibreSprite | Copyright (C)      2026  LibreSprite contributors
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

// The web build inside a lesson page (emscripten/embed/bridge.js).
// The page sets Module.embed and asks for a blank sprite or for saved bytes with embedLoad().
// After each change, the page gets the .aseprite and a PNG of the sprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/web_embed.h"

#include "app/app.h"
#include "app/app_menus.h"
#include "app/commands/command.h"
#include "app/commands/cmd_rotate.h"
#include "app/commands/commands.h"
#include "app/commands/params.h"
#include "app/document.h"
#include "app/document_access.h"
#include "app/document_undo.h"
#include "app/document_undo_observer.h"
#include "app/file/file.h"
#include "app/modules/editors.h"
#include "app/pref/preferences.h"
#include "app/ui/app_menuitem.h"
#include "app/ui/editor/editor.h"
#include "app/ui/home_view.h"
#include "app/ui/main_window.h"
#include "app/ui/workspace.h"
#include "app/ui_context.h"
#include "base/time.h"
#include "doc/image.h"
#include "doc/sprite.h"
#include "render/render.h"
#include "render/zoom.h"
#include "she/surface.h"
#include "she/system.h"
#include "ui/ui.h"

#include <emscripten/emscripten.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace app {
namespace web_embed {
namespace {

using namespace ui;

const char* kLoadPath = "/tmp/embed-load.aseprite";
const char* kSavePath = "/tmp/embed-save.aseprite";
const char* kPngPath = "/tmp/embed-save.png";

// Commands that leave the one sprite, open a file dialog or a URL, or change the canvas size that the lesson sets.
// Rotate is denied only for the sprite (see isDenied()).
const std::set<std::string> kDenied = {
  "About", "AutocropSprite", "CanvasSize", "CloseAllFiles", "CloseFile",
  "CropSprite", "DeveloperConsole", "DuplicateSprite", "DuplicateView",
  "Exit", "ExportSpriteSheet", "Home",
  "ImportSpriteSheet", "InstallScript", "KeyboardShortcuts", "Launch",
  "LoadMask", "LoadPalette", "NewFile", "NewSpriteFromSelection",
  "OpenFile", "OpenInFolder", "OpenScriptsFolder", "OpenWithApp", "Options",
  "PasteText", "Refresh", "RepeatLastExport", "RescanScripts", "RunScript",
  "SaveFileAs", "SaveFileCopyAs", "SaveMask", "SavePalette", "Share",
  "SpriteSize", "ToggleFullscreen",
};

struct Request {
  bool fromBytes;
  int width, height;
};

// Set on the browser thread by embedLoad(), taken on the UI thread.
std::mutex s_mutex;
std::unique_ptr<Request> s_request;

// UI thread only.
Document* s_doc = nullptr;
bool s_internal = false;  // our own NewFile is running
bool s_fit = false;       // zoom to fit on the next tick
bool s_changed = false;   // the sprite changed after the last snapshot
bool s_flush = false;     // Ctrl+S: send the snapshot now
base::tick_t s_lastSnapshot = 0;

class UndoObserver : public DocumentUndoObserver {
public:
  void onAddUndoState(DocumentUndo*) override { s_changed = true; }
  void onAfterUndo(DocumentUndo*) override { s_changed = true; }
  void onAfterRedo(DocumentUndo*) override { s_changed = true; }
  void onClearRedo(DocumentUndo*) override { }
} s_undoObserver;

// Rotating the sprite, not the selection or the cel, swaps its width and height.
bool isDenied(Command* command)
{
  if (auto rotate = dynamic_cast<RotateCommand*>(command))
    return !rotate->flipMask();
  return command && kDenied.count(command->id()) > 0;
}

// A menu item's params are not loaded into its command until it runs.
bool isDenied(AppMenuItem* item)
{
  Command* command = item->getCommand();
  if (command && command->id() == CommandId::Rotate)
    return item->getParams().get("target") != "mask";
  return isDenied(command);
}

void onBeforeCommand(CommandExecutionEvent& ev)
{
  if (s_internal)
    return;
  if (ev.command()->id() == "SaveFile") {
    s_flush = true;
    ev.cancel();
  }
  else if (isDenied(ev.command()))
    ev.cancel();
}

// Removes denied items, the recent files list, empty submenus and extra separators from a menu.
// Returns false when nothing is left.
// Items are detached, not deleted: AppMenus keeps pointers to some of them.
bool prune(Menu* menu)
{
  if (!menu)
    return false;

  Widget* recentList = AppMenus::instance()->getById<Widget>("recent_list");
  WidgetsList children = menu->children();
  for (Widget* child : children) {
    auto item = dynamic_cast<AppMenuItem*>(child);
    if (!item)
      continue;
    bool remove = (item == recentList || isDenied(item) ||
                   (item->getSubmenu() && !prune(item->getSubmenu())));
    if (remove)
      menu->removeChild(item);
  }

  bool afterSeparator = true;
  children = menu->children();
  for (Widget* child : children) {
    bool separator = (child->type() == kSeparatorWidget);
    if (separator && afterSeparator)
      menu->removeChild(child);
    else
      afterSeparator = separator;
  }
  if (!menu->children().empty() &&
      menu->children().back()->type() == kSeparatorWidget)
    menu->removeChild(menu->children().back());

  return !menu->children().empty();
}

void removeHomeView()
{
  Workspace* workspace = App::instance()->workspace();
  WorkspaceViews views(workspace->begin(), workspace->end());
  for (WorkspaceView* view : views)
    if (dynamic_cast<HomeView*>(view))
      workspace->removeView(view);
}

void openSprite(const Request& request)
{
  UIContext* ctx = UIContext::instance();
  removeHomeView();

  if (request.fromBytes) {
    // No context: no alerts and no recent files entry.
    std::unique_ptr<FileOp> fop(
      FileOp::createLoadDocumentOperation(nullptr, kLoadPath, FILE_LOAD_SEQUENCE_NONE));
    fop->operate();
    fop->done();
    fop->postLoad();
    if (fop->hasError() || !fop->document()) {
      std::string reason = (fop->hasError() ? fop->error() : "Cannot open the file");
      MAIN_THREAD_EM_ASM({ Module.embed.onLoadError(UTF8ToString($0)); }, reason.c_str());
      return;
    }
    fop->releaseDocument()->setContext(ctx);
  }
  else {
    Params params;
    params.set("width", std::to_string(request.width).c_str());
    params.set("height", std::to_string(request.height).c_str());
    params.set("color-mode", "rgb");
    params.set("background", "transparent");
    s_internal = true;
    ctx->executeCommand(CommandsModule::instance()->getCommandByName(CommandId::NewFile), params);
    s_internal = false;
  }

  s_doc = ctx->activeDocument();
  if (!s_doc) {
    MAIN_THREAD_EM_ASM({ Module.embed.onLoadError("Cannot create the sprite"); });
    return;
  }
  s_doc->undoHistory()->addObserver(&s_undoObserver);

  // At lesson zoom levels the default 16x16 checks fill the whole canvas.
  // Small checks and the pixel grid show each pixel as a square.
  DocumentPreferences& docPref = Preferences::instance().document(s_doc);
  docPref.bg.type(gen::BgType::CHECKED_2x2);
  docPref.show.pixelGrid(true);
  s_fit = true;
}

// Sets the largest zoom that shows all of the sprite with a margin.
// Returns false while the editor has no size.
bool fitToView()
{
  Editor* editor = UIContext::instance()->activeEditor();
  if (!editor)
    return true;
  gfx::Rect vp = View::getView(editor)->viewportBounds();
  if (vp.isEmpty())
    return false;
  const doc::Sprite* sprite = s_doc->sprite();
  render::Zoom zoom = render::Zoom::fromLinearScale(0);
  for (int i = render::Zoom::linearValues()-1; i > 0; --i) {
    render::Zoom z = render::Zoom::fromLinearScale(i);
    if (z.apply(sprite->width()) <= vp.w*9/10 &&
        z.apply(sprite->height()) <= vp.h*9/10) {
      zoom = z;
      break;
    }
  }
  editor->setZoom(zoom);
  editor->updateEditor();
  editor->setDefaultScroll();
  editor->invalidate();
  return true;
}

// A flattened PNG of the first frame.
std::vector<uint8_t> encodePng(const doc::Sprite* sprite)
{
  std::unique_ptr<doc::Image> image(
    doc::Image::create(doc::IMAGE_RGB, sprite->width(), sprite->height()));
  render::Render().renderSprite(image.get(), sprite, doc::frame_t(0));

  std::shared_ptr<she::Surface> surface(
    she::instance()->createRgbaSurface(image->width(), image->height()),
    [](she::Surface* s) { s->dispose(); });
  for (int y = 0; y < image->height(); ++y)
    for (int x = 0; x < image->width(); ++x)
      surface->putPixel(image->getPixel(x, y), x, y);
  return she::instance()->encodeSurfaceAsPNG(surface.get());
}

// Writes the .aseprite and the PNG to MEMFS and gives both to the page.
// Returns false if the sprite is locked: the next tick tries again.
bool snapshot()
{
  int width, height;
  try {
    DocumentReader reader(s_doc, 0);
    const doc::Sprite* sprite = s_doc->sprite();
    width = sprite->width();
    height = sprite->height();

    std::unique_ptr<FileOp> fop(
      FileOp::createSaveDocumentOperation(nullptr, s_doc, kSavePath, ""));
    fop->operate();
    fop->done();
    if (fop->hasError())
      throw std::runtime_error(fop->error());

    std::vector<uint8_t> png = encodePng(sprite);
    std::ofstream file(kPngPath, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(png.data()), png.size());
    file.close();
    if (png.empty() || !file.good())
      throw std::runtime_error("Cannot write the PNG");
  }
  catch (const LockedDocumentException&) {
    return false;
  }
  catch (const std::exception& e) {
    // Not retried: the next change makes a new snapshot.
    std::cerr << "Snapshot failed: " << e.what() << std::endl;
    s_changed = false;
    return true;
  }

  s_changed = false;
  s_lastSnapshot = base::current_tick();
  MAIN_THREAD_EM_ASM({
    Module.embed.onSnapshot(FS.readFile(UTF8ToString($0)), FS.readFile(UTF8ToString($1)), $2, $3);
  }, kSavePath, kPngPath, width, height);
  return true;
}

void onTick()
{
  std::unique_ptr<Request> request;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    request = std::move(s_request);
  }
  if (request && !s_doc) {
    openSprite(*request);
    return;
  }
  if (s_fit) {
    if (!fitToView())
      return;
    s_fit = false;
    MAIN_THREAD_EM_ASM({ Module.embed.onLoaded($0, $1); },
                       s_doc->sprite()->width(), s_doc->sprite()->height());
    return;
  }
  if (!s_doc || !(s_changed || s_flush))
    return;

  // Wait for a stroke or a drag to end, and space the snapshots.
  if (Manager::getDefault()->getCapture() ||
      (current_editor && current_editor->isMovingPixels()) ||
      (!s_flush && base::current_tick() - s_lastSnapshot < 250))
    return;

  if (s_changed && !snapshot())
    return;
  if (s_flush) {
    s_flush = false;
    MAIN_THREAD_EM_ASM({ Module.embed.onFlush(); });
  }
}

} // anonymous namespace

void start()
{
  if (!MAIN_THREAD_EM_ASM_INT({ return Module.embed ? 1 : 0; }))
    return;

  UIContext::instance()->BeforeCommandExecution.connect(&onBeforeCommand);

  AppMenus* menus = AppMenus::instance();
  for (Menu* menu : { menus->getRootMenu(),
                      menus->getTabPopupMenu(),
                      menus->getDocumentTabPopupMenu(),
                      menus->getPalettePopupMenu() })
    prune(menu);

  // One sprite: no tabs (a tab's close button does not run a command).
  MainWindow* window = App::instance()->mainWindow();
  window->tabsPlaceholder()->setVisible(false);
  window->layout();

  auto timer = new Timer(100);
  timer->Tick.connect(&onTick);
  timer->start();

  MAIN_THREAD_EM_ASM({ Module.embed.onAppReady(); });
}

} // namespace web_embed
} // namespace app

// Browser thread. The page calls it after onAppReady, with the size of a blank sprite.
// For saved work, the page first puts the .aseprite bytes in Module.embed.bytes.
extern "C" void embedLoad(int width, int height)
{
  using namespace app::web_embed;
  bool fromBytes = EM_ASM_INT({
    const bytes = Module.embed.bytes;
    if (!bytes)
      return 0;
    Module.embed.bytes = null;
    FS.writeFile(UTF8ToString($0), new Uint8Array(bytes));
    return 1;
  }, kLoadPath);

  std::lock_guard<std::mutex> lock(s_mutex);
  s_request.reset(new Request{fromBytes, width, height});
}
