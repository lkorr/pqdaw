#pragma once

// Files dragged in from Explorer (T5.7): our own OLE IDropTarget on the main window.
//
// Why not SDL3's: SDL 3.4's Windows drop target (SDL_windowswindow.c, SDLDropTarget) reports positions during the drag
// (SDL_EVENT_DROP_POSITION) but reads the file names only in Drop(), always answers DROPEFFECT_COPY and passes no key
// state. The drop rules need the files while hovering (ghost clips at their probed length, probing and head loading on
// the first hover), a refusal the OS can show (the "no" cursor over a return track), and Ctrl (all files on one track).
// So the app turns SDL's drop events off (which revokes SDL's target) and registers this one instead
// (docs/plans/M5.md T5.7: "if positions or hover feedback are missing, use our own IDropTarget via RegisterDragDrop").
//
// Threading: OLE calls an IDropTarget on the thread that registered it (the UI thread, an STA that SDL initialised
// with OleInitialize), from inside the message pump (SDL_WaitEvent / SDL_PollEvent). The sink therefore runs on the UI
// thread; it must return at once (the drag source waits), so the app queues the event, wakes its loop and answers with
// the effect the tree reported last (one event behind, which Explorer's polling hides).
//
// Not RT code. Windows only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/drag.h"
#include "pq/ui/input.h"

namespace pq::app {

struct OsDragEvent {
  enum class Type : uint8_t { Enter, Over, Leave, Drop };
  Type type = Type::Over;
  std::vector<std::string> paths;  // Enter and Drop: the files (absolute UTF-8, '\\' separators), in Explorer's order
  float x = 0, y = 0;              // window client coordinates (SDL window coordinates)
  ui::Mods mods = ui::mod::kNone;  // Ctrl / Shift / Alt from the drag's key state
};

class OsDropTarget {
public:
  using Sink = std::function<ui::DropEffect(const OsDragEvent&)>;
  // Screen → client coordinates (the registered window's ScreenToClient; tests pass their own).
  using ToClient = std::function<void(int32_t screenX, int32_t screenY, float& x, float& y)>;

  // Registers a drop target on `hwnd` (RegisterDragDrop; OLE must be initialised on this thread). Errors: Device.
  static Result<std::unique_ptr<OsDropTarget>> registerOn(void* hwnd, Sink sink);
  // An unregistered target (tests call its IDropTarget methods directly through comTarget()).
  static std::unique_ptr<OsDropTarget> unregistered(Sink sink, ToClient toClient);
  ~OsDropTarget();  // RevokeDragDrop when registered
  OsDropTarget(const OsDropTarget&) = delete;
  OsDropTarget& operator=(const OsDropTarget&) = delete;

  // The COM object (an IDropTarget*), for tests.
  void* comTarget() const noexcept;
  bool registered() const noexcept { return hwnd_ != nullptr; }

private:
  OsDropTarget() = default;
  struct Com;
  Com* com_ = nullptr;  // reference-counted COM object (released in the destructor)
  void* hwnd_ = nullptr;
};

// T5.10 (the browser's context menu): opens Explorer at the file's folder with the file selected; moves a file to the
// Recycle Bin (undoable there; false when it failed).
void showInExplorer(const std::string& pathUtf8);
bool moveToRecycleBin(const std::string& pathUtf8);

// The files of an OLE data object (CF_HDROP), as absolute UTF-8 paths. `dataObject` is an IDataObject*. Empty when it
// carries no files.
std::vector<std::string> filesOfDataObject(void* dataObject);

}  // namespace pq::app
