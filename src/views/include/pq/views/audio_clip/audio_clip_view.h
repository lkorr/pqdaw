#pragma once

// The audio Clip View (T5.6, SPEC §9.4; T6.8 warping): the detail view's clip panel when the clip shown
// (ViewContext::detailClip) is an audio clip; the piano roll (ClipView) covers the same slot for note clips. Live's
// audio clip view, simplified:
//
//   audio-box (left)                         | sample (right)
//   name, file, rate · bits · channels · s   | ruler strip: start marker, loop brace
//   Clip: Gain, Transpose, Detune, Reverse,  | warp strip (warped clips): the warp markers
//         Fade In, Fade Out                  | the sample's waveform: an Off clip's whole file on a frame axis; a warped
//   Sample: Warp (7 modes), BPM, :2 / *2,    |   clip's on its CONTENT axis (beats of the clip, each segment between
//         Loop, the mode's settings          |   markers stretched as it plays), with the beat grid; the clip's region
//                                            |   lit, faint lines at the sample's transients (SampleAnalysis)
//
// Edits (each one undo step; drags and box drags are one store gesture, so the clip follows live and plays the change):
//   Gain       dB box (−inf … +24)                         ops::setAudioClipGain
//   Transpose  typed: "7" = 7 steps of the clip's tuning, "700c" = cents, "+7st" = semitones, "3/2" = a ratio
//                                                           ops::setAudioClipTranspose (every mode but Re-Pitch)
//   Detune     cents box (±50)                             ops::setAudioClipDetune (every mode but Re-Pitch)
//   Reverse    toggle                                      ops::setAudioClipsReverse
//   Warp       Off / Re-Pitch / Beats / Tones / Texture / Complex / Complex Pro. From Off the BPM is seeded from the
//              file's tempo tag or name, else its detected tempo (confidence >= 0.6), else its length as a whole
//              number of 4/4 bars (sourceBpmFor)          ops::setAudioClipWarp
//   BPM, :2 *2 the source tempo (halved / doubled)         ops::setAudioClipWarp
//   Loop       loop on / off                               ops::setAudioClipLoop
//   mode rows  Beats: Preserve (Transients, 1/4 … 1/32), Loop Mode (Off / Forward / Back-Fwd), Envelope; Tones: Grain;
//              Texture: Grain, Flux; Complex Pro: Formants, Envelope   ops::setAudioClipWarpParams
//   Fade In / Fade Out  milliseconds at the clip's tempo   ops::setAudioClipFades
//   start marker  drag in the ruler strip (Off: the source frame at the clip start; warped: the content start,
//              snapped to 1/16 notes, Shift: free)          SetAudioClip
//   loop brace    warped: drag its ends or the whole brace (1/16 notes, Shift: free)   ops::setAudioClipLoop
//   warp markers  (warped clips) double-click the waveform or the strip: add one (at a transient within reach, else
//              where clicked); double-click a marker: remove it; drag a marker: move it in content time, its source
//              frame kept, so the audio around it stretches (1/16 notes, Shift: free); Ctrl-drag: slide the source
//              under it (its content tick kept); click selects, Delete removes the selected one; Esc cancels a drag.
//              ops::addWarpMarker / moveWarpMarker / slideWarpMarker / removeWarpMarker
//   right-click   Warp From Here (at the marker or point: markers after it go, the source reads at the detected tempo
//              from there), Warp as N-Bar Loop (N the power of two nearest the detected tempo, else 120 BPM), Delete
//              Warp Marker.  ops::warpFromHere / warpAsLoop (an Off clip warps in the "Default warp mode" preference)
//
// UI thread only.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views {

namespace audio_clip {
class SampleDisplay;
}

// "7" → 7 steps, "700c" / "700 ¢" → cents, "+7st" → 700 cents, "3/2" → ratio; nullopt for anything else.
std::optional<doc::AudioTranspose> parseTranspose(std::string_view text);
std::string formatTranspose(const doc::AudioTranspose& t);
// The source tempo a clip switching to Re-Pitch starts from: the file's tempo (acid chunk, file name) when known,
// else the tempo at which the file's duration is a whole number of 4/4 bars (the nearest to 120 BPM).
double sourceBpmFor(double fileBpm, double durationSeconds);
// T6.8: the bars of "Warp as N-Bar Loop" for a region of `seconds`: the power of two (1 … 64) whose tempo is nearest
// (in octaves) to `bpm` (> 0: the detected tempo), else to 120 BPM.
int32_t loopBarsFor(double seconds, double bpm);
// T6.8: the warp chooser's items, index = doc::AudioWarp.
inline constexpr const char* kWarpModeNames[] = {"Off", "Re-Pitch", "Beats", "Tones", "Texture", "Complex", "Complex Pro"};

class AudioClipView : public ui::Widget {
public:
  explicit AudioClipView(ViewContext& ctx);
  ~AudioClipView() override;

  bool hasClip() const noexcept { return clip_ != nullptr; }
  const doc::ClipRef& clipRef() const noexcept { return ref_; }
  const doc::AudioClip* clip() const noexcept { return clip_.get(); }

  // --- edits (also what the controls do; tests call them directly) ---
  bool setGain(float db);
  bool setTranspose(std::string_view text);  // false: not a transpose (status line)
  bool setDetune(float cents);
  bool setReverse(bool on);
  bool setWarp(doc::AudioWarp warp);
  bool setSourceBpm(double bpm);
  bool scaleSourceBpm(double factor);  // T6.8: :2 / *2
  bool setLoopEnabled(bool on);
  bool setFadesMs(double inMs, double outMs);
  bool setWarpParams(const doc::WarpParams& p);  // T6.8
  // T6.8: the warp commands (also the context menu's). `content`: a content tick of the clip.
  bool warpFromHere(Ticks content);
  bool warpAsLoop();
  bool removeWarpMarker(size_t index);
  // The tempo the warp commands use: the sample's detected tempo when confident (>= 0.6), else 0.
  double detectedBpm();

  // --- geometry (device px; tests script pointer input with these) ---
  ui::IRect sampleRect() const noexcept;    // the waveform
  ui::IRect rulerRect() const noexcept;     // the strip above it (marker, brace)
  float xOfFrame(double frame) const noexcept;
  double startMarkerFrame() const noexcept;  // where the start marker is drawn
  // T6.8: the warp strip (warped clips; empty otherwise), x of a content tick (warped) and of warp marker i.
  ui::IRect warpStripRect() const noexcept;
  float xOfContent(double content) const noexcept;
  float warpMarkerX(size_t i) const noexcept;
  int32_t selectedWarpMarker() const noexcept;  // −1: none
  // Opens the sample's context menu at device position `at` (what a right-click there does).
  ui::Menu* openSampleMenu(ui::Vec2 at);
  ui::Widget& box() noexcept { return *box_; }

  void paint(ui::PaintContext& ctx) override;
  void arrange(ui::LayoutPass& pass) override;
  bool onKey(const ui::KeyEvent& e) override;  // Esc cancels a marker / loop drag (M5 review M8)
  bool dragging() const noexcept;

  // Internal (the display).
  ViewContext& context() noexcept { return ctx_; }
  void refresh();

private:
  friend class audio_clip::SampleDisplay;
  bool dispatch(const doc::ops::EditResult& e, std::string_view what);
  void bindGesture(ui::ValueControl& c, std::string label, std::function<void(double)> apply);

  ViewContext& ctx_;
  doc::ClipRef ref_{};
  doc::AudioClipPtr clip_;
  doc::Snapshot snap_;
  ViewContext::Subscription docSub_, viewSub_;
  // Probe of the clip's file (bits, tempo tag), cached per resolved path.
  std::string probedPath_;
  uint16_t bits_ = 0;
  double fileBpm_ = 0.0;
  std::string container_;

  ui::Widget* box_ = nullptr;
  audio_clip::SampleDisplay* display_ = nullptr;
  ui::Label* name_ = nullptr;
  ui::Label* file_ = nullptr;
  ui::Label* format_ = nullptr;
  ui::DragBox* gain_ = nullptr;
  ui::TextEdit* transpose_ = nullptr;
  ui::DragBox* detune_ = nullptr;
  ui::Toggle* reverse_ = nullptr;
  ui::Dropdown* warp_ = nullptr;
  ui::DragBox* bpm_ = nullptr;
  ui::Button *halve_ = nullptr, *double_ = nullptr;  // T6.8
  ui::Toggle* loop_ = nullptr;
  // T6.8: the mode rows (shown for their mode) and their controls.
  ui::Widget *preserveRow_ = nullptr, *beatsLoopRow_ = nullptr, *beatsEnvRow_ = nullptr, *grainRow_ = nullptr,
             *fluxRow_ = nullptr, *formantsRow_ = nullptr, *envOrderRow_ = nullptr;
  ui::Dropdown *preserve_ = nullptr, *beatsLoop_ = nullptr;
  ui::DragBox *beatsEnv_ = nullptr, *grain_ = nullptr, *flux_ = nullptr, *formants_ = nullptr, *envOrder_ = nullptr;
  ui::DragBox* fadeIn_ = nullptr;
  ui::DragBox* fadeOut_ = nullptr;
};

// Adds the audio clip view to the shell's ClipDetail slot (next to the piano roll; one line in installDefaultContent).
AudioClipView& installAudioClipView(Shell& shell);
AudioClipView* findAudioClipView(Shell& shell);

}  // namespace pq::views
