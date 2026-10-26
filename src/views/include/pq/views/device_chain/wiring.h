#pragma once

// Cross-device wiring in the views (T4.7, SPEC §7.3 / §9 / §11 "Wiring"; the document side is pq/document/model.h
// "Modulation wires", the lowering pq/document/lower.h).
//
//   wire     press a device's output jack (the strip along the top of its panel) and drag: a cable follows the pointer;
//            every knob it can modulate on the shown chain is outlined. Drop on a knob: one undo step (AddWire,
//            amount 50 %). To reach another track, hover its row in the arrangement (the Device view switches to it
//            after a moment; near the top or bottom edge the track list scrolls) and drop on its knob. Esc cancels.
//   amount   drag the cable's badge (the dot in the middle of the cable) up / down: −100 % … +100 % of the knob's
//            travel (one store gesture = one undo step; a Param change: no compile). Double-click resets it to 50 %.
//   delete   click the badge and press Delete, or its right-click menu (Delete Wire; also Invert, Reducer ▸ for a
//            voice source into a global target).
//   show     View > Cables (view.cycleCables): Show → Fade → Hide. Hidden, the layer does not exist for painting or
//            hit testing (no cost).
// Cables are feathered bezier ribbons (ui/lines.h) over the whole window, coloured by rate: a voice (per-note) source,
// an audio-rate source, a param-rate source (theme `cable.*`). A wire with an automatic 1-sample delay (it closes a
// loop) shows "z⁻¹" on its badge; an inactive wire (an end switched off) is drawn dashed and dim. An end on a track not
// shown in the Device view is drawn at that track's header in the arrangement (or the edge of its list when scrolled
// away), so a cross-track cable leaves the Device view and rises to the track it reaches. Ends are recomputed after
// every layout pass (TreeObserver::onLayout: scrolling, a splitter, folding), and only the old and new cable rects
// are damaged, so cables follow scrolling at frame rate and cost nothing while nothing moves.
//
// The live ring (SPEC §11 "a modulated knob shows a live ring of its modulation range"): each modulated knob of the
// shown chain draws the range its wires can sweep (knob ± Σ|amount|, a faint arc) and the part the modulation swept
// since the last poll (a bright arc), read from a ModulationSource (the app: the engine's tap buses, metered on the
// audio thread like a track meter) at 30 Hz while the source is live and a modulated knob is shown. Nothing polls
// otherwise.
//
// UI thread only.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;

// A modulated param, and how far its modulation swept it since the previous read (knob-travel units, ≥ 0).
struct ModTarget {
  doc::DeviceId device;
  std::string param;
};
struct ModSweep {
  bool known = false;  // false: no tap for it (not lowered yet, or no source)
  float up = 0.0f;     // the farthest it went above the knob
  float down = 0.0f;   // … below the knob
};

// Where the rings' levels come from (the app implements it over the engine's tap buses; plain data in and out).
class ModulationSource {
public:
  virtual ~ModulationSource() = default;
  // True while the sweeps may be non-zero (the engine runs). The rings poll only then, and once more after.
  virtual bool modulationLive() const noexcept = 0;
  // Sweeps since this consumer's previous read, for `targets` into `out` (same size).
  virtual void readModulation(std::span<const ModTarget> targets, std::span<ModSweep> out) noexcept = 0;
};

// Fixed sweeps (tests, scenes).
class FixedModulationSource final : public ModulationSource {
public:
  void set(doc::DeviceId device, std::string param, ModSweep s);
  void setLive(bool live) noexcept { live_ = live; }
  bool modulationLive() const noexcept override { return live_; }
  void readModulation(std::span<const ModTarget> targets, std::span<ModSweep> out) noexcept override;
  uint64_t reads() const noexcept { return reads_; }

private:
  struct Entry {
    doc::DeviceId device;
    std::string param;
    ModSweep sweep;
  };
  std::vector<Entry> entries_;
  bool live_ = false;
  uint64_t reads_ = 0;
};

enum class CableMode : uint8_t { Show, Fade, Hide };

class DeviceChainView;
class ArrangementView;

// The cable layer: a full-window widget between the shell's main content and its overlay (ShellSlot::Cables).
class CableLayer : public ui::Widget, private ui::TreeObserver {
public:
  CableLayer(ViewContext& ctx, Shell& shell);
  ~CableLayer() override;

  CableMode mode() const noexcept { return mode_; }
  void setMode(CableMode m);
  void cycleMode();  // Show → Fade → Hide → Show (view.cycleCables)

  // ---- dragging a new cable (DeviceChainView starts it from a jack press) ----
  void beginDrag(doc::DeviceId from, std::string port, ui::Vec2 pos);
  bool dragging() const noexcept { return drag_.active; }
  // The drop target under `pos` while dragging (none: device invalid).
  struct Target {
    doc::DeviceId device;
    std::string param;
  };
  Target targetAt(ui::Vec2 pos) const;

  // ---- what is drawn (tests) ----
  struct Cable {
    doc::WireId wire;           // invalid: the cable being dragged
    ui::Vec2 a, b;              // device px: source (jack) end, target (knob) end
    bool aDocked = false;       // the source end is on a jack (else at a track header / edge)
    bool bDocked = false;       // the target end is on its knob (else at its panel / a header / an edge)
    doc::WireStatus status = doc::WireStatus::Lowered;
    uint8_t rate = 0;           // 0 param, 1 audio, 2 voice (the source's rate, from the device kind and export)
    ui::IRect badge{};          // the badge's rect (empty: no badge)
    ui::IRect bounds{};         // what the cable paints (damage)
  };
  std::span<const Cable> cables() const noexcept { return cables_; }
  doc::WireId selected() const noexcept { return selected_; }
  void select(doc::WireId w);
  bool deleteSelected();  // wire.delete: one undo step
  // Recomputes the ends now and damages what moved (normally after each layout pass); true when something moved.
  bool refresh();
  // The wire under a badge at `pos` (invalid: none).
  doc::WireId badgeAt(ui::Vec2 pos) const noexcept;

  void paint(ui::PaintContext& ctx) override;
  void arrange(ui::LayoutPass& pass) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  void onTimer(uint32_t id) override;
  void onCaptureLost() override;
  void onInteractionCancelled() override;

  struct Impl;

private:
  void onLayout() override;  // TreeObserver
  void onStore(const doc::StoreEvent& e);
  void rebuildStatus();
  void endDrag(bool commit, ui::Vec2 pos);

  ViewContext& ctx_;
  Shell& shell_;
  std::unique_ptr<Impl> impl_;
  ViewContext::Subscription sub_, observer_;
  CableMode mode_ = CableMode::Show;
  std::vector<Cable> cables_;
  doc::WireId selected_;
  struct Drag {
    bool active = false;
    doc::DeviceId from;
    std::string port;
    ui::Vec2 pos{};
    doc::TrackId hoverTrack;
    double hoverSince = 0.0;
    uint32_t timer = 0;
  } drag_;
};

// Installs the cable layer into ShellSlot::Cables and the view.cycleCables / wire.delete actions; links the Device
// view's jacks to it. After installDeviceChain.
CableLayer& installWiring(Shell& shell);
CableLayer* findCableLayer(Shell& shell) noexcept;

}  // namespace pq::views
