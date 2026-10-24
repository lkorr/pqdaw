#pragma once

// The main window layout (T3.7, SPEC §9: Ableton's layout).
//
//   ┌──────────────────────────────────────────────────────────────────────────────┐
//   │ control bar: tempo · time signature │ position · play stop rec │ loop … │ grid │  main/control-bar
//   ├─────────┬────────────────────────────────────────────────────────────────────┤
//   │ browser │ track area (arrangement T3.8; mixer view T3.10 in the same host)     │  main/body/top
//   │ (M5,    │                                                                      │
//   │ hidden) │                                                                      │
//   ├═════════╧════════════════════════ splitter ═══════════════════════════════════┤
//   │ info view ║ detail view: clip (piano roll T3.11) / device (M4)               │  main/body/bottom
//   ├────────────────────────────────────────────────────────────────────────────────┤
//   │ status line                                               Clip │ Device        │  main/status-bar
//   └──────────────────────────────────────────────────────────────────────────────┘
//   + overlay (menus, popovers: the MOS wheel T3.13/T3.14), the root's last child.
//
// Slots are empty host widgets (Stack layout) that later views fill; their placeholders paint a background only,
// and a slot that gets a child paints nothing itself:
//   ShellSlot::TrackArea     main/body/top/track-area           T3.8 adds the arrangement view here
//   ShellSlot::Mixer         main/body/top/track-area/mixer     T3.10's mixer view (hidden until Ctrl+Alt+M)
//   ShellSlot::ClipDetail    main/body/bottom/detail/clip       T3.11's clip view (piano roll)
//   ShellSlot::DeviceDetail  main/body/bottom/detail/device     M4's device chain
//   ShellSlot::Browser       main/body/top/browser              M5 (hidden: ViewContext::browserVisible)
// Views install themselves in installDefaultContent() (src/views/src/shell/default_content.cpp; one call per view)
// so the shell itself does not change as views arrive.
//
// The shell binds the tree to the context: keymap (ctx.actions()/keymap(), the default keymap file unless
// ShellOptions says otherwise), info view (a TreeObserver showing infoFor(tree.hovered())), and the layout state
// (detail view visible/mode, browser). The Shell object must live as long as the tree content; the ViewContext must
// outlive both.

#include <memory>

#include "pq/core/result.h"
#include "pq/ui/keymap_file.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views {

enum class ShellSlot : uint8_t { TrackArea, Mixer, ClipDetail, DeviceDetail, Browser };

struct ShellOptions {
  // Keymap to bind; null = assets/keymaps/ableton.json (ViewContext::applyDefaultKeymap).
  const ui::KeymapFile* keymap = nullptr;
  bool installContent = true;  // call installDefaultContent (tests of the bare shell pass false)
};

class Shell {
public:
  // Builds the layout as `tree`'s root (replacing any root) and binds keymap, info view and layout state. The tree
  // should already have its viewport and resources (menus measure text when they open).
  static Result<std::unique_ptr<Shell>> create(ui::UiTree& tree, ViewContext& ctx, const ShellOptions& options = {});
  ~Shell();
  Shell(const Shell&) = delete;
  Shell& operator=(const Shell&) = delete;

  ui::Widget& slot(ShellSlot s) const noexcept;
  ui::OverlayLayer& overlay() const noexcept { return *overlay_; }
  ViewContext& context() const noexcept { return ctx_; }
  ui::UiTree& tree() const noexcept { return tree_; }

  // The window lost focus (SDL_EVENT_WINDOW_FOCUS_LOST; T3.x-fix-edit, M3 review L19): the drag under the pointer is
  // cancelled (UiTree::cancelInteraction: clips, notes, breakpoints, value boxes) and a document gesture left open
  // is cancelled, so an alt-tab never leaves undo disabled or a drag following the pointer.
  void cancelInteraction();

  // The info view's current text and the control bar's position readout (tests).
  const std::string& infoTitle() const noexcept;
  const std::string& infoBody() const noexcept;
  const std::string& positionText() const noexcept;

  struct Impl;

private:
  Shell(ui::UiTree& tree, ViewContext& ctx);
  ui::UiTree& tree_;
  ViewContext& ctx_;
  ui::OverlayLayer* overlay_ = nullptr;
  std::unique_ptr<Impl> impl_;
};

// Fills the shell's slots with the views that exist (T3.7: placeholders only). One line per view; the order is the
// order their actions are registered in.
void installDefaultContent(Shell& shell);

}  // namespace pq::views
