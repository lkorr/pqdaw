#pragma once

// Track mixer (T3.10, SPEC §9.3): the mixer section of the arrangement's track headers and the basic mixer view
// (Ctrl+Alt+M), both built from one widget, MixerStrip, in two layouts.
//
// Header layout (Live 12's arrangement header, right of the name strip; logical px, one 20 px line per row height):
//   line 0  [ volume box ] ║ [ activator (track number) ] [ S ] [ arm ● | Pre/Post ]   ▌meter
//   line 1  [ pan          ] ║ [ output routing ▾ ]                                    ▌
//   line 2+ [ A send ][ B send ][ C send ] …                                             ▌
// The tuning chip sits under the track name (TrackHeader::chipSection: the name column's second line, which Live
// leaves empty). Lines that do not fit the row height are hidden (Live shows more of the mixer as a track grows). Groups have no
// arm, returns show their letter and a Pre/Post switch instead, the master has volume, pan and its meter only.
//
// Mixer view (ShellSlot::Mixer, toggled with Ctrl+Alt+M; it takes the track area while shown): one vertical strip
// per visible track (members of folded groups hidden, like the arrangement), then the returns, the master pinned at
// the right. Strip: name in the track colour, output, tuning, sends, pan, volume box, fader + meter, activator,
// solo, arm.
//
// Automation (T3.15): an automated control carries a small red marker in its corner (grey while the automation is
// overridden by a hand-made change, ViewContext::automationOverridden), as in Live.
//
// Behaviour (Ableton): the activator is the track's mute (Q15: a Param, no recompile; clicking it toggles every
// selected track when its own track is selected, like `0`); Solo is exclusive, Ctrl-click adds (doc::ops::soloTrack);
// arm likewise. Volume, pan and send boxes are Live number boxes (vertical drag, Shift fine, double-click to type,
// Delete = default) and the mixer view's fader; each drag / type-in / key is one store gesture = one undo step whose
// ChangeSet is paramOnly. The output chooser lists Master, the groups the track may feed, and Sends Only
// (Structural). The tuning chip is T3.14's TuningChip for the track (its main tuning's colour; a click opens the MOS
// wheel popover for the track); a right-click opens the assignment menu (Inherit / a main tuning / MOS Wheel…).
//
// Meters: PQ Meter-style ui::Meter per strip, fed from a plain-data MeterSource (the app implements it over the
// engine's taps, T3.17). The views poll it on a timer (kMeterPollSeconds) only while it reports live(): one batched
// read for every strip, and a meter repaints only when a drawn pixel changes, so a stopped project costs nothing.
//
// Solo semantics for the sound live in pq/document/audibility.h (effectiveAudible), shared with T3.17's lowering.
//
// Theme: group `mixer.*` (sampled from the user's Live 12 screenshot, Q47). UI thread only; nothing here is RT-safe.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/model.h"
#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;
class MixerController;

// ---- meters --------------------------------------------------------------------------------------------------------

// Where the meters' levels come from. Plain data in, plain data out: the views never see the engine.
class MeterSource {
public:
  virtual ~MeterSource() = default;
  // True while levels may be above silence (transport running, tails decaying, monitoring). The views poll only
  // then; after it turns false they read once more (to show the silence) and stop.
  virtual bool live() const noexcept = 0;
  // Levels for `ids` into `out` (same size): one call per poll for every strip. Unknown ids read as silence.
  virtual void read(std::span<const doc::TrackId> ids, std::span<ui::MeterLevels> out) noexcept = 0;
  // Clears a track's clip lamps (they latch until clicked, as in Live; T4.16-fix1). The next read shows it.
  virtual void resetClip(doc::TrackId /*id*/) noexcept {}
};

// Fixed levels per track (tests, screenshot scenes, benchmarks).
class FixedMeterSource final : public MeterSource {
public:
  void set(doc::TrackId id, const ui::MeterLevels& l);
  void setLive(bool live) noexcept { live_ = live; }
  bool live() const noexcept override { return live_; }
  void read(std::span<const doc::TrackId> ids, std::span<ui::MeterLevels> out) noexcept override;
  void resetClip(doc::TrackId id) noexcept override;
  uint64_t reads() const noexcept { return reads_; }

private:
  std::vector<std::pair<doc::TrackId, ui::MeterLevels>> levels_;  // sorted by id
  bool live_ = false;
  uint64_t reads_ = 0;
};

namespace mixer_metrics {
inline constexpr float kHeaderMixerWidth = 180.0f;  // the header's mixer section (ArrangementView::setHeaderMixerWidth)
inline constexpr float kLine = 20.0f;               // one header line
inline constexpr float kBoxTop = 3.0f;              // box offset inside a line
inline constexpr float kBox = 14.0f;                // box height
inline constexpr float kStripWidth = 80.0f;         // a mixer-view strip
inline constexpr double kMeterPollSeconds = 1.0 / 30.0;
}  // namespace mixer_metrics

// ---- value mapping ---------------------------------------------------------------------------------------------------

// The volume boxes and faders work on a fader position in [0, 1]: dB = 6 + 40·log10(pos) (an amplitude-squared law:
// 0 dB at 0.708, −20 dB at 0.282), clamped to [kMinVolumeDb, kMaxVolumeDb]; positions at or below −70 dB read −inf.
double volumeToFader(double db) noexcept;
double faderToVolume(double pos) noexcept;
// "-inf", "-12.0", "0.0", "6.0" (Live shows the number without its unit in the boxes).
std::string formatVolume(double db);
// Pan in [−1, 1] as Live shows it: "C", "25L", "50R".
std::string formatPan(double pan);
// Type-in parsers ("-inf" / "-12" / "-12 dB"; "C" / "25L" / "50R" / "-25"). nullopt = unparsable.
std::optional<double> parseVolume(std::string_view text);
std::optional<double> parsePan(std::string_view text);

// ---- strip ---------------------------------------------------------------------------------------------------------

enum class StripLayout : uint8_t { Header, Vertical };

class MixerStrip : public ui::Widget {
public:
  MixerStrip(std::string name, std::shared_ptr<MixerController> controller, StripLayout layout);
  ~MixerStrip() override;

  // Shows `track` of project `p` (its kind decides which controls exist). Cheap when nothing changed: controls are
  // updated in place and repaint only when their value changes; a control the user is dragging is left alone.
  void bind(const doc::Project& p, const doc::Track& track);
  doc::TrackId trackId() const noexcept { return id_; }
  doc::TrackKind kind() const noexcept { return kind_; }
  StripLayout layoutKind() const noexcept { return layout_; }

  // The strip's controls (tests). Null when the track kind has none.
  ui::DragBox* volume() const noexcept { return volume_; }
  ui::Fader* fader() const noexcept { return fader_; }
  ui::DragBox* pan() const noexcept { return pan_; }
  ui::Toggle* activator() const noexcept { return activator_; }
  ui::Toggle* solo() const noexcept { return solo_; }
  ui::Toggle* arm() const noexcept { return arm_; }
  ui::Toggle* post() const noexcept { return post_; }
  size_t sendCount() const noexcept { return sends_.size(); }
  ui::DragBox* send(size_t i) const noexcept { return i < sends_.size() ? sends_[i].box : nullptr; }
  doc::TrackId sendTarget(size_t i) const noexcept { return i < sends_.size() ? sends_[i].target : doc::TrackId{}; }
  ui::Control* output() const noexcept { return output_; }
  ui::Control* tuningChip() const noexcept { return chip_; }
  ui::Meter* meter() const noexcept { return meter_; }
  const std::string& title() const noexcept { return title_; }

  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  // T3.15: Live's automation marker on automated controls (red; grey while overridden).
  void paintOver(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  // Automated / overridden controls (bit 0 volume, 1 pan, 2 activator, 3 + i send i), for tests.
  uint32_t automatedMask() const noexcept { return automated_; }
  uint32_t overriddenMask() const noexcept { return overridden_; }

private:
  struct SendBox {
    doc::TrackId target;
    ui::DragBox* box = nullptr;
  };
  void build(doc::TrackKind kind);
  void syncSends(const doc::Project& p, const doc::Track& t);
  void resyncToggles();  // after a toggle's edit: the document's state (a refused edit flips the button back)
  void arrangeHeader(ui::LayoutPass& pass);
  void arrangeVertical(ui::LayoutPass& pass);

  std::shared_ptr<MixerController> ctl_;
  StripLayout layout_;
  doc::TrackId id_;
  doc::TrackKind kind_ = doc::TrackKind::Midi;
  bool built_ = false;
  std::string title_;  // vertical strips: the track name
  uint32_t colour_ = 0;
  ui::DragBox* volume_ = nullptr;
  ui::Fader* fader_ = nullptr;
  ui::DragBox* pan_ = nullptr;
  ui::Toggle* activator_ = nullptr;
  ui::Toggle* solo_ = nullptr;
  ui::Toggle* arm_ = nullptr;
  ui::Toggle* post_ = nullptr;
  std::vector<SendBox> sends_;
  ui::Control* output_ = nullptr;
  ui::Control* chip_ = nullptr;
  ui::Meter* meter_ = nullptr;
  uint32_t automated_ = 0;   // T3.15
  uint32_t overridden_ = 0;
};

// ---- mixer view ----------------------------------------------------------------------------------------------------

class MixerView : public ui::Widget {
public:
  MixerView(ViewContext& ctx, std::shared_ptr<MixerController> controller);
  ~MixerView() override;

  // Ctrl+Alt+M (view.toggleMixer): shows / hides the view (the arrangement takes the track area back).
  bool shown() const noexcept;
  void setShown(bool on);
  void toggle() { setShown(!shown()); }
  // Strips in display order: tracks (folded groups' members hidden), returns, then the master.
  std::span<MixerStrip* const> strips() const noexcept { return strips_; }
  MixerStrip* strip(doc::TrackId id) const noexcept;

  // The meter feed (null: no meters; the app sets it, T3.17). Shared by the header strips. Not owned.
  void setMeterSource(MeterSource* source);
  // Polls the source now (and keeps polling while it is live).
  void wakeMeters();
  // One poll now: a batched read for every strip's meter (benchmarks, tests). True while the source is live.
  bool pollMeters();
  MixerController& controller() const noexcept { return *ctl_; }

  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  void onTimer(uint32_t id) override;

private:
  void sync();
  ViewContext& ctx_;
  std::shared_ptr<MixerController> ctl_;
  ViewContext::Subscription docSub_, viewSub_;
  ui::ScrollView* scroll_ = nullptr;
  ui::Widget* row_ = nullptr;   // the scroll view's content
  ui::Widget* pinned_ = nullptr;  // the master strip's host at the right
  std::vector<MixerStrip*> strips_;
  bool stale_ = true;
  uint32_t meterTimer_ = 0;
};

// Installs the mixer: the mixer view in ShellSlot::Mixer, the arrangement headers' mixer section (through
// ArrangementView::setHeaderDecorator) and the view.toggleMixer action. Call after installArrangement.
MixerView& installMixer(Shell& shell);
// The installed mixer view (nullptr when none).
MixerView* findMixer(const Shell& shell) noexcept;

}  // namespace pq::views
