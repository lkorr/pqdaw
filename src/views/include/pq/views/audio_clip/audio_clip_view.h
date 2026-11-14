#pragma once

// The audio Clip View (T5.6, SPEC §9.4; T6.8 warping; AC.look: drawn as a device panel): the detail view's clip panel
// when the clip shown (ViewContext::detailClip) is an audio clip; the piano roll (ClipView) covers the same slot for note
// clips. Live's audio clip view, simplified:
//
//   audio-box (left): the name strip, file, rate · bits · channels · s, then three cards (audio_clip/clip_controls.h)
//     Clip   Transpose (the big knob), Detune, Gain knobs; Reverse chip, Fade In / Fade Out fill boxes
//     Warp   the seven modes as a chip grid; BPM box with :2 and ×2 chips; Loop chip
//     <mode> the warp mode's settings (Beats / Tones / Texture / Complex Pro; hidden for the others)
//   sample (right): the loop strip (start marker, loop brace), the time ruler (bar.beat of the content for warped clips,
//     seconds of the file for Off), the warp strip (warped: the markers, each segment's speed), the waveform (filled:
//     the peak body with a brighter RMS body inside, per channel) on a dark field, the clip's region lit and the rest
//     dimmed; a warped clip on its CONTENT axis (beats of the clip, each segment between markers stretched as it plays)
//     with the beat grid, each segment tinted by its speed against the source (faster / slower: two hues, stronger the
//     further from 1x); faint lines at the sample's transients (SampleAnalysis); the fades as curves over the ends.
//
// Edits (each one undo step; drags and wheel runs are one store gesture, so the clip follows live and plays the change):
//   Transpose  knob, value = the resolved cents (±kMaxTransposeCents); the document keeps an exact doc::AudioTranspose.
//              Drag: steps of the clip's tuning (Steps); Shift-drag: fine, free cents (Cents). Up / Down (and the wheel,
//              one notch each): the next step of the tuning above / below the current pitch (Steps). Shift+Up / Down
//              (Shift+wheel): ± one period of the tuning, the unit kept (Steps ± its degrees per period, Cents ± the
//              period's cents, a Ratio × or ÷ the period when the period is 2/1, 3/1 or an EDO's exact equave, else
//              cents) — the period, not 1200 c, for a non-octave tuning (Bohlen-Pierce: 3/1). Typed (a digit while
//              focused, or double-click): "7" = 7 steps, "386.3c" / "386.3 ¢" = cents, "+7st" = semitones, "5/4" = an
//              exact ratio; Enter commits, Esc cancels. Delete: 0. Readout: "+7 stp" (steps), "+7 st" / "+386.3 ¢"
//              (cents), "5/4 · 386.3 ¢" (a ratio and its true size).   ops::setAudioClipTranspose (all but Re-Pitch)
//   Detune     knob ±50 ¢                                  ops::setAudioClipDetune (every mode but Re-Pitch)
//   Gain       knob, −inf … +24 dB                         ops::setAudioClipGain
//   Reverse    lit chip                                    ops::setAudioClipsReverse
//   Fade In / Fade Out  fill boxes, ms at the clip's tempo; the fill is the fade's share of the clip
//              ops::setAudioClipFades
//   Warp       chips Off / Re-Pitch / Beats / Tones / Texture / Complex / Complex Pro. From Off the BPM is seeded from
//              the file's tempo tag or name, else its detected tempo (confidence >= 0.6), else its length as a whole
//              number of 4/4 bars (sourceBpmFor)          ops::setAudioClipWarp
//   BPM, :2 ×2 the source tempo (halved / doubled)         ops::setAudioClipWarp
//   Loop       lit chip                                    ops::setAudioClipLoop
//   mode card  Beats: Preserve chips (Trans, 1/4 … 1/32), Transient Loop chips (Off, → forward, ↔ back and forth),
//              Envelope knob (%); Tones: Grain knob; Texture: Grain, Flux (%) knobs; Complex Pro: Formants (%),
//              Envelope (bins) knobs                      ops::setAudioClipWarpParams
//   Knobs as ui::Knob: drag (Shift fine), wheel, Up / Down, Delete / double-click = default, a digit types a value.
//   start marker  drag in the loop strip (Off: the source frame at the clip start; warped: the content start,
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
#include "pq/tuning/tuning.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views {

namespace audio_clip {
class SampleDisplay;
class TransposeKnob;
class ClipKnob;
class Chip;
class FillBox;
class Section;
}

// "7" → 7 steps, "700c" / "700 ¢" → cents, "+7st" → 700 cents, "3/2" → ratio; nullopt for anything else.
std::optional<doc::AudioTranspose> parseTranspose(std::string_view text);
std::string formatTranspose(const doc::AudioTranspose& t);
// The source tempo a clip switching to Re-Pitch starts from: the file's tempo (acid chunk, file name) when known,
// else the tempo at which the file's duration is a whole number of 4/4 bars (the nearest to 120 BPM).
double sourceBpmFor(double fileBpm, double durationSeconds);
// T6.8: the bars of "Warp as N-Bar Loop" for a region of `seconds`: the power of two (1 … 64) whose tempo is nearest
// (in octaves) to `bpm` (> 0: the detected tempo), else to 120 BPM.
// The bar count "Warp as N-Bar Loop" uses for `seconds` of source in bars of `beatsPerBar` quarter notes (M6 review
// M11: the signature at the clip's start): with a detected tempo (bpm > 0) the whole number of bars nearest to it;
// without, the power of two (1 … 64) whose tempo is nearest to 120 BPM. Always within kMinBpm … kMaxBpm when one is.
int32_t loopBarsFor(double seconds, double bpm, double beatsPerBar = 4.0);
// AC.look: the transpose knob's readout: steps "+7 stp", whole semitones "+7 st", cents "+386.3 ¢", a ratio with its
// true size "5/4 · 386.3 ¢" (`cents`: the resolved size; only a ratio shows it).
std::string transposeReadout(const doc::AudioTranspose& t, double cents);
// T6.8: the warp modes' names, index = doc::AudioWarp (the chips' texts in the Clip View).
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
  bool setTranspose(const doc::AudioTranspose& t);  // AC.look: the knob's exact value
  // AC.look: the transpose `count` steps (octave: periods) up (dir > 0) or down from the clip's, as the knob's keys do;
  // nullopt when it would leave ±kMaxTransposeCents. And the cents a transpose resolves to in the clip's tuning.
  std::optional<doc::AudioTranspose> steppedTranspose(int dir, bool octave, int count = 1) const;
  double transposeCents(const doc::AudioTranspose& t) const;
  // The degree of the clip's tuning nearest `cents` (a Steps transpose; no tuning: semitones).
  doc::AudioTranspose nearestStep(double cents) const;
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
  double beatsPerBarAtClip() const;  // quarter notes per bar at the clip's start (M11)
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
  friend class audio_clip::TransposeKnob;
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
  // AC.look: the clip's tuning (built on refresh; nullopt: none resolves): the transpose knob's steps.
  std::optional<tuning::Tuning> tuning_;
  audio_clip::TransposeKnob* transpose_ = nullptr;
  audio_clip::ClipKnob *detune_ = nullptr, *gain_ = nullptr;
  audio_clip::Chip* reverse_ = nullptr;
  audio_clip::FillBox *fadeIn_ = nullptr, *fadeOut_ = nullptr;
  audio_clip::Chip* warp_[7] = {};  // index = doc::AudioWarp
  audio_clip::FillBox* bpm_ = nullptr;
  audio_clip::Chip *halve_ = nullptr, *double_ = nullptr;  // T6.8
  audio_clip::Chip* loop_ = nullptr;
  // T6.8: the mode card (titled with the mode; shown when the mode has settings) and its controls.
  audio_clip::Section* mode_ = nullptr;
  ui::Widget* beatsCol_ = nullptr;
  audio_clip::Chip* preserve_[5] = {};   // Transients, 1/4, 1/8, 1/16, 1/32
  audio_clip::Chip* beatsLoop_[3] = {};  // Off, Forward, Back-Fwd
  audio_clip::ClipKnob *beatsEnv_ = nullptr, *grain_ = nullptr, *flux_ = nullptr, *formants_ = nullptr, *envOrder_ = nullptr;
};

// Adds the audio clip view to the shell's ClipDetail slot (next to the piano roll; one line in installDefaultContent).
AudioClipView& installAudioClipView(Shell& shell);
AudioClipView* findAudioClipView(Shell& shell);

}  // namespace pq::views
