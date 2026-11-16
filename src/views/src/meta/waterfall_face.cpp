// The Waterfall face (VZ.7; docs/plans/VZ.md "VZ.7"), ported from PQ Meter (pq meter/src/modules/waterfall.cpp: draw,
// rebuildPalette, liveFrame; its options and look) into the analyser frame (analyser_face.h). Frequency × level × time:
// from the front (yaw 0) the newest curve exactly as the Spectrum draws it; turned, the history recedes as a terrain.
//
//   the terrain  up to 140 slices (80 in wireframe), picked by their ABSOLUTE number so a drawn slice stays drawn while it
//                scrolls (no flicker between interleaved sets), projected on the CPU (camera3d.h) into indexed vertex-
//                coloured strips (ui::FacePainter::meshDevice) between successive slices, painted back to front
//                (painter's order by the time axis' depth). Solid: each vertex lit by a fixed key light from the height-
//                field normal, loud areas blending toward emissive (glow), side walls, end curtains and a translucent
//                floor close the volume; the newest slice's ridge glows (three halo layers), faded out as the terrain
//                turns edge-on (where the painter's order flips). Wireframe: per-slice curves, the newest fifth glowing.
//   the leading edge  eased toward the live analysis every displayed frame (the analysis steps at ~90 Hz); the slices
//                slide by the sub-slice phase instead of stepping.
//   colour       a 64 × 64 (frequency × level) table: Heat-map (level through the colormap; Custom: stops in dB on a
//                ui::LevelAxis, VZ.2), Colormap (frequency), Blend, Spectral, Heat, Solid; saturation, hue rotate.
//   overlay      the cage, the frequency grid and dB rules at the newest slice, time labels, faded by how visible each
//                axis is.
// The camera: drag orbits (Shift pans, Ctrl rolls), Alt+wheel zooms, a double-click snaps to the nearest named view, the
// chips SPEC / SIDE / TOP / 3D snap to each; the pose is the instance's data (one undo step per gesture). Idle — the
// feed at rest, the camera still — asks for no frames. 320 × 190.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "analyser_face.h"
#include "camera3d.h"
#include "pq/dsp/waterfall.h"
#include "pq/meta/registry.h"
#include "pq/ui/colormap.h"
#include "pq/ui/theme.h"

namespace pq::views {

namespace {

using namespace analyser;
using sheet::kOnOff;

enum WfOpt : uint32_t {
  wFft, wMulti, wChan, wSlope, wScale, wAtt, wRel, wSpan,
  wYaw, wPitch, wRoll, wZoom, wPanX, wPanY, wFree, wPersp, wFade, wHeight, wSolid, wShade, wLine, wGlow, wGlowAmt, wGrid,
  wLabels, wCage, wFloor, wCeil, wGamma, wColour, wMap, wBase, wS2, wS3, wS4, wS5, wSat, wHue, wCount
};
constexpr const char* kChanNames[] = {"Mono (L+R)", "Left", "Right", "Mid", "Side"};
constexpr const char* kSlopeNames[] = {"Off", "3 dB/oct", "4.5 dB/oct", "6 dB/oct"};
constexpr float kSlopeDb[] = {0.0f, 3.0f, 4.5f, 6.0f};
constexpr const char* kColourNames[] = {"Heat-map (level)", "Colormap (freq)", "Blend", "Spectral", "Heat (level)", "Solid"};
constexpr const char* kMapNames[] = {"Magma", "Inferno", "Viridis", "Grayscale", "Ice", "Gold", "Silver", "Pastel", "Neon",
                                     "Copper", "Custom"};
const Opt kWfOpts[wCount] = {
    {"fft", "FFT size", 11, 9, 14, Opt::Pow2},
    {"multi", "Multi-resolution", 1, 0, 1, Opt::Toggle, kOnOff},
    {"chan", "Channel", 0, 0, 4, Opt::Choice, kChanNames},
    {"slope", "Slope", 0, 0, 3, Opt::Choice, kSlopeNames},
    {"scale", "Frequency scale", 0, 0, 2, Opt::Choice, dsp::kFreqScaleNames},
    {"att", "Attack", 25, 1, 200, Opt::LogNumber, nullptr, "%.0f ms"},
    {"rel", "Release", 350, 50, 2000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"span", "Span", 6, 2, 18, Opt::Number, nullptr, "%.1f s", 0.1},
    {"yaw", "Yaw", 45, -180, 180, Opt::Number, nullptr, "%.0f deg", 1},
    {"pitch", "Pitch", 15, -90, 90, Opt::Number, nullptr, "%.0f deg", 1},
    {"roll", "Roll", 0, -180, 180, Opt::Number, nullptr, "%.0f deg", 1},
    {"zoom", "Zoom", 1, 0.3, 4, Opt::LogNumber, nullptr, "%.2fx"},
    {"panx", "Pan X", 0, -1.5, 1.5, Opt::Number, nullptr, "%.2f", 0.01},
    {"pany", "Pan Y", 0, -1.5, 1.5, Opt::Number, nullptr, "%.2f", 0.01},
    {"free", "Free camera", 0, 0, 1, Opt::Toggle, kOnOff},
    {"persp", "Perspective", 0, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"fade", "Time fade", 0.3, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"height", "Height", 1, 0, 2, Opt::Number, nullptr, "%.2f", 0.01},
    {"solid", "Solid surface", 1, 0, 1, Opt::Toggle, kOnOff},
    {"shade", "Shading", 1, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"lw", "Line width", 1.6, 0.5, 4, Opt::Number, nullptr, "%.1f", 0.1},
    {"glow", "Glow", 1, 0, 1, Opt::Toggle, kOnOff},
    {"glowamt", "Glow amount", 1, 0, 2, Opt::Number, nullptr, "%.2f", 0.01},
    {"grid", "Frequency grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"labels", "Axis labels", 1, 0, 1, Opt::Toggle, kOnOff},
    {"cage", "Cage", 1, 0, 1, Opt::Toggle, kOnOff},
    {"floor", "Floor", -78, -140, -20, Opt::Number, nullptr, "%.0f dB", 1},
    {"ceil", "Ceiling", 0, -60, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"gamma", "Gamma", 1, 0.3, 3, Opt::LogNumber, nullptr, "%.2f"},
    {"col", "Colour mode", 0, 0, 5, Opt::Choice, kColourNames},
    {"map", "Colormap", 0, 0, 10, Opt::Choice, kMapNames},
    {"base", "Custom colours", 0, 0, 9, Opt::Choice, kMapNames},
    {"s2", "Stop 2", -60, -140, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"s3", "Stop 3", -45, -140, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"s4", "Stop 4", -30, -140, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"s5", "Stop 5", -12, -140, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"sat", "Saturation", 1, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"hue", "Hue rotate", 0, -180, 180, Opt::Number, nullptr, "%.0f deg", 1},
};
const cam3d::NamedView kViews[4] = {{"SPEC", 0.0f, 0.0f}, {"SIDE", 90.0f, 0.0f}, {"TOP", 0.0f, 90.0f}, {"3D", 45.0f, 15.0f}};
enum WfPart : int32_t { kSnap0 = kOwnBase, kCamPart = kOwnBase + 4 };

struct Rgb {
  float r, g, b;
};
Rgb hsv(float h, float s, float v) noexcept {
  h -= std::floor(h);
  const float i = std::floor(h * 6.0f), f = h * 6.0f - i;
  const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
  switch (static_cast<int>(i) % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
  }
}
Rgba pack(Rgb c, float a = 1.0f) noexcept {
  auto q = [](float x) { return static_cast<uint8_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return ui::rgba(q(c.r), q(c.g), q(c.b), q(a));
}
Rgba withA(Rgba c, float a) noexcept { return ui::withAlpha(c, static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * ui::alphaOf(c) + 0.5f)); }
Rgba brighten(Rgba c, float amt) noexcept {
  auto up = [&](uint32_t v) { return static_cast<uint8_t>(std::min(255.0f, static_cast<float>(v) + (255.0f - static_cast<float>(v)) * amt)); };
  return ui::rgba(up(ui::redOf(c)), up(ui::greenOf(c)), up(ui::blueOf(c)), static_cast<uint8_t>(ui::alphaOf(c)));
}
Rgba scaleRgb(Rgba c, float k) noexcept {
  auto ch = [&](uint32_t v) { return static_cast<uint8_t>(std::clamp(static_cast<float>(v) * k, 0.0f, 255.0f)); };
  return ui::rgba(ch(ui::redOf(c)), ch(ui::greenOf(c)), ch(ui::blueOf(c)), static_cast<uint8_t>(ui::alphaOf(c)));
}

class WaterfallFace final : public AnalyserFace {
public:
  static constexpr float kW = 320.0f, kH = 190.0f;
  static constexpr uint32_t kP = dsp::WaterfallAnalyzer::kPoints, kMax = dsp::WaterfallAnalyzer::kMaxSlices;
  explicit WaterfallFace(const meta::Type& t) : AnalyserFace(t, kW, kH, kWfOpts) {}

  // Tests.
  const cam3d::Pose& pose() const noexcept { return orbit_.pose; }
  size_t heldSlices() const noexcept { return static_cast<size_t>(std::min<uint64_t>(end_ - begin_, kMax)); }
  dsp::WaterfallConfig configForTest() const { return wfConfig(); }
  dsp::WaterfallConfig configFor(const meta::DataValue* d) {
    loadForTest(d);
    return wfConfig();
  }
  void snapForTest(int view) {
    orbit_.snapTo(kViews[view].yaw, kViews[view].pitch);
    orbit_.pose = {kViews[view].yaw, kViews[view].pitch, 0.0f, 1.0f, 0.0f, 0.0f};
    orbit_.stopSnap();
    writePose();
  }
  uint32_t lastVertices() const noexcept { return verts_; }

protected:
  float sheetW() const noexcept override { return 400.0f; }
  bool hasClear() const noexcept override { return false; }
  uint32_t wantFlags() const noexcept override { return devices::kWantWaterfall; }
  void sendRequest(devices::SpectrumFeed& f) const override { f.setWaterfall(wfConfig()); }
  dsp::WaterfallConfig wfConfig() const {
    dsp::WaterfallConfig c;
    c.fftSize = 1u << std::clamp(opts_.choice(wFft), 9, 14);
    c.multiRes = opts_.on(wMulti);
    c.channel = static_cast<dsp::WaterfallChannel>(std::clamp(opts_.choice(wChan), 0, 4));
    c.slopeDbPerOct = kSlopeDb[std::clamp(opts_.choice(wSlope), 0, 3)];
    c.scale = dsp::freqScaleOf(opts_.choice(wScale));
    c.attackMs = static_cast<float>(opts_[wAtt]);
    c.releaseMs = static_cast<float>(opts_[wRel]);
    c.spanSec = static_cast<float>(opts_[wSpan]);
    return c;
  }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis) {
      for (uint32_t i = wFft; i <= wSpan; ++i) out.push_back(i);
      return;
    }
    for (uint32_t i = wYaw; i <= wSolid; ++i) out.push_back(i);
    if (opts_.on(wSolid)) out.push_back(wShade);
    out.insert(out.end(), {wLine, wGlow});
    if (opts_.on(wGlow)) out.push_back(wGlowAmt);
    out.insert(out.end(), {wGrid, wLabels, wCage, wFloor, wCeil, wGamma, wColour});
    const int col = opts_.choice(wColour);
    if (col <= 1) {
      out.push_back(wMap);
      if (opts_.choice(wMap) == ui::Gradient::kCustomIndex) out.insert(out.end(), {wBase, wS2, wS3, wS4, wS5});
    }
    out.push_back(wSat);
    if (col >= 2 && col <= 4) out.push_back(wHue);
  }
  void optionSet(uint32_t o) override {
    if (o >= wS2 && o <= wS5) {
      float st[4], lo, hi;
      for (int i = 0; i < 4; ++i) st[i] = static_cast<float>(opts_[wS2 + static_cast<uint32_t>(i)]);
      ui::stopFence(st, static_cast<int>(o - wS2), levelAxis(), lo, hi);
      opts_.set(o, std::clamp(opts_[o], static_cast<double>(lo), static_cast<double>(hi)));
    }
  }
  void feedChanged() override {
    seq_ = 0;
    begin_ = end_ = 0;
    gen_ = ~0u;
    liveValid_ = false;
  }

  // ---- reading: the new slices into the face's ring, the live curve ----
  bool readFeed(devices::SpectrumFeed& f, bool& settled) override {
    const uint64_t seq = f.waterfallRead(seq_, st_, live_, settled);
    if (seq == seq_) return false;
    seq_ = seq;
    rate_ = f.config().sampleRate;
    if (st_.generation != gen_) {  // another analysis (or the first look): read what it holds
      gen_ = st_.generation;
      begin_ = end_ = st_.slices > kMax ? st_.slices - kMax : 0;
      liveValid_ = false;
    }
    if (ring_.empty()) ring_.assign(static_cast<size_t>(kMax) * kP, -140.0f);
    const uint64_t from = std::max(end_, st_.slices > kMax ? st_.slices - kMax : 0);
    for (uint64_t s = from; s < st_.slices;) {
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(st_.slices - s, kMax - (s % kMax)));
      const uint32_t got = f.waterfallSlices(s, n, ring_.data() + static_cast<size_t>(s % kMax) * kP);
      if (got == 0) break;
      s += got;
    }
    end_ = st_.slices;
    begin_ = std::max(begin_, end_ > kMax ? end_ - kMax : 0);
    return true;
  }
  // The leading edge eases toward the live curve (PQ liveFrame); the camera's snap eases.
  bool animate(float dt, bool fresh) override {
    bool moving = orbit_.tick(dt);
    if (live_.size() == kP) {
      if (fresh || !liveValid_) {
        edge_.assign(live_.begin(), live_.end());
        liveValid_ = true;
      } else {
        const float hopMs = 1000.0f * static_cast<float>(st_.pitch) / static_cast<float>(std::max(1.0, rate_));
        const float easeMs = std::clamp(0.6f * std::min(hopMs, 11.0f), 4.0f,
                                        std::max(4.0f, 0.5f * std::min(st_.config.attackMs, st_.config.releaseMs)));
        const float a = 1.0f - std::exp(-dt / (easeMs * 0.001f));
        for (uint32_t i = 0; i < kP; ++i) {
          const float d = live_[i] - edge_[i];
          edge_[i] += d * a;
          if (std::fabs(d) > 0.05f) moving = true;
          else edge_[i] = live_[i];
        }
      }
    }
    return moving;
  }

  // ---- the camera ----
  int32_t ownHit(Vec2 l) const noexcept override {
    for (int i = 0; i < 4; ++i)
      if (inside(snapChip(i), l)) return kSnap0 + i;
    return inside(well(), l) ? kCamPart : kNone;
  }
  Rect snapChip(int i) const noexcept {
    const float x0 = 10.0f + static_cast<float>(i) * 25.0f;
    return {x0, 10.0f, x0 + 23.0f, 20.0f};
  }
  bool ownDown(int32_t part, const FacePointer& e) override {
    if (part >= kSnap0 && part < kSnap0 + 4) {
      orbit_.snapTo(kViews[part - kSnap0].yaw, kViews[part - kSnap0].pitch);
      writePose();
      return true;
    }
    if (part != kCamPart) return false;
    if (e.clicks >= 2) {
      orbit_.snapNearest(kViews, 4);
      writePose();
      return true;
    }
    const auto mode = (e.mods & ui::mod::kShift) ? cam3d::Orbit::Drag::Pan
                      : (e.mods & ui::mod::kCtrl) ? cam3d::Orbit::Drag::Roll
                                                  : cam3d::Orbit::Drag::Orbit;
    orbit_.begin(mode, e.device);
    pxW_ = (kW - 16.0f) * e.pxPerUnit;
    pxH_ = (kH - 16.0f) * e.pxPerUnit;
    lineDrag_ = true;
    beginEdit("Camera");
    return true;
  }
  bool ownDrag(const FacePointer& e) override {
    if (!lineDrag_) return false;
    orbit_.move(e.device, pxW_, pxH_);
    if (wakeHost_) wakeHost_();
    return true;
  }
  void ownUp() override {
    orbit_.end();
    writePose();  // (inside the drag's gesture: one undo step; a release snap writes where it lands)
  }
  void cancelDrag() override {
    orbit_.end();
    orbit_.stopSnap();
    AnalyserFace::cancelDrag();  // (undoes the gesture; the pose follows the data again)
    lastPose_ = {-999.0f};
  }
  bool ownWheel(const FacePointer& e) override {
    if (!(e.mods & ui::mod::kAlt) || !inside(well(), e.local)) return false;
    orbit_.wheel(e.wheel.y);
    writePose();
    return true;
  }
  ui::Cursor ownCursor(int32_t part) const noexcept override { return part == kCamPart ? ui::Cursor::Move : ui::Cursor::Pointer; }
  bool ownDescribe(int32_t part, std::string& title, std::string& body) const override {
    if (part >= kSnap0 && part < kSnap0 + 4) {
      static const char* const kWhat[4] = {"The Spectrum's view: frequency across, level up (the newest curve).",
                                           "Time across, level up: a slice at one frequency scrolling.",
                                           "From above: frequency across, time up, coloured by level.",
                                           "The default 3D view (yaw 45, pitch 15)."};
      title = std::string("View: ") + kViews[part - kSnap0].chip;
      body = kWhat[part - kSnap0];
      return true;
    }
    if (part != kCamPart) return false;
    title = "Waterfall";
    body = "Frequency x level x time. Drag to orbit (Shift: pan, Ctrl: roll), Alt+wheel to zoom, double-click to snap to the "
           "nearest view. The signal is not changed.";
    return true;
  }

  void paintBody(ui::FacePainter& p, const ui::Theme& th, bool labels) override {
    paintScene(p, th, labels);
    if (labels)  // over the picture: the named views stay reachable from every angle
      for (int i = 0; i < 4; ++i) chipAt(p, snapChip(i), kViews[i].chip, false, th);
  }
  void paintScene(ui::FacePainter& p, const ui::Theme& th, bool labels);

private:
  cam3d::Pose poseOpts() const noexcept {
    return {static_cast<float>(opts_[wYaw]), static_cast<float>(opts_[wPitch]), static_cast<float>(opts_[wRoll]),
            static_cast<float>(opts_[wZoom]), static_cast<float>(opts_[wPanX]), static_cast<float>(opts_[wPanY])};
  }
  // The pose into the data — where a snap in flight lands (one write: setOption stores the whole record).
  void writePose() {
    const cam3d::Pose t = orbit_.snapping() ? snapTargetPose() : orbit_.pose;
    opts_.set(wYaw, t.yaw), opts_.set(wPitch, t.pitch), opts_.set(wRoll, t.roll);
    opts_.set(wZoom, t.zoom), opts_.set(wPanX, t.panX), opts_.set(wPanY, t.panY);
    lastPose_ = poseOpts();
    setOption(wYaw, opts_[wYaw]);
  }
  cam3d::Pose snapTargetPose() const noexcept {
    cam3d::Orbit o = orbit_;
    for (int i = 0; i < 400 && o.tick(0.1f);) ++i;
    return o.pose;
  }
  ui::LevelAxis levelAxis() const noexcept {
    const float lo = static_cast<float>(opts_[wFloor]);
    return {lo, std::max(static_cast<float>(opts_[wCeil]), lo + 10.0f), static_cast<float>(opts_[wGamma])};
  }
  void rebuildPalette();

  cam3d::Orbit orbit_;
  cam3d::Pose lastPose_{-999.0f};
  float pxW_ = 1.0f, pxH_ = 1.0f;
  devices::WaterfallState st_;
  std::vector<float> live_, edge_, ring_;
  bool liveValid_ = false;
  uint64_t seq_ = 0, begin_ = 0, end_ = 0;
  uint32_t gen_ = ~0u;
  double rate_ = 48000.0;
  // The palette and its key.
  std::vector<Rgba> pal_;
  std::vector<double> palKey_;
  ui::Gradient custom_;
  // Paint scratch.
  std::vector<Vec2> pts_, prevPts_, botPts_, ridge_, mv_;
  std::vector<Rgba> cols_, prevCols_, mc_;
  std::vector<float> h_, prevH_, lt_, ridgeLt_;
  std::vector<uint32_t> mi_, drawIdx_;
  uint32_t verts_ = 0;
};

void WaterfallFace::rebuildPalette() {
  constexpr int kPalF = 64, kPalL = 64;
  std::vector<double> key = {opts_[wColour], opts_[wMap], opts_[wBase], opts_[wS2], opts_[wS3], opts_[wS4], opts_[wS5],
                             opts_[wFloor], opts_[wCeil], opts_[wGamma], opts_[wSat], opts_[wHue]};
  if (key == palKey_ && pal_.size() == static_cast<size_t>(kPalF) * kPalL) return;
  palKey_ = key;
  pal_.resize(static_cast<size_t>(kPalF) * kPalL);
  const int mode = opts_.choice(wColour), map = opts_.choice(wMap);
  const ui::ColorLut* lut;
  if (map == ui::Gradient::kCustomIndex) {
    float st[4];
    for (int i = 0; i < 4; ++i) st[i] = static_cast<float>(opts_[wS2 + static_cast<uint32_t>(i)]);
    ui::levelGradient(custom_, static_cast<ui::Colormap>(std::clamp(opts_.choice(wBase), 0, ui::kColormapCount - 1)), st, levelAxis());
    lut = &custom_.lut;
  } else {
    lut = &ui::colormapLut(static_cast<ui::Colormap>(std::clamp(map, 0, ui::kColormapCount - 1)));
  }
  const Rgb curve{0.43f, 0.85f, 1.0f};
  const float rot = static_cast<float>(opts_[wHue]) / 360.0f, sat = static_cast<float>(opts_[wSat]);
  auto fromLut = [&](float t) {
    const Rgba c = ui::lutAt(*lut, t);
    return Rgb{ui::redOf(c) / 255.0f, ui::greenOf(c) / 255.0f, ui::blueOf(c) / 255.0f};
  };
  for (int fi = 0; fi < kPalF; ++fi) {
    const float f = static_cast<float>(fi) / (kPalF - 1);
    for (int li = 0; li < kPalL; ++li) {
      const float l = static_cast<float>(li) / (kPalL - 1);
      Rgb c;
      switch (mode) {
        case 0: c = fromLut(l); break;  // Heat-map (level)
        case 1: {                        // Colormap (frequency), level as brightness
          const Rgb m = fromLut(f);
          const float lm = 0.15f + 0.85f * l;
          c = {m.r * lm, m.g * lm, m.b * lm};
          break;
        }
        case 3: c = hsv(0.92f + f * 0.80f + rot, 0.72f, 0.55f + 0.45f * l); break;  // Spectral
        case 4: {                                                                  // Heat (level)
          const float s = 0.95f - 0.75f * std::max(0.0f, l - 0.75f) * 4.0f;
          c = hsv(0.72f - 0.72f * l + rot, std::clamp(s, 0.0f, 1.0f), 0.35f + 0.65f * l);
          break;
        }
        case 5: c = {curve.r * (0.55f + 0.45f * l), curve.g * (0.55f + 0.45f * l), curve.b * (0.55f + 0.45f * l)}; break;
        default: {  // Blend
          const float s = 0.82f - 0.42f * std::max(0.0f, l - 0.7f) / 0.3f;
          c = hsv(0.95f + f * 0.72f - 0.10f * l + rot, std::clamp(s, 0.0f, 1.0f), 0.30f + 0.70f * l);
          break;
        }
      }
      if (sat < 1.0f) {
        const float lum = 0.30f * c.r + 0.59f * c.g + 0.11f * c.b;
        c = {c.r * sat + curve.r * lum * (1.0f - sat), c.g * sat + curve.g * lum * (1.0f - sat), c.b * sat + curve.b * lum * (1.0f - sat)};
      }
      pal_[static_cast<size_t>(fi) * kPalL + static_cast<size_t>(li)] = pack(c);
    }
  }
}

void WaterfallFace::paintScene(ui::FacePainter& p, const ui::Theme& /*th*/, bool labels) {
  using namespace ui::signal_theme;
  // The pose follows the data unless the camera is moving (an undo or a sheet row moves it).
  if (const cam3d::Pose q = poseOpts(); !orbit_.dragging() && !orbit_.snapping() && !(q == lastPose_)) {
    orbit_.pose = q;
    lastPose_ = q;
  }
  verts_ = 0;
  const uint64_t n64 = end_ - begin_;
  if (n64 == 0 || ring_.empty()) {
    if (labels) p.textAligned({14.0f, 32.0f}, feed_ != nullptr ? "waiting for audio..." : "plays in a track's chain", 6.0f, kGridLabel, 0.0f);
    return;
  }
  rebuildPalette();
  const ui::FaceTransform& tf = p.transform();
  const Rect plotW{8.0f, 8.0f, kW - 8.0f, kH - 8.0f};
  const Rect plot = deviceRect(p, plotW);
  const float floorDb = static_cast<float>(opts_[wFloor]), ceilDb = std::max(static_cast<float>(opts_[wCeil]), floorDb + 10.0f);
  const float gamma = static_cast<float>(opts_[wGamma]), invRange = 1.0f / (ceilDb - floorDb);
  auto levelT = [&](float db) {
    const float t = std::clamp((db - floorDb) * invRange, 0.0f, 1.0f);
    return gamma == 1.0f ? t : std::pow(t, gamma);
  };
  auto palAt = [&](float ft, float lt) {
    const int fi = std::clamp(static_cast<int>(ft * 63.0f + 0.5f), 0, 63), li = std::clamp(static_cast<int>(lt * 63.0f + 0.5f), 0, 63);
    return pal_[static_cast<size_t>(fi) * 64 + static_cast<size_t>(li)];
  };
  const cam3d::Projector proj(orbit_.pose, opts_.on(wFree), static_cast<float>(opts_[wPersp]), plot);
  const float* R = proj.R;
  // Box (F, H, T): F across at yaw 0, H up, T depth (+1 newest).
  auto P = [&](float T, float F, float H) { return proj(F, H, T); };
  const int nSlices = static_cast<int>(std::min<uint64_t>(n64, kMax));
  const uint64_t first = end_ - static_cast<uint64_t>(nSlices);
  auto sliceAt = [&](int i) { return ring_.data() + static_cast<size_t>((first + static_cast<uint64_t>(i)) % kMax) * kP; };
  const bool solid = opts_.on(wSolid);
  const int maxDrawn = std::min(nSlices, solid ? 140 : 80);
  const int step = (nSlices + maxDrawn - 1) / std::max(1, maxDrawn);
  drawIdx_.clear();
  drawIdx_.push_back(0);
  const int phase0 = static_cast<int>((static_cast<uint64_t>(step) - first % static_cast<uint64_t>(step)) % static_cast<uint64_t>(step));
  for (int i = phase0; i < nSlices - 1; i += step)
    if (i > 0) drawIdx_.push_back(static_cast<uint32_t>(i));
  if (nSlices > 1) drawIdx_.push_back(static_cast<uint32_t>(nSlices - 1));
  const int nDrawn = static_cast<int>(drawIdx_.size());
  const bool oldestFirst = R[8] >= 0.0f;
  const float edgeOn = std::clamp(std::fabs(R[8]) / 0.2f, 0.0f, 1.0f), ridgeFade = edgeOn * edgeOn * (3.0f - 2.0f * edgeOn);
  const int drawPts = std::min<int>(kP, std::max(64, static_cast<int>((plot.x1 - plot.x0) * 0.5f)));
  const float Hs = static_cast<float>(opts_[wHeight]), shading = static_cast<float>(opts_[wShade]);
  const bool glow = opts_.on(wGlow);
  const float glowAmt = static_cast<float>(opts_[wGlowAmt]), lineW = static_cast<float>(opts_[wLine]) * tf.scale * 0.5f;
  const float timeFade = static_cast<float>(opts_[wFade]);
  pts_.resize(static_cast<size_t>(drawPts));
  cols_.resize(static_cast<size_t>(drawPts));
  h_.resize(static_cast<size_t>(drawPts));
  lt_.resize(static_cast<size_t>(drawPts));
  prevPts_.clear();
  ridge_.clear();
  ridgeLt_.clear();
  // ---- lighting (a fixed key light, up-left-front in view space; undersides dimmed) ----
  constexpr float kLx = -0.38f, kLy = 0.62f, kLz = 0.69f;
  const float underDim = 1.0f - 0.45f * shading, emissive = 1.0f + (glow ? 0.5f * glowAmt : 0.0f);
  auto faceLit = [&](float nF, float nH, float nT) {
    float nx = R[0] * nF + R[1] * nH + R[2] * nT, ny = R[3] * nF + R[4] * nH + R[5] * nT, nz = R[6] * nF + R[7] * nH + R[8] * nT;
    const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz + 1e-12f);
    nx *= inv, ny *= inv, nz *= inv;
    float dim = 1.0f;
    if (nz < 0.0f) nx = -nx, ny = -ny, nz = -nz, dim = underDim;
    const float lit = (0.35f + 0.75f * std::max(0.0f, nx * kLx + ny * kLy + nz * kLz)) * dim;
    return 1.0f + (lit - 1.0f) * shading;
  };
  auto litColour = [&](Rgba base, float lit, float a) { return withA(lit >= 1.0f ? brighten(base, std::min(1.0f, lit - 1.0f)) : scaleRgb(base, lit), a); };
  auto shade = [&](Rgba base, float nF, float nH, float nT, float lt, float a) {
    const float lit = faceLit(nF, nH, nT);
    return litColour(base, lit + (emissive - lit) * lt * lt, a);
  };
  // ---- mesh writers (device px) ----
  auto strip = [&](const Vec2* top, const Rgba* tc, const Vec2* bot, const Rgba* bc, int n) {
    if (n < 2) return;
    mv_.clear(), mc_.clear(), mi_.clear();
    for (int i = 0; i < n; ++i) mv_.push_back(top[i]), mc_.push_back(tc[i]);
    for (int i = 0; i < n; ++i) mv_.push_back(bot[i]), mc_.push_back(bc[i]);
    for (int i = 0; i + 1 < n; ++i) {
      const uint32_t a = static_cast<uint32_t>(i), b = a + 1, c = static_cast<uint32_t>(n + i + 1), d = static_cast<uint32_t>(n + i);
      mi_.insert(mi_.end(), {a, b, c, a, c, d});
    }
    p.meshDevice(mv_, mc_, mi_);
    verts_ += static_cast<uint32_t>(mv_.size());
  };
  auto polyline = [&](const Vec2* pts, const Rgba* cs, int n, float thick) {
    if (n < 2 || thick <= 0.0f) return;
    mv_.clear(), mc_.clear(), mi_.clear();
    const float hw = 0.5f * thick;
    for (int i = 0; i + 1 < n; ++i) {
      float dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1e-6f) dx /= len, dy /= len;
      else dx = 1.0f, dy = 0.0f;
      const float nx = -dy * hw, ny = dx * hw;
      const auto base = static_cast<uint32_t>(mv_.size());
      mv_.insert(mv_.end(), {{pts[i].x + nx, pts[i].y + ny}, {pts[i + 1].x + nx, pts[i + 1].y + ny}, {pts[i + 1].x - nx, pts[i + 1].y - ny},
                             {pts[i].x - nx, pts[i].y - ny}});
      mc_.insert(mc_.end(), {cs[i], cs[i + 1], cs[i + 1], cs[i]});
      mi_.insert(mi_.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    p.meshDevice(mv_, mc_, mi_);
    verts_ += static_cast<uint32_t>(mv_.size());
  };
  // ---- closing faces ----
  const bool floorFirst = R[7] >= 0.0f;
  auto drawFloor = [&] {
    const Vec2 q[4] = {P(-1, -1, -Hs), P(-1, 1, -Hs), P(1, 1, -Hs), P(1, -1, -Hs)};
    const Rgba fc = withA(scaleRgb(palAt(0.5f, 0.12f), 0.6f), 0.5f);
    const Rgba c4[4] = {fc, fc, fc, fc};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    p.meshDevice(q, c4, idx);
  };
  auto curtain = [&](float T, float a, float outT) {
    a *= ridgeFade;
    if (a <= 0.004f) return;
    botPts_.resize(static_cast<size_t>(drawPts));
    std::vector<Rgba>& top = prevCols_;  // (scratch: rebuilt per slice after this)
    static thread_local std::vector<Rgba> topC, botC;
    topC.resize(static_cast<size_t>(drawPts));
    botC.resize(static_cast<size_t>(drawPts));
    (void)top;
    for (int i = 0; i < drawPts; ++i) {
      const float ft = static_cast<float>(i) / static_cast<float>(drawPts - 1);
      botPts_[static_cast<size_t>(i)] = P(T, 2.0f * ft - 1.0f, -Hs);
      const Rgba base = palAt(ft, lt_[static_cast<size_t>(i)]);
      topC[static_cast<size_t>(i)] = shade(base, 0.0f, 0.0f, outT, lt_[static_cast<size_t>(i)], a);
      botC[static_cast<size_t>(i)] = litColour(base, 0.10f, a);
    }
    strip(pts_.data(), topC.data(), botPts_.data(), botC.data(), drawPts);
  };
  auto drawRidge = [&] {
    const int n = static_cast<int>(ridge_.size());
    if (n < 2 || ridgeFade <= 0.004f) return;
    static thread_local std::vector<Rgba> rc;
    rc.resize(static_cast<size_t>(n));
    if (glow && glowAmt > 0.01f) {
      constexpr float kLayers[3][2] = {{6.0f, 0.05f}, {3.5f, 0.08f}, {2.0f, 0.12f}};
      for (const auto& L : kLayers) {
        for (int i = 0; i < n; ++i) {
          const float lt = ridgeLt_[static_cast<size_t>(i)];
          rc[static_cast<size_t>(i)] = withA(palAt(static_cast<float>(i) / (n - 1), lt), L[1] * glowAmt * ridgeFade * (0.15f + 0.85f * lt * lt * lt));
        }
        polyline(ridge_.data(), rc.data(), n, std::max(1.0f, lineW * L[0]));
      }
    }
    for (int i = 0; i < n; ++i) {
      const float lt = ridgeLt_[static_cast<size_t>(i)];
      rc[static_cast<size_t>(i)] = withA(brighten(palAt(static_cast<float>(i) / (n - 1), lt), 0.20f + 0.45f * lt), ridgeFade);
    }
    polyline(ridge_.data(), rc.data(), n, std::max(0.9f, lineW));
  };
  if (solid && floorFirst) drawFloor();
  const float invDen = 1.0f / static_cast<float>(std::max(1, nSlices - 1)), dF = 2.0f / static_cast<float>(drawPts - 1);
  const float phase = st_.phase;
  float prevT = 0.0f, prevAlpha = 1.0f, prevLtE[2] = {0.0f, 0.0f};
  for (int di = 0; di < nDrawn; ++di) {
    const int si = static_cast<int>(oldestFirst ? drawIdx_[static_cast<size_t>(di)] : drawIdx_[static_cast<size_t>(nDrawn - 1 - di)]);
    const bool newest = si == nSlices - 1;
    const float* lvl = newest && liveValid_ && edge_.size() == kP ? edge_.data() : sliceAt(si);
    const float age = (static_cast<float>(nSlices - 1 - si) + phase) * invDen;
    const float alpha = std::clamp(1.0f - age * timeFade, 0.0f, 1.0f);
    const float T = std::max(-1.0f, 1.0f - 2.0f * age);
    for (int i = 0; i < drawPts; ++i) {
      const float ft = static_cast<float>(i) / static_cast<float>(drawPts - 1);
      const float fp = ft * (kP - 1);
      const int i0 = std::min(static_cast<int>(fp), static_cast<int>(kP) - 2);
      const float fr = fp - static_cast<float>(i0);
      const float lt = levelT(lvl[i0] * (1.0f - fr) + lvl[i0 + 1] * fr);
      lt_[static_cast<size_t>(i)] = lt;
      h_[static_cast<size_t>(i)] = (2.0f * lt - 1.0f) * Hs;
      pts_[static_cast<size_t>(i)] = P(T, 2.0f * ft - 1.0f, h_[static_cast<size_t>(i)]);
    }
    const bool havePrev = di > 0 && static_cast<int>(prevPts_.size()) == drawPts;
    if (solid) {
      const float invDT = havePrev && std::fabs(T - prevT) > 1e-6f ? 1.0f / (T - prevT) : 0.0f;
      for (int i = 0; i < drawPts; ++i) {
        const int a = std::max(0, i - 1), b = std::min(drawPts - 1, i + 1);
        const float dHdF = (h_[static_cast<size_t>(b)] - h_[static_cast<size_t>(a)]) / (static_cast<float>(b - a) * dF);
        const float dHdT = havePrev ? (h_[static_cast<size_t>(i)] - prevH_[static_cast<size_t>(i)]) * invDT : 0.0f;
        const float ft = static_cast<float>(i) / static_cast<float>(drawPts - 1);
        cols_[static_cast<size_t>(i)] = shade(palAt(ft, lt_[static_cast<size_t>(i)]), -dHdF, 1.0f, -dHdT, lt_[static_cast<size_t>(i)], alpha);
      }
      if (di == 0) curtain(T, alpha, newest ? 1.0f : -1.0f);
      if (havePrev) {
        const float farF = R[6] > 0.0f ? -1.0f : 1.0f;
        auto wall = [&](float Fs) {
          const int e = Fs > 0.0f ? drawPts - 1 : 0, ei = Fs > 0.0f ? 1 : 0;
          const float ftE = Fs > 0.0f ? 1.0f : 0.0f;
          const Rgba bPrev = palAt(ftE, prevLtE[ei]), bCur = palAt(ftE, lt_[static_cast<size_t>(e)]);
          const Vec2 top[2] = {prevPts_[static_cast<size_t>(e)], pts_[static_cast<size_t>(e)]};
          const Vec2 bot[2] = {P(prevT, Fs, -Hs), P(T, Fs, -Hs)};
          const Rgba topC[2] = {shade(bPrev, Fs, 0.0f, 0.0f, prevLtE[ei], prevAlpha), shade(bCur, Fs, 0.0f, 0.0f, lt_[static_cast<size_t>(e)], alpha)};
          const Rgba botC[2] = {litColour(bPrev, 0.10f, prevAlpha), litColour(bCur, 0.10f, alpha)};
          strip(top, topC, bot, botC, 2);
        };
        wall(farF);
        strip(prevPts_.data(), prevCols_.data(), pts_.data(), cols_.data(), drawPts);
        wall(-farF);
      }
      if (di == nDrawn - 1 && nDrawn > 1) curtain(T, alpha, newest ? 1.0f : -1.0f);
    } else {
      for (int i = 0; i < drawPts; ++i)
        cols_[static_cast<size_t>(i)] = withA(palAt(static_cast<float>(i) / static_cast<float>(drawPts - 1), lt_[static_cast<size_t>(i)]), alpha);
      if (glow && glowAmt > 0.01f && si >= nSlices - std::max(1, nSlices / 5)) {
        static thread_local std::vector<Rgba> gc;
        gc.resize(static_cast<size_t>(drawPts));
        constexpr float kLayers[3][2] = {{6.0f, 0.045f}, {3.5f, 0.070f}, {2.0f, 0.110f}};
        for (const auto& L : kLayers) {
          for (int i = 0; i < drawPts; ++i) {
            const float k = lt_[static_cast<size_t>(i)] * lt_[static_cast<size_t>(i)] * lt_[static_cast<size_t>(i)];
            gc[static_cast<size_t>(i)] = withA(cols_[static_cast<size_t>(i)], L[1] * glowAmt * (0.15f + 0.85f * k) * alpha);
          }
          polyline(pts_.data(), gc.data(), drawPts, std::max(1.0f, lineW * L[0]));
        }
      }
      polyline(pts_.data(), cols_.data(), drawPts, std::max(1.0f, lineW * 1.6f));
      static thread_local std::vector<Rgba> core;
      core.resize(static_cast<size_t>(drawPts));
      for (int i = 0; i < drawPts; ++i) core[static_cast<size_t>(i)] = brighten(cols_[static_cast<size_t>(i)], 0.30f + 0.40f * lt_[static_cast<size_t>(i)]);
      polyline(pts_.data(), core.data(), drawPts, std::max(0.8f, lineW * 0.7f));
    }
    if (newest) {
      ridge_.assign(pts_.begin(), pts_.end());
      ridgeLt_.assign(lt_.begin(), lt_.end());
    }
    if (solid && !oldestFirst && di == 0) drawRidge();
    prevPts_.assign(pts_.begin(), pts_.end());
    prevCols_.assign(cols_.begin(), cols_.end());
    prevH_.assign(h_.begin(), h_.end());
    prevT = T;
    prevAlpha = alpha;
    prevLtE[0] = lt_.front();
    prevLtE[1] = lt_.back();
  }
  if (solid && oldestFirst) drawRidge();
  if (solid && !floorFirst) drawFloor();
  // ---- overlay ----
  const float timeVis = std::sqrt(R[2] * R[2] + R[5] * R[5]), freqVis = std::sqrt(R[0] * R[0] + R[3] * R[3]);
  const float levelVis = std::sqrt(R[1] * R[1] + R[4] * R[4]);
  auto world = [&](Vec2 d) { return tf.unmap(d); };
  if (opts_.on(wCage)) {
    const float a = 90.0f * timeVis * freqVis;
    if (a > 6.0f) {
      const Rgba col = ui::rgba(150, 160, 185, static_cast<uint8_t>(std::min(255.0f, a)));
      Vec2 c[8];
      for (int i = 0; i < 8; ++i) c[i] = world(P((i & 1) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f, (i & 2) ? Hs : -Hs));
      static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      for (const auto& e : kEdges) p.line(c[e[0]], c[e[1]], 0.5f, col);
    }
  }
  const dsp::FreqAxis axis(st_.config.scale, dsp::WaterfallAnalyzer::kFMin, dsp::WaterfallAnalyzer::kFMax);
  if (opts_.on(wGrid) && freqVis > 0.05f) {
    dsp::FreqGridLine grid[16];
    const int ng = dsp::freqGridLines(st_.config.scale, dsp::WaterfallAnalyzer::kFMin, dsp::WaterfallAnalyzer::kFMax, grid, 16);
    const Rgba gc = ui::mulAlpha(kGrid, freqVis), lc = ui::mulAlpha(kGridLabel, freqVis);
    for (int i = 0; i < ng; ++i) {
      const float F = 2.0f * axis.unit(grid[i].hz) - 1.0f;
      const Vec2 bot = world(P(1.0f, F, -Hs)), top = world(P(1.0f, F, Hs));
      p.line(bot, top, 0.5f, gc);
      if (labels && inside(plotW, bot)) p.textAligned({bot.x + 1.5f, bot.y - 4.0f}, grid[i].label, 5.0f, lc, 0.0f);
    }
    for (float db = std::ceil(floorDb / 12.0f) * 12.0f; db <= ceilDb; db += 12.0f) {
      const float H = (2.0f * levelT(db) - 1.0f) * Hs;
      const Vec2 l = world(P(1.0f, -1.0f, H)), r = world(P(1.0f, 1.0f, H));
      p.line(l, r, 0.5f, gc);
      if (labels && levelVis > 0.3f && inside(plotW, r)) {  // (edge-on, every rule lands on one line)
        char lbl[12];
        std::snprintf(lbl, sizeof lbl, "%.0f", db);
        p.textAligned({r.x - 2.0f, r.y - 3.0f}, lbl, 5.0f, lc, 1.0f);
      }
    }
  }
  if (labels && opts_.on(wLabels)) {
    const float ta = 0.75f * timeVis * timeVis;
    if (ta > 0.03f) {
      const float total = static_cast<float>(nSlices) * static_cast<float>(st_.pitch) / static_cast<float>(std::max(1.0, rate_));
      double stepS = 1.0;
      while (total / stepS > 8.0) stepS *= 2.0;
      while (total / stepS < 3.0 && stepS > 0.05) stepS *= 0.5;
      for (double t = stepS; t < total; t += stepS) {
        const Vec2 a0 = world(P(1.0f - 2.0f * static_cast<float>(t / total), -1.0f, -Hs));
        if (!inside(plotW, a0)) continue;
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "-%.2gs", t);
        p.textAligned({a0.x + 1.5f, a0.y - 3.0f}, lbl, 5.0f, ui::rgba(190, 195, 210, static_cast<uint8_t>(255.0f * ta)), 0.0f);
      }
    }
  }
}

}  // namespace

std::unique_ptr<MetaFace> makeWaterfallFace(const meta::Type& t) { return std::make_unique<WaterfallFace>(t); }

bool waterfallShown(const MetaFace& f, WaterfallShown& out) {
  const auto* w = dynamic_cast<const WaterfallFace*>(&f);
  if (w == nullptr) return false;
  out.acquired = w->acquiredForTest();
  out.frozen = w->frozenForTest();
  const cam3d::Pose& q = w->pose();
  out.yaw = q.yaw, out.pitch = q.pitch, out.roll = q.roll, out.zoom = q.zoom;
  out.slices = w->heldSlices();
  out.vertices = w->lastVertices();
  out.config = w->configForTest();
  return true;
}

bool waterfallTestControl(MetaFace& f, std::string_view key, double value) {
  auto* w = dynamic_cast<WaterfallFace*>(&f);
  if (w == nullptr) return false;
  if (key == "freeze") w->setFrozenForTest(value != 0.0);
  else if (key == "sheet") w->setSheetForTest(value != 0.0);
  else if (key == "view") w->snapForTest(static_cast<int>(value));
  else w->setOptionForTest(key, value);
  return true;
}

dsp::WaterfallConfig waterfallRequest(const meta::DataValue* data) {
  const meta::Type* t = meta::findType("waterfall");
  if (t == nullptr) return {};
  std::unique_ptr<MetaFace> f = makeFace(*t);
  auto* w = f ? dynamic_cast<WaterfallFace*>(f.get()) : nullptr;
  return w != nullptr ? w->configFor(data) : dsp::WaterfallConfig{};
}

}  // namespace pq::views
