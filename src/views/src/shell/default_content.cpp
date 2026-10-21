// The content installed into the shell's slots. Each view adds one line here (and registers its actions in
// ctx.actions() from its installer, before the shell applies the keymap):
//   T3.8  installArrangement(shell);   → ShellSlot::TrackArea
//   T3.10 installMixer(shell);         → ShellSlot::Mixer
//   T3.11 installClipView(shell);      → ShellSlot::ClipDetail
//   T3.14 installMosWheel(shell);      → shell.overlay() (popover), the tuning.openMosWheel action
// Until a view is installed its slot shows a placeholder: a background and, for the detail view, an empty-state line.

#include "pq/views/shell/shell.h"
#include "shell_widgets.h"

namespace pq::views {

void installDefaultContent(Shell& shell) {
  static_cast<shell::SlotHost&>(shell.slot(ShellSlot::ClipDetail)).setHint("No clip selected");
  static_cast<shell::SlotHost&>(shell.slot(ShellSlot::DeviceDetail)).setHint("No devices");
}

}  // namespace pq::views
