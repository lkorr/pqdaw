#pragma once

// A device's front panel (T4.6, SPEC §7.3 / §9): a Live-style device frame — export jacks along the top edge, a title
// bar (activator, fold, title, preset slot) — over a body laid out from a PanelDesc (pq/views/devices/panel_desc.h):
// knobs, number boxes, toggles, choosers, tabs and displays (Saturator's transfer curve, EQ Eight's response with
// draggable band dots over a spectrum, a plain spectrum).
//
// The panel holds the values it shows and reports edits to a DeviceTarget: a param edit is a param write (no
// recompile); a structural const edit is a recompile (the target decides how; SPEC §7.5 crossfade). Who implements the
// target:
//   - now (T4.6): the stand-alone device preview (pq/views/devices/device_view.h): params → PatchPlayer::setParam,
//     consts → the generated host patch's text through the circuit editor's PatchEditor (check → hot swap);
//   - T4.1: the document's device instance (one undo step per gesture; consts edit the instance's call arguments,
//     which recompiles that track's unit).
// External changes (undo, automation, another view) come back through setParamValue / setConstValue and repaint only
// the controls bound to that name (and the displays that read it).
//
// Hooks for T4.7 (cross-device wiring): jacks() / jackRect() (one per `export`), onJackPressed, controlFor(param) (the
// drop target of a cable), setModulation(param, ring) (the live range ring on a knob).
//
// UI thread only.

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/devices/panel_desc.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views::devices {

class SpectrumFeed;

class DeviceTarget {
public:
  virtual ~DeviceTarget() = default;
  // A param value edit (internal units). A drag / type-in / reset is one gesture (T4.1: one undo step).
  virtual void paramGestureBegin(std::string_view /*param*/) {}
  virtual void setParam(std::string_view param, double value) = 0;
  virtual void paramGestureEnd(std::string_view /*param*/) {}
  // A structural setting (an `in … : real const`): recompiles the device.
  virtual void setConst(std::string_view name, double value) = 0;
  // The title bar's activator (Live: device on / off).
  virtual void setActive(bool on) = 0;
  // The analysed output of the device for the spectrum displays, or null (no spectrum: the display shows its grid).
  virtual SpectrumFeed* outputSpectrum() { return nullptr; }
  // The rate the device runs at (displays that show a digital response: EQ Eight's bilinear curves).
  virtual double sampleRate() const { return 48000.0; }
  // TMM.10: a face control's meta instance at name path `path` ("" = this chain meta device itself): its type (null:
  // the panel falls back to the type its patch is an expansion of, for ""), the target its face edits (null: the
  // type's defaults, read-only) and its voice reader (null: no dots).
  virtual const meta::Type* metaType(std::string_view /*path*/) const { return nullptr; }
  virtual std::unique_ptr<MetaTarget> metaTarget(std::string_view /*path*/, const meta::Type& /*type*/);
  virtual VoiceTapReader* metaVoices(std::string_view /*path*/) { return nullptr; }
};

class DevicePanel : public ui::Widget {
public:
  // `values`: initial param / const values by name (missing: the patch defaults).
  DevicePanel(std::string name, DeviceSignature sig, PanelDesc desc, DeviceTarget* target,
              const std::map<std::string, double, std::less<>>& values = {});
  ~DevicePanel() override;

  const DeviceSignature& signature() const noexcept { return sig_; }
  const PanelDesc& desc() const noexcept { return desc_; }
  void setTarget(DeviceTarget* t) noexcept { target_ = t; }

  // External value changes (no target call): repaint the bound controls and displays only.
  void setParamValue(std::string_view param, double value);
  void setConstValue(std::string_view name, double value);
  double paramValue(std::string_view param) const;
  double constValue(std::string_view name) const;
  // Selector state (view state, e.g. EQ Eight's selected band): rebinding the controls that use it.
  void select(std::string_view selector, int32_t index);
  int32_t selected(std::string_view selector) const;

  bool active() const noexcept { return active_; }
  void setActive(bool on);  // programmatic (no target call)
  bool folded() const noexcept { return folded_; }
  void setFolded(bool f);
  std::function<void()> onSizeChanged;  // folded / unfolded (the host re-lays out its chain)
  // Logical size of the whole panel as laid out now (folded: the title bar only).
  float logicalWidth() const noexcept;
  float logicalHeight() const noexcept;
  // TMM.10: the width when the panel is `panelHeight` logical px tall: a face panel (PanelDesc::fitFace) scales its
  // face to the height (within kFaceMinScale … kFaceMaxScale), any other panel is logicalWidth().
  float logicalWidthFor(float panelHeight) const noexcept;
  static constexpr float kFaceMinScale = 0.5f, kFaceMaxScale = 1.5f;
  static constexpr float kFaceMinWidth = 212.0f;  // the title bar keeps room for a meta module's name
  // TMM.10: the device-px rect of the control showing `param`: its widget's (visible), or the part of a face that
  // edits it; nullopt when nothing shows it now. (controlFor answers for widgets only.)
  std::optional<ui::IRect> controlRect(std::string_view param) const;
  // TMM.10: the face controls' voice readers may have become live (a note while nothing animates): they poll again.
  void wakeFaces();
  // The source started playing: displays that show the output spectrum start drawing it (if shown).
  void wakeDisplays();
  // T4.4-fix1: a user device (it plays its own patch, edited in the circuit editor): the title bar shows an "edited"
  // marker after the title. A double-click on the title runs onTitleDoubleClick when set (the Device view: Open in
  // Circuit View), else folds the panel as the triangle does.
  void setEdited(bool e);
  bool edited() const noexcept { return edited_; }
  std::function<void()> onTitleDoubleClick;
  // T5.11: the title bar's preset slot shows `name` ("Default" when empty); with onPresetClick set, a click on it runs
  // it (the Device view: the browser hot-swaps this device's presets).
  void setPresetName(std::string name);
  const std::string& presetName() const noexcept { return presetName_; }
  std::function<void()> onPresetClick;
  // The preset slot's device-px rect (empty: folded or too narrow; tests).
  ui::IRect presetSlotRect() const;
  // T4.17: the title (the device's own name, or the description's) and an in-place rename: a field over the title
  // (Enter commits through `commit` with the text, Esc or an empty text cancels, focus loss commits).
  void setTitle(std::string title);
  const std::string& title() const noexcept;
  void beginRename(std::function<void(const std::string&)> commit);
  bool renaming() const noexcept;

  // ---- T4.7 hooks ----
  struct Jack {
    std::string name;  // the export
    ui::IRect rect;    // device px, from the last layout
  };
  std::span<const Jack> jacks() const noexcept;
  std::function<void(std::string_view exportName, ui::Vec2 pos)> onJackPressed;
  // T4.7: the pointer events (Move, Up) that follow a jack press while the jack strip holds the pointer (a cable drag).
  std::function<void(const ui::PointerEvent& e)> onJackDrag;
  // The control bound to `param` right now (knob, box, toggle, chooser), or null.
  ui::Widget* controlFor(std::string_view param) const noexcept;
  void setModulation(std::string_view param, const ui::ModulationRing& ring);

  // Tests / scenes.
  ui::Widget* control(std::string_view id) const noexcept;
  ui::Widget& body() const noexcept { return *body_; }

  void paint(ui::PaintContext& ctx) override;
  void paintOver(ui::PaintContext& ctx) override;

  struct Impl;

private:
  friend struct Impl;
  DeviceSignature sig_;
  PanelDesc desc_;
  DeviceTarget* target_;
  std::unique_ptr<Impl> impl_;
  ui::Widget* body_ = nullptr;
  mutable std::vector<Jack> jacks_;
  bool active_ = true;
  bool folded_ = false;
  bool edited_ = false;
  std::string presetName_;
  ui::Widget* title_ = nullptr;
};

}  // namespace pq::views::devices
