// The Stereo Spectrogram face (VZ.7; docs/plans/VZ.md "VZ.7"), ported from PQ Meter (pq meter/src/modules/
// stereo_spectrogram.cpp: draw, redraw, drawRibbon, ingest, prune; its options and look) into the analyser frame
// (analyser_face.h). Time × frequency × pan: at yaw 0 a spectrogram (time across, frequency up), at yaw 90 the Stereo
// Image's heat map (pan across), at pitch 90 pan over time.
//
//   Point cloud  the feed's reassigned points (pq/dsp/stereo_cloud.h) of the span, each projected (camera3d.h) and
//                splatted (a tent, max-combined, radius by the dot size and how much time faces the camera) into a power
//                accumulator of 1024² × detail, then resolved per canvas pixel (max over its cells) through the colour:
//                a colormap by level (Custom: stops in dB on a ui::LevelAxis, VZ.2) or by frequency / pan / time / the
//                band ramp, gamma, the colour fade, the age fade as darkening or transparency (heat decay coupled to the
//                time axis' depth, time fade) and an optional bloom. The canvas (≤ 768² × detail, the plot's device px)
//                is the face's own texture (ui::TexturePool, as the Spectrogram's): re-splatted only when the cloud, the
//                camera or an option changed, and only the rows lit now or last time are uploaded.
//   Ribbon       the feed's ribbon slices (the Stereo Image's pan / width / level per band) as a lit band surface between
//                slices with the width filled about the position, noise gate, precision, Y smoothing, the newest slice's
//                position line.
//   overlay      the cage, the frequency grid (where frequency faces the camera), the centre line, time and L / C / R
//                labels, each faded by how visible its axis is.
// The camera: drag orbits (Shift pans, Ctrl rolls), Alt+wheel zooms, a double-click snaps to the nearest named view, the
// chips SPEC / IMAGE / PAN / 3D snap to each; the pose is the instance's data. Idle — silent for the span, the camera
// still — asks for no frames. 300 × 200.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include "analyser_face.h"
#include "camera3d.h"
#include "pq/dsp/stereo_cloud.h"
#include "pq/meta/registry.h"
#include "pq/ui/colormap.h"
#include "pq/ui/texture_pool.h"
#include "pq/ui/renderer.h"
#include "pq/ui/theme.h"

namespace pq::views {

namespace {

using namespace analyser;
using sheet::kOnOff;

enum SsOpt : uint32_t {
  sFft, sReassign, sConsensus, sLo, sRibAtt, sRibRel, sRibCoh, sScale, sSpan,
  sMode, sYaw, sPitch, sRoll, sZoom, sPanX, sPanY, sFree, sPersp, sFade, sDecay, sAngle, sDot, sGlow, sGlowR, sDetail,
  sRibFill, sRibWidth, sRibLine, sGate, sPrec, sSmoothY, sGrid, sCentre, sLabels, sCage, sFloor, sCeil, sGamma, sCFade,
  sAgeFade, sSource, sMap, sBase, sS2, sS3, sS4, sS5, sCount
};
constexpr const char* kLoNames[] = {"1x (fastest)", "2x", "4x", "8x (finest)"};
constexpr const char* kModeNames[] = {"Point cloud", "Ribbon"};
constexpr const char* kDetailNames[] = {"Standard", "High", "Ultra"};
constexpr const char* kAgeNames[] = {"Colour ramp", "Transparency"};
constexpr const char* kSourceNames[] = {"Level", "Frequency", "Pan", "Time", "Band ramp"};
constexpr const char* kMapNames[] = {"Magma", "Inferno", "Viridis", "Grayscale", "Ice", "Gold", "Silver", "Pastel", "Neon",
                                     "Copper", "Custom"};
enum Source : int { kLevel, kFrequency, kPan, kTime, kBandRamp };
const Opt kSsOpts[sCount] = {
    {"fft", "FFT size", 11, 9, 13, Opt::Pow2},
    {"reassign", "Reassignment", 1, 0, 1, Opt::Toggle, kOnOff},
    {"cons", "Consensus", 0.5, 0, 2, Opt::Number, nullptr, "%.2f bins", 0.05},
    {"lo", "Low-band window", 1, 0, 3, Opt::Choice, kLoNames},
    {"ratt", "Ribbon attack", 60, 1, 400, Opt::LogNumber, nullptr, "%.0f ms"},
    {"rrel", "Ribbon release", 400, 20, 3000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"rcoh", "Width smoothing", 400, 20, 3000, Opt::LogNumber, nullptr, "%.0f ms"},
    {"scale", "Frequency scale", 0, 0, 2, Opt::Choice, dsp::kFreqScaleNames},
    {"span", "Span", 6, 2, 18, Opt::Number, nullptr, "%.1f s", 0.1},
    {"mode", "Mode", 0, 0, 1, Opt::Choice, kModeNames},
    {"yaw", "Yaw", 45, -180, 180, Opt::Number, nullptr, "%.0f deg", 1},
    {"pitch", "Pitch", 15, -90, 90, Opt::Number, nullptr, "%.0f deg", 1},
    {"roll", "Roll", 0, -180, 180, Opt::Number, nullptr, "%.0f deg", 1},
    {"zoom", "Zoom", 1, 0.3, 4, Opt::LogNumber, nullptr, "%.2fx"},
    {"panx", "Pan X", 0, -1.5, 1.5, Opt::Number, nullptr, "%.2f", 0.01},
    {"pany", "Pan Y", 0, -1.5, 1.5, Opt::Number, nullptr, "%.2f", 0.01},
    {"free", "Free camera", 0, 0, 1, Opt::Toggle, kOnOff},
    {"persp", "Perspective", 0, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"fade", "Time fade", 0, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"decay", "Heat decay", 10, 0.1, 10, Opt::LogNumber, nullptr, "%.2f s"},
    {"angle", "Angle coupling", 0, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"dot", "Dot size", 2.5, 1, 6, Opt::Number, nullptr, "%.1f px", 0.1},
    {"glow", "Glow", 0, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"glowr", "Glow radius", 16, 6, 40, Opt::Number, nullptr, "%.0f px", 1},
    {"detail", "Detail", 0, 0, 2, Opt::Choice, kDetailNames},
    {"rfill", "Ribbon fill", 0.55, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"rwidth", "Width scale", 1, 0, 2, Opt::Number, nullptr, "%.2fx", 0.01},
    {"rline", "Line width", 1.4, 0.5, 4, Opt::Number, nullptr, "%.1f px", 0.1},
    {"gate", "Noise gate", 0, 0, 80, Opt::Number, nullptr, "%.0f dB", 1},
    {"prec", "Precision", 0, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"sy", "Y smoothing", 0, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"grid", "Frequency grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"centre", "Centre line", 1, 0, 1, Opt::Toggle, kOnOff},
    {"labels", "Axis labels", 1, 0, 1, Opt::Toggle, kOnOff},
    {"cage", "Cage", 1, 0, 1, Opt::Toggle, kOnOff},
    {"floor", "Floor", -100, -140, -40, Opt::Number, nullptr, "%.0f dB", 1},
    {"ceil", "Ceiling", -24, -60, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"gamma", "Gamma", 1.57, 0.3, 3, Opt::LogNumber, nullptr, "%.2f"},
    {"cfade", "Colour fade", 0.4, 0, 1, Opt::Number, nullptr, "%.2f", 0.01},
    {"age", "Age fade", 1, 0, 1, Opt::Choice, kAgeNames},
    {"src", "Colour source", 0, 0, 4, Opt::Choice, kSourceNames},
    {"map", "Colormap", 0, 0, 10, Opt::Choice, kMapNames},
    {"base", "Custom colours", 0, 0, 9, Opt::Choice, kMapNames},
    {"s2", "Stop 2", -85, -140, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"s3", "Stop 3", -70, -140, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"s4", "Stop 4", -55, -140, 12, Opt::Number, nullptr, "%.0f dB", 1},
    {"s5", "Stop 5", -35, -140, 12, Opt::Number, nullptr, "%.0f dB", 1},
};
const cam3d::NamedView kViews[4] = {{"SPEC", 0.0f, 0.0f}, {"IMAGE", 90.0f, 0.0f}, {"PAN", 0.0f, 90.0f}, {"3D", 45.0f, 15.0f}};
enum SsPart : int32_t { kSnap0 = kOwnBase, kCamPart = kOwnBase + 4 };
constexpr int kAccum = 1024, kMaxCanvas = 768;
constexpr float kMaxSplatRadius = 3.0f;
constexpr size_t kMaxCloud = 100000;
constexpr uint32_t kRibMaxSlices = 480, kRibTargetSlices = 360;
// PQ's band ramp: the theme's low / mid / high band colours, two stops interpolated, at 200 / 1k / 5k Hz.
constexpr float kBandStops[5][3] = {{0.95f, 0.36f, 0.25f}, {0.975f, 0.575f, 0.245f}, {1.00f, 0.79f, 0.24f},
                                    {0.715f, 0.82f, 0.62f}, {0.43f, 0.85f, 1.00f}};

struct CloudPt {
  double tSamples;
  float logF, pan, power;
  float rx = -1.0f, ry = 0.0f;  // where it was splatted in the scrolling canvas (accumulator cells; < 0: not)
  uint32_t mark = 0;            // the repair that last re-splatted it
};

class StereoSpectrogramFace final : public AnalyserFace {
public:
  static constexpr float kW = 300.0f, kH = 200.0f;
  explicit StereoSpectrogramFace(const meta::Type& t) : AnalyserFace(t, kW, kH, kSsOpts) {}
  ~StereoSpectrogramFace() override {
    if (tex_ != 0 && pool_) pool_->destroy(tex_);
  }

  // Tests.
  const cam3d::Pose& pose() const noexcept { return orbit_.pose; }
  size_t cloudSize() const noexcept { return cloud_.size(); }
  size_t ribbonSize() const noexcept { return ribbon_.size(); }
  uint64_t uploadedRows() const noexcept { return uploadedRows_; }
  uint64_t redraws() const noexcept { return redraws_; }
  uint64_t rebuilds() const noexcept { return rebuilds_; }
  bool scrolling() const noexcept { return scrolling_; }
  size_t verifyForTest() { return verifyCanvas(); }
  dsp::StereoCloudConfig configForTest() const { return cloudConfig(); }
  dsp::StereoCloudConfig configFor(const meta::DataValue* d) {
    loadForTest(d);
    return cloudConfig();
  }
  void snapForTest(int view) {
    orbit_.pose = {kViews[view].yaw, kViews[view].pitch, 0.0f, 1.0f, 0.0f, 0.0f};
    orbit_.stopSnap();
    writePose();
  }
  // The canvas pixel the splat puts a point at (time age u 0 … 1, frequency unit, pan), canvas size w × h (tests).
  static Vec2 canvasAt(const cam3d::Pose& q, float u, float fUnit, float pan, int w, int h) {
    const cam3d::Projector pr(q, false, 0.0f, Rect{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
    return pr(1.0f - 2.0f * u, 2.0f * fUnit - 1.0f, pan);
  }

protected:
  float sheetW() const noexcept override { return 420.0f; }
  bool hasClear() const noexcept override { return false; }
  uint32_t wantFlags() const noexcept override { return devices::kWantCloud; }
  void sendRequest(devices::SpectrumFeed& f) const override { f.setCloud(cloudConfig()); }
  dsp::StereoCloudConfig cloudConfig() const {
    dsp::StereoCloudConfig c;
    c.fftSize = 1u << std::clamp(opts_.choice(sFft), 9, 13);
    c.reassign = opts_.on(sReassign);
    c.consensus = static_cast<float>(opts_[sConsensus]);
    c.scale = dsp::freqScaleOf(opts_.choice(sScale));
    c.loFactor = 1u << std::clamp(opts_.choice(sLo), 0, 3);
    c.ribAttackMs = static_cast<float>(opts_[sRibAtt]);
    c.ribReleaseMs = static_cast<float>(opts_[sRibRel]);
    c.ribCoherenceMs = static_cast<float>(opts_[sRibCoh]);
    c.floorDb = static_cast<float>(opts_[sFloor]);
    c.restSec = static_cast<float>(opts_[sSpan]) + 0.5f;
    return c;
  }
  void rows(int tab, std::vector<uint32_t>& out) const override {
    out.clear();
    if (tab == sheet::kAnalysis) {
      for (uint32_t i = sFft; i <= sSpan; ++i) out.push_back(i);
      return;
    }
    for (uint32_t i = sMode; i <= sAngle; ++i) out.push_back(i);
    if (opts_.choice(sMode) == 0) out.insert(out.end(), {sDot, sGlow, sGlowR, sDetail});
    else out.insert(out.end(), {sRibFill, sRibWidth, sRibLine, sGate, sPrec, sSmoothY});
    out.insert(out.end(), {sGrid, sCentre, sLabels, sCage, sFloor, sCeil, sGamma, sCFade, sAgeFade, sSource});
    if (opts_.choice(sSource) != kBandRamp) {
      out.push_back(sMap);
      if (opts_.choice(sMap) == ui::Gradient::kCustomIndex) out.insert(out.end(), {sBase, sS2, sS3, sS4, sS5});
    }
  }
  void optionSet(uint32_t o) override {
    if (o >= sS2 && o <= sS5) {
      float st[4], lo, hi;
      for (int i = 0; i < 4; ++i) st[i] = static_cast<float>(opts_[sS2 + static_cast<uint32_t>(i)]);
      ui::stopFence(st, static_cast<int>(o - sS2), levelAxis(), lo, hi);
      opts_.set(o, std::clamp(opts_[o], static_cast<double>(lo), static_cast<double>(hi)));
    }
  }
  void feedChanged() override {
    seq_ = 0;
    gen_ = ~0u;
    clearCloud();
    ribbon_.clear();
    ribbonRaw_.clear();
    head_ = -1.0;
    valid_ = false;
  }

  // ---- reading: the new points and slices ----
  bool readFeed(devices::SpectrumFeed& f, bool& settled) override {
    const uint64_t seq = f.cloudRead(seq_, st_, settled);
    if (seq == seq_) return false;
    seq_ = seq;
    rate_ = f.config().sampleRate;
    if (st_.generation != gen_) {  // a restart: the old cloud is gone
      gen_ = st_.generation;
      clearCloud();
      cloudMax_ = 0.0f;
      ribbon_.clear();
      ribbonRaw_.clear();
      ribCounter_ = 0;
      head_ = -1.0;
      readPts_ = readSlices_ = 0;
    }
    const dsp::FreqAxis axis(st_.config.scale, st_.config.fMin, st_.config.fMax);
    readPts_ = std::max(readPts_, st_.points > devices::SpectrumFeed::kCloudRing ? st_.points - devices::SpectrumFeed::kCloudRing : 0);
    scratch_.resize(4096);
    while (readPts_ < st_.points) {
      const uint32_t got = f.cloudPoints(readPts_, static_cast<uint32_t>(std::min<uint64_t>(4096, st_.points - readPts_)), scratch_.data());
      if (got == 0) break;
      for (uint32_t i = 0; i < got; ++i) {
        const dsp::CloudPoint& q = scratch_[i];
        if (q.freqHz < st_.config.fMin || q.freqHz > st_.config.fMax) continue;
        cloud_.push_back({q.tSamples, axis.unit(q.freqHz), std::clamp(q.pan, -1.0f, 1.0f), q.power});
        cloudMax_ = std::max(cloudMax_, q.power);
      }
      readPts_ += got;
    }
    readSlices_ = std::max(readSlices_, st_.slices > devices::SpectrumFeed::kCloudSlices ? st_.slices - devices::SpectrumFeed::kCloudSlices : 0);
    const double span = opts_[sSpan];
    const uint32_t stride = std::max<uint32_t>(1, static_cast<uint32_t>(span * rate_ / std::max(1u, st_.hop) / kRibTargetSlices));
    const int rad = smoothRadius();
    sliceBuf_.resize(64);
    while (readSlices_ < st_.slices) {
      const uint32_t got = f.cloudSlices(readSlices_, static_cast<uint32_t>(std::min<uint64_t>(64, st_.slices - readSlices_)), sliceBuf_.data());
      if (got == 0) break;
      for (uint32_t i = 0; i < got; ++i) {
        if (ribCounter_++ % stride != 0) continue;
        ribbonRaw_.push_back(sliceBuf_[i]);
        ribbon_.push_back(sliceBuf_[i]);
        smooth(ribbon_.back(), rad);
      }
      readSlices_ += got;
    }
    if (rad != smoothed_) {
      smoothed_ = rad;
      for (size_t i = 0; i < ribbon_.size() && i < ribbonRaw_.size(); ++i) {
        ribbon_[i] = ribbonRaw_[i];
        smooth(ribbon_[i], rad);
      }
    }
    if (st_.head > head_) head_ = st_.head;
    // Prune to the span.
    const double oldest = head_ - span * rate_;
    const bool track = valid_ && scrolling_;
    while (!cloud_.empty() && (cloud_.front().tSamples < oldest || cloud_.size() > kMaxCloud)) {
      if (track && frontSeq_ < splatSeq_ && cloud_.front().rx >= 0.0f) expiring_.push_back({cloud_.front().rx, cloud_.front().ry});
      cloud_.pop_front();
      ++frontSeq_;
    }
    splatSeq_ = std::max(splatSeq_, frontSeq_);
    if (expiring_.size() > 65536) {  // not painted for a long while: rebuild instead
      expiring_.clear();
      valid_ = false;
    }
    while (!ribbon_.empty() && (ribbon_.front().tSamples < oldest || ribbon_.size() > kRibMaxSlices)) {
      ribbon_.pop_front();
      if (!ribbonRaw_.empty()) ribbonRaw_.pop_front();
    }
    return true;
  }
  bool animate(float dt, bool) override { return orbit_.tick(dt) || canvasStale_; }  // (a stale canvas: one more frame)

  // ---- the camera (as the Waterfall's) ----
  Rect snapChip(int i) const noexcept {
    const float x0 = 10.0f + static_cast<float>(i) * 27.0f;
    return {x0, 10.0f, x0 + 25.0f, 20.0f};
  }
  int32_t ownHit(Vec2 l) const noexcept override {
    for (int i = 0; i < 4; ++i)
      if (inside(snapChip(i), l)) return kSnap0 + i;
    return inside(well(), l) ? kCamPart : kNone;
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
    orbit_.begin((e.mods & ui::mod::kShift) ? cam3d::Orbit::Drag::Pan
                 : (e.mods & ui::mod::kCtrl) ? cam3d::Orbit::Drag::Roll
                                             : cam3d::Orbit::Drag::Orbit,
                 e.device);
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
    writePose();
  }
  void cancelDrag() override {
    orbit_.end();
    orbit_.stopSnap();
    AnalyserFace::cancelDrag();
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
      static const char* const kWhat[4] = {"A spectrogram: time across, frequency up.",
                                           "The Stereo Image's heat map: pan across, frequency up.",
                                           "From above: time across, pan up.", "The default 3D view (yaw 45, pitch 15)."};
      title = std::string("View: ") + kViews[part - kSnap0].chip;
      body = kWhat[part - kSnap0];
      return true;
    }
    if (part != kCamPart) return false;
    title = "Stereo Spectrogram";
    body = "Time x frequency x pan: every reassigned partial of both channels as a point. Drag to orbit (Shift: pan, Ctrl: "
           "roll), Alt+wheel to zoom, double-click to snap to the nearest view. The signal is not changed.";
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
    return {static_cast<float>(opts_[sYaw]), static_cast<float>(opts_[sPitch]), static_cast<float>(opts_[sRoll]),
            static_cast<float>(opts_[sZoom]), static_cast<float>(opts_[sPanX]), static_cast<float>(opts_[sPanY])};
  }
  void writePose() {
    cam3d::Pose t = orbit_.pose;
    if (orbit_.snapping()) {
      cam3d::Orbit o = orbit_;
      for (int i = 0; i < 400 && o.tick(0.1f); ++i) {
      }
      t = o.pose;
    }
    opts_.set(sYaw, t.yaw), opts_.set(sPitch, t.pitch), opts_.set(sRoll, t.roll);
    opts_.set(sZoom, t.zoom), opts_.set(sPanX, t.panX), opts_.set(sPanY, t.panY);
    lastPose_ = poseOpts();
    setOption(sYaw, opts_[sYaw]);
  }
  ui::LevelAxis levelAxis() const noexcept {
    const float lo = static_cast<float>(opts_[sFloor]);
    return {lo, std::max(static_cast<float>(opts_[sCeil]), lo + 10.0f), static_cast<float>(opts_[sGamma])};
  }
  int smoothRadius() const noexcept {
    const double a = opts_[sSmoothY];
    if (a <= 0.01) return 0;
    return std::clamp(static_cast<int>(a * 6.0 * dsp::StereoCloud::kRibBands / 256.0 + 0.5), 1, 24);
  }
  static void smooth(dsp::StereoCloud::RibSlice& s, int r) {
    if (r < 1) return;
    const dsp::StereoCloud::RibSlice src = s;
    constexpr int n = static_cast<int>(dsp::StereoCloud::kRibBands);
    for (int b = 0; b < n; ++b) {
      float sp = 0, sw = 0, sd = 0, wt = 0;
      for (int k = -r; k <= r; ++k) {
        const auto q = static_cast<size_t>(std::clamp(b + k, 0, n - 1));
        const float d = 1.0f - static_cast<float>(std::abs(k)) / static_cast<float>(r + 1);
        sp += src.pos[q] * d, sw += src.width[q] * d, sd += src.db[q] * d, wt += d;
      }
      s.pos[static_cast<size_t>(b)] = sp / wt, s.width[static_cast<size_t>(b)] = sw / wt, s.db[static_cast<size_t>(b)] = sd / wt;
    }
  }
  const ui::ColorLut& lut() {
    if (opts_.choice(sMap) == ui::Gradient::kCustomIndex) {
      float st[4];
      for (int i = 0; i < 4; ++i) st[i] = static_cast<float>(opts_[sS2 + static_cast<uint32_t>(i)]);
      ui::levelGradient(custom_, static_cast<ui::Colormap>(std::clamp(opts_.choice(sBase), 0, ui::kColormapCount - 1)), st, levelAxis());
      return custom_.lut;
    }
    return ui::colormapLut(static_cast<ui::Colormap>(std::clamp(opts_.choice(sMap), 0, ui::kColormapCount - 1)));
  }
  void bandRamp(float* fr) const {  // 256 × rgb, the stops at 200 / 1k / 5k Hz on the axis
    const dsp::FreqAxis axis(st_.config.scale, st_.config.fMin, st_.config.fMax);
    const float at[5] = {0.0f, axis.unit(200.0f), axis.unit(1000.0f), axis.unit(5000.0f), 1.0f};
    for (int i = 0; i < 256; ++i) {
      const float t = i / 255.0f;
      int s = 0;
      while (s < 3 && t > at[s + 1]) ++s;
      const float sp = at[s + 1] - at[s], u = sp > 1e-6f ? std::clamp((t - at[s]) / sp, 0.0f, 1.0f) : 0.0f;
      for (int c = 0; c < 3; ++c) fr[i * 3 + c] = kBandStops[s][c] + (kBandStops[s + 1][c] - kBandStops[s][c]) * u;
    }
  }
  // ---- the canvas (PQ's splat), here a scrolling canvas: see "The canvas" below the class ----
  struct Shade {  // the resolve's colouring for one frame
    const ui::ColorLut* lut = nullptr;
    const float* glut = nullptr;
    float fr[256 * 3];
    bool band = false, hasC = false, hasA = false, timeAttr = false;
    float cfade = 0.0f, invCF = 0.0f, invRange = 1.0f, floorDb = 0.0f, gain = 1.0f, headRel = 0.0f, invSpan = 1.0f;
    uint32_t emptyOut = 0;
    uint32_t colour(float best, float bestA, float bestC) const noexcept;
  };
  void shadeFor(const ui::ColorLut& lut, Shade& s);
  void layout(int w, int h, int det);
  bool setUp(float timeDepth);
  void pointValue(const CloudPt& p, float& val, float& rcp, float& attr) const noexcept;
  template <bool Wrap>
  void splat(float qx, float qy, float val, float rcp, float attr, float* acc, float* cq, float* aq) noexcept;
  void clearAt(float qx, float qy) noexcept;
  void markDirty(float qx, float qy) noexcept;
  template <class Fn>
  void forTiles(float qx, float qy, Fn&& fn) noexcept;
  uint32_t texel(const Shade& s, const float* acc, const float* cq, const float* aq, int x, int y) const noexcept;
  void resolveAll(const Shade& s, int xEnd, int yEnd, int& lx0, int& lx1, int& ly0, int& ly1);
  void bloom(int lx0, int lx1, int& ly0, int& ly1, float glowAmt);
  void rebuild(const ui::ColorLut& lut, float timeDepth, bool allowScroll);
  void advance(const ui::ColorLut& lut);
  void renormalise();
  void uploadRect(int x0, int y0, int x1, int y1);
  void uploadDirty();
  void clearCloud() {
    frontSeq_ += cloud_.size();
    splatSeq_ = frontSeq_;
    cloud_.clear();
    expiring_.clear();
    valid_ = false;
  }
  size_t verifyCanvas();
  void paintRibbon(ui::FacePainter& p, const Rect& plot, const ui::ColorLut& lut);

  cam3d::Orbit orbit_;
  cam3d::Pose lastPose_{-999.0f};
  float pxW_ = 1.0f, pxH_ = 1.0f;
  devices::CloudState st_;
  uint64_t seq_ = 0, readPts_ = 0, readSlices_ = 0, ribCounter_ = 0;
  uint32_t gen_ = ~0u;
  int smoothed_ = 0;
  double rate_ = 48000.0, head_ = -1.0;
  std::deque<CloudPt> cloud_;
  float cloudMax_ = 0.0f;
  std::deque<dsp::StereoCloud::RibSlice> ribbon_, ribbonRaw_;
  std::vector<dsp::CloudPoint> scratch_;
  std::vector<dsp::StereoCloud::RibSlice> sliceBuf_;
  ui::Gradient custom_;
  // The canvas: the window (the plot's device px), the period (the window + margins: the texture's texels that scroll),
  // the accumulator (accW_ / perW_ cells a texel, exactly: the period's cells).
  std::shared_ptr<ui::TexturePool> pool_;
  ui::TextureId tex_ = 0;
  int texW_ = 0, texH_ = 0, canW_ = 0, canH_ = 0, det_ = 0, perW_ = 0, perH_ = 0, accW_ = 0, accH_ = 0;
  double cpx_ = 1.0, cpy_ = 1.0;  // cells a texel
  std::vector<float> accum_, accC_, accA_, bloomA_, bloomB_;
  std::vector<int> rowLo_, rowHi_;  // per accumulator row: the cells that may be lit
  std::vector<uint32_t> rgba_;      // perW_ × perH_
  std::vector<int> dLo_, dHi_;      // per texel row: the texels to resolve (scrolling)
  int texY0_ = 0, texY1_ = -1;      // the texture rows that may differ from empty
  uint32_t texEmpty_ = 0;
  bool valid_ = false, scrolling_ = false, canvasStale_ = false;
  double rebuiltAt_ = -1e9;
  static constexpr double kMovingRebuild = 0.030;  // s between re-splats while the camera moves
  std::vector<double> drawnKey_;
  double drawnHead_ = -1.0;
  // The splat's constants (fixed between rebuilds).
  cam3d::Projector proj_;
  float Rx_ = 1.0f, Ry_ = 1.0f;
  int radX_ = 1, radY_ = 1, source_ = 0;
  bool sepAmp_ = false;
  double fadeK_ = 0.0, spanSec_ = 6.0;  // the age fade (nepers a sample)
  double tRef_ = 0.0, anchor_ = 0.0, vx_ = 0.0, vy_ = 0.0;  // the gain's reference; the scroll (texels a sample)
  int kx_ = 0, ky_ = 0;                                      // the scroll applied (texels, mod the period)
  // Scrolling: which points lie under which tile of the accumulator, and the points that left.
  static constexpr int kTile = 16;
  int tilesW_ = 0, tilesH_ = 0;
  std::vector<std::vector<uint64_t>> tiles_;
  std::vector<uint32_t> tileMark_;
  std::vector<int> touched_;
  uint32_t markNow_ = 0;
  uint64_t frontSeq_ = 0, splatSeq_ = 0;  // cloud_.front()'s sequence; the first not splatted
  std::vector<Vec2> expiring_;
  uint64_t uploadedRows_ = 0, redraws_ = 0, rebuilds_ = 0;
  std::vector<float> gammaLut_;
  float gammaG_ = -1.0f;
  // Ribbon scratch.
  std::vector<Vec2> mv_;
  std::vector<Rgba> mc_;
  std::vector<uint32_t> mi_;
  std::vector<size_t> idx_;
};

// ---- The canvas ----
// PQ re-splats its whole cloud into the accumulator and re-resolves the whole canvas every frame that data arrives
// (≈ 9 ms at a 1440p face). Here, when the camera, the options and the size are unchanged, the canvas SCROLLS instead:
// - Orthographic, the time axis is a constant screen velocity, so the accumulator and the canvas live in a reference
//   frame that the picture moves through: a point is splatted once, at its position less the scroll so far, and the
//   texture is drawn offset by the scroll (whole texels; up to four quads, the canvas being toroidal). The period is
//   the window plus margins that hold the footprints over the edges, with accW_ / perW_ exactly the cells a texel.
// - The age fade is exponential in age, so it is a gain: values are stored relative to tRef_ (renormalised when the gain
//   gets large) and the resolve multiplies by the frame's gain (max commutes with it).
// - A point leaving the span (or the count cap) clears its footprint and re-splats the live points under the tiles it
//   covered (the tile lists): max can't be subtracted, but re-splatting a live point is idempotent, so the cells end
//   exactly as a full re-splat would leave them.
// - Only the texels under changed cells are resolved and uploaded (unless the gain or the Time colours move every texel).
// A full rebuild (PQ's redraw) on a camera / option / size change; every frame while scrolling can't hold (perspective,
// glow, or a box not inside the window).

uint32_t StereoSpectrogramFace::Shade::colour(float best, float bestA, float bestC) const noexcept {
  best *= gain;
  if (best <= 1e-12f) return emptyOut;
  const float lvl = hasA ? std::max(bestA, 1e-20f) : best;
  float t = std::clamp((10.0f * std::log10(lvl) - floorDb) * invRange, 0.0f, 1.0f);
  if (glut) t = glut[static_cast<int>(t * 8191.0f + 0.5f)];
  int alpha = 255;
  if (hasA) {
    alpha = static_cast<int>(std::clamp(1.0f + 10.0f * std::log10(std::max(best, 1e-20f) / lvl) * invRange, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (alpha <= 0) return emptyOut;
  }
  if (timeAttr) bestC = 1.0f - (headRel - bestC) * invSpan;
  const float cf = cfade > 0.0f ? std::clamp(t * invCF, 0.0f, 1.0f) : 1.0f;
  float rr, gg, bb;
  if (band) {
    const float* c = &fr[3 * static_cast<int>(std::clamp(bestC, 0.0f, 1.0f) * 255.0f + 0.5f)];
    const float over = std::max(0.0f, t * 1.45f - 1.0f);
    rr = (c[0] * t + (1.0f - c[0] * t) * over) * cf, gg = (c[1] * t + (1.0f - c[1] * t) * over) * cf, bb = (c[2] * t + (1.0f - c[2] * t) * over) * cf;
  } else {
    const Rgba c = hasC ? ui::lutAt(*lut, bestC) : ui::lutAt(*lut, t);
    const float s = hasC ? t * cf : cf;
    rr = ui::redOf(c) / 255.0f * s, gg = ui::greenOf(c) / 255.0f * s, bb = ui::blueOf(c) / 255.0f * s;
  }
  return ui::rgba(static_cast<uint8_t>(rr * 255.0f + 0.5f), static_cast<uint8_t>(gg * 255.0f + 0.5f), static_cast<uint8_t>(bb * 255.0f + 0.5f),
                  static_cast<uint8_t>(alpha));
}

void StereoSpectrogramFace::shadeFor(const ui::ColorLut& lut, Shade& s) {
  const float floorDb = static_cast<float>(opts_[sFloor]), ceilDb = std::max(static_cast<float>(opts_[sCeil]), floorDb + 10.0f);
  s.lut = &lut;
  s.floorDb = floorDb;
  s.invRange = 1.0f / (ceilDb - floorDb);
  const float gamma = static_cast<float>(opts_[sGamma]);
  if (gamma != 1.0f && gammaG_ != gamma) {
    gammaLut_.resize(8192);
    gammaG_ = gamma;
    for (int i = 0; i < 8192; ++i) gammaLut_[static_cast<size_t>(i)] = std::pow(i / 8191.0f, gamma);
  }
  s.glut = gamma != 1.0f ? gammaLut_.data() : nullptr;
  s.band = source_ == kBandRamp;
  if (s.band) bandRamp(s.fr);
  s.hasC = source_ != kLevel;
  s.hasA = sepAmp_;
  s.timeAttr = source_ == kTime;
  s.cfade = static_cast<float>(opts_[sCFade]);
  s.invCF = s.cfade > 0.0f ? 1.0f / s.cfade : 0.0f;
  const double rel = head_ - tRef_;
  s.gain = fadeK_ > 0.0 ? static_cast<float>(std::exp(-fadeK_ * rel)) : 1.0f;
  s.headRel = static_cast<float>(rel / rate_);
  s.invSpan = static_cast<float>(1.0 / spanSec_);
  const uint32_t empty = lut[0];
  s.emptyOut = sepAmp_ ? (empty & 0x00FFFFFFu) : empty;
}

// The window w × h texels; the period: the smallest ≥ the window + the margins with an exact cell ratio (else the
// closest of 256).
void StereoSpectrogramFace::layout(int w, int h, int det) {
  if (w == canW_ && h == canH_ && det == det_) return;
  canW_ = w, canH_ = h, det_ = det;
  const int acc = kAccum * det, margin = 2 * (static_cast<int>(std::ceil(kMaxSplatRadius * static_cast<float>(det))) + 3);
  auto period = [&](int W, int& P, int& A) {
    const int minP = W + (margin * W + acc - 1) / acc + 1;
    double bestErr = 2.0;
    for (int p = minP; p < minP + 256; ++p) {
      const long long num = static_cast<long long>(p) * acc;
      const long long a = (num + W / 2) / W;
      const double err = std::fabs(static_cast<double>(a) - static_cast<double>(num) / W);
      if (err < bestErr) bestErr = err, P = p, A = static_cast<int>(a);
      if (num % W == 0) break;
    }
  };
  period(w, perW_, accW_);
  period(h, perH_, accH_);
  cpx_ = static_cast<double>(accW_) / perW_, cpy_ = static_cast<double>(accH_) / perH_;
  const size_t an = static_cast<size_t>(accW_) * static_cast<size_t>(accH_);
  accum_.assign(an, 0.0f);
  accC_.clear(), accA_.clear();
  rowLo_.assign(static_cast<size_t>(accH_), accW_);
  rowHi_.assign(static_cast<size_t>(accH_), -1);
  rgba_.assign(static_cast<size_t>(perW_) * static_cast<size_t>(perH_), 0u);
  dLo_.assign(static_cast<size_t>(perH_), perW_);
  dHi_.assign(static_cast<size_t>(perH_), -1);
  tilesW_ = (accW_ + kTile - 1) / kTile, tilesH_ = (accH_ + kTile - 1) / kTile;
  tiles_.resize(static_cast<size_t>(tilesW_) * static_cast<size_t>(tilesH_));
  for (auto& t : tiles_) t.clear();
  tileMark_.assign(tiles_.size(), 0u);
  texY0_ = 0, texY1_ = perH_ - 1;
  valid_ = false;
}

// The splat's constants for the camera and options; true when the canvas can scroll.
bool StereoSpectrogramFace::setUp(float timeDepth) {
  const float winW = static_cast<float>(canW_ * cpx_), winH = static_cast<float>(canH_ * cpy_);
  proj_ = cam3d::Projector(orbit_.pose, opts_.on(sFree), static_cast<float>(opts_[sPersp]), Rect{0.0f, 0.0f, winW, winH});
  const float radCap = kMaxSplatRadius * static_cast<float>(det_), dotR = std::max(0.6f, static_cast<float>(opts_[sDot]));
  const float scaleX = static_cast<float>(cpx_), scaleY = static_cast<float>(cpy_);
  Rx_ = std::min(1.1f + (dotR * scaleX - 1.1f) * timeDepth, radCap), Ry_ = std::min(0.6f + (dotR * scaleY - 0.6f) * timeDepth, radCap);
  radX_ = static_cast<int>(std::ceil(Rx_)), radY_ = static_cast<int>(std::ceil(Ry_));
  spanSec_ = opts_[sSpan];
  const float floorDb = static_cast<float>(opts_[sFloor]), ceilDb = std::max(static_cast<float>(opts_[sCeil]), floorDb + 10.0f);
  const float range = ceilDb - floorDb;
  const float fadeRate = timeDepth * static_cast<float>(opts_[sAngle]) * range * 0.2302585f / std::max(0.01f, static_cast<float>(opts_[sDecay]));
  const float tfK = static_cast<float>(opts_[sFade]) * range * 0.2302585f;
  fadeK_ = (fadeRate + tfK / spanSec_) / rate_;  // fade = exp(−age·fadeRate − u·tfK) = exp(−fadeK_ · age in samples)
  source_ = opts_.choice(sSource);
  sepAmp_ = opts_.choice(sAgeFade) == 1;
  const size_t an = accum_.size();
  if (source_ != kLevel && accC_.size() != an) accC_.assign(an, 0.5f);
  if (sepAmp_ && accA_.size() != an) accA_.assign(an, 0.0f);
  // The scroll: dT/dhead = −2 / span (in samples); the projection is affine without perspective.
  const Vec2 o = proj_(0.0f, 0.0f, 0.0f), t1 = proj_(1.0f, 0.0f, 0.0f);
  const double k = -2.0 / (spanSec_ * rate_);
  vx_ = (t1.x - o.x) * k / cpx_, vy_ = (t1.y - o.y) * k / cpy_;
  if (proj_.perspK != 0.0f || opts_[sGlow] > 0.01) return false;
  for (int c = 0; c < 8; ++c) {
    const Vec2 q = proj_((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : -1.0f);
    if (q.x < -1.0f || q.x > winW + 1.0f || q.y < -1.0f || q.y > winH + 1.0f) return false;
  }
  return true;
}

void StereoSpectrogramFace::pointValue(const CloudPt& p, float& val, float& rcp, float& attr) const noexcept {
  const double dt = p.tSamples - tRef_;
  const float g = fadeK_ > 0.0 ? static_cast<float>(std::exp(fadeK_ * dt)) : 1.0f;
  val = p.power * g;
  rcp = sepAmp_ ? 1.0f / std::max(g, 1e-20f) : 0.0f;
  attr = source_ == kPan ? std::clamp(0.5f * (p.pan + 1.0f), 0.0f, 1.0f) : source_ == kTime ? static_cast<float>(dt / rate_) : p.logF;
}

// PQ's splat of one point (a tent footprint, max into the cells; the colour / unfaded amplitude of the winner beside).
// Wrap: toroidal (the scrolling frame); else clipped to the period (the margins are off the window).
template <bool Wrap>
void StereoSpectrogramFace::splat(float qx, float qy, float val, float rcp, float attr, float* acc, float* cq, float* aq) noexcept {
  const int aw = accW_, ah = accH_, radX = radX_, radY = radY_;
  const float Rx = Rx_, Ry = Ry_;
  const int cx = static_cast<int>(std::floor(qx)), cy = static_cast<int>(std::floor(qy));
  const float fx = qx - static_cast<float>(cx), fy = qy - static_cast<float>(cy);
  const int ox0 = std::max(-radX, static_cast<int>(std::floor(fx - Rx)) + 1), ox1 = std::min(radX + 1, static_cast<int>(std::ceil(fx + Rx)) - 1);
  const int oy0 = std::max(-radY, static_cast<int>(std::floor(fy - Ry)) + 1), oy1 = std::min(radY + 1, static_cast<int>(std::ceil(fy + Ry)) - 1);
  if (ox0 > ox1 || oy0 > oy1) return;
  float wx[24], wy[24], wxMax = 0.0f, wyMax = 0.0f;
  for (int o = ox0; o <= ox1; ++o) wxMax = std::max(wxMax, wx[o + radX] = std::max(0.0f, Rx - std::fabs(static_cast<float>(o) - fx)));
  for (int o = oy0; o <= oy1; ++o) wyMax = std::max(wyMax, wy[o + radY] = std::max(0.0f, Ry - std::fabs(static_cast<float>(o) - fy)));
  if (wxMax <= 0.0f || wyMax <= 0.0f) return;
  const float nrm = val / (wxMax * wyMax);
  int px0 = cx + ox0, px1 = cx + ox1;
  if constexpr (!Wrap) {
    px0 = std::max(0, px0), px1 = std::min(aw - 1, px1);
    if (px0 > px1) return;
  }
  for (int oy = oy0; oy <= oy1; ++oy) {
    const float wyv = wy[oy + radY];
    int py = cy + oy;
    if constexpr (Wrap) {
      py = py < 0 ? py + ah : py >= ah ? py - ah : py;
    } else if (py < 0 || py >= ah) {
      continue;
    }
    if (wyv <= 0.0f) continue;
    const auto ry = static_cast<size_t>(py);
    float* row = &acc[ry * static_cast<size_t>(aw)];
    auto put = [&](int c, float v) {
      if (v > row[c]) {
        row[c] = v;
        const size_t off = ry * static_cast<size_t>(aw) + static_cast<size_t>(c);
        if (cq) cq[off] = attr;
        if (aq) aq[off] = v * rcp;
      }
    };
    bool seam = false;  // (the footprint over the period's seam)
    if constexpr (Wrap) seam = px0 < 0 || px1 >= aw;
    if (seam) {
      const int c0 = px0 < 0 ? px0 + aw : px0, c1 = px1 >= aw ? px1 - aw : px1;
      if (c0 <= c1) rowLo_[ry] = std::min(rowLo_[ry], c0), rowHi_[ry] = std::max(rowHi_[ry], c1);
      else rowLo_[ry] = 0, rowHi_[ry] = aw - 1;
      for (int px = px0; px <= px1; ++px) {
        const int c = px < 0 ? px + aw : px >= aw ? px - aw : px;
        put(c, nrm * wx[px - cx + radX] * wyv);
      }
    } else {
      rowLo_[ry] = std::min(rowLo_[ry], px0);
      rowHi_[ry] = std::max(rowHi_[ry], px1);
      for (int px = px0; px <= px1; ++px) put(px, nrm * wx[px - cx + radX] * wyv);
    }
  }
}

// Zero the cells a point's footprint can cover (scrolling frame).
void StereoSpectrogramFace::clearAt(float qx, float qy) noexcept {
  const int cx = static_cast<int>(std::floor(qx)), cy = static_cast<int>(std::floor(qy));
  for (int oy = -radY_; oy <= radY_ + 1; ++oy) {
    int py = cy + oy;
    py = py < 0 ? py + accH_ : py >= accH_ ? py - accH_ : py;
    float* row = &accum_[static_cast<size_t>(py) * static_cast<size_t>(accW_)];
    for (int ox = -radX_; ox <= radX_ + 1; ++ox) {
      const int c = cx + ox;
      row[c < 0 ? c + accW_ : c >= accW_ ? c - accW_ : c] = 0.0f;
    }
  }
}

// The texels over a footprint's cells (with a texel to spare) want resolving.
void StereoSpectrogramFace::markDirty(float qx, float qy) noexcept {
  const int x0 = static_cast<int>(std::floor((qx - static_cast<float>(radX_) - 1.0f) / cpx_)) - 1;
  const int x1 = static_cast<int>(std::floor((qx + static_cast<float>(radX_) + 2.0f) / cpx_)) + 1;
  const int y0 = static_cast<int>(std::floor((qy - static_cast<float>(radY_) - 1.0f) / cpy_)) - 1;
  const int y1 = static_cast<int>(std::floor((qy + static_cast<float>(radY_) + 2.0f) / cpy_)) + 1;
  const int xw = ((x0 % perW_) + perW_) % perW_, n = x1 - x0;
  for (int y = y0; y <= y1; ++y) {
    const auto r = static_cast<size_t>(((y % perH_) + perH_) % perH_);
    if (xw + n >= perW_) {
      dLo_[r] = 0, dHi_[r] = perW_ - 1;
    } else {
      dLo_[r] = std::min(dLo_[r], xw), dHi_[r] = std::max(dHi_[r], xw + n);
    }
  }
}

// fn(tile) for every tile a footprint at (qx, qy) can touch (scrolling frame).
template <class Fn>
void StereoSpectrogramFace::forTiles(float qx, float qy, Fn&& fn) noexcept {
  const int cx = static_cast<int>(std::floor(qx)), cy = static_cast<int>(std::floor(qy));
  auto range = [](int lo, int n, int a, int tiles, int (&out)[4]) {  // [t0, t1] (and [t2, t3]) of the cells lo … lo + n
    lo = ((lo % a) + a) % a;
    const int hi = lo + n;
    if (hi < a) {
      out[0] = lo / kTile, out[1] = hi / kTile, out[2] = 0, out[3] = -1;
    } else {
      out[0] = lo / kTile, out[1] = tiles - 1, out[2] = 0, out[3] = (hi - a) / kTile;
    }
  };
  int tx[4], ty[4];
  range(cx - radX_ - 1, 2 * radX_ + 3, accW_, tilesW_, tx);
  range(cy - radY_ - 1, 2 * radY_ + 3, accH_, tilesH_, ty);
  for (int j = 0; j < 4; j += 2)
    for (int y = ty[j]; y <= ty[j + 1]; ++y)
      for (int i = 0; i < 4; i += 2)
        for (int x = tx[i]; x <= tx[i + 1]; ++x) fn(y * tilesW_ + x);
}

// One texel: the max of its cells, through the colour.
uint32_t StereoSpectrogramFace::texel(const Shade& s, const float* acc, const float* cq, const float* aq, int x, int y) const noexcept {
  const int r0 = static_cast<int>(static_cast<long long>(y) * accH_ / perH_), r1 = static_cast<int>(static_cast<long long>(y + 1) * accH_ / perH_);
  const int c0 = static_cast<int>(static_cast<long long>(x) * accW_ / perW_), c1 = static_cast<int>(static_cast<long long>(x + 1) * accW_ / perW_);
  float best = 0.0f, bestA = 0.0f, bestC = 0.5f;
  for (int r = r0; r < r1; ++r) {
    const auto rr = static_cast<size_t>(r);
    const float* row = &acc[rr * static_cast<size_t>(accW_)];
    for (int cc = std::max(c0, rowLo_[rr]); cc <= std::min(c1 - 1, rowHi_[rr]); ++cc)
      if (row[cc] > best) {
        best = row[cc];
        const size_t off = rr * static_cast<size_t>(accW_) + static_cast<size_t>(cc);
        if (cq) bestC = cq[off];
        if (aq) bestA = aq[off];
      }
  }
  return s.colour(best, bestA, bestC);
}

// Every texel of rows [0, yEnd) × columns [0, xEnd); the lit box out.
void StereoSpectrogramFace::resolveAll(const Shade& s, int xEnd, int yEnd, int& lx0, int& lx1, int& ly0, int& ly1) {
  const float* cq = s.hasC ? accC_.data() : nullptr;
  const float* aq = s.hasA ? accA_.data() : nullptr;
  lx0 = xEnd, lx1 = -1, ly0 = yEnd, ly1 = -1;
  for (int y = 0; y < yEnd; ++y) {
    const int r0 = static_cast<int>(static_cast<long long>(y) * accH_ / perH_), r1 = static_cast<int>(static_cast<long long>(y + 1) * accH_ / perH_);
    uint32_t* out = &rgba_[static_cast<size_t>(y) * static_cast<size_t>(perW_)];
    int uLo = accW_, uHi = -1;
    for (int r = r0; r < r1; ++r) uLo = std::min(uLo, rowLo_[static_cast<size_t>(r)]), uHi = std::max(uHi, rowHi_[static_cast<size_t>(r)]);
    if (uHi < uLo) {
      std::fill(out, out + xEnd, s.emptyOut);
      continue;
    }
    const int xLo = std::max(0, static_cast<int>(static_cast<long long>(uLo) * perW_ / accW_) - 1);
    const int xHi = std::min(xEnd - 1, static_cast<int>(static_cast<long long>(uHi) * perW_ / accW_) + 1);
    if (xHi < xLo) {
      std::fill(out, out + xEnd, s.emptyOut);
      continue;
    }
    ly0 = std::min(ly0, y), ly1 = y, lx0 = std::min(lx0, xLo), lx1 = std::max(lx1, xHi);
    std::fill(out, out + xLo, s.emptyOut);
    std::fill(out + xHi + 1, out + xEnd, s.emptyOut);
    for (int x = xLo; x <= xHi; ++x) out[x] = texel(s, accum_.data(), cq, aq, x, y);
  }
}

// The bloom (PQ): the lit area at a quarter size, a separable tent blur, added back (the window; full rebuilds only).
void StereoSpectrogramFace::bloom(int lx0, int lx1, int& ly0, int& ly1, float glowAmt) {
  const int w = canW_, h = canH_, stride = perW_;
  constexpr int kDs = 4;
  const int bw = (w + kDs - 1) / kDs, bh = (h + kDs - 1) / kDs;
  const size_t bn = static_cast<size_t>(bw) * static_cast<size_t>(bh) * 3;
  bloomA_.assign(bn, 0.0f);
  bloomB_.resize(bn);
  for (int y = ly0; y <= ly1; ++y)
    for (int x = lx0; x <= lx1; ++x) {
      const uint32_t c = rgba_[static_cast<size_t>(y) * stride + static_cast<size_t>(x)];
      const float am = sepAmp_ ? ui::alphaOf(c) / 255.0f : 1.0f;
      float* a = &bloomA_[(static_cast<size_t>(y / kDs) * bw + static_cast<size_t>(x / kDs)) * 3];
      a[0] += ui::redOf(c) * am, a[1] += ui::greenOf(c) * am, a[2] += ui::blueOf(c) * am;
    }
  const int rad = std::clamp(static_cast<int>(opts_[sGlowR] / kDs + 0.5), 1, 12);
  float wts[13], wsum = static_cast<float>(rad + 1);
  wts[0] = wsum;
  for (int i = 1; i <= rad; ++i) wts[i] = static_cast<float>(rad + 1 - i), wsum += 2.0f * wts[i];
  auto blur = [&](const float* src, float* dst, bool horiz) {
    const int na = horiz ? bh : bw, nb = horiz ? bw : bh;
    for (int a = 0; a < na; ++a)
      for (int b = 0; b < nb; ++b) {
        float s0 = 0, s1 = 0, s2 = 0;
        for (int o = -rad; o <= rad; ++o) {
          const int bc = std::clamp(b + o, 0, nb - 1);
          const size_t i = horiz ? (static_cast<size_t>(a) * bw + bc) * 3 : (static_cast<size_t>(bc) * bw + a) * 3;
          const float wv = wts[o < 0 ? -o : o];
          s0 += src[i] * wv, s1 += src[i + 1] * wv, s2 += src[i + 2] * wv;
        }
        const size_t i = horiz ? (static_cast<size_t>(a) * bw + b) * 3 : (static_cast<size_t>(b) * bw + a) * 3;
        dst[i] = s0 / wsum, dst[i + 1] = s1 / wsum, dst[i + 2] = s2 / wsum;
      }
  };
  blur(bloomA_.data(), bloomB_.data(), true);
  blur(bloomB_.data(), bloomA_.data(), false);
  const float scale = glowAmt * 12.0f / (kDs * kDs);
  for (int y = 0; y < h; ++y) {
    const int by = std::min(bh - 1, y / kDs);
    bool any = false;
    for (int x = 0; x < w; ++x) {
      const float* m = &bloomA_[(static_cast<size_t>(by) * bw + static_cast<size_t>(std::min(bw - 1, x / kDs))) * 3];
      const float ar = m[0] * scale, ag = m[1] * scale, ab = m[2] * scale;
      if (ar < 0.5f && ag < 0.5f && ab < 0.5f) continue;
      uint32_t& c = rgba_[static_cast<size_t>(y) * stride + static_cast<size_t>(x)];
      const auto add = [](uint32_t v, float a) { return static_cast<uint8_t>(std::min(255, static_cast<int>(v) + static_cast<int>(a + 0.5f))); };
      const uint8_t na = sepAmp_ ? add(ui::alphaOf(c), std::max(ar, std::max(ag, ab))) : 255;
      c = ui::rgba(add(ui::redOf(c), ar), add(ui::greenOf(c), ag), add(ui::blueOf(c), ab), na);
      any = true;
    }
    if (any) ly0 = std::min(ly0, y), ly1 = std::max(ly1, y);
  }
}

// PQ's redraw: the whole cloud splatted afresh, every texel resolved.
void StereoSpectrogramFace::rebuild(const ui::ColorLut& lut, float timeDepth, bool allowScroll) {
  ++rebuilds_, ++redraws_;
  for (int r = 0; r < accH_; ++r) {  // clear only what was touched
    const auto rr = static_cast<size_t>(r);
    if (rowHi_[rr] < rowLo_[rr]) continue;
    std::memset(&accum_[rr * static_cast<size_t>(accW_) + static_cast<size_t>(rowLo_[rr])], 0, static_cast<size_t>(rowHi_[rr] - rowLo_[rr] + 1) * sizeof(float));
    rowLo_[rr] = accW_, rowHi_[rr] = -1;
  }
  const bool scroll = setUp(timeDepth) && allowScroll;
  if (scroll || scrolling_)
    for (auto& t : tiles_) t.clear();
  scrolling_ = scroll;
  anchor_ = tRef_ = head_;
  kx_ = ky_ = 0;
  expiring_.clear();
  float* const cq = source_ != kLevel ? accC_.data() : nullptr;
  float* const aq = sepAmp_ ? accA_.data() : nullptr;
  const double spanS = spanSec_ * rate_, fadePerSample = fadeK_;
  double deadT = head_ - spanS - 1.0;
  if (fadePerSample > 0.0 && cloudMax_ > 0.0f) deadT = std::max(deadT, head_ - (std::log(std::max(cloudMax_, 1e-14f)) + 34.54) / fadePerSample);
  const auto invSpan = static_cast<float>(1.0 / spanSec_);
  uint64_t seq = frontSeq_;
  for (CloudPt& pt : cloud_) {
    const uint64_t me = seq++;
    pt.rx = -1.0f;
    if (pt.tSamples < deadT) continue;
    const float age = static_cast<float>((head_ - pt.tSamples) / rate_);
    if (age < 0.0f) continue;
    const float u = age * invSpan;
    if (u > 1.0f) continue;
    float val, rcp, attr;
    pointValue(pt, val, rcp, attr);
    if (val <= 1e-14f) continue;
    const Vec2 q = proj_(1.0f - 2.0f * u, 2.0f * pt.logF - 1.0f, pt.pan);
    if (scroll) {
      pt.rx = q.x < 0.0f ? q.x + static_cast<float>(accW_) : q.x, pt.ry = q.y < 0.0f ? q.y + static_cast<float>(accH_) : q.y;
      splat<true>(pt.rx, pt.ry, val, rcp, attr, accum_.data(), cq, aq);
      forTiles(pt.rx, pt.ry, [&](int t) { tiles_[static_cast<size_t>(t)].push_back(me); });
    } else {
      splat<false>(q.x, q.y, val, rcp, attr, accum_.data(), cq, aq);
    }
  }
  splatSeq_ = frontSeq_ + cloud_.size();
  Shade s;
  shadeFor(lut, s);
  int lx0, lx1, ly0, ly1;
  resolveAll(s, scroll ? perW_ : canW_, scroll ? perH_ : canH_, lx0, lx1, ly0, ly1);
  const float glowAmt = static_cast<float>(opts_[sGlow]) * timeDepth;
  if (!scroll && glowAmt > 0.01f && ly1 >= ly0 && lx1 >= lx0) bloom(lx0, lx1, ly0, ly1, glowAmt);
  for (size_t r = 0; r < dLo_.size(); ++r) dLo_[r] = perW_, dHi_[r] = -1;
  // Only the rows lit now or since the last rebuild change (the rest stay empty both times).
  int y0 = std::min(ly0, texY0_), y1 = std::max(ly1, texY1_);
  if (s.emptyOut != texEmpty_) y0 = 0, y1 = perH_ - 1, texEmpty_ = s.emptyOut;
  texY0_ = ly0, texY1_ = ly1;
  if (y1 >= y0) uploadRect(0, y0, perW_ - 1, y1);
  drawnHead_ = head_;
}

void StereoSpectrogramFace::renormalise() {
  const double d = head_ - tRef_;
  const float f = fadeK_ > 0.0 ? static_cast<float>(std::exp(-fadeK_ * d)) : 1.0f, shift = static_cast<float>(d / rate_);
  const bool shiftC = source_ == kTime && !accC_.empty();
  for (int r = 0; r < accH_; ++r) {
    const auto rr = static_cast<size_t>(r);
    for (int c = rowLo_[rr]; c <= rowHi_[rr]; ++c) {
      const size_t off = rr * static_cast<size_t>(accW_) + static_cast<size_t>(c);
      accum_[off] *= f;
      if (shiftC) accC_[off] -= shift;
    }
  }
  tRef_ = head_;
}

// The new data: the scroll, the points that left, the new points; the texels they touched.
void StereoSpectrogramFace::advance(const ui::ColorLut& lut) {
  ++redraws_;
  const double el = head_ - anchor_;
  kx_ = static_cast<int>(((std::llround(vx_ * el) % perW_) + perW_) % perW_);
  ky_ = static_cast<int>(((std::llround(vy_ * el) % perH_) + perH_) % perH_);
  const double offX = kx_ * cpx_, offY = ky_ * cpy_;
  const bool everyTexel = fadeK_ > 0.0 || source_ == kTime;
  if ((fadeK_ > 0.0 && fadeK_ * (head_ - tRef_) > 30.0) || (source_ == kTime && (head_ - tRef_) / rate_ > 60.0)) renormalise();
  float* const cq = source_ != kLevel ? accC_.data() : nullptr;
  float* const aq = sepAmp_ ? accA_.data() : nullptr;
  if (!expiring_.empty()) {  // clear what they covered, then re-splat the live points under those tiles
    ++markNow_;
    touched_.clear();
    for (const Vec2& e : expiring_) {
      clearAt(e.x, e.y);
      if (!everyTexel) markDirty(e.x, e.y);
      forTiles(e.x, e.y, [&](int t) {
        if (tileMark_[static_cast<size_t>(t)] != markNow_) tileMark_[static_cast<size_t>(t)] = markNow_, touched_.push_back(t);
      });
    }
    expiring_.clear();
    for (const int t : touched_) {
      auto& list = tiles_[static_cast<size_t>(t)];
      size_t keep = 0;
      for (const uint64_t sq : list) {
        if (sq < frontSeq_) continue;  // gone
        list[keep++] = sq;
        CloudPt& p = cloud_[static_cast<size_t>(sq - frontSeq_)];
        if (p.mark == markNow_) continue;
        p.mark = markNow_;
        float val, rcp, attr;
        pointValue(p, val, rcp, attr);
        splat<true>(p.rx, p.ry, val, rcp, attr, accum_.data(), cq, aq);
      }
      list.resize(keep);
    }
  }
  const double gainNow = fadeK_ > 0.0 ? std::exp(-fadeK_ * (head_ - tRef_)) : 1.0;
  const auto invSpan = static_cast<float>(1.0 / spanSec_);
  const uint64_t end = frontSeq_ + cloud_.size();
  for (uint64_t sq = splatSeq_; sq < end; ++sq) {
    CloudPt& pt = cloud_[static_cast<size_t>(sq - frontSeq_)];
    pt.rx = -1.0f;
    const float age = static_cast<float>((head_ - pt.tSamples) / rate_);
    const float u = age * invSpan;
    if (age < 0.0f || u > 1.0f) continue;
    float val, rcp, attr;
    pointValue(pt, val, rcp, attr);
    if (val * gainNow <= 1e-14) continue;
    const Vec2 q = proj_(1.0f - 2.0f * u, 2.0f * pt.logF - 1.0f, pt.pan);
    double rx = std::fmod(q.x - offX, static_cast<double>(accW_)), ry = std::fmod(q.y - offY, static_cast<double>(accH_));
    if (rx < 0.0) rx += accW_;
    if (ry < 0.0) ry += accH_;
    pt.rx = std::min(static_cast<float>(rx), std::nextafter(static_cast<float>(accW_), 0.0f));
    pt.ry = std::min(static_cast<float>(ry), std::nextafter(static_cast<float>(accH_), 0.0f));
    splat<true>(pt.rx, pt.ry, val, rcp, attr, accum_.data(), cq, aq);
    if (!everyTexel) markDirty(pt.rx, pt.ry);
    forTiles(pt.rx, pt.ry, [&](int t) { tiles_[static_cast<size_t>(t)].push_back(sq); });
  }
  splatSeq_ = end;
  Shade s;
  shadeFor(lut, s);
  if (everyTexel) {  // the gain or the Time colours move every lit texel
    int lx0, lx1, ly0, ly1;
    resolveAll(s, perW_, perH_, lx0, lx1, ly0, ly1);
    const int y0 = std::min(ly0, texY0_), y1 = std::max(ly1, texY1_);
    texY0_ = ly0, texY1_ = ly1;
    if (y1 >= y0) uploadRect(0, y0, perW_ - 1, y1);
  } else {
    const float* cqr = s.hasC ? accC_.data() : nullptr;
    const float* aqr = s.hasA ? accA_.data() : nullptr;
    for (int y = 0; y < perH_; ++y) {
      const auto r = static_cast<size_t>(y);
      uint32_t* out = &rgba_[r * static_cast<size_t>(perW_)];
      for (int x = dLo_[r]; x <= dHi_[r]; ++x) out[x] = texel(s, accum_.data(), cqr, aqr, x, y);
    }
    uploadDirty();
  }
  drawnHead_ = head_;
}

void StereoSpectrogramFace::uploadRect(int x0, int y0, int x1, int y1) {
  uploadedRows_ += static_cast<uint64_t>(y1 - y0 + 1);
  if (!pool_ || tex_ == 0) return;
  const auto* bytes = reinterpret_cast<const uint8_t*>(rgba_.data() + static_cast<size_t>(y0) * static_cast<size_t>(perW_) + static_cast<size_t>(x0));
  const size_t pitch = static_cast<size_t>(perW_) * 4, len = static_cast<size_t>(y1 - y0) * pitch + static_cast<size_t>(x1 - x0 + 1) * 4;
  (void)pool_->update(tex_, ui::TextureRect{static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(y1 - y0 + 1)},
                      std::span<const uint8_t>(bytes, len), static_cast<uint32_t>(pitch));
}

// The dirty texels, as runs of rows (each run's column bounds).
void StereoSpectrogramFace::uploadDirty() {
  int y = 0;
  while (y < perH_) {
    if (dHi_[static_cast<size_t>(y)] < dLo_[static_cast<size_t>(y)]) {
      ++y;
      continue;
    }
    const int y0 = y;
    int x0 = perW_, x1 = -1;
    for (; y < perH_ && dHi_[static_cast<size_t>(y)] >= dLo_[static_cast<size_t>(y)]; ++y) {
      const auto r = static_cast<size_t>(y);
      x0 = std::min(x0, dLo_[r]), x1 = std::max(x1, dHi_[r]);
      dLo_[r] = perW_, dHi_[r] = -1;
    }
    uploadRect(x0, y0, x1, y - 1);
    texY0_ = std::min(texY0_, y0), texY1_ = std::max(texY1_, y - 1);
  }
}

// Tests: the canvas against a fresh splat of every live point where it lies now (texels that differ).
size_t StereoSpectrogramFace::verifyCanvas() {
  if (!valid_ || !scrolling_) return 0;
  std::vector<float> a(accum_.size(), 0.0f), c(accC_.empty() ? 0 : accC_.size(), 0.5f), al(accA_.empty() ? 0 : accA_.size(), 0.0f);
  float* const cq = source_ != kLevel ? c.data() : nullptr;
  float* const aq = sepAmp_ ? al.data() : nullptr;
  uint64_t seq = frontSeq_;
  for (const CloudPt& p : cloud_) {
    if (seq++ >= splatSeq_ || p.rx < 0.0f) continue;
    float val, rcp, attr;
    pointValue(p, val, rcp, attr);
    splat<true>(p.rx, p.ry, val, rcp, attr, a.data(), cq, aq);
  }
  Shade s;
  shadeFor(lut(), s);
  size_t bad = 0;
  for (int y = 0; y < perH_; ++y)
    for (int x = 0; x < perW_; ++x)
      if (texel(s, a.data(), cq, aq, x, y) != rgba_[static_cast<size_t>(y) * static_cast<size_t>(perW_) + static_cast<size_t>(x)]) ++bad;
  return bad;
}

// PQ drawRibbon: the band surface between slices, the width filled about the position, the newest position line.
void StereoSpectrogramFace::paintRibbon(ui::FacePainter& p, const Rect& plot, const ui::ColorLut& lut) {
  const size_t n = ribbon_.size();
  if (n < 1) return;
  const cam3d::Projector pr(orbit_.pose, opts_.on(sFree), static_cast<float>(opts_[sPersp]), plot);
  const float timeDepth = std::min(1.0f, std::fabs(pr.R[6]));
  const float floorDb = static_cast<float>(opts_[sFloor]), ceilDb = std::max(static_cast<float>(opts_[sCeil]), floorDb + 10.0f), range = ceilDb - floorDb;
  const float fadeDbPerSec = timeDepth * static_cast<float>(opts_[sAngle]) * range / std::max(0.01f, static_cast<float>(opts_[sDecay]));
  const int source = opts_.choice(sSource);
  const bool sepAmp = opts_.choice(sAgeFade) == 1, band = source == kBandRamp;
  float fr[256 * 3];
  if (band) bandRamp(fr);
  const float gamma = static_cast<float>(opts_[sGamma]), cfade = static_cast<float>(opts_[sCFade]);
  const float gate = static_cast<float>(opts_[sGate]), prec = static_cast<float>(opts_[sPrec]);
  const float fill = static_cast<float>(opts_[sRibFill]), wscale = static_cast<float>(opts_[sRibWidth]);
  auto colourAt = [&](float t, float attr, float aMul, float fade) {
    const auto a = static_cast<uint8_t>(std::clamp(t * aMul * fade, 0.0f, 1.0f) * 255.0f + 0.5f);
    const float cf = cfade > 0.0f ? std::clamp(t / cfade, 0.0f, 1.0f) : 1.0f;
    float r, g, b;
    if (band) {
      const float* c = &fr[3 * static_cast<int>(std::clamp(attr, 0.0f, 1.0f) * 255.0f + 0.5f)];
      const float over = std::max(0.0f, t * 1.45f - 1.0f);
      r = c[0] * t + (1.0f - c[0] * t) * over, g = c[1] * t + (1.0f - c[1] * t) * over, b = c[2] * t + (1.0f - c[2] * t) * over;
    } else {
      const Rgba c = source != kLevel ? ui::lutAt(lut, attr) : ui::lutAt(lut, t);
      const float s = source != kLevel ? t : 1.0f;
      r = ui::redOf(c) / 255.0f * s, g = ui::greenOf(c) / 255.0f * s, b = ui::blueOf(c) / 255.0f * s;
    }
    return ui::rgba(static_cast<uint8_t>(r * cf * 255.0f + 0.5f), static_cast<uint8_t>(g * cf * 255.0f + 0.5f), static_cast<uint8_t>(b * cf * 255.0f + 0.5f), a);
  };
  constexpr uint32_t B = dsp::StereoCloud::kRibBands;
  const float invB = 1.0f / (B - 1);
  constexpr size_t kMaxDrawn = 120;
  const size_t step = (n + kMaxDrawn - 1) / kMaxDrawn;
  std::array<Vec2, B> curP{}, prevP{};
  std::array<float, B> curT{}, prevT{}, lineT{}, lineC{};
  std::array<Vec2, B> lineP{};
  bool havePrev = false, haveLine = false;
  const double span = opts_[sSpan];
  auto attrOf = [&](const dsp::StereoCloud::RibSlice& s, uint32_t b, float u) {
    return source == kPan ? std::clamp(0.5f * (s.pos[b] + 1.0f), 0.0f, 1.0f) : source == kTime ? 1.0f - u : (static_cast<float>(b) + 0.5f) * invB;
  };
  idx_.clear();
  for (size_t i = 0; i < n; i += step) idx_.push_back(i);
  if (idx_.back() != n - 1) idx_.push_back(n - 1);
  for (const size_t si : idx_) {
    const auto& s = ribbon_[si];
    const float age = static_cast<float>((head_ - s.tSamples) / rate_);
    const float u = age / static_cast<float>(span);
    if (age >= 0.0f && u <= 1.0f) {
      const float T = 1.0f - 2.0f * u;
      const float fadeRaw = age * fadeDbPerSec + u * static_cast<float>(opts_[sFade]) * range;
      const float fadeDb = sepAmp ? 0.0f : fadeRaw, sliceFade = sepAmp ? std::clamp(1.0f - fadeRaw / range, 0.0f, 1.0f) : 1.0f;
      std::array<float, B> mask{};
      if (prec > 0.004f) {
        float run = -1e9f;
        for (uint32_t b = 0; b < B; ++b) mask[b] = run = std::max(run - 4.5f, s.db[b]);
        run = -1e9f;
        for (uint32_t b = B; b-- > 0;) mask[b] = std::max(mask[b], run = std::max(run - 4.5f, s.db[b]));
      }
      for (uint32_t b = 0; b < B; ++b) {
        float t = std::clamp((s.db[b] - fadeDb - floorDb) / range, 0.0f, 1.0f);
        if (t > 0.0f && gate > 0.0f) {
          const float x = std::clamp((s.db[b] - fadeDb - floorDb - gate) / 6.0f, 0.0f, 1.0f);
          t *= x * x * (3.0f - 2.0f * x);
        }
        if (t > 0.0f && prec > 0.004f) t *= 1.0f - prec * std::min(1.0f, (mask[b] - s.db[b]) / 3.0f);
        if (gamma != 1.0f && t > 0.0f) t = std::pow(t, gamma);
        curT[b] = t;
        curP[b] = pr(T, 2.0f * b * invB - 1.0f, std::clamp(s.pos[b], -1.0f, 1.0f));
      }
      mv_.clear(), mc_.clear(), mi_.clear();
      auto quad = [&](Vec2 a, Vec2 bq, Vec2 c, Vec2 d, Rgba col) {
        const auto base = static_cast<uint32_t>(mv_.size());
        mv_.insert(mv_.end(), {a, bq, c, d});
        mc_.insert(mc_.end(), {col, col, col, col});
        mi_.insert(mi_.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
      };
      if (sliceFade > 0.0f) {
        if (havePrev)
          for (uint32_t b = 0; b + 1 < B; ++b) {
            const float tAvg = 0.25f * (curT[b] + curT[b + 1] + prevT[b] + prevT[b + 1]);
            if (tAvg >= 0.004f) quad(prevP[b], prevP[b + 1], curP[b + 1], curP[b], colourAt(tAvg, attrOf(s, b, u), 1.0f, sliceFade));
          }
        if (fill > 0.004f)
          for (uint32_t b = 0; b + 1 < B; ++b) {
            const float tAvg = 0.5f * (curT[b] + curT[b + 1]);
            const float wa = std::clamp(s.width[b] * wscale, 0.0f, 1.0f), wb = std::clamp(s.width[b + 1] * wscale, 0.0f, 1.0f);
            if (tAvg * fill < 0.004f || (wa < 0.01f && wb < 0.01f)) continue;
            const float Fa = 2.0f * b * invB - 1.0f, Fb = 2.0f * (b + 1) * invB - 1.0f;
            const float pa = std::clamp(s.pos[b], -1.0f, 1.0f), pb = std::clamp(s.pos[b + 1], -1.0f, 1.0f);
            quad(pr(T, Fa, std::max(-1.0f, pa - wa)), pr(T, Fa, std::min(1.0f, pa + wa)), pr(T, Fb, std::min(1.0f, pb + wb)),
                 pr(T, Fb, std::max(-1.0f, pb - wb)), colourAt(tAvg, attrOf(s, b, u), fill, sliceFade));
          }
        if (!mv_.empty()) p.meshDevice(mv_, mc_, mi_);
      }
      if (si == n - 1) {
        lineP = curP, lineT = curT, haveLine = true;
        for (uint32_t b = 0; b < B; ++b) lineC[b] = attrOf(s, b, u);
      }
      prevP = curP, prevT = curT, havePrev = true;
    }
  }
  if (!haveLine) return;
  const float lw = static_cast<float>(opts_[sRibLine]) * 0.5f * p.transform().scale;
  mv_.clear(), mc_.clear(), mi_.clear();
  for (uint32_t b = 0; b + 1 < B; ++b) {
    const float tAvg = 0.5f * (lineT[b] + lineT[b + 1]);
    if (tAvg < 0.004f) continue;
    const Rgba c = colourAt(std::min(1.0f, tAvg * 1.3f), 0.5f * (lineC[b] + lineC[b + 1]), std::min(1.0f, tAvg * 2.5f), 1.0f);
    float dx = lineP[b + 1].x - lineP[b].x, dy = lineP[b + 1].y - lineP[b].y;
    const float len = std::max(1e-6f, std::sqrt(dx * dx + dy * dy));
    const float nx = -dy / len * 0.5f * lw, ny = dx / len * 0.5f * lw;
    const auto base = static_cast<uint32_t>(mv_.size());
    mv_.insert(mv_.end(), {{lineP[b].x + nx, lineP[b].y + ny}, {lineP[b + 1].x + nx, lineP[b + 1].y + ny}, {lineP[b + 1].x - nx, lineP[b + 1].y - ny},
                           {lineP[b].x - nx, lineP[b].y - ny}});
    mc_.insert(mc_.end(), {c, c, c, c});
    mi_.insert(mi_.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
  if (!mv_.empty()) p.meshDevice(mv_, mc_, mi_);
}

void StereoSpectrogramFace::paintScene(ui::FacePainter& p, const ui::Theme& /*th*/, bool labels) {
  using namespace ui::signal_theme;
  if (const cam3d::Pose q = poseOpts(); !orbit_.dragging() && !orbit_.snapping() && !(q == lastPose_)) {
    orbit_.pose = q;
    lastPose_ = q;
  }
  const ui::FaceTransform& tf = p.transform();
  const Rect plotW{8.0f, 8.0f, kW - 8.0f, kH - 8.0f};
  const Rect plot = deviceRect(p, plotW);
  if (head_ < 0.0) {
    if (labels) p.textAligned({14.0f, 32.0f}, feed_ != nullptr ? "waiting for audio..." : "plays in a track's chain", 6.0f, kGridLabel, 0.0f);
    return;
  }
  const ui::ColorLut& colours = lut();
  const cam3d::Projector proj(orbit_.pose, opts_.on(sFree), static_cast<float>(opts_[sPersp]), plot);
  const float* R = proj.R;
  if (opts_.choice(sMode) == 1) {
    paintRibbon(p, plot, colours);
  } else {
    // The canvas: the plot's device px (≤ 768² × detail) and the margins; the texture rounded up to 64.
    const int det = opts_.choice(sDetail) + 1, cap = kMaxCanvas * det;
    const int needW = std::clamp(static_cast<int>(plot.x1 - plot.x0), 16, cap), needH = std::clamp(static_cast<int>(plot.y1 - plot.y0), 16, cap);
    layout(needW, needH, det);
    ui::TexturePool* pool = p.images();
    if (pool != nullptr && pool->attached()) {
      if (pool_.get() != pool) {
        if (tex_ != 0 && pool_) pool_->destroy(tex_);
        tex_ = 0;
        pool_ = pool->share();
      }
      int wantW = texW_, wantH = texH_;
      if (tex_ == 0 || perW_ > texW_ || perW_ <= texW_ - 128) wantW = (perW_ + 63) / 64 * 64;
      if (tex_ == 0 || perH_ > texH_ || perH_ <= texH_ - 128) wantH = (perH_ + 63) / 64 * 64;
      if (tex_ == 0 || wantW != texW_ || wantH != texH_) {
        if (tex_ != 0) pool_->destroy(tex_);
        tex_ = 0;
        if (auto t = pool_->create(static_cast<uint32_t>(wantW), static_cast<uint32_t>(wantH), ui::TextureFilter::Linear)) {
          tex_ = *t;
          texW_ = wantW, texH_ = wantH;
        }
        valid_ = false;
        texY0_ = 0, texY1_ = perH_ - 1;
      }
    }
    const float timeDepth = std::min(1.0f, std::fabs(R[6]));
    std::vector<double> key = {orbit_.pose.yaw, orbit_.pose.pitch, orbit_.pose.roll, orbit_.pose.zoom, orbit_.pose.panX, orbit_.pose.panY};
    for (uint32_t o = sSpan; o < sCount; ++o) key.push_back(opts_[o]);
    const uint64_t end = frontSeq_ + cloud_.size();
    // A moving camera rebuilds unscrolled (no tile lists to fill each frame); the first update after it stops scrolls.
    const bool moving = orbit_.dragging() || orbit_.snapping() ||
                        (drawnKey_.size() == key.size() && !std::equal(key.begin(), key.begin() + 6, drawnKey_.begin()));
    if (!valid_ || key != drawnKey_ || (end - splatSeq_) > 20000) {
      const double now = clockNow();
      if (valid_ && moving && now - rebuiltAt_ < kMovingRebuild) {  // a moving camera re-splats ≤ ~30 times a second
        canvasStale_ = true;
      } else {
        rebuild(colours, timeDepth, !moving);
        rebuiltAt_ = now;
        canvasStale_ = false;
        drawnKey_ = std::move(key);
        valid_ = true;
      }
    } else if (head_ != drawnHead_ || splatSeq_ != end || !expiring_.empty()) {
      if (scrolling_) advance(colours);
      else rebuild(colours, timeDepth, true);
    }
    if (tex_ != 0 && pool_.get() == p.images()) {  // the period drawn offset by the scroll (≤ 4 quads)
      const float sx = (plot.x1 - plot.x0) / static_cast<float>(canW_), sy = (plot.y1 - plot.y0) / static_cast<float>(canH_);
      const int X0 = (perW_ - kx_) % perW_, Y0 = (perH_ - ky_) % perH_;
      const int nx = std::min(canW_, perW_ - X0), ny = std::min(canH_, perH_ - Y0);
      const int xs[2][3] = {{X0, nx, 0}, {0, canW_ - nx, nx}}, ys[2][3] = {{Y0, ny, 0}, {0, canH_ - ny, ny}};  // texel, count, window
      for (const auto& yv : ys)
        for (const auto& xv : xs) {
          if (xv[1] <= 0 || yv[1] <= 0) continue;
          const Rect r{plot.x0 + static_cast<float>(xv[2]) * sx, plot.y0 + static_cast<float>(yv[2]) * sy, plot.x0 + static_cast<float>(xv[2] + xv[1]) * sx,
                       plot.y0 + static_cast<float>(yv[2] + yv[1]) * sy};
          p.drawList().addImage(tex_, r, {static_cast<float>(xv[0]) / texW_, static_cast<float>(yv[0]) / texH_},
                                {static_cast<float>(xv[0] + xv[1]) / texW_, static_cast<float>(yv[0] + yv[1]) / texH_});
        }
    }
  }
  // ---- overlay ----
  auto world = [&](Vec2 d) { return tf.unmap(d); };
  auto at = [&](float u, float pan, float fUnit) { return world(proj(1.0f - 2.0f * u, 2.0f * fUnit - 1.0f, pan)); };
  const float timeVis = std::sqrt(R[0] * R[0] + R[3] * R[3]), panVis = std::sqrt(R[2] * R[2] + R[5] * R[5]);
  const float freqPure = std::max(0.0f, 1.0f - 3.0f * (std::fabs(R[3]) + std::fabs(R[5])));
  if (opts_.on(sCage)) {
    const float a = 90.0f * timeVis * panVis;
    if (a > 6.0f) {
      const Rgba col = ui::rgba(150, 160, 185, static_cast<uint8_t>(std::min(255.0f, a)));
      Vec2 c[8];
      for (int i = 0; i < 8; ++i) c[i] = at((i & 1) ? 1.0f : 0.0f, (i & 4) ? 1.0f : -1.0f, (i & 2) ? 1.0f : 0.0f);
      static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      for (const auto& e : kEdges) p.line(c[e[0]], c[e[1]], 0.5f, col);
    }
  }
  const dsp::FreqAxis axis(st_.config.scale, st_.config.fMin, st_.config.fMax);
  if (opts_.on(sGrid) && freqPure > 0.05f) {
    dsp::FreqGridLine grid[16];
    const int ng = dsp::freqGridLines(st_.config.scale, st_.config.fMin, st_.config.fMax, grid, 16);
    const Rgba gc = ui::mulAlpha(kGrid, freqPure), lc = ui::mulAlpha(kGridLabel, freqPure);
    float lastY = -1e9f;
    for (int i = 0; i < ng; ++i) {
      const float y = at(0.0f, 0.0f, axis.unit(grid[i].hz)).y;
      if (y < plotW.y0 || y > plotW.y1) continue;
      p.line({plotW.x0, y}, {plotW.x1, y}, 0.5f, gc);
      if (labels && std::fabs(y - lastY) >= 7.0f && y - 4.0f > plotW.y0 + 12.0f) {
        p.textAligned({plotW.x0 + 2.0f, y - 3.5f}, grid[i].label, 5.0f, lc, 0.0f);
        lastY = y;
      }
    }
  }
  if (opts_.on(sCentre) && panVis > 0.01f) p.line(at(0.0f, 0.0f, 0.0f), at(0.0f, 0.0f, 1.0f), 0.5f, ui::rgba(255, 255, 255, static_cast<uint8_t>(70.0f * panVis)));
  if (labels && opts_.on(sLabels)) {
    const float ta = 0.75f * timeVis * timeVis;
    const double span = opts_[sSpan];
    if (ta > 0.03f) {
      double stepS = 1.0;
      while (span / stepS > 8.0) stepS *= 2.0;
      while (span / stepS < 3.0 && stepS > 0.05) stepS *= 0.5;
      for (double t = stepS; t < span; t += stepS) {
        const Vec2 a0 = at(static_cast<float>(t / span), 0.0f, 0.0f);
        if (!inside(plotW, a0)) continue;
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "-%.2gs", t);
        p.textAligned({a0.x + 1.5f, std::min(a0.y - 3.0f, plotW.y1 - 4.0f)}, lbl, 5.0f, ui::rgba(190, 195, 210, static_cast<uint8_t>(255.0f * ta)), 0.0f);
      }
    }
    const float pa = 0.8f * panVis * panVis;
    if (pa > 0.03f) {
      const char* const kPanL[3] = {"L", "C", "R"};
      for (int i = 0; i < 3; ++i) {
        const Vec2 a0 = at(0.0f, static_cast<float>(i - 1), 0.0f);
        p.textAligned({std::clamp(a0.x, plotW.x0 + 3.0f, plotW.x1 - 3.0f), std::min(a0.y - 4.0f, plotW.y1 - 4.0f)}, kPanL[i], 5.5f,
                      ui::rgba(200, 205, 220, static_cast<uint8_t>(255.0f * pa)), 0.5f);
      }
    }
  }
}

}  // namespace

std::unique_ptr<MetaFace> makeStereoSpectrogramFace(const meta::Type& t) { return std::make_unique<StereoSpectrogramFace>(t); }

bool stereoSpectrogramShown(const MetaFace& f, StereoSpectrogramShown& out) {
  const auto* s = dynamic_cast<const StereoSpectrogramFace*>(&f);
  if (s == nullptr) return false;
  out.acquired = s->acquiredForTest();
  out.frozen = s->frozenForTest();
  const cam3d::Pose& q = s->pose();
  out.yaw = q.yaw, out.pitch = q.pitch, out.roll = q.roll, out.zoom = q.zoom;
  out.cloud = s->cloudSize();
  out.ribbon = s->ribbonSize();
  out.uploadedRows = s->uploadedRows();
  out.redraws = s->redraws();
  out.rebuilds = s->rebuilds();
  out.scrolling = s->scrolling();
  out.config = s->configForTest();
  return true;
}

bool stereoSpectrogramTestControl(MetaFace& f, std::string_view key, double value) {
  auto* s = dynamic_cast<StereoSpectrogramFace*>(&f);
  if (s == nullptr) return false;
  if (key == "freeze") s->setFrozenForTest(value != 0.0);
  else if (key == "sheet") s->setSheetForTest(value != 0.0);
  else if (key == "view") s->snapForTest(static_cast<int>(value));
  else s->setOptionForTest(key, value);
  return true;
}

size_t stereoSpectrogramCanvasCheck(MetaFace& f) {
  auto* s = dynamic_cast<StereoSpectrogramFace*>(&f);
  return s != nullptr ? s->verifyForTest() : 0;
}

dsp::StereoCloudConfig stereoSpectrogramRequest(const meta::DataValue* data) {
  const meta::Type* t = meta::findType("stereo_spectrogram");
  if (t == nullptr) return {};
  std::unique_ptr<MetaFace> f = makeFace(*t);
  auto* s = f ? dynamic_cast<StereoSpectrogramFace*>(f.get()) : nullptr;
  return s != nullptr ? s->configFor(data) : dsp::StereoCloudConfig{};
}

ui::Vec2 stereoSpectrogramCanvasAt(float yaw, float pitch, float u, float fUnit, float pan, int w, int h) {
  cam3d::Pose q;
  q.yaw = yaw, q.pitch = pitch;
  return StereoSpectrogramFace::canvasAt(q, u, fUnit, pan, w, h);
}

}  // namespace pq::views
