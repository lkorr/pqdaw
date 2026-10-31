#pragma once

// The browser panel's parts (T5.10; private to src/browser): collection chips, the sidebar, the column header, the
// virtualised list, the preview strip.

#include <cstdint>
#include <string>

#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/browser/browser_panel.h"

namespace pq::views::browser {

namespace metrics {
inline constexpr float kRowHeight = 18.0f;      // list and sidebar rows
inline constexpr float kHeaderHeight = 18.0f;   // the list's column header
inline constexpr float kIndent = 12.0f;         // per tree level
inline constexpr float kSidebarWidth = 132.0f;
inline constexpr float kPanelWidth = 440.0f;    // the browser slot's width
inline constexpr float kChipSize = 12.0f;
inline constexpr float kStripHeight = 44.0f;
// Column widths (logical px); Name takes the rest.
inline constexpr float kTypeWidth = 40.0f, kDurWidth = 48.0f, kBpmWidth = 34.0f, kRateWidth = 40.0f;
}  // namespace metrics

// The colour of collection c (0..6: Live's red, orange, yellow, green, blue, purple, grey).
ui::Rgba collectionColour(uint32_t c) noexcept;

class Chips : public ui::Control {
public:
  explicit Chips(BrowserPanel& panel);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  ui::IRect chipRect(uint32_t c) const noexcept;

private:
  BrowserPanel& panel_;
};

class Sidebar : public ui::Control {
public:
  explicit Sidebar(BrowserPanel& panel);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  // The items in display order (headers have header = true).
  struct Item {
    std::string label;
    bool header = false;
    BrowserSource source{};
    ui::Rgba swatch = 0;
  };
  const std::vector<Item>& items() const noexcept { return items_; }
  void rebuild();
  ui::IRect itemRect(size_t i) const noexcept;

private:
  BrowserPanel& panel_;
  std::vector<Item> items_;
};

class ColumnHeader : public ui::Control {
public:
  explicit ColumnHeader(BrowserPanel& panel);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  // Device-px x ranges of the columns within `r` (Name first).
  static void columns(const ui::IRect& r, float scale, int32_t (&x0)[5], int32_t (&x1)[5]) noexcept;
  ui::IRect columnRect(BrowserColumn c) const noexcept;

private:
  BrowserPanel& panel_;
};

class List : public ui::Widget {
public:
  explicit List(BrowserPanel& panel);
  ui::ISize contentExtent(ui::ISize viewport) const override;
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { pressed_ = -1; }
  void arrange(ui::LayoutPass& pass) override;
  void onTimer(uint32_t id) override;
  // Row under a device-px point (−1: none) and a row's rect.
  int32_t rowAt(ui::Vec2 p) const noexcept;
  ui::IRect rowRect(int32_t row) const noexcept;
  int32_t rowHeight() const noexcept;
  // Scrolls the row into view.
  void reveal(int32_t row);
  // Inline rename of a row (User Library files): a text field over its name; Enter renames the file.
  void beginRename(int32_t row);
  bool renaming() const noexcept { return rename_ != nullptr; }
  ui::TextEdit* renameField() const noexcept { return rename_; }

private:
  void endRename(bool commit);
  BrowserPanel& panel_;
  int32_t pressed_ = -1;
  ui::Vec2 pressAt_{0, 0};
  bool dragStarted_ = false;
  ui::TextEdit* rename_ = nullptr;
  ui::Widget* doomed_ = nullptr;  // a finished rename field, removed on the next timer
  int32_t renameRow_ = -1;
};

// The selected sample's waveform: click = scrub (or start the preview there), the playhead while it plays.
class Waveform : public ui::Control {
public:
  explicit Waveform(BrowserPanel& panel);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  BrowserPanel& panel_;
};

class PreviewStrip : public ui::Widget {
public:
  explicit PreviewStrip(BrowserPanel& panel);
  void paint(ui::PaintContext& ctx) override;
  void sync();  // toggles / volume from the panel's state
  ui::Toggle& previewToggle() const noexcept { return *preview_; }
  ui::Toggle& syncToggle() const noexcept { return *sync_; }
  ui::DragBox& volume() const noexcept { return *volume_; }
  Waveform& waveform() const noexcept { return *wave_; }

private:
  BrowserPanel& panel_;
  ui::Toggle* preview_ = nullptr;
  ui::Toggle* sync_ = nullptr;
  ui::DragBox* volume_ = nullptr;
  Waveform* wave_ = nullptr;
};

}  // namespace pq::views::browser
