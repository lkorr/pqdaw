#pragma once

// The audio Clip View (T5.6, SPEC §9.4): the detail view's clip panel when the clip shown (ViewContext::detailClip) is
// an audio clip; the piano roll (ClipView) covers the same slot for note clips. Live's audio clip view, simplified:
//
//   audio-box (left)                         | sample (right)
//   name, file, rate · bits · channels · s   | ruler strip: start marker, Re-Pitch loop brace
//   Gain, Transpose, Detune, Reverse         | the whole sample's waveform (clip gain applied), the clip's region lit
//   Warp (Off / Re-Pitch), BPM, Loop         |
//   Fade In, Fade Out                        |
//
// Edits (each one undo step; drags and box drags are one store gesture, so the clip follows live and plays the change):
//   Gain       dB box (−inf … +24)                         ops::setAudioClipGain
//   Transpose  typed: "7" = 7 steps of the clip's tuning, "700c" = cents, "+7st" = semitones, "3/2" = a ratio
//                                                           ops::setAudioClipTranspose (warp Off only: greyed in Re-Pitch)
//   Detune     cents box (±50)                             ops::setAudioClipDetune (warp Off only)
//   Reverse    toggle                                      ops::setAudioClipsReverse
//   Warp       Off / Re-Pitch; switching to Re-Pitch seeds the BPM from the file's tempo tag or name, else from its
//              length as a whole number of 4/4 bars (sourceBpmFor)   ops::setAudioClipWarp
//   BPM        the Re-Pitch source tempo                   ops::setAudioClipWarp
//   Loop       Re-Pitch loop on / off                      ops::setAudioClipLoop
//   Fade In / Fade Out  milliseconds at the clip's tempo   ops::setAudioClipFades
//   start marker  drag in the ruler strip (Off: the source frame at the clip start; Re-Pitch: the content start,
//              snapped to 1/16 notes, Shift: free)          SetAudioClip
//   loop brace    Re-Pitch: drag its ends or the whole brace (1/16 notes, Shift: free)   ops::setAudioClipLoop
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
  bool setLoopEnabled(bool on);
  bool setFadesMs(double inMs, double outMs);

  // --- geometry (device px; tests script pointer input with these) ---
  ui::IRect sampleRect() const noexcept;    // the waveform
  ui::IRect rulerRect() const noexcept;     // the strip above it (marker, brace)
  float xOfFrame(double frame) const noexcept;
  double startMarkerFrame() const noexcept;  // where the start marker is drawn
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
  ui::Toggle* loop_ = nullptr;
  ui::DragBox* fadeIn_ = nullptr;
  ui::DragBox* fadeOut_ = nullptr;
};

// Adds the audio clip view to the shell's ClipDetail slot (next to the piano roll; one line in installDefaultContent).
AudioClipView& installAudioClipView(Shell& shell);
AudioClipView* findAudioClipView(Shell& shell);

}  // namespace pq::views
