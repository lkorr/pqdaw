// The Sampler engine's face (SM.10, docs/plans/SM.md "Face (👁)" Closed): an oval plate holding the sample's waveform,
// a glowing playhead per sounding voice. The visual family of the Envelope's and the Noise's faces (a dark body, a quiet
// well, the signal in the accent, voices as glowing neon marks). Label-free:
//
//   waveform  the played region (Start … End) of the sample through the shared renderer (WF.1: ui::drawWaveform — the
//             overview, or below its resolution the samples themselves, asked of the target and drawn when they arrive:
//             a short sample is its samples, never the overview stretched), all channels in the one lane, drawn as a
//             STATIC LAYER keyed on the sample, Start, End, the overview's progress and the waveforms' revision (the
//             host's tile: re-rasterised only when one of them changes); no sample: a flat line and "drop a sample"
//   region    along the well's foot, the whole sample as a hairline with the played region (Start … End) lit and the
//             One-Shot / Slice fades as short ramps at its ends (a modulated Start / End / fade shows its ghost here)
//   voices    each sounding voice a vertical playhead with a glow at where it reads (the engine's voice tap, request 1:
//             the playhead or a wired Position, 0 … 1 of the sample), fading in and out over 80 ms
// The controls are the Sampler group's panel's (pq/document/modules.h samplerBlocks: Transpose · Filter · Volume, the
// Expand chevron); this face has none. The group's panel shows it through its `samplewave` element (the well).
// CPU: nothing while no voice sounds (the reader is not polled); a hidden face (Face::setShown false) tells its reader,
// which lets go of the tap (no reader there: the audio side's write is one relaxed load). World units 152 × 84.
//
// SM.11 (docs/plans/SM.md "Face (👁)" Expanded): two more sections, outside the closed face's world (drawn only through
// the Sampler group's panel, each clipped to its section; a paint whose clip misses one skips it):
//   editor  `sampleedit` (kEditor, 328 × 120): the sample's waveform (WF.1: ui::drawWaveform over the VIEW, the whole
//           sample until zoomed — the wheel zooms about the pointer down to 64 device px per sample, Shift + wheel
//           scrolls, a drag on the ruler scrolls sideways and zooms up / down, a double-click on the ruler (no warp
//           markers shown) or on the strip under the waveform shows the whole sample again; view state: no undo step,
//           not saved; a handle dragged past an edge scrolls the view, Start / End flags out of view wait at the edge),
//           the played region lit (Start / End drawn as handles on the flag strip under the waveform), the loop bar
//           (Classic with a loop: its ends and its body drag) with the loop fade shaded before both ends, the One-Shot /
//           Slice fades as shaded ramps, the slice lines (Slice mode, or Beats: drag one; double-click the waveform to
//           add one, a line to remove it), a ruler above (seconds with warp Off; else beats through the warp markers —
//           pq/dsp/warp.h WarpBeatLaw — or sampleBpm), the markers on it in the stretch modes (drag one along
//           the ruler: its frame moves, its beat stays; double-click the ruler to add one at the beat it reads there,
//           one to remove it), each sounding voice's playhead. Every drag is one undo step (MetaTarget begin … end: the
//           params and the data in one gesture), every double-click one step; the data never recompiles.
//   root    `sampleroot` (kRoot, 120 × 16): the root as "261.63 Hz", or (data `rootText` set) as the nearest identity of
//           the track's tuning and the cents left over ("C3 +2.0¢", MetaTarget::pitchName); click it to type Hz
//           ("440", "440 Hz") or an identity ("A3", "7\31 3 -5¢"), Return commits (one step: root and rootText
//           together); the Hz | ID cells switch the display (an identity keeps the root exactly where it is).
//
// SM.13 (docs/plans/SM.md "Granular", "Face"): in Gran mode (mode 3) —
//   region  the GRAIN REGION on the closed well and on the editor's waveform: from Position (0 … 1 of Start … End) one
//           grain size wide, the spray time shaded either side; drag its body to move Position, its right edge to set
//           the grain size (one undo step per drag)
//   grains  a glowing dot per live grain (the engine's grain tap, request 2, through the voice reader: grains()) where it
//           reads, its size and glow by its window's amplitude, its height by its pan, in place of the playheads; the
//           reader holds the tap only while the face is shown (hidden: no reader there, the kernel's write is one load)
//   ratios  `sampleharm` (kHarm, 328 × 24): the harmonic grains' ratio list (the data's `harmonics`, (n, d) pairs; none:
//           1, 2, 3/2, 3, 4, 5) as chips: click one to type n or n/d (whole numbers 1 … 65536; Return commits, one undo
//           step), its × removes it, + adds the next whole number; data writes, never a recompile

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "../common/waveform_data.h"
#include "faces.h"
#include "pq/dsp/warp_voice.h"
#include "pq/media/waveform.h"
#include "pq/meta/sampler.h"
#include "pq/ui/waveform.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

constexpr float kW = 152.0f, kH = 84.0f;
constexpr Vec2 kCentre{kW * 0.5f, kH * 0.5f};
constexpr float kRx = kW * 0.5f - 0.6f, kRy = kH * 0.5f - 0.6f;
constexpr Rect kWell{24.0f, 8.0f, 128.0f, 42.0f};  // the waveform's well (the panel's samplewave element)
constexpr float kInset = 3.0f;                     // the waveform inside the well
constexpr float kFade = 0.08f;
constexpr int kEllipseSteps = 64;
constexpr size_t kMaxGrainDots = 512;  // SM.13: the grains read per poll (TapVoices::kMaxGrains)

// SM.11: the expanded editor's and the root field's sections (face units; the panel's `sampleedit` / `sampleroot`).
constexpr Rect kEditor{0.0f, 200.0f, 328.0f, 320.0f};
constexpr float kEdX0 = 6.0f, kEdX1 = 322.0f;            // the whole sample, 0 … 1
constexpr float kRulerY0 = 201.0f, kRulerY1 = 213.0f;    // seconds or beats; the warp markers
constexpr float kWaveY0 = 215.0f, kWaveY1 = 299.0f;      // the waveform (the loop bar along its top)
constexpr float kLoopY1 = 221.0f;
constexpr float kFlagY0 = 300.0f, kFlagY1 = 310.0f;      // the Start / End handles
constexpr Rect kRoot{0.0f, 330.0f, 120.0f, 346.0f};
// SM.12: the slicing row (`sampleslice`, 328 × 30): Transient | Beat | Region | Manual, the amount, Slice to Notes.
constexpr Rect kSlicing{0.0f, 360.0f, 328.0f, 390.0f};
constexpr float kSliceRowY0 = 361.0f, kSliceRowY1 = 375.0f;
constexpr Rect kSliceAmount{208.0f, 361.0f, 262.0f, 375.0f}, kSliceNotes{266.0f, 361.0f, 327.5f, 375.0f};
// SM.13: the harmonic ratio chips (`sampleharm`, 328 × 24): two rows of kHarmPerRow chips beside the label.
constexpr Rect kHarm{0.0f, 400.0f, 328.0f, 424.0f};
constexpr float kHarmX0 = 40.0f, kHarmChipW = 28.0f, kHarmGap = 2.0f, kHarmX = 8.0f;  // kHarmX: the × at a chip's right
constexpr size_t kHarmPerRow = 9, kHarmMax = 2 * kHarmPerRow - 1;                       // (+ takes the last cell)
Rect harmCell(size_t i) noexcept {
  const float x0 = kHarmX0 + static_cast<float>(i % kHarmPerRow) * (kHarmChipW + kHarmGap);
  const float y0 = kHarm.y0 + 0.5f + (i >= kHarmPerRow ? 12.0f : 0.0f);
  return {x0, y0, x0 + kHarmChipW, y0 + 11.0f};
}
constexpr const char* kSliceByNames[4] = {"Manual", "Transient", "Beat", "Region"};  // (doc::ops::SliceBy order)
constexpr int kSliceCellOrder[4] = {1, 2, 3, 0};  // drawn Transient | Beat | Region | Manual
constexpr double kSliceBeats[7] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0};
constexpr const char* kSliceBeatNames[7] = {"1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"};
Rect sliceCell(int k) noexcept {
  const float w = 200.0f / 4.0f;
  return {0.5f + w * static_cast<float>(k), kSliceRowY0, w * static_cast<float>(k + 1) - 1.5f, kSliceRowY1};
}
constexpr Rect kRootText{0.5f, 330.5f, 82.0f, 345.5f}, kRootHz{84.0f, 330.5f, 101.0f, 345.5f}, kRootId{103.0f, 330.5f, 119.5f, 345.5f};
enum EditPart : int32_t {
  kPStart = 0, kPEnd, kPLoopStart, kPLoopEnd, kPLoop, kPWave, kPRuler, kPRootText, kPRootHz, kPRootId,
  kPEditor = 10,  // SM.17: the editor where nothing is (its presses are the editor's: they never move the block)
  kPSliceBy0 = 20, kPSliceAmount = 24, kPSliceNotes = 25,  // SM.12 (kPSliceBy0 + the drawn cell)
  kPGrain = 30, kPGrainSize = 31, kPHarmAdd = 32,          // SM.13: the grain region (body, right edge), + a ratio
  kPHarm0 = 40, kPHarmX0 = 70,                              // SM.13: ratio chip i (its text, its ×)
  kPSlice0 = 1000, kPMarker0 = 100000,
};
bool within(const Rect& r, Vec2 l) noexcept { return l.x >= r.x0 && l.x <= r.x1 && l.y >= r.y0 && l.y <= r.y1; }
constexpr double kMaxPxPerSample = 64.0;  // WF.1: the editor's deepest zoom (device px per sample)
// Whether face rect `r` meets the painter's clip (a section's paint clips the face to it: the others are skipped).
bool meetsClip(ui::FacePainter& p, const Rect& r) noexcept {
  const ui::FaceTransform& t = p.transform();
  const ui::ClipRect c = p.drawList().clip();
  const float x0 = t.origin.x + r.x0 * t.scale, x1 = t.origin.x + r.x1 * t.scale;
  const float y0 = t.origin.y + r.y0 * t.scale, y1 = t.origin.y + r.y1 * t.scale;
  return x1 > static_cast<float>(c.x0) && x0 < static_cast<float>(c.x1) && y1 > static_cast<float>(c.y0) && y0 < static_cast<float>(c.y1);
}
std::string lowerTrim(std::string_view v) {
  std::string out;
  bool space = false;
  for (const char ch : v) {
    if (ch == ' ' || ch == '	') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
  }
  return out;
}
std::string centsText(double c) {
  if (std::fabs(c) < 0.05) return {};
  char b[24];
  std::snprintf(b, sizeof b, " %+.1f\xC2\xA2", c);  // "+2.0¢"
  return b;
}

void ellipsePoints(std::vector<Vec2>& out, Vec2 c, float rx, float ry, bool close) {
  out.clear();
  for (int i = 0; i < kEllipseSteps + (close ? 1 : 0); ++i) {
    const float a = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(kEllipseSteps);
    out.push_back({c.x + rx * std::cos(a), c.y + ry * std::sin(a)});
  }
}

using meta::SamplerMode;
using pq::dsp::AudioWarp;

class SamplerFace final : public MetaFace {
public:
  explicit SamplerFace(const meta::Type& t) : MetaFace(t) {}
  ~SamplerFace() override {
    if (said_ != nullptr) said_->forget(this);
  }

  Vec2 worldSize() const noexcept override { return {kW, kH}; }
  FaceShape shape() const noexcept override { return {true, kCentre, kRx, kRy}; }
  float inputY(size_t, size_t) const noexcept override { return kH * 0.5f; }
  float outputY(size_t, size_t) const noexcept override { return kH * 0.5f; }
  bool ownsPlate() const noexcept override { return true; }

  // ---- shown (SM.10) ----
  void setShown(bool shown) override {
    shown_ = shown;
    say();
  }

  // ---- painting ----
  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : layer == 1 ? th.face.track : ui::mulAlpha(th.face.signal, 0.85f);
  }
  uint64_t staticKey(uint32_t layer) const override {
    if (layer != 2) return 0;
    refresh();
    return smp_.key;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      ellipsePoints(pts_, kCentre, kRx, kRy, false);
      p.convex(pts_, w);
      return;
    }
    if (layer == 1) {
      p.roundedRect(kWell, 5.0f, ui::mulAlpha(w, 0.32f));
      return;
    }
    if (!meetsClip(p, kWell)) return;  // (another section's paint: the editor, the root field)
    refresh();
    const float mid = (kWell.y0 + kWell.y1) * 0.5f;
    const media::WaveformOverview* o = smp_.overview;
    if (o == nullptr || smp_.built == 0 || !(smp_.f1 > smp_.f0)) {  // no sample (or its overview not there yet): the zero line
      p.line({kWell.x0 + kInset, mid}, {kWell.x1 - kInset, mid}, 0.8f, ui::mulAlpha(w, 0.5f));
      return;
    }
    // WF.1: the shared renderer (the samples themselves below the overview's resolution).
    wellStats_ = drawSample(p, *o, smp_.f0, smp_.f1, {kWell.x0 + kInset, kWell.y0 + kInset, kWell.x1 - kInset, kWell.y1 - kInset}, w, true,
                            -3.0e38f, 3.0e38f);
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    if (!said_ || said_ != voices()) say(true);  // painted: on screen (a host that never says counts it shown)
    const ui::Theme& th = *env.theme;
    const double now = clockNow();
    refresh();
    if (smp_.sample == 0)
      p.text({(kWell.x0 + kWell.x1) * 0.5f, (kWell.y0 + kWell.y1) * 0.5f - 6.0f}, "Drop a sample here", 6.5f, ui::mulAlpha(th.face.ink, 0.8f));
    paintRegion(p, th);
    const bool gran = granular();
    if (gran) {  // SM.13: the grain region over the well, the grains in it
      const Lane well = wellLane();
      paintGrainRegion(p, th, well, kWell.y0 + 1.5f, kWell.y1 - 1.5f);
      if (!env.ghost) paintGrains(p, th, well, kWell.y0 + 1.5f, kWell.y1 - 1.5f, 1.0f);
    }
    if (!env.ghost && !gran && !shown_v_.empty()) {
      const float y0 = kWell.y0 + 1.5f, y1 = kWell.y1 - 1.5f;
      p.beginGlow();
      for (const Shown& s : shown_v_) {
        const float x = headX(s.v.phase);
        const auto a = static_cast<uint32_t>(150.0f * alphaOf(s, now));
        p.glow({x, (y0 + y1) * 0.5f}, 9.0f, ui::withAlpha(th.face.glow, a));
      }
      p.endGlow();
      for (const Shown& s : shown_v_) {
        const float x = headX(s.v.phase), a = alphaOf(s, now);
        p.line({x, y0}, {x, y1}, 1.3f, ui::mulAlpha(th.face.signal, a));
        p.disc({x, y0}, 1.4f, ui::mulAlpha(ui::rgba(255, 255, 255), a));
      }
    }
    if (env.selected || env.hovered) {
      ellipsePoints(pts_, kCentre, kRx, kRy, true);
      p.polyline(pts_, env.selected ? 1.75f : 1.25f, env.selected ? th.circuit.selected : th.circuit.hover);
    }
    if (!env.ghost && meetsClip(p, kEditor)) paintEditor(p, th, now);  // SM.11
    if (!env.ghost && meetsClip(p, kRoot)) paintRoot(p, th);
    if (!env.ghost && meetsClip(p, kSlicing)) paintSlicing(p, th);
    if (!env.ghost && meetsClip(p, kHarm)) paintHarm(p, th);  // SM.13
  }

  void liveRects(std::vector<Rect>& out) const override { out.push_back({kWell.x0 - 6.0f, kWell.y0 - 4.0f, kWell.x1 + 6.0f, kWell.y1 + 4.0f}); }
  bool animating() const noexcept override { return !shown_v_.empty() || grainCount_ != 0 || edgeScrolling(); }

  // ---- interaction: the closed face is the frame only (the panel has its controls); SM.11: the editor and the root ----
  int32_t hitTest(Vec2 l, float ppu) const noexcept override {
    if (within(kWell, l) && granular()) {  // SM.13: the grain region on the closed well
      const int32_t g = grainPart(wellLane(), l.x, ppu);
      if (g != kNone) return g;
    }
    if (within(kEditor, l)) return editorPart(l, ppu);
    if (within(kHarm, l)) return harmPart(l);  // SM.13
    if (within(kRoot, l)) return within(kRootHz, l) ? kPRootHz : within(kRootId, l) ? kPRootId : kPRootText;
    if (within(kSlicing, l)) {  // SM.12
      for (int k = 0; k < 4; ++k)
        if (within(sliceCell(k), l)) return kPSliceBy0 + k;
      if (within(kSliceAmount, l)) return kPSliceAmount;
      if (within(kSliceNotes, l)) return kPSliceNotes;
      return kFrame;
    }
    return shape().contains(l) ? kFrame : kNone;
  }
  bool onPointer(const FacePointer& e) override;
  bool dragging() const noexcept override { return drag_ != kNone; }
  void cancelDrag() override {
    if (drag_ == kNone) return;
    if (drag_ == kPRuler) {  // WF.1: a view drag: the view as it was (nothing to undo)
      viewA_ = rulerA_;
      viewB_ = rulerB_;
      drag_ = kNone;
      return;
    }
    drag_ = kNone;
    target().cancel();
  }
  bool onKey(const ui::KeyEvent& e) override;
  bool onText(const ui::TextEvent& e) override {
    if (harmEdit_ >= 0) {  // SM.13: a ratio takes digits and one '/'
      for (const char c : e.text())
        if ((c >= '0' && c <= '9') || (c == '/' && harmTyped_.find('/') == std::string::npos)) harmTyped_.push_back(c);
      return true;
    }
    if (!typing_) return false;
    typed_ += e.text();
    return true;
  }
  bool wantsText() const noexcept override { return typing_ || harmEdit_ >= 0; }
  void focusLost() override {
    if (typing_) commitRoot();
    if (harmEdit_ >= 0) commitHarm();
  }
  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part == kPStart || part == kPEnd || part == kPLoopStart || part == kPLoopEnd || part >= kPSlice0) return ui::Cursor::ResizeEW;
    if (part == kPLoop || part == kPGrain) return ui::Cursor::Move;
    if (part == kPGrainSize) return ui::Cursor::ResizeEW;
    if (part >= kPHarm0 && part < kPHarm0 + static_cast<int32_t>(kHarmMax)) return ui::Cursor::Text;
    if (part == kPHarmAdd || (part >= kPHarmX0 && part < kPHarmX0 + static_cast<int32_t>(kHarmMax))) return ui::Cursor::Pointer;
    if (part == kPSliceAmount) return ui::Cursor::ResizeNS;
    if (part >= kPSliceBy0 && part <= kPSliceNotes) return ui::Cursor::Pointer;
    if (part == kPRootText) return ui::Cursor::Text;
    if (part == kPRootHz || part == kPRootId) return ui::Cursor::Pointer;
    if (part == kPRuler) return ui::Cursor::ResizeNS;  // WF.1: a drag scrolls and zooms the view
    if (part == kPEditor || part == kPWave) return ui::Cursor::Arrow;  // SM.17: not the block's grab
    return MetaFace::cursor(part);
  }
  void describe(int32_t part, std::string& title, std::string& body) const override {
    if (part == kFrame || part == kNone) {
      title = "Sampler";
      body = smp_.sample == 0 ? "Drop an audio file on it to load a sample."
             : granular()      ? "The sample from Start to End; each live grain glows where it reads, sized by its level."
                               : "The sample from Start to End; each sounding voice's playhead glows where it reads.";
      return;
    }
    if (part >= kPHarm0 && part < kPHarm0 + static_cast<int32_t>(kHarmMax)) {
      title = "Harmonic Ratio";
      body = "Each new grain takes the next ratio of the played pitch. Click to type n or n/d (whole numbers).";
      return;
    }
    if (part >= kPHarmX0 && part < kPHarmX0 + static_cast<int32_t>(kHarmMax)) {
      title = "Remove Ratio";
      body = "Click: take this ratio out of the list.";
      return;
    }
    switch (part) {
      case kPStart: title = "Sample Start"; body = "Drag: where a note starts reading the sample."; return;
      case kPEnd: title = "Sample End"; body = "Drag: where the sample ends."; return;
      case kPLoopStart: title = "Loop Start"; body = "Drag: where the loop starts (its end stays)."; return;
      case kPLoopEnd: title = "Loop End"; body = "Drag: where the loop ends."; return;
      case kPLoop: title = "Loop"; body = "Drag: move the loop (its length stays)."; return;
      case kPWave:
      case kPEditor:
        title = "Sample Editor";
        body = smp_.sample == 0 ? "Drop an audio file on the Sampler to load a sample."
               : granular()      ? "The whole sample. Drag the Start and End lines; drag the grain region to move Position, its right edge to set the grain size."
               : slicesShown()   ? "The whole sample. Drag the Start and End lines and the slice lines; double-click the waveform to add a slice."
                                 : "The whole sample. Drag the Start and End lines (or their flags below); the loop bar along the top moves and resizes the loop.";
        body += " Wheel: zoom about the pointer (to single samples); Shift + wheel: scroll; double-click under the waveform: the whole sample.";
        return;
      case kPRuler:
        if (markersShown()) {
          title = "Beat Ruler";
          body = "The sample's beats through its warp markers. Double-click: add a warp marker here. Drag: scroll (sideways) and zoom (up / down).";
        } else {
          title = "Ruler";
          body = "Drag: scroll (sideways) and zoom (up / down). Double-click: the whole sample. The wheel zooms about the pointer, Shift + wheel scrolls.";
        }
        return;
      case kPRootText: title = "Root"; body = "The pitch the sample plays at rate 1. Click to type Hz or an identity (A3, 7\\31 3, +cents)."; return;
      case kPRootHz: title = "Root in Hz"; body = "Show the root as Hz."; return;
      case kPSliceAmount:
        title = "Slice Amount";
        body = "Drag up / down: the transient sensitivity, the beat grid or the number of parts (re-slices; one undo step).";
        return;
      case kPSliceNotes:
        title = "Slice to Notes";
        body = "A note clip at the start marker: one note per slice, in order and timing, on its slice key.";
        return;
      case kPRootId: title = "Root as Identity"; body = "Show the root as the nearest pitch of the track's tuning and its cents."; return;
      case kPGrain: title = "Grain Position"; body = "Drag: where in Start \xE2\x80\xA6 End the grains read (Position)."; return;
      case kPGrainSize: title = "Grain Size"; body = "Drag: how long each grain is."; return;
      case kPHarmAdd: title = "Add Ratio"; body = "Click: add the next whole-number ratio to the list."; return;
      default: break;
    }
    if (part >= kPSliceBy0 && part < kPSliceBy0 + 4) {
      title = std::string("Slice by ") + kSliceByNames[kSliceCellOrder[part - kPSliceBy0]];
      body = "Click: make the slices this way now (Manual keeps them as drawn).";
      return;
    }
    if (part >= kPMarker0) {
      title = "Warp Marker";
      body = "Drag along the ruler: this beat falls there in the sample. Double-click: remove it.";
      return;
    }
    if (part >= kPSlice0) {
      title = "Slice " + std::to_string(part - kPSlice0 + 1);
      body = "Drag: move the slice. Double-click: remove it.";
      return;
    }
    MetaFace::describe(part, title, body);
  }

  // (tests) the playheads shown and the waveform's columns; SM.13: the grains shown
  size_t playheads() const noexcept { return shown_v_.size(); }
  size_t grainDots() const noexcept { return grainCount_; }
  // WF.1 (tests): the editor's view and what the last paints drew.
  void wave(SamplerFaceWave& out) const noexcept {
    out.viewA = viewA_;
    out.viewB = viewB_;
    out.well = wellStats_;
    out.editor = editStats_;
  }

protected:
  bool voicesPolled(bool changed) override {
    const double now = clockNow();
    if (changed) {
      for (Shown& s : shown_v_) s.seen = false;
      for (uint32_t i = 0; i < voiceCount_; ++i) {
        const FaceVoice& v = voiceBuf_[i];
        Shown* found = nullptr;
        for (Shown& s : shown_v_)
          if (s.v.slot == v.slot) found = &s;
        if (found == nullptr) {
          shown_v_.push_back({});
          found = &shown_v_.back();
          found->born = justBound_ ? now - kFade : now;
        } else if (found->dying) {
          found->born = now - static_cast<double>(alphaOf(*found, now)) * kFade;
        }
        found->v = v;
        found->dying = false;
        found->seen = true;
      }
      for (Shown& s : shown_v_)
        if (!s.seen && !s.dying) {
          s.dying = true;
          s.died = now;
        }
    }
    justBound_ = false;
    const size_t before = shown_v_.size();
    std::erase_if(shown_v_, [&](const Shown& s) { return s.dying && now - s.died >= kFade; });
    bool fading = shown_v_.size() != before;
    for (const Shown& s : shown_v_) fading = fading || s.dying || now - s.born < kFade;
    if (shown_v_.size() > 2 * kMaxVoices) shown_v_.erase(shown_v_.begin(), shown_v_.end() - 2 * kMaxVoices);
    bool scrolled = false;
    if (edgeScrolling() && now - edgeAt_ >= 1.0 / 60.0) {  // WF.1: a handle held past the editor's edge keeps scrolling
      dragTo(dragX_);
      scrolled = true;
    }
    return pollGrains() || changed || fading || scrolled;
  }

private:
  struct Shown {
    FaceVoice v;
    double born = 0.0, died = 0.0;
    bool dying = false, seen = false;
  };
  // What the waveform is drawn from, read again when the sample, Start, End, the overview's progress or the waveforms'
  // revision change (WF.1: no columns of its own; pq/ui/waveform.h draws it).
  struct Source {
    uint32_t sample = 0;
    double start = -1.0, end = -1.0;
    const media::WaveformOverview* overview = nullptr;
    uint64_t built = 0, revision = ~uint64_t{0};
    uint64_t key = 0;
    double f0 = 0.0, f1 = 0.0;  // the played region in frames (f1 > f0: something to draw)
  };
  static float alphaOf(const Shown& s, double now) noexcept {
    const float in = std::clamp(static_cast<float>((now - s.born) / kFade), 0.0f, 1.0f);
    const float out = s.dying ? std::clamp(1.0f - static_cast<float>((now - s.died) / kFade), 0.0f, 1.0f) : 1.0f;
    return std::min(in, out);
  }
  // The region strip: the sample 0 … 1 as a hairline, Start … End lit (as the target reads them now: a ghost pass's
  // target has the modulated values), the fades as ramps (their seconds over the sample's, or over 2 s unknown).
  void paintRegion(ui::FacePainter& p, const ui::Theme& th) const {
    const MetaTarget& t = target();
    const float x0 = kWell.x0 + kInset, x1 = kWell.x1 - kInset, y = kWell.y1 - 2.2f;
    const auto at = [&](double u) { return x0 + static_cast<float>(std::clamp(u, 0.0, 1.0)) * (x1 - x0); };
    const double s = t.value(meta::kSmpStart), e = t.value(meta::kSmpEnd);
    p.line({x0, y}, {x1, y}, 0.5f, ui::mulAlpha(th.face.frame, 0.8f));
    const float a = at(std::min(s, e)), b = at(std::max(s, e));
    p.line({a, y}, {std::max(b, a + 0.5f), y}, 0.9f, ui::mulAlpha(th.face.value, 0.6f));
    const double len = smp_.overview != nullptr && smp_.overview->sampleRate() > 0
                           ? static_cast<double>(smp_.overview->frames()) / smp_.overview->sampleRate()
                           : 2.0;
    const double fi = t.value(meta::kSmpFadeIn) / std::max(len, 1e-3), fo = t.value(meta::kSmpFadeOut) / std::max(len, 1e-3);
    if (fi > 0.0) p.line({a, y + 1.2f}, {at(std::min(s, e) + fi), y - 1.2f}, 0.8f, ui::mulAlpha(th.face.value, 0.8f));
    if (fo > 0.0) p.line({at(std::max(s, e) - fo), y - 1.2f}, {b, y + 1.2f}, 0.8f, ui::mulAlpha(th.face.value, 0.8f));
  }
  // Where a voice reading at `pos` (0 … 1 of the sample) is drawn: its place in the Start … End window of the well.
  float headX(float pos) const noexcept {
    const double s = smp_.start, e = smp_.end > smp_.start ? smp_.end : smp_.start + 1e-6;
    const double u = std::isfinite(pos) ? std::clamp((static_cast<double>(pos) - s) / (e - s), 0.0, 1.0) : 0.0;
    return kWell.x0 + kInset + static_cast<float>(u) * (kWell.x1 - kWell.x0 - 2.0f * kInset);
  }
  // Tells the reader this face is shown or not (once per change; `paint`: the face is being painted, so it is).
  void say(bool paint = false) {
    VoiceTapReader* r = voices();
    if (r != said_) {
      if (said_ != nullptr) said_->forget(this);
      said_ = r;
      if (said_ == nullptr) return;
      if (paint) shown_ = true;
      said_->shown(this, shown_);
      return;
    }
    if (said_ != nullptr) said_->shown(this, shown_);
  }
  void refresh() const {
    const MetaTarget& t = target();
    const uint32_t sample = meta::samplerSample(t.data().get());
    const double start = t.value(meta::kSmpStart), end = t.value(meta::kSmpEnd);
    const uint64_t rev = t.overviewRevision();
    if (sample == smp_.sample && start == smp_.start && end == smp_.end && rev == smp_.revision &&
        (smp_.overview == nullptr || smp_.overview->builtFrames() == smp_.built))
      return;
    smp_.sample = sample;
    smp_.start = start;
    smp_.end = end;
    smp_.revision = rev;
    smp_.overview = sample != 0 ? t.sampleOverview(sample) : nullptr;
    smp_.built = smp_.overview != nullptr ? smp_.overview->builtFrames() : 0;
    uint64_t key = 0x53414D50ull;  // "SAMP"
    auto mixIn = [&key](uint64_t v) { key = (key ^ v) * 0x100000001B3ull; };
    mixIn(sample);
    mixIn(std::bit_cast<uint64_t>(start));
    mixIn(std::bit_cast<uint64_t>(end));
    mixIn(smp_.built);
    mixIn(reinterpret_cast<uintptr_t>(smp_.overview));
    mixIn(rev);  // WF.1: a sample window arrived (the well draws the samples then)
    smp_.key = key | 1u;
    smp_.f0 = smp_.f1 = 0.0;
    if (sample != viewSample_) {  // another sample: its whole length
      viewSample_ = sample;
      viewA_ = 0.0;
      viewB_ = 1.0;
    }
    const media::WaveformOverview* o = smp_.overview;
    if (o == nullptr || o->frames() == 0 || o->levelCount() == 0 || smp_.built == 0) return;
    smp_.f0 = std::clamp(std::min(start, end), 0.0, 1.0) * static_cast<double>(o->frames());
    smp_.f1 = std::clamp(std::max(start, end), 0.0, 1.0) * static_cast<double>(o->frames());
  }
  // WF.1: frames [f0, f1) of the sample across face rect `r` through the shared renderer (pq/ui/waveform.h): the
  // overview, or below its resolution the samples (asked of the target: read in the background, the overview drawn
  // stretched until they arrive, when the waveforms' revision moves and the host repaints). Whole device columns from
  // the rect's rounded left edge; [clipX0, clipX1) in device px.
  ui::WaveformStats drawSample(ui::FacePainter& p, const media::WaveformOverview& o, double f0, double f1, const Rect& r, Rgba colour,
                               bool merge, float clipX0, float clipX1) const {
    const ui::FaceTransform& t = p.transform();
    ui::WaveformDraw d;
    d.rect = {std::round(t.origin.x + r.x0 * t.scale), t.origin.y + r.y0 * t.scale, std::round(t.origin.x + r.x1 * t.scale),
              t.origin.y + r.y1 * t.scale};
    if (!(d.rect.x1 > d.rect.x0) || !(f1 > f0)) return {};
    d.frame0 = f0;
    d.frame1 = f1;
    d.colour = p.colour(colour);
    d.merge = merge;
    d.lineWidth = std::max(1.0f, 0.6f * t.scale);
    d.clipX0 = clipX0;
    d.clipX1 = clipX1;
    ui::WaveformLevel lv[kMaxOverviewLevels];
    ui::WaveformData data = waveformData(o, nullptr, lv);
    if (ui::waveformLevelFor(data, (f1 - f0) / static_cast<double>(d.rect.x1 - d.rect.x0)) < 0)
      if (const media::SampleWindow* w = target().sampleWindow(smp_.sample, static_cast<int64_t>(std::floor(f0)) - ui::kWaveformSampleMargin,
                                                               static_cast<int64_t>(std::ceil(f1)) + ui::kWaveformSampleMargin))
        data = waveformData(o, w, lv);
    return ui::drawWaveform(p.drawList(), data, d);
  }

  // ---- SM.11: the editor ----
  struct Geometry {  // what the editor reads of the target now
    double frames = 0.0, rate = 48000.0;  // the sample's (0: unknown, no overview yet)
    double start = 0.0, end = 1.0, loopStart = 0.0, loopEnd = 1.0, loopFade = 0.0, fadeIn = 0.0, fadeOut = 0.0;
    double sampleBpm = 120.0;
    SamplerMode mode = SamplerMode::Classic;
    dsp::AudioWarp warp = dsp::AudioWarp::Off;
    bool loop = false;
    std::vector<float> slices, markers;
  };
  Geometry geometry() const;
  bool slicesShown() const;
  bool markersShown() const;
  int32_t editorPart(Vec2 l, float ppu) const noexcept;
  void paintEditor(ui::FacePainter& p, const ui::Theme& th, double now);
  void paintRoot(ui::FacePainter& p, const ui::Theme& th);
  // SM.12: the slicing row, and the data's slicing state (sliceBy: a doc::ops::SliceBy; its amounts).
  void paintSlicing(ui::FacePainter& p, const ui::Theme& th);
  struct Slicing {
    int by = 1;
    double sensitivity = 1.0, beats = 1.0, count = 8.0;
    double amount() const noexcept { return by == 1 ? sensitivity : by == 2 ? beats : count; }
  };
  Slicing slicing() const;
  std::string amountText(const Slicing& sl) const;
  // ---- SM.13: Granular ----
  // A horizontal map of sample fractions u0 … u1 onto face x0 … x1 (the closed well: Start … End; the editor: 0 … 1).
  struct Lane {
    float x0, x1;
    double u0, u1;
    float x(double u) const noexcept {
      const double t = u1 > u0 ? (u - u0) / (u1 - u0) : 0.0;
      return x0 + static_cast<float>(std::clamp(t, 0.0, 1.0)) * (x1 - x0);
    }
    double u(float xx) const noexcept {
      return u0 + static_cast<double>(std::clamp((xx - x0) / std::max(x1 - x0, 1e-3f), 0.0f, 1.0f)) * (u1 - u0);
    }
  };
  Lane wellLane() const {
    refresh();
    const double s = std::min(smp_.start, smp_.end), e = std::max(smp_.start, smp_.end);
    return {kWell.x0 + kInset, kWell.x1 - kInset, s, e > s ? e : s + 1e-6};
  }
  Lane editorLane() const noexcept { return {kEdX0, kEdX1, viewA_, viewB_}; }
  // ---- WF.1: the editor's view (fractions viewA_ … viewB_ of the sample across kEdX0 … kEdX1) ----
  // Where sample fraction u is drawn (not clamped: outside the view it lies outside the editor), the same clamped to
  // the editor, whether it is in view, and the fraction at face x (clamped to the view and the sample).
  float edX(double u) const noexcept {
    return kEdX0 + static_cast<float>(std::clamp((u - viewA_) / (viewB_ - viewA_), -1.0e4, 1.0e4)) * (kEdX1 - kEdX0);
  }
  float edXc(double u) const noexcept { return std::clamp(edX(u), kEdX0, kEdX1); }
  bool edIn(double u) const noexcept { return u >= viewA_ - 1e-12 && u <= viewB_ + 1e-12; }
  double edU(float x) const noexcept {
    return std::clamp(viewA_ + std::clamp(static_cast<double>((x - kEdX0) / (kEdX1 - kEdX0)), 0.0, 1.0) * (viewB_ - viewA_), 0.0, 1.0);
  }
  double sampleFrames() const {
    refresh();
    return smp_.overview != nullptr ? static_cast<double>(smp_.overview->frames()) : 0.0;
  }
  // The view from `a`, `span` wide: no narrower than kMaxPxPerSample allows (edPpu_: device px per unit as last
  // painted or pressed), inside the sample.
  double clampSpan(double span) const {
    const double frames = std::max(1.0, sampleFrames());
    const double least = std::min(1.0, static_cast<double>((kEdX1 - kEdX0) * edPpu_) / (kMaxPxPerSample * frames));
    return std::clamp(span, least, 1.0);
  }
  void setView(double a, double span) {
    span = clampSpan(span);
    viewA_ = std::clamp(a, 0.0, 1.0 - span);
    viewB_ = viewA_ + span;
  }
  bool inView(Vec2 l) const noexcept { return l.x >= kEditor.x0 && l.x <= kEditor.x1 && l.y >= kRulerY0 && l.y <= kFlagY1 + 1.0f; }
  bool wheelView(const FacePointer& e);
  // A handle of the editor is dragged with the pointer past the editor's side: the view scrolls under it.
  bool edgeScrolling() const noexcept {
    return drag_ != kNone && drag_ != kPRuler && drag_ != kPSliceAmount && drag_ != kPGrain && drag_ != kPGrainSize &&
           (dragX_ < kEdX0 || dragX_ > kEdX1) && viewB_ - viewA_ < 1.0;
  }
  // Where the grains read: [a, b) of the sample (Position in Start … End, one grain size wide) and the spray time either
  // side (fractions of the sample; the sizes need its length: unknown, the region is a hairline).
  struct GrainSpan {
    double a = 0.0, b = 0.0, spray = 0.0, seconds = 0.0;
  };
  bool granular() const { return std::lround(target().setting(meta::kSmpMode)) == static_cast<long>(SamplerMode::Granular); }
  GrainSpan grainSpan() const;
  int32_t grainPart(const Lane& lane, float x, float ppu) const noexcept;
  void paintGrainRegion(ui::FacePainter& p, const ui::Theme& th, const Lane& lane, float y0, float y1) const;
  void paintGrains(ui::FacePainter& p, const ui::Theme& th, const Lane& lane, float y0, float y1, float k) const;
  bool pollGrains();
  // The ratio list ((n, d) pairs: the data's, else the default), the chips' part at l, their paint, a typed ratio.
  std::vector<std::pair<uint32_t, uint32_t>> harmonics() const;
  void writeHarmonics(const std::vector<std::pair<uint32_t, uint32_t>>& h);
  int32_t harmPart(Vec2 l) const;
  void paintHarm(ui::FacePainter& p, const ui::Theme& th);
  void commitHarm();
  std::string rootLabel() const;
  void commitRoot();
  void editFloats(const char* field, std::vector<float> v);
  void dragTo(float x);

  std::vector<Shown> shown_v_;
  std::vector<FaceGrain> grainBuf_;  // SM.13: the live grains of the last poll (grainCount_ of them)
  uint32_t grainCount_ = 0;
  bool dragWell_ = false;            // SM.13: the grain drag began on the closed well (its lane), else on the editor
  double grainGrab_ = 0.0;           // SM.13: the pointer's offset from the region's start (a body drag)
  int32_t harmEdit_ = -1;            // SM.13: the ratio being typed (its index)
  std::string harmTyped_;
  // WF.1: the editor's view (view state: not undoable, not saved; another sample fits again).
  mutable double viewA_ = 0.0, viewB_ = 1.0;
  mutable uint32_t viewSample_ = 0;
  float edPpu_ = 1.0f;                 // device px per face unit (the deepest zoom is in device px per sample)
  Vec2 rulerPress_{0.0f, 0.0f};        // a ruler drag: where it began (device px), the fraction under it, the view then
  double rulerU_ = 0.0, rulerA_ = 0.0, rulerB_ = 1.0;
  float dragX_ = 0.0f;                 // a handle drag: the pointer's face x (the edge scroll)
  double edgeAt_ = 0.0;                // the clock of the last edge-scroll step
  mutable ui::WaveformStats wellStats_, editStats_;
  int32_t drag_ = kNone;   // the part being dragged
  double grab_ = 0.0;      // kPLoop: the pointer's offset from the loop's start (0 … 1)
  float dragY_ = 0.0f;     // SM.12: the amount drag's start (device px) and amount
  double dragAmount_ = 0.0, lastAmount_ = 0.0;
  int dragBy_ = 1;
  bool typing_ = false;    // the root field takes the keys
  std::string typed_;
  mutable std::string rootCache_;
  mutable uint64_t rootKey_ = 0;
  mutable Source smp_;
  mutable std::vector<Vec2> pts_;
  VoiceTapReader* said_ = nullptr;  // the reader told whether this face is shown
  bool shown_ = true;
};

// ---- SM.11: the expanded editor --------------------------------------------------------------------------------------

SamplerFace::Geometry SamplerFace::geometry() const {
  const MetaTarget& t = target();
  Geometry g;
  refresh();
  if (const media::WaveformOverview* o = smp_.overview; o != nullptr && o->frames() > 0) {
    g.frames = static_cast<double>(o->frames());
    g.rate = o->sampleRate() > 0 ? static_cast<double>(o->sampleRate()) : 48000.0;
  }
  g.start = t.value(meta::kSmpStart);
  g.end = t.value(meta::kSmpEnd);
  g.loopStart = t.value(meta::kSmpLoopStart);
  g.loopEnd = std::min(1.0, g.loopStart + t.value(meta::kSmpLoopLength));
  g.loopFade = t.value(meta::kSmpLoopFade);
  g.fadeIn = t.value(meta::kSmpFadeIn);
  g.fadeOut = t.value(meta::kSmpFadeOut);
  g.sampleBpm = std::max(1.0, t.value(meta::kSmpSampleBpm));
  g.mode = static_cast<SamplerMode>(std::clamp(static_cast<int>(std::lround(t.setting(meta::kSmpMode))), 0, 3));
  g.warp = static_cast<dsp::AudioWarp>(std::clamp(static_cast<int>(std::lround(t.setting(meta::kSmpWarp))), 0, 6));
  // SM.17: the warp that PLAYS (pq/meta/sampler.h): Granular warps nothing, Slice plays a stored stretch mode unwarped —
  // the ruler then reads seconds and the fades show, as the panel's Warp row says Off.
  if (g.mode == SamplerMode::Granular || (g.mode == SamplerMode::Slice && g.warp >= dsp::AudioWarp::Beats)) g.warp = dsp::AudioWarp::Off;
  g.loop = g.mode == SamplerMode::Classic && std::lround(t.setting(meta::kSmpLoop)) != 0;
  const meta::DataPtr d = t.data();
  g.slices = meta::samplerSlices(d.get());
  g.markers = meta::samplerMarkers(d.get());
  return g;
}

bool SamplerFace::slicesShown() const {  // (Granular warps nothing: SM.13)
  const MetaTarget& t = target();
  const long mode = std::lround(t.setting(meta::kSmpMode));
  return mode == static_cast<long>(SamplerMode::Slice) ||
         (mode != static_cast<long>(SamplerMode::Granular) && std::lround(t.setting(meta::kSmpWarp)) == static_cast<long>(dsp::AudioWarp::Beats));
}
bool SamplerFace::markersShown() const {
  const MetaTarget& t = target();
  const long mode = std::lround(t.setting(meta::kSmpMode));
  return mode != static_cast<long>(SamplerMode::Slice) && mode != static_cast<long>(SamplerMode::Granular) &&
         std::lround(t.setting(meta::kSmpWarp)) >= static_cast<long>(dsp::AudioWarp::Beats);
}

int32_t SamplerFace::editorPart(Vec2 l, float ppu) const noexcept {
  const float tol = std::max(1.5f, 5.0f / std::max(ppu, 0.1f));  // ≈ 5 device px either side of a line
  const Geometry g = geometry();
  const auto near = [&](double u) { return std::fabs(l.x - edX(u)) <= tol; };
  if (l.y >= kFlagY0 - 1.0f && l.y <= kFlagY1 + 1.0f) {  // the flags (End wins where they meet: it is drawn on top)
    // WF.1: a flag out of view waits at the editor's edge.
    if (std::fabs(l.x - edXc(g.end)) <= tol) return kPEnd;
    if (std::fabs(l.x - edXc(g.start)) <= tol) return kPStart;
    return kPEditor;
  }
  if (l.y >= kRulerY0 && l.y < kWaveY0 - 0.5f) {
    if (g.frames <= 0.0) return kPEditor;
    if (markersShown())
      for (size_t i = 0; i + 1 < g.markers.size(); i += 2)
        if (near(static_cast<double>(g.markers[i]) / g.frames)) return kPMarker0 + static_cast<int32_t>(i / 2);
    return kPRuler;  // (WF.1: with or without markers: a drag on it scrolls and zooms)
  }
  if (l.y >= kWaveY0 && l.y <= kWaveY1) {
    if (g.mode == SamplerMode::Granular)  // SM.13: the grain region first (it lies inside Start … End)
      if (const int32_t gp = grainPart(editorLane(), l.x, ppu); gp != kNone) return gp;
    if (g.loop && l.y <= kLoopY1 + 1.0f) {
      if (near(g.loopEnd)) return kPLoopEnd;
      if (near(g.loopStart)) return kPLoopStart;
      if (l.x > edX(g.loopStart) && l.x < edX(g.loopEnd)) return kPLoop;
    }
    if (near(g.end)) return kPEnd;
    if (near(g.start)) return kPStart;
    if (slicesShown() && g.frames > 0.0)
      for (size_t i = 0; i < g.slices.size(); ++i)
        if (near(static_cast<double>(g.slices[i]) / g.frames)) return kPSlice0 + static_cast<int32_t>(i);
    return kPWave;
  }
  return kPEditor;
}

void SamplerFace::editFloats(const char* field, std::vector<float> v) {
  const meta::DataPtr d = target().data();
  target().setData(meta::samplerWith(d.get(), field, meta::DataValue::floatArray(std::move(v))));
}

// The nearest guide of the track's tuning to `hz` within a fifth either side (null: none).
static const PitchGuide* nearestGuide(const MetaTarget& t, double hz, std::vector<PitchGuide>& gs) {
  t.pitchGuides(static_cast<float>(hz / 1.5), static_cast<float>(hz * 1.5), gs);
  const PitchGuide* best = nullptr;
  for (const PitchGuide& x : gs)
    if (best == nullptr || std::fabs(std::log2(x.hz / hz)) < std::fabs(std::log2(best->hz / hz))) best = &x;
  return best;
}

bool SamplerFace::onPointer(const FacePointer& e) {
  using K = FacePointer::Kind;
  MetaTarget& t = target();
  if (e.kind == K::Wheel) return wheelView(e);  // WF.1
  if (e.kind == K::Move && drag_ == kPRuler) {  // WF.1: sideways scrolls, up / down zooms about the press (down: in)
    edPpu_ = e.pxPerUnit;
    const double w = static_cast<double>((kEdX1 - kEdX0) * e.pxPerUnit);
    const double span0 = rulerB_ - rulerA_;
    const double span = clampSpan(span0 * std::exp(-static_cast<double>(e.device.y - rulerPress_.y) / (90.0 * std::max(0.1, static_cast<double>(e.uiScale)))));
    const double frac = (rulerU_ - rulerA_) / span0 + static_cast<double>(e.device.x - rulerPress_.x) / std::max(1.0, w);
    setView(rulerU_ - frac * span, span);
    return true;
  }
  if (e.kind == K::Move && drag_ == kPSliceAmount) {
    const double dy = static_cast<double>(dragY_ - e.device.y) / std::max(0.1, static_cast<double>(e.uiScale));
    double a = dragAmount_;
    if (dragBy_ == 1) {
      a = std::clamp(std::round((dragAmount_ + dy / 150.0) * 100.0) / 100.0, 0.0, 1.0);
    } else if (dragBy_ == 2) {
      size_t i = 0;
      while (i + 1 < std::size(kSliceBeats) && kSliceBeats[i] < dragAmount_ - 1e-9) ++i;
      i = static_cast<size_t>(std::clamp<long>(static_cast<long>(i) + std::lround(dy / 12.0), 0, static_cast<long>(std::size(kSliceBeats)) - 1));
      a = kSliceBeats[i];
    } else {
      a = std::clamp(std::round(dragAmount_ + dy / 6.0), 2.0, 64.0);
    }
    if (a != lastAmount_) {
      lastAmount_ = a;
      (void)t.sliceSample(dragBy_, a);
    }
    return true;
  }
  if (e.kind == K::Move && (drag_ == kPGrain || drag_ == kPGrainSize)) {  // SM.13
    const Lane lane = dragWell_ ? wellLane() : editorLane();
    const GrainSpan sp = grainSpan();
    const double u = lane.u(e.local.x);
    const Geometry g = geometry();
    const double s = std::min(g.start, g.end), w = std::max(g.start, g.end) - s;
    if (drag_ == kPGrain) {
      if (w > 0.0) t.set(meta::kSmpGrainPosition, std::clamp((u - grainGrab_ - s) / w, 0.0, 1.0));
    } else if (sp.seconds > 0.0) {
      t.set(meta::kSmpGrainSize, std::clamp((u - sp.a) * sp.seconds, 0.005, 1.0));
    }
    return true;
  }
  if (e.kind == K::Move && drag_ != kNone) {
    dragTo(e.local.x);
    return true;
  }
  if (e.kind == K::Up) {
    if (drag_ == kNone) return false;
    const bool view = drag_ == kPRuler;
    drag_ = kNone;
    if (!view) t.end();
    return true;
  }
  if (e.kind != K::Down || e.button != ui::PointerButton::Left) return false;
  const int32_t part = hitTest(e.local, e.pxPerUnit);
  if (part != kPRootText && typing_) commitRoot();
  if (harmEdit_ >= 0 && part != kPHarm0 + harmEdit_) commitHarm();
  if (part == kFrame || part == kNone) return false;
  if (part == kPEditor) {  // SM.17: a press in the editor is never the block's
    if (e.clicks >= 2 && inView(e.local)) setView(0.0, 1.0);  // WF.1: the whole sample again
    return true;
  }
  if (part == kPRuler && (e.clicks < 2 || !markersShown())) {  // WF.1: the view (no undo step, read-only targets too)
    edPpu_ = e.pxPerUnit;
    if (e.clicks >= 2) {
      setView(0.0, 1.0);
      return true;
    }
    drag_ = kPRuler;
    rulerPress_ = e.device;
    rulerA_ = viewA_;
    rulerB_ = viewB_;
    rulerU_ = viewA_ + static_cast<double>((e.local.x - kEdX0) / (kEdX1 - kEdX0)) * (viewB_ - viewA_);
    return true;
  }
  if (!t.editable()) return within(kEditor, e.local);
  if (part == kPGrain || part == kPGrainSize) {  // SM.13: the grain region (one undo step per drag)
    dragWell_ = within(kWell, e.local);
    const Lane lane = dragWell_ ? wellLane() : editorLane();
    grainGrab_ = lane.u(e.local.x) - grainSpan().a;
    drag_ = part;
    t.begin(part == kPGrain ? "Grain Position" : "Grain Size");
    return true;
  }
  if (part >= kPHarm0 && part < kPHarm0 + static_cast<int32_t>(kHarmMax)) {  // type a ratio in its place
    harmEdit_ = part - kPHarm0;
    harmTyped_.clear();
    return true;
  }
  if (part >= kPHarmX0 && part < kPHarmX0 + static_cast<int32_t>(kHarmMax)) {  // remove it (one step; one stays)
    auto h = harmonics();
    const auto i = static_cast<size_t>(part - kPHarmX0);
    if (h.size() <= 1) {
      t.notice("The list keeps one ratio at least");
      return true;
    }
    if (i < h.size()) h.erase(h.begin() + static_cast<std::ptrdiff_t>(i));
    writeHarmonics(h);
    return true;
  }
  if (part == kPHarmAdd) {  // the next whole number above the list's largest ratio (one step)
    auto h = harmonics();
    if (h.size() >= kHarmMax) return true;
    uint32_t next = 1;
    for (const auto& [n, d] : h) next = std::max(next, n / d + 1);
    h.emplace_back(std::min<uint32_t>(next, 65536u), 1u);
    writeHarmonics(h);
    return true;
  }
  const Geometry g = geometry();
  const double u = edU(e.local.x);
  if (part >= kPSliceBy0 && part < kPSliceBy0 + 4) {  // SM.12: slice by this way now (one step)
    const int by = kSliceCellOrder[part - kPSliceBy0];
    Slicing sl = slicing();
    sl.by = by;
    (void)t.sliceSample(by, sl.amount());
    return true;
  }
  if (part == kPSliceNotes) {
    (void)t.sliceToNotes();
    return true;
  }
  if (part == kPSliceAmount) {  // a vertical drag re-slices at each new amount: one undo step
    const Slicing sl = slicing();
    if (sl.by == 0) return true;  // (Manual has none)
    dragBy_ = sl.by;
    dragAmount_ = lastAmount_ = sl.amount();
    dragY_ = e.device.y;
    drag_ = kPSliceAmount;
    t.begin("Slice Amount");
    return true;
  }
  switch (part) {
    case kPRootText:
      if (!typing_) {
        typing_ = true;
        typed_.clear();
      }
      return true;
    case kPRootHz:
    case kPRootId: {
      const meta::DataPtr d = t.data();
      const bool id = part == kPRootId;
      if (id == !meta::samplerRootText(d.get()).empty()) return true;  // shown so already
      std::string text;
      if (id) {
        const double root = t.value(meta::kSmpRoot);
        std::vector<PitchGuide> gs;
        const PitchGuide* best = nearestGuide(t, root, gs);
        text = best != nullptr ? t.pitchName(*best) : std::string();
        if (text.empty()) {
          t.notice("The track has no tuning to name the root in");
          return true;
        }
        text += centsText(1200.0 * std::log2(root / best->hz));
      }
      t.setData(meta::samplerWith(d.get(), "rootText", meta::DataValue::str(std::move(text))));
      return true;
    }
    case kPWave:
      if (e.clicks < 2 || !slicesShown() || g.frames <= 0.0) return true;  // (the editor's press: nothing to do)
      {  // a slice here (one step)
        std::vector<float> v = g.slices;
        const float f = static_cast<float>(std::round(u * g.frames));
        if (std::find(v.begin(), v.end(), f) != v.end()) return true;
        v.insert(std::upper_bound(v.begin(), v.end(), f), f);
        editFloats("slices", std::move(v));
      }
      return true;
    case kPRuler:
      if (g.frames <= 0.0) return true;
      {  // (a double-click with the markers shown) a warp marker at the beat the ruler reads here (one step; nothing moves)
        std::vector<float> m = g.markers;
        const dsp::WarpBeatLaw law(m, 60.0 * g.rate / g.sampleBpm);
        const auto f = static_cast<float>(std::round(u * g.frames));
        const auto beat = static_cast<float>(law.law.contentAt(f));
        size_t at = 0;
        while (at + 1 < m.size() && m[at] < f) at += 2;
        if (at + 1 < m.size() && m[at] == f) return true;
        if ((at >= 2 && !(m[at - 1] < beat)) || (at + 1 < m.size() && !(beat < m[at + 1]))) return true;  // (not ascending)
        m.insert(m.begin() + static_cast<std::ptrdiff_t>(at), {f, beat});
        editFloats("markers", std::move(m));
      }
      return true;
    default: break;
  }
  if (part >= kPSlice0 && e.clicks >= 2) {  // a slice / marker removed (one step)
    const bool marker = part >= kPMarker0;
    std::vector<float> v = marker ? g.markers : g.slices;
    const auto i = static_cast<size_t>(part - (marker ? kPMarker0 : kPSlice0));
    if (marker && 2 * i + 1 < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(2 * i), v.begin() + static_cast<std::ptrdiff_t>(2 * i + 2));
    else if (!marker && i < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    editFloats(marker ? "markers" : "slices", std::move(v));
    return true;
  }
  static constexpr const char* kLabels[] = {"Sample Start", "Sample End", "Loop Start", "Loop End", "Move Loop"};
  const char* label = part >= kPMarker0 ? "Move Warp Marker" : part >= kPSlice0 ? "Move Slice" : kLabels[part];
  grab_ = part == kPLoop ? u - g.loopStart : 0.0;
  drag_ = part;
  dragX_ = e.local.x;
  edPpu_ = e.pxPerUnit;
  t.begin(label);
  return true;
}

// WF.1: the wheel over the editor — zoom about the pointer (Ctrl too), Shift (or a sideways wheel) scrolls; Alt passes
// it on to the host. View state: nothing is edited.
bool SamplerFace::wheelView(const FacePointer& e) {
  if (!inView(e.local) || (e.mods & ui::mod::kAlt) != 0 || sampleFrames() <= 0.0) return false;
  edPpu_ = e.pxPerUnit;
  const double span = viewB_ - viewA_;
  if ((e.mods & ui::mod::kShift) != 0 || e.wheel.x != 0.0f) {
    const double notches = e.wheel.x != 0.0f ? e.wheel.x : -e.wheel.y;  // Shift + wheel up = left (ScrollView's rule)
    setView(viewA_ + notches * span * 0.125, span);
    return true;
  }
  if (e.wheel.y == 0.0f) return false;
  const double frac = std::clamp(static_cast<double>((e.local.x - kEdX0) / (kEdX1 - kEdX0)), 0.0, 1.0);
  const double anchor = viewA_ + frac * span;
  const double next = clampSpan(span * std::pow(0.8, static_cast<double>(e.wheel.y)));
  setView(anchor - frac * next, next);
  return true;
}

void SamplerFace::dragTo(float x) {
  MetaTarget& t = target();
  dragX_ = x;
  if (edgeScrolling()) {  // WF.1: past the editor's side the view follows (by how far past, at most a tenth per step)
    const float over = x < kEdX0 ? x - kEdX0 : x - kEdX1;
    const double span = viewB_ - viewA_;
    setView(viewA_ + std::clamp(static_cast<double>(over / (kEdX1 - kEdX0)), -0.1, 0.1) * span, span);
    edgeAt_ = clockNow();
  }
  const Geometry g = geometry();
  const double u = edU(x);
  constexpr double kGap = 1e-4;  // Start stays before End, a loop keeps a length
  switch (drag_) {
    case kPStart: t.set(meta::kSmpStart, std::min(u, g.end - kGap)); return;
    case kPEnd: t.set(meta::kSmpEnd, std::max(u, g.start + kGap)); return;
    case kPLoopStart: {
      const double ls = std::min(u, g.loopEnd - kGap);
      t.set(meta::kSmpLoopStart, ls);
      t.set(meta::kSmpLoopLength, g.loopEnd - ls);
      return;
    }
    case kPLoopEnd: t.set(meta::kSmpLoopLength, std::max(u, g.loopStart + kGap) - g.loopStart); return;
    case kPLoop: {
      const double len = g.loopEnd - g.loopStart;
      t.set(meta::kSmpLoopStart, std::clamp(u - grab_, 0.0, std::max(0.0, 1.0 - len)));
      return;
    }
    default: break;
  }
  if (g.frames <= 0.0) return;
  const double f = std::round(u * g.frames);
  if (drag_ >= kPMarker0) {  // its frame between its neighbours' (strictly), its beat kept
    std::vector<float> m = g.markers;
    const auto i = static_cast<size_t>(drag_ - kPMarker0);
    if (2 * i + 1 >= m.size()) return;
    const double lo = i > 0 ? m[2 * i - 2] + 1.0 : 0.0, hi = 2 * i + 2 < m.size() ? m[2 * i + 2] - 1.0 : g.frames;
    m[2 * i] = static_cast<float>(std::clamp(f, lo, std::max(lo, hi)));
    editFloats("markers", std::move(m));
    return;
  }
  if (drag_ >= kPSlice0) {  // between its neighbours (the order is the slice keys')
    std::vector<float> v = g.slices;
    const auto i = static_cast<size_t>(drag_ - kPSlice0);
    if (i >= v.size()) return;
    const double lo = i > 0 ? v[i - 1] + 1.0 : 0.0, hi = i + 1 < v.size() ? v[i + 1] - 1.0 : g.frames - 1.0;
    v[i] = static_cast<float>(std::clamp(f, lo, std::max(lo, hi)));
    editFloats("slices", std::move(v));
  }
}

// The root as shown: Hz, or (rootText set) the nearest identity of the track's tuning + cents (the stored text when the
// target knows no tuning). Cached by the root, the data and the guides' key.
std::string SamplerFace::rootLabel() const {
  const MetaTarget& t = target();
  const double root = t.value(meta::kSmpRoot);
  const meta::DataPtr d = t.data();
  const uint64_t key = ((std::bit_cast<uint64_t>(root) ^ (t.guidesKey() * 0x9E3779B97F4A7C15ull)) +
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.get()))) | 1u;
  if (key == rootKey_) return rootCache_;
  rootKey_ = key;
  const std::string text = meta::samplerRootText(d.get());
  char b[32];
  std::snprintf(b, sizeof b, root < 1000.0 ? "%.2f Hz" : "%.1f Hz", root);
  rootCache_ = b;
  if (text.empty()) return rootCache_;
  std::vector<PitchGuide> gs;
  const PitchGuide* best = nearestGuide(t, root, gs);
  const std::string name = best != nullptr ? t.pitchName(*best) : std::string();
  rootCache_ = name.empty() ? text : name + centsText(1200.0 * std::log2(root / best->hz));
  return rootCache_;
}

// Typed text: Hz ("440", "440 Hz", "1.2k") sets the root and the Hz display; an identity of the track's tuning with
// optional cents ("A3", "7\31 3 -5c", "C4 +12¢") sets the root to it and the identity display. One step.
void SamplerFace::commitRoot() {
  typing_ = false;
  MetaTarget& t = target();
  const std::string in = lowerTrim(typed_);
  typed_.clear();
  if (in.empty()) return;
  const meta::DataPtr d = t.data();
  {
    char* end = nullptr;
    double hz = std::strtod(in.c_str(), &end);
    std::string_view rest(end);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    if (end != in.c_str() && (rest.empty() || rest == "hz" || rest == "k" || rest == "khz")) {
      if (rest == "k" || rest == "khz") hz *= 1000.0;
      if (!(hz >= 8.0 && hz <= 20000.0)) {
        t.notice("The root is 8 … 20000 Hz");
        return;
      }
      t.begin("Sampler Root");
      t.set(meta::kSmpRoot, hz);
      t.setData(meta::samplerWith(d.get(), "rootText", meta::DataValue::str({})));
      t.end();
      return;
    }
  }
  // An identity: "<name>" or "<name> <±cents>[c|¢|cents]".
  std::string name = in;
  double cents = 0.0;
  if (const size_t sp = in.find_last_of(' '); sp != std::string::npos && sp + 1 < in.size() && (in[sp + 1] == '+' || in[sp + 1] == '-')) {
    const std::string tail = in.substr(sp + 1);
    char* end = nullptr;
    const double c = std::strtod(tail.c_str(), &end);
    const std::string_view unit(end);
    if (end != tail.c_str() && (unit.empty() || unit == "c" || unit == "\xC2\xA2" || unit == "cents" || unit == "ct")) {
      cents = c;
      name = in.substr(0, sp);
    }
  }
  std::vector<PitchGuide> gs;
  t.pitchGuides(8.0f, 20000.0f, gs);
  for (const PitchGuide& x : gs) {
    const std::string pn = t.pitchName(x);
    if (lowerTrim(pn) != name) continue;
    const double hz = static_cast<double>(x.hz) * std::exp2(cents / 1200.0);
    if (!(hz >= 8.0 && hz <= 20000.0)) break;
    t.begin("Sampler Root");
    t.set(meta::kSmpRoot, hz);
    t.setData(meta::samplerWith(d.get(), "rootText", meta::DataValue::str(pn + centsText(cents))));
    t.end();
    return;
  }
  t.notice("'" + in + "' is neither Hz nor a pitch of the track's tuning");
}

bool SamplerFace::onKey(const ui::KeyEvent& e) {
  if (harmEdit_ >= 0 && e.down) {  // SM.13: a ratio being typed
    if (e.key == ui::key::kEscape) {
      harmEdit_ = -1;
      harmTyped_.clear();
    } else if (e.key == ui::key::kBackspace) {
      if (!harmTyped_.empty()) harmTyped_.pop_back();
    } else if (e.key == ui::key::kReturn) {
      commitHarm();
    }
    return true;
  }
  if (!typing_ || !e.down) return false;
  if (e.key == ui::key::kEscape) {
    typing_ = false;
    typed_.clear();
  } else if (e.key == ui::key::kBackspace) {
    if (!typed_.empty()) typed_.pop_back();
  } else if (e.key == ui::key::kReturn) {
    commitRoot();
  }
  return true;  // the field holds the keys while typing
}

void SamplerFace::paintEditor(ui::FacePainter& p, const ui::Theme& th, double now) {
  const Geometry g = geometry();
  const ui::FaceTransform& tf = p.transform();
  edPpu_ = tf.scale;
  if (viewB_ - viewA_ < 1.0) setView(viewA_, viewB_ - viewA_);  // (the scale changed: the deepest zoom with it)
  const Rgba white = ui::rgba(255, 255, 255);
  // The wells: the ruler's and the waveform's.
  p.roundedRect({kEdX0 - 4.0f, kRulerY0, kEdX1 + 4.0f, kRulerY1}, 3.0f, ui::mulAlpha(th.face.track, 0.55f));
  p.roundedRect({kEdX0 - 4.0f, kWaveY0, kEdX1 + 4.0f, kWaveY1}, 5.0f, ui::mulAlpha(th.face.track, 0.85f));
  const float mid = (kWaveY0 + kWaveY1) * 0.5f;
  // WF.1: what follows the view is clipped to the wells (a line just out of view stops at their side).
  ui::DrawList& dl = p.drawList();
  dl.pushClip({tf.origin.x + (kEdX0 - 4.0f) * tf.scale, tf.origin.y + kRulerY0 * tf.scale, tf.origin.x + (kEdX1 + 4.0f) * tf.scale,
               tf.origin.y + (kFlagY1 + 0.5f) * tf.scale});
  const media::WaveformOverview* o = smp_.overview;
  editStats_ = {};
  if (o == nullptr || g.frames <= 0.0 || smp_.built == 0) {
    p.line({kEdX0, mid}, {kEdX1, mid}, 0.8f, ui::mulAlpha(th.face.ink, 0.4f));
    p.text({(kEdX0 + kEdX1) * 0.5f, mid - 10.0f}, smp_.sample == 0 ? "Drop an audio file on the Sampler to load a sample" : "Reading the sample \xE2\x80\xA6", 7.0f,
           ui::mulAlpha(th.face.ink, 0.55f));
  } else {
    // WF.1: the view's frames through the shared renderer (the samples themselves below the overview's resolution,
    // down to the curve and a dot per sample), the played region lit, the rest dim.
    const Rect wr{kEdX0, kWaveY0 + 4.0f, kEdX1, kWaveY1 - 4.0f};
    const double fa = viewA_ * g.frames, fb = viewB_ * g.frames;
    const float x0 = std::round(tf.origin.x + kEdX0 * tf.scale), x1 = std::round(tf.origin.x + kEdX1 * tf.scale);
    const float xa = std::clamp(std::round(tf.origin.x + edXc(std::min(g.start, g.end)) * tf.scale), x0, x1);
    const float xb = std::clamp(std::round(tf.origin.x + edXc(std::max(g.start, g.end)) * tf.scale), xa, x1);
    const Rgba lit = ui::mulAlpha(th.face.signal, 0.9f), dim = ui::mulAlpha(th.face.ink, 0.28f);
    if (xa > x0) editStats_ = drawSample(p, *o, fa, fb, wr, dim, false, -3.0e38f, xa);
    if (xb < x1) {
      const ui::WaveformStats st = drawSample(p, *o, fa, fb, wr, dim, false, xb, 3.0e38f);
      if (editStats_.columns == 0) editStats_ = st;
    }
    if (xb > xa) editStats_ = drawSample(p, *o, fa, fb, wr, lit, false, xa, xb);
  }
  // The ruler: seconds (warp Off) or the sample's beats through its markers (bars numbered), over the view.
  if (g.frames > 0.0) {
    const double len = g.frames / g.rate;
    const Rgba tick = ui::mulAlpha(th.face.ink, 0.45f), strong = ui::mulAlpha(th.face.ink, 0.8f), faint = ui::mulAlpha(th.face.ink, 0.28f);
    const float labelY = (kRulerY0 + kRulerY1) * 0.5f;
    const double span = viewB_ - viewA_;
    if (g.warp == dsp::AudioWarp::Off) {
      const double t0 = viewA_ * len, t1 = viewB_ * len;
      const double perSecond = static_cast<double>(kEdX1 - kEdX0) / std::max(1e-12, t1 - t0);
      double stepS = 60.0;  // the finest step leaving ≥ 28 units between ticks
      for (const double c : {1e-6, 2e-6, 5e-6, 1e-5, 2e-5, 5e-5, 1e-4, 2e-4, 5e-4, 0.001, 0.002, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0,
                             5.0, 10.0, 30.0, 60.0})
        if (perSecond * c >= 28.0) {
          stepS = c;
          break;
        }
      const int decimals = stepS >= 1.0 ? 0 : std::clamp(static_cast<int>(std::ceil(-std::log10(stepS) - 1e-9)), 2, 6);
      auto k = static_cast<int64_t>(std::ceil(t0 / stepS - 1e-9));
      for (int n = 0; static_cast<double>(k) * stepS <= t1 + 1e-12 && n < 400; ++k, ++n) {
        const float x = edX(static_cast<double>(k) * stepS / len);
        p.line({x, kRulerY1 - 4.0f}, {x, kRulerY1 - 0.5f}, 0.6f, tick);
        char lb[24];
        std::snprintf(lb, sizeof lb, "%.*f", decimals, static_cast<double>(k) * stepS);
        if (x + 14.0f < kEdX1) p.textAligned({x + 1.5f, labelY}, lb, 5.5f, tick, 0.0f);
      }
    } else {
      const bool useMarkers = markersShown();
      const dsp::WarpBeatLaw law(useMarkers ? std::span<const float>(g.markers) : std::span<const float>(),
                                 60.0 * g.rate / g.sampleBpm);
      const double b0 = law.law.contentAt(viewA_ * g.frames), b1 = law.law.contentAt(viewB_ * g.frames);
      const double perBeat = static_cast<double>(kEdX1 - kEdX0) / std::max(1e-9, b1 - b0);
      double every = perBeat >= 6.0 ? 1.0 : perBeat >= 1.5 ? 4.0 : 16.0;
      while (every > 1.0 / 64.0 && perBeat * every >= 48.0) every *= 0.5;  // WF.1: zoomed in, the beat's divisions
      const bool beatLabels = perBeat >= 40.0;
      int n = 0;
      for (double q = std::ceil(b0 / every - 1e-9) * every; q <= b1 + 1e-9 && n < 600; q += every, ++n) {
        const float x = edX(law.law.frameAt(q) / g.frames);
        const bool bar = std::fabs(std::remainder(q, 4.0)) < 1e-9, beat = std::fabs(std::remainder(q, 1.0)) < 1e-9;
        p.line({x, bar ? kRulerY0 + 2.0f : beat ? kRulerY1 - 4.0f : kRulerY1 - 2.5f}, {x, kRulerY1 - 0.5f}, bar ? 0.8f : 0.6f,
               bar ? strong : beat ? tick : faint);
        if (x + 10.0f >= kEdX1) continue;
        char lb[24];
        const int barNo = static_cast<int>(std::floor(q / 4.0 + 1e-9)) + 1;
        if (bar && perBeat * 4.0 >= 14.0) {
          std::snprintf(lb, sizeof lb, "%d", barNo);
          p.textAligned({x + 1.5f, labelY}, lb, 5.5f, strong, 0.0f);
        } else if (beat && beatLabels) {
          std::snprintf(lb, sizeof lb, "%d.%d", barNo, static_cast<int>(std::lround(q - 4.0 * std::floor(q / 4.0 + 1e-9))) + 1);
          p.textAligned({x + 1.5f, labelY}, lb, 5.5f, tick, 0.0f);
        }
      }
      if (useMarkers)  // the warp markers: a pin on the ruler, a faint line through the waveform
        for (size_t i = 0; i + 1 < g.markers.size(); i += 2) {
          const double u = static_cast<double>(g.markers[i]) / g.frames;
          if (u < viewA_ - span * 0.05 || u > viewB_ + span * 0.05) continue;
          const float x = edX(u);
          const int32_t part = kPMarker0 + static_cast<int32_t>(i / 2);
          const bool hot = hovered_ == part || drag_ == part;
          p.line({x, kWaveY0}, {x, kWaveY1}, 0.6f, ui::mulAlpha(th.face.value, 0.35f));
          p.triangle({x - 3.5f, kRulerY0 + 1.0f}, {x + 3.5f, kRulerY0 + 1.0f}, {x, kRulerY1 - 1.0f}, hot ? th.face.hover : th.face.value);
        }
    }
  }
  // The loop (Classic with a loop): its bar along the waveform's top, its fade shaded before both ends.
  if (g.loop && g.loopEnd >= viewA_ && g.loopStart <= viewB_) {
    const float a = edXc(g.loopStart), b = edXc(g.loopEnd);
    if (g.loopFade > 0.0 && g.frames > 0.0 && g.warp <= dsp::AudioWarp::RePitch) {  // (a stretch mode has no loop crossfade)
      const double w = g.loopFade / (g.frames / g.rate);
      for (const double e : {g.loopStart, g.loopEnd})
        if (e >= viewA_ && e - w <= viewB_)
          p.roundedRect({edXc(e - w), kLoopY1, std::max(kEdX0 + 0.5f, edXc(e)), kWaveY1 - 1.0f}, 0.0f, ui::mulAlpha(th.face.value, 0.12f));
    }
    const bool hotBar = hovered_ == kPLoop || drag_ == kPLoop;
    p.roundedRect({a, kWaveY0 + 0.5f, std::max(b, a + 1.0f), kLoopY1}, 2.0f, ui::mulAlpha(th.face.value, hotBar ? 0.75f : 0.5f));
    for (const auto& [u, part] : {std::pair{g.loopStart, kPLoopStart}, std::pair{g.loopEnd, kPLoopEnd}}) {
      if (!edIn(u)) continue;
      const float x = edX(u);
      const bool hot = hovered_ == part || drag_ == part;
      p.line({x, kWaveY0 + 0.5f}, {x, kWaveY1 - 0.5f}, hot ? 1.2f : 0.8f, ui::mulAlpha(th.face.value, hot ? 1.0f : 0.7f));
    }
  }
  // One-Shot / Slice fades (warp Off / Re-Pitch): shaded ramps after Start and before End (the sample's seconds). Each
  // is drawn over the part of it in view (the ramp's height there).
  if ((g.mode == SamplerMode::OneShot || g.mode == SamplerMode::Slice) && g.frames > 0.0 && g.warp <= dsp::AudioWarp::RePitch) {
    const double len = g.frames / g.rate;
    const float top = kWaveY0 + 1.0f, bottom = kWaveY1 - 1.0f;
    // A ramp from `from` (the line at the bottom) to `to` (at the top), shaded above the line.
    const auto ramp = [&](double from, double to) {
      const double lo = std::max(std::min(from, to), viewA_), hi = std::min(std::max(from, to), viewB_);
      if (!(hi > lo) || from == to) return;
      const auto yAt = [&](double u) { return bottom + (top - bottom) * static_cast<float>((u - from) / (to - from)); };
      const float xl = edX(lo), xh = edX(hi), yl = yAt(lo), yh = yAt(hi);
      const Vec2 quad[4] = {{xl, top}, {xh, top}, {xh, yh}, {xl, yl}};
      p.convex(quad, ui::mulAlpha(th.face.body, 0.55f));
      p.line({xl, yl}, {xh, yh}, 0.8f, ui::mulAlpha(th.face.value, 0.8f));
    };
    if (g.fadeIn > 0.0) ramp(g.start, g.start + g.fadeIn / len);
    if (g.fadeOut > 0.0) ramp(g.end, g.end - g.fadeOut / len);
  }
  // Slice lines, numbered (slice n plays on key 36 + n − 1).
  if (slicesShown() && g.frames > 0.0)
    for (size_t i = 0; i < g.slices.size(); ++i) {
      const double u = static_cast<double>(g.slices[i]) / g.frames;
      if (!edIn(u)) continue;
      const float x = edX(u);
      const int32_t part = kPSlice0 + static_cast<int32_t>(i);
      const bool hot = hovered_ == part || drag_ == part;
      p.line({x, kWaveY0 + 1.0f}, {x, kWaveY1 - 1.0f}, hot ? 1.3f : 0.9f, hot ? th.face.hover : ui::mulAlpha(white, 0.75f));
      const float nx = i + 1 < g.slices.size() ? std::min(kEdX1, edX(static_cast<double>(g.slices[i + 1]) / g.frames)) : kEdX1;
      if (nx - x >= 9.0f) p.textAligned({x + 1.5f, kWaveY1 - 5.0f}, std::to_string(i + 1), 5.5f, ui::mulAlpha(th.face.ink, 0.8f), 0.0f);
    }
  // Start / End: a line through the waveform and a flag below it (WF.1: out of view the flag waits at the edge, dim).
  for (const auto& [u, part] : {std::pair{g.start, kPStart}, std::pair{g.end, kPEnd}}) {
    const bool in = edIn(u);
    const float x = edXc(u);
    const bool hot = hovered_ == part || drag_ == part;
    const Rgba col = hot ? th.face.hover : th.face.handle;
    if (in) p.line({x, kWaveY0 + 0.5f}, {x, kWaveY1 - 0.5f}, hot ? 1.4f : 1.0f, col);
    const float d = part == kPStart ? 6.0f : -6.0f;
    p.triangle({x, kFlagY0}, {x + d, kFlagY0 + 4.5f}, {x, kFlagY1}, in ? col : ui::mulAlpha(col, 0.5f));
  }
  // SM.13, Granular: the grain region and the live grains in place of the playheads.
  if (g.mode == SamplerMode::Granular) {
    const GrainSpan sp = grainSpan();
    if (sp.b + sp.spray >= viewA_ && sp.a - sp.spray <= viewB_) paintGrainRegion(p, th, editorLane(), kWaveY0 + 0.5f, kWaveY1 - 0.5f);
    paintGrains(p, th, editorLane(), kWaveY0 + 2.0f, kWaveY1 - 2.0f, 1.4f);
  }
  // The voices' playheads (0 … 1 of the sample).
  for (const Shown& sv : shown_v_) {
    if (g.mode == SamplerMode::Granular) break;
    const double u = std::isfinite(sv.v.phase) ? static_cast<double>(sv.v.phase) : 0.0;
    if (!edIn(u)) continue;
    const float a = alphaOf(sv, now);
    const float x = edX(u);
    p.line({x, kWaveY0 + 1.0f}, {x, kWaveY1 - 1.0f}, 1.2f, ui::mulAlpha(th.face.signal, a));
    p.disc({x, kWaveY0 + 1.5f}, 1.4f, ui::mulAlpha(white, a));
  }
  dl.popClip();
  // The info line: the region's times, or the slices and their keys.
  if (g.frames > 0.0) {
    const double len = g.frames / g.rate;
    char b[96];
    if (g.mode == SamplerMode::Slice && g.slices.empty())  // SM.17: say what to do
      std::snprintf(b, sizeof b, "no slices yet: choose a way below, or double-click the waveform");
    else if (g.mode == SamplerMode::Slice && g.slices.size() == 1)
      std::snprintf(b, sizeof b, "1 slice  \xC2\xB7  key 36");
    else if (g.mode == SamplerMode::Slice)
      std::snprintf(b, sizeof b, "%zu slices  \xC2\xB7  keys 36 \xE2\x80\xA6 %zu", g.slices.size(), 35 + std::max<size_t>(g.slices.size(), 1));
    else if (g.mode == SamplerMode::Granular) {
      const GrainSpan sp = grainSpan();
      std::snprintf(b, sizeof b, "grains from %.3f s  \xC2\xB7  %.0f ms each  \xC2\xB7  %u live", sp.a * len,
                    target().value(meta::kSmpGrainSize) * 1000.0, grainCount_);
    } else
      std::snprintf(b, sizeof b, "%.3f s \xE2\x80\x93 %.3f s  of %.3f s", g.start * len, g.end * len, len);
    p.textAligned({kEdX0, kFlagY1 + 5.0f}, b, 6.0f, ui::mulAlpha(th.face.ink, 0.75f), 0.0f);
  }
}

// ---- SM.13: Granular ---------------------------------------------------------------------------------------------------

SamplerFace::GrainSpan SamplerFace::grainSpan() const {
  const MetaTarget& t = target();
  refresh();
  GrainSpan sp;
  const double s = std::clamp(std::min(t.value(meta::kSmpStart), t.value(meta::kSmpEnd)), 0.0, 1.0);
  const double e = std::clamp(std::max(t.value(meta::kSmpStart), t.value(meta::kSmpEnd)), 0.0, 1.0);
  sp.a = s + std::clamp(t.value(meta::kSmpGrainPosition), 0.0, 1.0) * (e - s);
  const media::WaveformOverview* o = smp_.overview;
  sp.seconds = o != nullptr && o->sampleRate() > 0 && o->frames() > 0 ? static_cast<double>(o->frames()) / o->sampleRate() : 0.0;
  if (sp.seconds > 0.0) {
    sp.b = std::min(1.0, sp.a + t.value(meta::kSmpGrainSize) / sp.seconds);
    sp.spray = t.value(meta::kSmpSprayTime) / sp.seconds;
  } else {
    sp.b = sp.a;
  }
  return sp;
}

// The region's right edge (the grain size), else its body (Position; at least ≈ 8 device px wide to grab), else none.
int32_t SamplerFace::grainPart(const Lane& lane, float x, float ppu) const noexcept {
  const float tol = std::max(1.0f, 4.0f / std::max(ppu, 0.1f));
  const GrainSpan sp = grainSpan();
  const float a = lane.x(sp.a), b = lane.x(sp.b);
  if (sp.seconds > 0.0 && std::fabs(x - b) <= tol && b - a >= tol) return kPGrainSize;
  if (x >= a - tol && x <= std::max(b, a + tol)) return kPGrain;
  return kNone;
}

void SamplerFace::paintGrainRegion(ui::FacePainter& p, const ui::Theme& th, const Lane& lane, float y0, float y1) const {
  const GrainSpan sp = grainSpan();
  const float a = lane.x(sp.a), b = std::max(lane.x(sp.b), a + 0.6f);
  const bool hotBody = hovered_ == kPGrain || drag_ == kPGrain, hotSize = hovered_ == kPGrainSize || drag_ == kPGrainSize;
  if (sp.spray > 0.0)  // where spray may move a grain
    p.roundedRect({lane.x(sp.a - sp.spray), y0, lane.x(sp.b + sp.spray), y1}, 0.0f, ui::mulAlpha(th.face.value, 0.08f));
  p.roundedRect({a, y0, b, y1}, 1.5f, ui::mulAlpha(th.face.value, hotBody ? 0.3f : 0.2f));
  p.line({a, y0}, {a, y1}, hotBody ? 1.3f : 1.0f, ui::mulAlpha(th.face.value, 0.95f));
  p.line({b, y0}, {b, y1}, hotSize ? 1.3f : 0.8f, hotSize ? th.face.hover : ui::mulAlpha(th.face.value, 0.7f));
  const float m = (y0 + y1) * 0.5f;  // the size grip
  p.roundedRect({b - 1.2f, m - 3.0f, b + 1.2f, m + 3.0f}, 1.0f, hotSize ? th.face.hover : th.face.value);
}

// A dot per live grain where it reads (its height by its pan, left above), its size and glow by its window's amplitude:
// the wavetable's voice dots (a glow sprite, a disc in the signal colour, a white core). k: the editor's larger scale.
void SamplerFace::paintGrains(ui::FacePainter& p, const ui::Theme& th, const Lane& lane, float y0, float y1, float k) const {
  if (grainCount_ == 0) return;
  const float mid = (y0 + y1) * 0.5f, half = (y1 - y0) * 0.5f - 2.5f * k;
  const auto at = [&](const FaceGrain& g) {
    return Vec2{lane.x(std::isfinite(g.position) ? g.position : 0.0), mid - std::clamp(g.pan, -1.0f, 1.0f) * half};
  };
  const auto out = [&](const FaceGrain& g) {  // WF.1: outside the lane's view (the zoomed editor)
    return std::isfinite(g.position) && (g.position < lane.u0 - 1e-9 || g.position > lane.u1 + 1e-9);
  };
  p.beginGlow();
  for (uint32_t i = 0; i < grainCount_; ++i) {
    if (out(grainBuf_[i])) continue;
    const float amp = std::clamp(grainBuf_[i].amplitude, 0.0f, 1.0f);
    p.glow(at(grainBuf_[i]), (3.0f + 6.0f * amp) * k, ui::withAlpha(th.face.glow, static_cast<uint32_t>(30.0f + 120.0f * amp)));
  }
  p.endGlow();
  for (uint32_t i = 0; i < grainCount_; ++i) {
    if (out(grainBuf_[i])) continue;
    const float amp = std::clamp(grainBuf_[i].amplitude, 0.0f, 1.0f);
    const Vec2 d = at(grainBuf_[i]);
    p.disc(d, (0.7f + 1.7f * amp) * k, ui::mulAlpha(th.face.signal, 0.5f + 0.5f * amp));
    p.disc(d, (0.3f + 0.8f * amp) * k, ui::mulAlpha(ui::rgba(255, 255, 255), 0.4f + 0.6f * amp));
  }
}

// Reads the voice reader's grains (none in other modes: the instance has no grain tap); true when they changed.
bool SamplerFace::pollGrains() {
  VoiceTapReader* r = voices();
  if (grainBuf_.size() < kMaxGrainDots) grainBuf_.resize(kMaxGrainDots);
  const uint32_t before = grainCount_;
  std::array<FaceGrain, 4> head{};  // (a cheap change check: the count and the first few grains)
  std::copy_n(grainBuf_.begin(), std::min<size_t>(before, head.size()), head.begin());
  grainCount_ = r != nullptr ? r->grains(grainBuf_) : 0;
  if (grainCount_ != before) return true;
  for (size_t i = 0; i < std::min<size_t>(grainCount_, head.size()); ++i)
    if (grainBuf_[i].position != head[i].position || grainBuf_[i].amplitude != head[i].amplitude) return true;
  return false;
}

std::vector<std::pair<uint32_t, uint32_t>> SamplerFace::harmonics() const {
  std::vector<std::pair<uint32_t, uint32_t>> h;
  const meta::DataPtr d = target().data();
  if (const meta::DataValue* x = d != nullptr ? d->field("harmonics") : nullptr; x != nullptr && x->kind == meta::DataValue::Kind::Floats)
    for (size_t i = 0; i + 1 < x->floats.size(); i += 2) {
      const float n = x->floats[i], dd = x->floats[i + 1];
      if (n >= 1.0f && dd >= 1.0f && n <= 65536.0f && dd <= 65536.0f && n == std::floor(n) && dd == std::floor(dd))
        h.emplace_back(static_cast<uint32_t>(n), static_cast<uint32_t>(dd));
    }
  if (h.empty()) h = {{1, 1}, {2, 1}, {3, 2}, {3, 1}, {4, 1}, {5, 1}};  // (the atom's default: pq/meta/sampler.h)
  if (h.size() > kHarmMax) h.resize(kHarmMax);
  return h;
}

void SamplerFace::writeHarmonics(const std::vector<std::pair<uint32_t, uint32_t>>& h) {
  std::vector<float> v;
  v.reserve(2 * h.size());
  for (const auto& [n, d] : h) {
    v.push_back(static_cast<float>(n));
    v.push_back(static_cast<float>(d));
  }
  editFloats("harmonics", std::move(v));
}

int32_t SamplerFace::harmPart(Vec2 l) const {
  const size_t n = harmonics().size();
  for (size_t i = 0; i < n; ++i) {
    const Rect r = harmCell(i);
    if (!within(r, l)) continue;
    return l.x >= r.x1 - kHarmX ? kPHarmX0 + static_cast<int32_t>(i) : kPHarm0 + static_cast<int32_t>(i);
  }
  if (n < kHarmMax && within(harmCell(n), l)) return kPHarmAdd;
  return kFrame;
}

void SamplerFace::paintHarm(ui::FacePainter& p, const ui::Theme& th) {
  const bool on = std::lround(target().setting(meta::kSmpHarmonic)) != 0;
  const float dim = on ? 1.0f : 0.55f;  // (Harmonic grains off: the list waits, dimmed)
  p.textAligned({1.0f, kHarm.y0 + 6.0f}, "Ratios", 6.5f, ui::mulAlpha(th.face.ink, 0.85f * dim), 0.0f);
  p.textAligned({1.0f, kHarm.y0 + 17.5f}, on ? "n / d" : "off", 5.5f, ui::mulAlpha(th.face.ink, 0.55f), 0.0f);
  const auto h = harmonics();
  const Rgba cell = ui::lerp(th.face.body, th.face.track, 0.6f);
  for (size_t i = 0; i < h.size(); ++i) {
    const Rect r = harmCell(i);
    const bool editing = harmEdit_ == static_cast<int32_t>(i);
    const bool hot = hovered_ == kPHarm0 + static_cast<int32_t>(i), hotX = hovered_ == kPHarmX0 + static_cast<int32_t>(i);
    p.roundedRect(r, 3.0f, editing ? ui::lerp(th.face.body, th.face.track, 0.4f) : hot ? ui::lerp(cell, th.face.hover, 0.35f) : cell);
    if (editing) {
      const Vec2 ring[5] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}, {r.x0, r.y0}};
      p.polyline(ring, 0.8f, th.circuit.selected);
    }
    std::string text = editing ? harmTyped_ + "|"
                               : std::to_string(h[i].first) + (h[i].second != 1 ? "/" + std::to_string(h[i].second) : std::string());
    p.text({(r.x0 + r.x1 - kHarmX) * 0.5f + 1.0f, (r.y0 + r.y1) * 0.5f}, text, 6.0f,
           editing ? th.face.ink : ui::mulAlpha(th.face.value, dim));
    const float cx = r.x1 - kHarmX * 0.5f, cy = (r.y0 + r.y1) * 0.5f;  // the ×
    const Rgba xc = hotX ? th.face.hover : ui::mulAlpha(th.face.ink, 0.5f * dim);
    p.line({cx - 1.6f, cy - 1.6f}, {cx + 1.6f, cy + 1.6f}, 0.7f, xc);
    p.line({cx - 1.6f, cy + 1.6f}, {cx + 1.6f, cy - 1.6f}, 0.7f, xc);
  }
  if (h.size() < kHarmMax) {
    const Rect r = harmCell(h.size());
    const bool hot = hovered_ == kPHarmAdd;
    p.roundedRect(r, 3.0f, hot ? ui::lerp(cell, th.face.hover, 0.35f) : ui::lerp(th.face.body, th.face.track, 0.35f));
    p.text({(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, "+", 7.5f, ui::mulAlpha(th.face.ink, dim));
  }
}

// The typed ratio: "n" or "n/d", whole numbers 1 … 65536 (one undo step); empty: unchanged; anything else: a notice.
void SamplerFace::commitHarm() {
  const int32_t i = harmEdit_;
  harmEdit_ = -1;
  const std::string in = std::move(harmTyped_);
  harmTyped_.clear();
  if (in.empty() || i < 0) return;
  const size_t slash = in.find('/');
  const auto whole = [](std::string_view v, uint32_t& out) {
    if (v.empty() || v.size() > 6) return false;
    uint64_t x = 0;
    for (const char c : v) {
      if (c < '0' || c > '9') return false;
      x = x * 10 + static_cast<uint64_t>(c - '0');
    }
    if (x < 1 || x > 65536) return false;
    out = static_cast<uint32_t>(x);
    return true;
  };
  uint32_t n = 0, d = 1;
  const std::string_view v(in);
  if (!whole(v.substr(0, slash), n) || (slash != std::string::npos && !whole(v.substr(slash + 1), d))) {
    target().notice("A ratio is n or n/d, whole numbers 1 \xE2\x80\xA6 65536");
    return;
  }
  auto h = harmonics();
  if (static_cast<size_t>(i) >= h.size()) return;
  if (h[static_cast<size_t>(i)] == std::pair{n, d}) return;
  h[static_cast<size_t>(i)] = {n, d};
  writeHarmonics(h);
}

// ---- SM.12: the slicing row --------------------------------------------------------------------------------------------

SamplerFace::Slicing SamplerFace::slicing() const {
  const meta::DataPtr d = target().data();
  Slicing sl;
  const auto num = [&](const char* f, double def) {
    const meta::DataValue* x = d != nullptr ? d->field(f) : nullptr;
    return x != nullptr && x->kind == meta::DataValue::Kind::Number ? x->number : def;
  };
  sl.by = std::clamp(static_cast<int>(std::lround(num("sliceBy", 1.0))), 0, 3);
  sl.sensitivity = std::clamp(num("sensitivity", 1.0), 0.0, 1.0);
  sl.beats = num("sliceBeats", 1.0);
  sl.count = std::clamp(num("sliceCount", 8.0), 2.0, 64.0);
  return sl;
}

std::string SamplerFace::amountText(const Slicing& sl) const {
  char b[32];
  switch (sl.by) {
    case 1: std::snprintf(b, sizeof b, "%d %%", static_cast<int>(std::lround(sl.sensitivity * 100.0))); return b;
    case 2:
      for (size_t i = 0; i < std::size(kSliceBeats); ++i)
        if (std::fabs(kSliceBeats[i] - sl.beats) < 1e-9) return kSliceBeatNames[i];
      std::snprintf(b, sizeof b, "%.2f beats", sl.beats);
      return b;
    case 3: std::snprintf(b, sizeof b, "%d parts", static_cast<int>(sl.count)); return b;
    default: return "\xE2\x80\x94";  // (Manual: none)
  }
}

void SamplerFace::paintSlicing(ui::FacePainter& p, const ui::Theme& th) {
  const Slicing sl = slicing();
  const Rgba cell = ui::lerp(th.face.body, th.face.track, 0.6f);
  for (int k = 0; k < 4; ++k) {
    const Rect r = sliceCell(k);
    const bool on = kSliceCellOrder[k] == sl.by;
    p.roundedRect(r, 3.0f, on ? th.face.value : hovered_ == kPSliceBy0 + k ? ui::lerp(cell, th.face.hover, 0.35f) : cell);
    p.text({(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, kSliceByNames[kSliceCellOrder[k]], 6.5f, on ? th.face.body : th.face.ink);
  }
  const bool hotAmount = hovered_ == kPSliceAmount || drag_ == kPSliceAmount;
  p.roundedRect(kSliceAmount, 3.0f, ui::lerp(th.face.body, th.face.track, 0.5f));
  const Vec2 ring[5] = {{kSliceAmount.x0, kSliceAmount.y0}, {kSliceAmount.x1, kSliceAmount.y0}, {kSliceAmount.x1, kSliceAmount.y1},
                        {kSliceAmount.x0, kSliceAmount.y1}, {kSliceAmount.x0, kSliceAmount.y0}};
  p.polyline(ring, 0.8f, hotAmount ? th.face.hover : th.face.frame);
  p.text({(kSliceAmount.x0 + kSliceAmount.x1) * 0.5f, (kSliceAmount.y0 + kSliceAmount.y1) * 0.5f}, amountText(sl), 6.5f,
         sl.by == 0 ? th.face.ink : th.face.value);
  p.roundedRect(kSliceNotes, 3.0f, hovered_ == kPSliceNotes ? ui::lerp(cell, th.face.hover, 0.35f) : cell);
  p.text({(kSliceNotes.x0 + kSliceNotes.x1) * 0.5f, (kSliceNotes.y0 + kSliceNotes.y1) * 0.5f}, "Slice to Notes", 6.5f, th.face.ink);
  p.textAligned({1.0f, 383.0f}, "Slice n plays on key 36 + n. Double-click the waveform to add a slice, a line to remove it.",
                6.0f, ui::mulAlpha(th.face.ink, 0.7f), 0.0f);
}

void SamplerFace::paintRoot(ui::FacePainter& p, const ui::Theme& th) {
  const meta::DataPtr d = target().data();
  const bool id = !meta::samplerRootText(d.get()).empty();
  p.roundedRect(kRootText, 3.0f, ui::lerp(th.face.body, th.face.track, 0.5f));
  const Rgba frame = typing_ ? th.circuit.selected : hovered_ == kPRootText ? th.face.hover : th.face.frame;
  const Vec2 ring[5] = {{kRootText.x0, kRootText.y0}, {kRootText.x1, kRootText.y0}, {kRootText.x1, kRootText.y1},
                        {kRootText.x0, kRootText.y1}, {kRootText.x0, kRootText.y0}};
  p.polyline(ring, 0.8f, frame);
  const Vec2 c{(kRootText.x0 + kRootText.x1) * 0.5f, (kRootText.y0 + kRootText.y1) * 0.5f};
  if (typing_) p.text(c, typed_ + "|", 7.0f, th.face.ink);
  else p.text(c, rootLabel(), 7.0f, th.face.value);
  struct Cell {
    Rect r;
    bool on;
    const char* label;
    int32_t part;
  };
  for (const Cell& k : {Cell{kRootHz, !id, "Hz", kPRootHz}, Cell{kRootId, id, "ID", kPRootId}}) {
    p.roundedRect(k.r, 3.0f, k.on ? th.face.value : ui::lerp(th.face.body, th.face.track, hovered_ == k.part ? 0.9f : 0.6f));
    p.text({(k.r.x0 + k.r.x1) * 0.5f, (k.r.y0 + k.r.y1) * 0.5f}, k.label, 6.0f, k.on ? th.face.body : th.face.ink);
  }
}

}  // namespace

std::unique_ptr<MetaFace> makeSamplerFace(const meta::Type& t) { return std::make_unique<SamplerFace>(t); }

size_t samplerFacePlayheads(const MetaFace& f) {
  const auto* s = dynamic_cast<const SamplerFace*>(&f);
  return s != nullptr ? s->playheads() : 0;
}

bool samplerFaceWave(const MetaFace& f, SamplerFaceWave& out) {
  const auto* s = dynamic_cast<const SamplerFace*>(&f);
  if (s == nullptr) return false;
  s->wave(out);
  return true;
}

size_t samplerFaceGrainDots(const MetaFace& f) {
  const auto* s = dynamic_cast<const SamplerFace*>(&f);
  return s != nullptr ? s->grainDots() : 0;
}

}  // namespace pq::views
