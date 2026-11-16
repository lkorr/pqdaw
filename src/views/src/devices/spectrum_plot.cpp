// The reusable spectrum + response display (pq/views/devices/spectrum_plot.h; TX.spectrum, VZ.2).

#include "pq/views/devices/spectrum_plot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pq/ui/colormap.h"
#include "pq/ui/lines.h"

namespace pq::views::devices {

namespace {

constexpr double kEaseSeconds = 0.045;  // PQ Meter's display smoothing
constexpr float kBarFadeDb = 12.0f;     // VZ.2 S11: the bars fade in over the 12 dB above the colour window's bottom

// PQ Meter's fixed signal-display colours (SPEC §11.1: its palette applies to signal displays).
constexpr ui::Rgba kPeakColour = ui::rgba(232, 240, 255);
constexpr ui::Rgba kAverageColour = ui::rgba(255, 188, 96);  // warm amber: never the cool peak line
constexpr ui::Rgba kSlopeColour = ui::rgba(172, 192, 222, 170);
constexpr ui::Rgba kInputColour = ui::rgba(150, 162, 186);
constexpr ui::Rgba kResponseColour = ui::hex(0xAC8EFF);  // the violet accent
constexpr ui::Rgba kLabelPad = ui::rgba(8, 8, 12, 150);
constexpr ui::Rgba kHoverLine = ui::rgba(255, 255, 255, 60);
constexpr ui::Rgba kHoverText = ui::rgba(255, 255, 255, 210);
constexpr ui::Rgba kHoverNote = ui::rgba(160, 220, 255, 190);
constexpr float kCurveR = 0.43f, kCurveG = 0.85f, kCurveB = 1.0f;  // PQ Meter's curve colour (Solid)

float textWidth(const PlotText& t, std::string_view s) {
  return t.width ? t.width(s) : 0.6f * t.lineHeight * static_cast<float>(s.size());
}

// The nearest 12-EDO note of hz (C3 = 261.626 Hz, Ableton naming) and the cents from it.
void noteName(float hz, char* out, size_t n) {
  static const char* const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (!(hz > 8.0f)) {
    out[0] = '\0';
    return;
  }
  const double semis = 60.0 + 12.0 * std::log2(hz / 261.6256);
  const long midi = std::lround(semis);
  const int cents = static_cast<int>(std::lround((semis - static_cast<double>(midi)) * 100.0));
  const long octave = midi / 12 - 2 - (midi < 0 && midi % 12 != 0 ? 1 : 0);
  const char* name = kNames[((midi % 12) + 12) % 12];
  if (cents == 0) std::snprintf(out, n, "%s%ld", name, octave);
  else std::snprintf(out, n, "%s%ld %+d ct", name, octave, cents);
}

struct Rgb {
  float r, g, b;
};
Rgb hsv(float h, float s, float v) noexcept {
  h -= std::floor(h);
  const float i = std::floor(h * 6.0f);
  const float f = h * 6.0f - i;
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
ui::Rgba pack(Rgb c) noexcept {
  auto q = [](float x) { return static_cast<uint32_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return ui::rgba(q(c.r), q(c.g), q(c.b), 255);
}
Rgb unpack(ui::Rgba c) noexcept {
  return {static_cast<float>(ui::redOf(c)) / 255.0f, static_cast<float>(ui::greenOf(c)) / 255.0f,
          static_cast<float>(ui::blueOf(c)) / 255.0f};
}

// What the palette depends on.
bool samePalette(const SpectrumPlotStyle& a, const SpectrumPlotStyle& b) noexcept {
  if (a.colour != b.colour || a.colormap != b.colormap) return false;
  if (a.colour == SpectrumColour::Solid && (a.ownSolid != b.ownSolid || a.solid != b.solid)) return false;
  if (a.colormap < ui::Gradient::kCustomIndex) return true;
  if (a.customBase != b.customBase || a.colourFollow != b.colourFollow || !(a.customColours == b.customColours)) return false;
  if (a.colourFollow ? (a.floorDb != b.floorDb || a.ceilDb != b.ceilDb) : a.colourLoDb != b.colourLoDb) return false;
  for (int i = 0; i < 4; ++i)
    if (a.stopsDb[i] != b.stopsDb[i]) return false;
  return true;
}

}  // namespace

float SpectrumAxis::x(float hz) const noexcept {
  if (offsetHz > 0.0f)
    return x0 + (x1 - x0) * std::log1p(std::max(hz, 0.0f) / offsetHz) / std::log1p(topHz / offsetHz);
  return x0 + (x1 - x0) * freq.unit(std::clamp(hz, freq.fMin(), freq.fMax()));
}
float SpectrumAxis::hz(float px) const noexcept {
  const float t = std::clamp((px - x0) / std::max(x1 - x0, 1e-3f), 0.0f, 1.0f);
  if (offsetHz > 0.0f) return offsetHz * std::expm1(t * std::log1p(topHz / offsetHz));
  return freq.hz(t);
}

SpectrumPlot::~SpectrumPlot() { release(); }

void SpectrumPlot::setFeed(SpectrumFeed* f, std::function<void()> requestFrame) {
  requestFrame_ = std::move(requestFrame);
  // By serial too: a new feed can be allocated where the released, freed one was (a held feed is never freed).
  const uint64_t serial = f != nullptr ? f->serial() : 0;
  if (f == feed_ && serial == feedSerial_) return;
  feedSerial_ = serial;
  release();
  feed_ = f;
  seq_ = 0;
  target_ = {};
  shown_ = {};
}

uint32_t SpectrumPlot::wants() const noexcept {
  uint32_t w = kWantOutput;
  if (style_.input) w |= kWantInput;
  if (style_.response) w |= kWantResponse;
  return w;
}

void SpectrumPlot::setStyle(const SpectrumPlotStyle& s) {
  const uint32_t before = wants();
  style_ = s;
  if (acquired_ && wants() != before) {
    release();
    acquire();
    if (requestFrame_) requestFrame_();
  }
  if (!style_.input) shown_.in.clear();
  if (!style_.response) {
    shown_.response.clear();
    shown_.confidence.clear();
  }
}

void SpectrumPlot::acquire() {
  if (acquired_ || !feed_) return;
  acquiredWants_ = wants();
  feed_->acquire(acquiredWants_);
  if (requestFrame_) feed_->addWaker(this, requestFrame_);
  acquired_ = true;
  last_ = 0.0;
}

void SpectrumPlot::release() {
  if (!acquired_) return;
  feed_->removeWaker(this);
  feed_->release(acquiredWants_);
  acquired_ = false;
}

void SpectrumPlot::wake() {
  if (!feed_) return;
  if (!acquired_) {
    acquire();
    if (requestFrame_) requestFrame_();
  }
  // Nothing shown yet (a display that just appeared): what the feed holds now, at once (the frames ease from there).
  if (shown_.out.empty()) {
    bool settled = false;
    seq_ = feed_->snapshot(seq_, target_, settled);
    shown_ = target_;
    if (!style_.input) shown_.in.clear();
    if (!style_.response) {
      shown_.response.clear();
      shown_.confidence.clear();
    }
  }
}

bool SpectrumPlot::frame(double now, bool shown) {
  if (!feed_) return false;
  if (!shown) {
    release();
    return false;
  }
  if (!acquired_) acquire();
  bool settled = false;
  const uint64_t s = feed_->snapshot(seq_, target_, settled);
  const bool fresh = s != seq_;
  seq_ = s;
  const double dt = std::clamp(last_ > 0.0 ? now - last_ : 0.0, 0.0, 0.1);
  last_ = now;
  const float a = static_cast<float>(1.0 - std::exp(-dt / kEaseSeconds));
  float moved = 0.0f;
  // A new axis (a tuning): the points stand for other frequencies — no ease across it.
  if (!(target_.axis == shown_.axis)) {
    shown_ = target_;
    moved = 1.0f;
  }
  auto ease = [&](std::vector<float>& shown, const std::vector<float>& target, bool riseAtOnce) {
    if (shown.size() != target.size()) {
      shown = target;
      return;
    }
    for (size_t i = 0; i < shown.size(); ++i) {
      const float d = (target[i] - shown[i]) * a;
      if (riseAtOnce && target[i] > shown[i]) {
        moved = std::max(moved, target[i] - shown[i]);
        shown[i] = target[i];
        continue;
      }
      shown[i] += d;
      moved = std::max(moved, std::fabs(d));
    }
  };
  ease(shown_.out, target_.out, false);
  ease(shown_.peaks, target_.peaks, true);
  ease(shown_.average, target_.average, false);
  if (style_.input) ease(shown_.in, target_.in, false);
  if (style_.response) {
    ease(shown_.response, target_.response, false);
    ease(shown_.confidence, target_.confidence, false);
  }
  const bool changed = fresh || moved > 0.01f;
  const bool animate = changed || !settled || feed_->live();
  if (animate) {
    if (requestFrame_) requestFrame_();
  } else if (!feed_->watching()) {
    release();
  }
  return changed;
}

void SpectrumPlot::setShownForTest(SpectrumSnapshot s, float) { shown_ = std::move(s); }

float SpectrumPlot::sampleAt(const std::vector<float>& v, float hz) const noexcept {
  const size_t P = v.size();
  if (P < 2) return P == 1 ? v[0] : style_.floorDb - 1.0f;
  const float t = std::clamp(shown_.axis.unit(hz), 0.0f, 1.0f) * static_cast<float>(P - 1);
  const size_t i0 = std::min(P - 2, static_cast<size_t>(t));
  const float f = t - static_cast<float>(i0);
  return v[i0] + (v[i0 + 1] - v[i0]) * f;
}

float SpectrumPlot::levelAt(float hz) const noexcept { return sampleAt(shown_.out, hz); }

float SpectrumPlot::responseAt(float hz, float* confidence) const noexcept {
  const float c = shown_.confidence.empty() ? 0.0f : std::clamp(sampleAt(shown_.confidence, hz), 0.0f, 1.0f);
  if (confidence) *confidence = c;
  return shown_.response.empty() ? 0.0f : sampleAt(shown_.response, hz);
}

float SpectrumPlot::levelY(const ui::Rect& r, float db) const noexcept {
  const float t = std::clamp((db - style_.floorDb) / std::max(1.0f, style_.ceilDb - style_.floorDb), 0.0f, 1.0f);
  return r.y1 - t * (r.height() - 4.0f);
}

float SpectrumPlot::responseY(const ui::Rect& r, float db) const noexcept {
  const float span = std::max(1.0f, style_.respTopDb - style_.respBottomDb);
  return r.y0 + style_.respInset + (style_.respTopDb - db) / span * (r.height() - 2.0f * style_.respInset);
}

// ---- colour ------------------------------------------------------------------------------------------------------------

// Level over the colour window (0 … 1): floor … ceiling while following the scale, else Quiet … 0 dBFS.
float SpectrumPlot::colourT(float db) const noexcept {
  const float lo = style_.colourFollow ? style_.floorDb : style_.colourLoDb;
  const float hi = style_.colourFollow ? style_.ceilDb : 0.0f;
  return std::clamp((db - lo) / std::max(1.0f, hi - lo), 0.0f, 1.0f);
}

void SpectrumPlot::rebuildPalette() const {
  if (palValid_ && samePalette(palFor_, style_)) return;
  palFor_ = style_;
  palValid_ = true;
  palette_.resize(static_cast<size_t>(kPalF) * kPalL);
  // The ramp the colormap modes read: a preset, or the Custom stops placed by their dB on the colour window.
  static thread_local ui::Gradient custom;
  const ui::ColorLut* lut = &ui::colormapLut(static_cast<ui::Colormap>(std::clamp(style_.colormap, 0, ui::kColormapCount - 1)));
  if (style_.colormap >= ui::Gradient::kCustomIndex) {
    ui::LevelAxis ax{style_.colourFollow ? style_.floorDb : style_.colourLoDb, style_.colourFollow ? style_.ceilDb : 0.0f, 1.0f};
    ui::levelGradient(custom, static_cast<ui::Colormap>(std::clamp(style_.customBase, 0, ui::kColormapCount - 1)), style_.stopsDb, ax, &style_.customColours);
    lut = &custom.lut;
  }
  for (int fi = 0; fi < kPalF; ++fi) {
    const float f = static_cast<float>(fi) / static_cast<float>(kPalF - 1);
    for (int li = 0; li < kPalL; ++li) {
      const float l = static_cast<float>(li) / static_cast<float>(kPalL - 1);
      Rgb c;
      switch (style_.colour) {
        case SpectrumColour::Solid: {
          const float k = 0.55f + 0.45f * l;  // a little level lift: not a dead flat stripe
          const Rgb base = style_.ownSolid ? unpack(style_.solid) : Rgb{kCurveR, kCurveG, kCurveB};
          c = {base.r * k, base.g * k, base.b * k};
          break;
        }
        case SpectrumColour::Spectral: c = hsv(0.92f + f * 0.80f, 0.72f, 0.55f + 0.45f * l); break;
        case SpectrumColour::Heat: {
          const float s = 0.95f - 0.75f * std::max(0.0f, l - 0.75f) * 4.0f;
          c = hsv(0.72f - 0.72f * l, std::clamp(s, 0.0f, 1.0f), 0.35f + 0.65f * l);
          break;
        }
        case SpectrumColour::Colormap: c = unpack(ui::lutAt(*lut, l)); break;
        case SpectrumColour::ColormapFreq: {
          const Rgb m = unpack(ui::lutAt(*lut, f));
          const float lm = 0.15f + 0.85f * l;
          c = {m.r * lm, m.g * lm, m.b * lm};
          break;
        }
        case SpectrumColour::Blend:
        default: {
          const float s = 0.82f - 0.42f * std::max(0.0f, l - 0.7f) / 0.3f;
          c = hsv(0.95f + f * 0.72f - 0.10f * l, std::clamp(s, 0.0f, 1.0f), 0.30f + 0.70f * l);
          break;
        }
      }
      palette_[static_cast<size_t>(fi) * kPalL + static_cast<size_t>(li)] = pack(c);
    }
  }
}

ui::Rgba SpectrumPlot::colourAt(float ft, float db) const noexcept {
  rebuildPalette();
  const int fi = std::clamp(static_cast<int>(ft * static_cast<float>(kPalF - 1) + 0.5f), 0, kPalF - 1);
  const int li = std::clamp(static_cast<int>(colourT(db) * static_cast<float>(kPalL - 1) + 0.5f), 0, kPalL - 1);
  return palette_[static_cast<size_t>(fi) * kPalL + static_cast<size_t>(li)];
}

// ---- painting ------------------------------------------------------------------------------------------------------------

void SpectrumPlot::paintBackdrop(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax) const {
  if (!style_.bands) return;
  rebuildPalette();
  const float edges[4] = {r.x0, std::clamp(ax.x(250.0f), r.x0, r.x1), std::clamp(ax.x(4000.0f), r.x0, r.x1), r.x1};
  // The tint ramps up toward the floor and back out over the last rows (no hard edge on the panel's last row).
  const float yTail = std::max(r.y0, r.y1 - std::min(10.0f, r.height() * 0.05f));
  for (int b = 0; b < 3; ++b) {
    if (edges[b + 1] <= edges[b] + 1.0f) continue;
    // The palette at each third's place (PQ Meter: paletteAt(b / 3 + 0.16, 0.85)), very faint.
    const float ft = static_cast<float>(b) / 3.0f + 0.16f;
    const int fi = std::clamp(static_cast<int>(ft * static_cast<float>(kPalF - 1) + 0.5f), 0, kPalF - 1);
    const int li = static_cast<int>(0.85f * static_cast<float>(kPalL - 1) + 0.5f);
    const ui::Rgba tint = ui::setAlpha(palette_[static_cast<size_t>(fi) * kPalL + static_cast<size_t>(li)], 0.055f);
    const ui::Rgba clear = ui::setAlpha(tint, 0.0f);
    dl.addRectFilledMultiColor({edges[b], r.y0, edges[b + 1], yTail}, clear, clear, tint, tint);
    dl.addRectFilledMultiColor({edges[b], yTail, edges[b + 1], r.y1}, tint, tint, clear, clear);
  }
}

void SpectrumPlot::paintGrid(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, const ui::Theme& th, float,
                             bool dbRules) const {
  if (!style_.grid) return;
  dsp::FreqGridLine lines[24];
  const int n = dsp::freqGridLines(ax.freq, lines, 24);
  for (int i = 0; i < n; ++i) {
    const float f = lines[i].hz;
    const float x = std::round(ax.x(f));
    if (x <= r.x0 || x >= r.x1) continue;
    const bool major = ax.freq.scale() != dsp::FreqScale::Linear && (f == 100.0f || f == 1000.0f || f == 10000.0f);
    dl.addRectFilled({x, r.y0, x + 1.0f, r.y1}, major ? th.device.displayGridMajor : th.device.displayGrid);
  }
  if (!dbRules) return;
  for (float db = std::ceil(style_.floorDb / 12.0f) * 12.0f; db <= style_.ceilDb; db += 12.0f) {
    const float y = std::round(levelY(r, db));
    if (y <= r.y0 || y >= r.y1) continue;
    dl.addRectFilled({r.x0, y, r.x1, y + 1.0f}, th.device.displayGrid);
  }
}

void SpectrumPlot::paintSpectrum(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, float scale) {
  if (shown_.out.empty()) return;
  rebuildPalette();
  const uint32_t steps = std::clamp<uint32_t>(static_cast<uint32_t>(r.width()), 32, 1024);
  poly_.resize(steps);
  cols_.resize(steps);
  lvl_.resize(steps);
  vis_.resize(steps);
  db_.resize(steps);
  tmp_.resize(steps);
  const float range = std::max(1.0f, style_.ceilDb - style_.floorDb);
  // A vertex at the floor fades out over the lowest 4 % of the range: silence draws no curve at all.
  auto visT = [&](float db) { return std::clamp((db - style_.floorDb) / std::max(1.0f, 0.04f * range), 0.0f, 1.0f); };
  auto xAt = [&](uint32_t s) { return r.x0 + r.width() * static_cast<float>(s) / static_cast<float>(steps - 1); };
  // The palette's frequency coordinate: the display's own axis fraction.
  auto ftAt = [&](uint32_t s) { return static_cast<float>(s) / static_cast<float>(steps - 1); };
  const bool bars = style_.draw != SpectrumDraw::Curve;
  const bool curve = style_.draw != SpectrumDraw::Bars;

  // 1. The input, faint and flat behind (no glow): what reached the device.
  if (style_.input && shown_.in.size() >= 2) {
    bool any = false;
    for (uint32_t s = 0; s < steps; ++s) {
      const float x = xAt(s), db = sampleAt(shown_.in, ax.hz(x));
      poly_[s] = {x, levelY(r, db)};
      cols_[s] = ui::setAlpha(kInputColour, 0.55f * visT(db) * style_.alpha);
      any = any || visT(db) > 0.0f;
    }
    if (any) {
      for (uint32_t s = 0; s < steps; ++s) tmp_[s] = ui::mulAlpha(cols_[s], 0.18f);
      ui::gradientBand(dl, poly_.data(), tmp_.data(), r.y1, 0.0f, steps);
      ui::gradientPolyline(dl, poly_.data(), cols_.data(), steps, std::max(1.0f, 1.1f * scale));
    }
  }

  // 2. The output: the palette by frequency and level (one pass builds the curve, its colours and levels).
  bool any = false;
  for (uint32_t s = 0; s < steps; ++s) {
    const float x = xAt(s), db = sampleAt(shown_.out, ax.hz(x));
    const float vis = visT(db);
    any = any || vis > 0.0f;
    db_[s] = db;
    poly_[s] = {x, levelY(r, db)};
    lvl_[s] = colourT(db);
    vis_[s] = vis;
    const int fi = std::clamp(static_cast<int>(ftAt(s) * static_cast<float>(kPalF - 1) + 0.5f), 0, kPalF - 1);
    const int li = std::clamp(static_cast<int>(lvl_[s] * static_cast<float>(kPalL - 1) + 0.5f), 0, kPalL - 1);
    cols_[s] = palette_[static_cast<size_t>(fi) * kPalL + static_cast<size_t>(li)];
  }
  // 2a. Colour bars (S11): straight through the palette (a stop's dB is the level that paints it), fading in over the
  //     12 dB above the colour window's bottom (less for a narrow window).
  if (bars) {
    const float lo = style_.colourFollow ? style_.floorDb : style_.colourLoDb;
    const float hi = style_.colourFollow ? style_.ceilDb : 0.0f;
    const float fade = std::min(kBarFadeDb, 0.4f * std::max(1.0f, hi - lo));
    bool anyBar = false;
    for (uint32_t s = 0; s < steps; ++s) {
      const float aBar = std::clamp((db_[s] - lo) / fade, 0.0f, 1.0f) * style_.alpha;
      anyBar = anyBar || aBar > 0.003f;
      tmp_[s] = ui::mulAlpha(cols_[s], aBar);
    }
    if (anyBar) {
      const uint32_t segs = steps - 1;
      const ui::DrawList::PrimSpan sp = dl.primReserveSpan(segs * 6, segs * 4);
      const ui::Vec2 uv = dl.whiteUv();
      ui::Vertex* v = sp.v;
      uint32_t* ix = sp.i;
      for (uint32_t i = 0; i < segs; ++i) {
        const float t0 = curve ? poly_[i].y : r.y0, t1 = curve ? poly_[i + 1].y : r.y0;
        const uint32_t b = sp.base + i * 4;
        v[0] = {{poly_[i].x, t0}, uv, tmp_[i]};
        v[1] = {{poly_[i + 1].x, t1}, uv, tmp_[i + 1]};
        v[2] = {{poly_[i + 1].x, r.y1}, uv, tmp_[i + 1]};
        v[3] = {{poly_[i].x, r.y1}, uv, tmp_[i]};
        ix[0] = b;
        ix[1] = b + 1;
        ix[2] = b + 2;
        ix[3] = b;
        ix[4] = b + 2;
        ix[5] = b + 3;
        v += 4;
        ix += 6;
      }
    }
  }
  // 2b. The curve: fill (not under bars: they already paint that space), one optional glow pass, the core.
  if (any && curve) {
    for (uint32_t s = 0; s < steps; ++s) cols_[s] = ui::mulAlpha(cols_[s], vis_[s] * style_.alpha);
    ui::SpectrumCurveStyle st;
    st.glowAmount = style_.glow;
    st.glow = style_.glow > 0.01f;
    st.fillAlpha = bars ? 0.0f : style_.fill * style_.alpha;
    st.lineWidth = style_.lineWidth * scale;
    ui::spectrumCurve(dl, poly_.data(), cols_.data(), lvl_.data(), steps, r.y1, st);
  }

  // 3. The peak hold (curve styles): through the knot spline, faded where it sits on the curve and where it reached
  //    the floor.
  if (style_.peaks && curve && shown_.peaks.size() >= 2) {
    pk_.resize(steps);
    pkSm_.resize(steps);
    for (uint32_t s = 0; s < steps; ++s) pk_[s] = sampleAt(shown_.peaks, ax.hz(xAt(s)));
    ui::splineSmooth(pk_.data(), pkSm_.data(), steps, std::max(2, static_cast<int>(std::lround(8.0f * scale))));
    bool anyPeak = false;
    for (uint32_t s = 0; s < steps; ++s) {
      const float db = pkSm_[s];
      const float lead = std::clamp((db - db_[s]) / 6.0f, 0.0f, 1.0f);
      const float pv = visT(db);
      anyPeak = anyPeak || pv > 0.0f;
      poly_[s] = {xAt(s), levelY(r, db)};
      cols_[s] = ui::setAlpha(kPeakColour, pv * (70.0f + 150.0f * lead) / 255.0f * style_.alpha);
    }
    if (anyPeak) ui::gradientPolyline(dl, poly_.data(), cols_.data(), steps, std::max(1.0f, 1.3f * scale));
  }

  // 4. The long-term average (S6): every style (a slow reference, like the slope guide).
  if (style_.average && shown_.average.size() >= 2) {
    bool anyAvg = false;
    for (uint32_t s = 0; s < steps; ++s) {
      const float db = sampleAt(shown_.average, ax.hz(xAt(s)));
      const float vis = visT(db);
      anyAvg = anyAvg || vis > 0.0f;
      poly_[s] = {xAt(s), levelY(r, db)};
      cols_[s] = ui::setAlpha(kAverageColour, vis * 230.0f / 255.0f * style_.alpha);
    }
    if (anyAvg) ui::gradientPolyline(dl, poly_.data(), cols_.data(), steps, std::max(1.0f, 1.8f * scale));
  }

  // 5. The slope guide: dashed, pivoted at 1 kHz on the middle of the range. On Log a straight line (the hybrid's
  //    linear foot drawn through, a guide is a straight reference, not a plot); on Mel / Linear the curve a dB/oct
  //    tilt is there, held flat below the 40 Hz corner (the true curve dives to −inf at 0 Hz).
  if (style_.slopeGuide && ax.offsetHz <= 0.0f) {
    const float anchor = 0.5f * (style_.floorDb + style_.ceilDb);
    auto yOf = [&](float db) { return r.y1 - (db - style_.floorDb) / range * (r.height() - 4.0f); };
    const float dashOn = 7.0f * scale, dashOff = 5.0f * scale, lw = std::max(1.0f, scale);
    float phase = 0.0f;
    auto dashTo = [&](ui::Vec2 a, ui::Vec2 b) {
      const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
      if (len <= 1e-4f) return;
      const float ux = dx / len, uy = dy / len, period = dashOn + dashOff;
      float d = 0.0f;
      while (d < len) {
        const float in = std::fmod(phase, period);
        const bool on = in < dashOn;
        const float run = std::min(len - d, (on ? dashOn : period) - in);
        if (on) {
          const ui::Vec2 seg[2] = {{a.x + ux * d, a.y + uy * d}, {a.x + ux * (d + run), a.y + uy * (d + run)}};
          ui::smoothPolyline(dl, seg, 2, kSlopeColour, lw);
        }
        d += run;
        phase += run;
      }
    };
    const dsp::FreqAxis& fa = ax.freq;
    const float slope = style_.tiltDbPerOct;
    if (fa.scale() == dsp::FreqScale::Log) {
      const float octPerT = fa.hybrid() ? fa.hybridShape().L / (1.0f - fa.hybridShape().A) : std::log2(fa.fMax() / fa.fMin());
      const float dbAt1 = anchor + slope * std::log2(fa.fMax() / 1000.0f);
      const float y0 = yOf(dbAt1 - slope * octPerT), y1 = yOf(dbAt1);
      float tA = 0.0f, tB = 1.0f;
      if (std::fabs(y1 - y0) > 1e-6f) {
        const float ta = (r.y0 - y0) / (y1 - y0), tb = (r.y1 - y0) / (y1 - y0);
        tA = std::clamp(std::min(ta, tb), 0.0f, 1.0f);
        tB = std::clamp(std::max(ta, tb), 0.0f, 1.0f);
      } else if (y0 < r.y0 || y0 > r.y1) {
        tB = tA;
      }
      if (tB > tA)
        dashTo({r.x0 + tA * r.width(), y0 + (y1 - y0) * tA}, {r.x0 + tB * r.width(), y0 + (y1 - y0) * tB});
    } else {
      constexpr int kSeg = 96;
      ui::Vec2 prev{};
      for (int i = 0; i <= kSeg; ++i) {
        const float t = static_cast<float>(i) / kSeg;
        const float f = std::max(fa.hz(t), dsp::kFreqAxisCornerHz);
        const ui::Vec2 q{r.x0 + t * r.width(), std::clamp(yOf(anchor + slope * std::log2(f / 1000.0f)), r.y0, r.y1)};
        if (i > 0) dashTo(prev, q);
        prev = q;
      }
    }
  }
}

void SpectrumPlot::paintResponse(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, float scale) {
  if (!style_.response || shown_.response.size() < 2 || shown_.confidence.size() != shown_.response.size()) return;
  const uint32_t steps = std::clamp<uint32_t>(static_cast<uint32_t>(r.width() / std::max(1.0f, 1.5f * scale)), 16, 1024);
  poly_.resize(steps);
  cols_.resize(steps);
  tmp_.resize(steps);
  bool any = false;
  for (uint32_t s = 0; s < steps; ++s) {
    const float x = r.x0 + r.width() * static_cast<float>(s) / static_cast<float>(steps - 1);
    const float hz = ax.hz(x);
    const float c = std::clamp(sampleAt(shown_.confidence, hz), 0.0f, 1.0f);
    const float y = std::clamp(responseY(r, sampleAt(shown_.response, hz)), r.y0 - 2.0f, r.y1 + 2.0f);
    poly_[s] = {x, y};
    cols_[s] = ui::setAlpha(kResponseColour, 0.95f * c);
    any = any || c > 0.0f;
  }
  if (!any) return;
  // A soft halo under a crisp feathered line: it must read against the spectrum's bright core.
  for (uint32_t s = 0; s < steps; ++s) tmp_[s] = ui::mulAlpha(cols_[s], 0.22f);
  ui::gradientPolyline(dl, poly_.data(), tmp_.data(), steps, std::max(2.0f, 4.0f * scale));
  ui::smoothPolylineCols(dl, poly_.data(), cols_.data(), steps, std::max(1.0f, 1.4f * scale));
}

void SpectrumPlot::paintLabels(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, const ui::Theme& th,
                               float scale, const PlotText& text) const {
  if (!text.draw) return;
  const float lh = text.lineHeight;
  if (style_.freqLabels && r.height() > 2.0f * lh) {
    dsp::FreqGridLine lines[24];
    const int n = dsp::freqGridLines(ax.freq, lines, 24);
    float usedL[24], usedR[24];
    size_t used = 0;
    const float ly = r.y1 - lh - std::round(1.0f * scale);
    for (int i = 0; i < n; ++i) {  // priority order: decades (or every other step) first, the rest where they fit
      const float x = std::round(ax.x(lines[i].hz) + 3.0f * scale);
      const float w = textWidth(text, lines[i].label);
      const float a = x - 2.0f * scale, b = x + w + 2.0f * scale;
      if (a < r.x0 || b > r.x1) continue;  // would overhang the plot
      bool clash = false;
      for (size_t u = 0; u < used && !clash; ++u) clash = !(b + 4.0f * scale <= usedL[u] || a >= usedR[u] + 4.0f * scale);
      if (clash) continue;
      usedL[used] = a;
      usedR[used] = b;
      ++used;
      dl.addRoundedRectFilled({a, ly - 1.0f, b, ly + lh + 1.0f}, 2.0f * scale, kLabelPad);
      text.draw({x, ly}, lines[i].label, th.device.displayText);
    }
  }
  if (style_.dbLabels && r.width() >= 70.0f * scale) {
    char buf[16];
    for (float db = std::ceil(style_.floorDb / 12.0f) * 12.0f; db <= style_.ceilDb; db += 12.0f) {
      const float y = levelY(r, db);
      if (y - 0.5f * lh < r.y0 + 2.0f || y + 0.5f * lh > r.y1 - lh - 4.0f * scale) continue;  // clear of the freq row
      std::snprintf(buf, sizeof buf, "%.0f", db);
      const float w = textWidth(text, buf);
      text.draw({std::round(r.x1 - w - 4.0f * scale), std::round(y - 0.5f * lh)}, buf, th.device.displayText);
    }
  }
  // S7: the tilt tag, top left, whenever the analysis is tilted (why most mixes read nearly flat at 4.5).
  if (style_.tiltTag && std::fabs(style_.tiltDbPerOct) >= 0.05f && r.width() >= 120.0f * scale) {
    char tl[32];
    std::snprintf(tl, sizeof tl, "tilt %+.1f dB/oct", style_.tiltDbPerOct);
    text.draw({std::round(r.x0 + 6.0f * scale), std::round(r.y0 + 4.0f * scale)}, tl, th.device.displayText);
  }
}

void SpectrumPlot::paintHover(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, float px, float scale,
                              const PlotText& text) const {
  if (px < r.x0 || px >= r.x1) return;
  dl.addRectFilled({std::round(px), r.y0, std::round(px) + 1.0f, r.y1}, kHoverLine);
  if (!text.draw) return;
  const float hz = ax.hz(px);
  char f[32], note[32], lv[40], rs[40];
  if (hz >= 1000.0f) std::snprintf(f, sizeof f, "%.2f kHz", hz / 1000.0f);
  else std::snprintf(f, sizeof f, "%.0f Hz", hz);
  noteName(hz, note, sizeof note);
  lv[0] = rs[0] = '\0';
  const bool level = !shown_.out.empty() && levelAt(hz) > style_.floorDb;
  if (level) std::snprintf(lv, sizeof lv, "%.1f dB", levelAt(hz));
  float conf = 0.0f;
  const float h = responseAt(hz, &conf);
  if (style_.response && conf > 0.5f) std::snprintf(rs, sizeof rs, "response %+.1f dB", h);
  const float ft = std::clamp((px - r.x0) / std::max(1.0f, r.width()), 0.0f, 1.0f);
  const char* lines[4] = {f, note, lv, rs};
  const ui::Rgba colours[4] = {kHoverText, kHoverNote, ui::brighten(colourAt(ft, levelAt(hz)), 0.35f), kResponseColour};
  float w = 0.0f;
  for (const char* l : lines)
    if (l[0]) w = std::max(w, textWidth(text, l));
  const float gap = 8.0f * scale;
  const float tx = px + gap + w > r.x1 - 4.0f * scale ? px - gap - w : px + gap;
  float ty = r.y0 + 4.0f * scale;
  for (int i = 0; i < 4; ++i) {
    if (!lines[i][0]) continue;
    text.draw({std::round(tx), std::round(ty)}, lines[i], colours[i]);
    ty += text.lineHeight + 2.0f * scale;
  }
}

void SpectrumPlot::paintAll(ui::DrawList& dl, const ui::Rect& r, const ui::Theme& th, float scale, const PlotText& text,
                            float hoverX) {
  SpectrumAxis ax{r.x0, r.x1};
  ax.freq = style_.axis;
  paintBackdrop(dl, r, ax);
  paintGrid(dl, r, ax, th, scale);
  paintSpectrum(dl, r, ax, scale);
  paintResponse(dl, r, ax, scale);
  paintLabels(dl, r, ax, th, scale, text);
  if (hoverX >= 0.0f) paintHover(dl, r, ax, hoverX, scale, text);
}

}  // namespace pq::views::devices
