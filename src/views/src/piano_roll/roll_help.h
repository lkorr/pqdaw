#pragma once

// KB.help: the piano roll's help panel (the `?` button left of Fold / Scale; private to pq_views).
//
// The text is DATA (roll_help_content.cpp): sections -> entries {gesture, title, text}. The widget here only lays it
// out (gesture | description, a section list on the left, scrollable) and never names a feature, so a task that
// changes what the roll does edits the table and nothing else.
//
// Gesture markup (HelpEntry::gesture):
//   {action.id}   the key that action has in the LOADED keymap, drawn as key caps ("Ctrl" + "G"); "unbound" when the
//                 keymap binds no key to it. Never write a key that the keymap owns as literal text.
//   [Alt]         a literal key cap (modifiers of mouse gestures, keys that are not keymap actions)
//   \n            a new line in the gesture column
//   anything else plain words
//
// Cost: built when the panel opens (one pass over the table, one keymap scan per {action}); laid out once per
// width / scale (cached); a paint draws only the entries inside the clip rect. Nothing runs while it is closed.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/actions.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"

namespace pq::views {
class ViewContext;
}

namespace pq::views::roll {

struct HelpEntry {
  std::string_view gesture;  // markup, see above
  std::string_view title;
  std::string_view text;     // one or two plain sentences
};

struct HelpSection {
  std::string_view title;
  std::string_view intro;  // a short paragraph under the heading (may be empty)
  std::span<const HelpEntry> entries;
};

// The table (roll_help_content.cpp).
std::span<const HelpSection> rollHelpSections() noexcept;

// One piece of a gesture as shown.
struct HelpToken {
  enum class Kind : uint8_t { Key, Word, Unbound, Break };
  Kind kind = Kind::Word;
  bool glue = false;  // drawn tight against the token before it and never wrapped away from it (inside a chord)
  std::string text;
};

// `markup` resolved against `keymap` (see the header comment).
std::vector<HelpToken> parseHelpGesture(std::string_view markup, const ui::Keymap& keymap);
// The tokens as plain text: "Ctrl+G", "Alt + drag a note's left edge", "unbound"; lines joined by '\n'.
std::string helpGestureText(std::span<const HelpToken> tokens);
// The chord `action` has in `keymap` ("Ctrl+G"; the piano roll's scope first); empty: no key.
std::string helpActionKeys(std::string_view action, const ui::Keymap& keymap);

class HelpContent;
class HelpRail;

class RollHelpPopover : public ui::Popover {
public:
  static constexpr float kWidth = 860.0f;      // logical px (less in a narrow window)
  static constexpr float kMaxHeight = 640.0f;  // logical px (less when the panels are shorter)

  // `within`: the widget it must stay inside (null: the layer); `left`: its left edge (device px; it is as wide as
  // the room from there to the right edge allows, up to kWidth).
  RollHelpPopover(const ui::Keymap& keymap, const ui::Widget* within, int32_t left);

  size_t sectionCount() const noexcept;
  size_t entryCount(size_t section) const noexcept;
  // The gesture of an entry as shown (helpGestureText).
  std::string gestureText(size_t section, size_t entry) const;
  // Scrolls so that the section's heading is at the top (as a click on its name in the list does).
  void showSection(size_t section);
  int32_t scrollY() const noexcept;
  int32_t maxScrollY() const noexcept;
  void scrollToY(int32_t y);

  ui::ISize contentExtent(ui::ISize viewport) const override;
  void arrange(ui::LayoutPass& pass) override;
  bool onKey(const ui::KeyEvent& e) override;
  bool onPointer(const ui::PointerEvent& e) override;
  ui::InfoText info() const override;

private:
  const ui::Widget* within_;
  int32_t left_;
  ui::Label* title_ = nullptr;
  ui::Label* hint_ = nullptr;
  HelpRail* rail_ = nullptr;
  ui::ScrollView* scroll_ = nullptr;
  HelpContent* content_ = nullptr;
  ui::Scrollbar* bar_ = nullptr;
};

// The open help panel of `from`'s tree (null: closed).
RollHelpPopover* openRollHelpPopover(const ui::Widget& from);
// Opens the panel over the roll (`roll`: the clip view, its bottom edge and the note area's left edge place it);
// when it is already open, closes it and returns null. `onClosed` runs once when it closes, for any reason.
RollHelpPopover* toggleRollHelp(ui::Widget& from, ViewContext& ctx, const ui::IRect& over, std::function<void()> onClosed);

}  // namespace pq::views::roll
