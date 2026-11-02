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
//   path     (TX.free-rack) the layer also draws the chain's own signal path (the notes into the first device, audio from
//            device to device along the frames' feet; a switched-off device passed by; free devices take no part), wires
//            into inputs (sockets on a frame's left edge: a drop target too), note-source wires (from the chain's left
//            end) and wires from the free-device rack (from the blocks' pins, or its collapsed tab) down to the knobs.
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

// Fixed sweeps (tests, scenes); zero while not live, as the engine's while it sleeps.
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
  // TX.free-rack: what a cable is — a document wire (a modulation wire into a param, a wire into an input: TX.patching's
  // parallel routes, a note-source wire), or a link of the chain's own signal path (DeviceChainView::chainLinks: series
  // audio, the notes into the first device; not a document wire: not selectable or deletable, its badge reads it).
  enum class Kind : uint8_t { Wire, Chain };
  struct Cable {
    doc::WireId wire;           // invalid: a chain link (or the cable being dragged)
    Kind kind = Kind::Wire;
    uint64_t key = 0;           // stable identity across refreshes (a wire's id; a link's ends)
    ui::Vec2 a, b;              // device px: source (jack) end, target (knob) end
    bool aDocked = false;       // the source end is on a jack (else at a track header / edge)
    bool bDocked = false;       // the target end is on its knob (else at its panel / a header / an edge)
    uint8_t da = 0, db = 0;     // TX.free-rack: which way each end leaves (DeviceChainView::EndDir)
    bool input = false;         // TX.free-rack: it ends on an input (a signal path, not a param)
    doc::WireStatus status = doc::WireStatus::Lowered;
    uint8_t rate = 0;           // 0 param, 1 audio, 2 voice (the source's rate, from the device kind and export)
    ui::IRect badge{};          // the badge's rect (empty: no badge)
    ui::IRect bounds{};         // what the cable paints (damage)
    std::string label;          // TX.free-rack: what its badge's tooltip says ("Saturator → Utility · audio (chain)")
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
  // T4.16c: while dragging, Crosshair over a knob it can wire, Arrow over the arrangement's rows, else NotAllowed.
  ui::Cursor cursorAt(ui::Vec2 pos) const override;
  // T4.16c: a hovered badge lights its cable (only the old and new cables repaint). TX.free-rack: by cable key.
  void hoverBadge(uint64_t key, bool on);
  doc::WireId hoveredWire() const noexcept;
  uint64_t hoveredKey() const noexcept { return hoverKey_; }
  // TX.free-rack: the cable a badge stands for (null: gone).
  const Cable* cableOf(uint64_t key) const noexcept;

  struct Impl;

private:
  void onLayout() override;  // TreeObserver
  void onStore(const doc::StoreEvent& e);
  void rebuildStatus();
  void endDrag(bool commit, ui::Vec2 pos);
  void updateDragCursor();

  ViewContext& ctx_;
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
    uint32_t hoverTicks = 0;  // drag ticks (1/60 s) over hoverTrack
    uint32_t timer = 0;
    ui::Cursor cursor = ui::Cursor::Crosshair;  // what a release at `pos` would do (updated on each move)
  } drag_;
  uint64_t hoverKey_ = 0;
};

// Installs the cable layer into ShellSlot::Cables and the view.cycleCables / wire.delete actions; links the Device
// view's jacks to it. After installDeviceChain.
CableLayer& installWiring(Shell& shell);
CableLayer* findCableLayer(Shell& shell) noexcept;

}  // namespace pq::views
