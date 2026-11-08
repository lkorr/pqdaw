#pragma once

// GU.13 (GU.f2 / GU.f3, docs/plans/GU.md "One hierarchy"; QUESTIONS Q231): ONE PANEL SYSTEM. A block that is not a
// group — a native engine, an atom or a value block on a track's main path — is drawn in the Device view as a panel of
// the group-panel system (group_panel_face.h): a SYNTHETIC panel (leafPanel below: the engine's module panel, else one
// control element per param and setting), its GroupPanelFace bound to the document through makeDocPanelTarget, hosted
// by a PanelHost. So a leaf's knob IS a group panel's knob: one element code, one look, one set of gestures (drag,
// double-click default, the wheel, type-in, Delete, the modulation ring, the curve glyph).
//
// BlockFrame is the Live-style frame around such a panel: the export jacks along the top edge, a title bar (activator,
// fold triangle, title with in-place rename, the preset slot), the panel below it (scaled within minScale … maxScale to
// the frame's height: an engine's face grows with the Device view, generated controls keep their size). A rack's macro
// panel is one too (its own PanelTarget over the macros). UI thread only.

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/model.h"
#include "pq/document/panel.h"
#include "pq/ui/widget.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/panel/group_panel_face.h"

namespace pq::views::panel {

class BlockFrame final : public ui::Widget {
public:
  struct Options {
    std::string title;
    std::vector<std::string> exports;  // the export jacks along the top edge (none: no strip)
    float minScale = 1.0f, maxScale = 1.0f;  // the panel's scale range as the frame's height changes
    float minWidth = 0.0f;                   // logical px (a face frame keeps room for a module's name)
  };
  BlockFrame(std::string name, std::unique_ptr<GroupPanelFace> face, Options o);
  ~BlockFrame() override;

  PanelHost& host() const noexcept { return *host_; }
  GroupPanelFace& face() const noexcept;
  PanelTarget& target() const noexcept { return face().target(); }

  // ---- the frame ----
  bool active() const noexcept { return active_; }
  void setActive(bool on);                  // programmatic (no callback)
  std::function<void(bool on)> onActivate;  // the title bar's activator was clicked
  bool folded() const noexcept { return folded_; }
  void setFolded(bool f);
  void setFoldable(bool f);
  bool foldable() const noexcept { return foldable_; }
  std::function<void()> onSizeChanged;  // folded / unfolded (the host re-lays out its chain)
  // Logical size as laid out at scale 1 (folded: the title bar only), and the width when the frame is `frameHeight`
  // logical px tall (the panel scaled to the room it has, within the options' range).
  float logicalWidth() const noexcept;
  float logicalHeight() const noexcept;
  float logicalWidthFor(float frameHeight) const noexcept;
  float panelScaleFor(float frameHeight) const noexcept;
  static constexpr float kJackStripH = 10.0f, kTitleH = 20.0f, kPad = 6.0f, kFoldedW = 132.0f;
  // T5.11: the preset slot ("Default" when empty); with onPresetClick set a click runs it.
  void setPresetName(std::string name);
  const std::string& presetName() const noexcept { return presetName_; }
  std::function<void()> onPresetClick;
  ui::IRect presetSlotRect() const;  // device px (empty: folded or too narrow)
  // T4.17: the title and an in-place rename (Enter commits through `commit`, Esc / an empty text cancels).
  void setTitle(std::string title);
  const std::string& title() const noexcept;
  void beginRename(std::function<void(const std::string&)> commit);
  bool renaming() const noexcept;
  // The title bar's band (device px, below the jack strip).
  ui::IRect titleRect() const noexcept;

  // ---- T4.7: the export jacks ----
  struct Jack {
    std::string name;
    ui::IRect rect;  // device px, from the last layout
  };
  std::span<const Jack> jacks() const noexcept;
  std::function<void(std::string_view exportName, ui::Vec2 pos)> onJackPressed;
  std::function<void(const ui::PointerEvent& e)> onJackDrag;  // the Move / Up of a cable drag from a jack

  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  void paintOver(ui::PaintContext& ctx) override;

private:
  float panelHeight() const noexcept;
  PanelHost* host_ = nullptr;
  ui::Widget* strip_ = nullptr;
  ui::Widget* title_ = nullptr;
  Options opt_;
  mutable std::vector<Jack> jacks_;
  bool active_ = true, folded_ = false, foldable_ = true;
  std::string presetName_;
};

// The synthetic panel of a LEAF block `d` with signature `sig` (its patch's): a native engine with a module panel
// (doc::enginePanel: the Wavetable's, the Envelope's, the analysers') is that panel; any other block one control per
// visible param — a knob, a toggle for a bool, a number box for a stepped one — and one number box per structural
// setting, flowed into columns of 64 logical px. `face` (out): whether it is an engine's face (it scales with the frame).
doc::Panel leafPanel(const devices::DeviceSignature& sig, const doc::Device& d, bool* face = nullptr);

}  // namespace pq::views::panel
