// The reusable spectrum + response display (pq/views/devices/spectrum_plot.h; TX.spectrum).

#include "pq/views/devices/spectrum_plot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pq/ui/colormap.h"
#include "pq/ui/lines.h"

namespace pq::views::devices {

namespace {

constexpr double kEaseSeconds = 0.045;  // PQ Meter's display smoothing
const float kLogSpan = std::log(SpectrumAxis::kHiHz / SpectrumAxis::kLoHz);

// PQ Meter's fixed signal-display colours (SPEC §11.1: its palette applies to signal displays).
constexpr ui::Rgba kPeakColour = ui::rgba(232, 240, 255);
constexpr ui::Rgba kInputColour = ui::rgba(150, 162, 186);
constexpr ui::Rgba kResponseColour = ui::hex(0xAC8EFF);  // the violet accent
constexpr ui::Rgba kLabelPad = ui::rgba(8, 8, 12, 150);
constexpr ui::Rgba kHoverLine = ui::rgba(255, 255, 255, 60);
constexpr ui::Rgba kHoverText = ui::rgba(255, 255, 255, 210);
constexpr ui::Rgba kHoverNote = ui::rgba(160, 220, 255, 190);

constexpr float kGridHz[] = {20, 30, 50, 100, 200, 300, 500, 1000, 2000, 3000, 5000, 10000, 20000};
// Labels by priority: decades and the ends first, the in-between ticks where they fit.
struct Label {
  float hz;
  const char* text;
};
constexpr Label kLabels[] = {{20, "20"},   {100, "100"}, {1000, "1k"},  {10000, "10k"}, {20000, "20k"},
                             {50, "50"},   {200, "200"}, {500, "500"},  {2000, "2k"},   {5000, "5k"},
                             {30, "30"},   {300, "300"}, {3000, "3k"}};

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

}  // namespace

float SpectrumAxis::x(float hz) const noexcept {
  return x0 + (x1 - x0) * std::log(std::max(hz, 1.0f) / kLoHz) / kLogSpan;
}
float SpectrumAxis::hz(float px) const noexcept {
  return kLoHz * std::exp(kLogSpan * std::clamp((px - x0) / std::max(x1 - x0, 1e-3f), 0.0f, 1.0f));
}

SpectrumPlot::~SpectrumPlot() { release(); }

void SpectrumPlot::setFeed(SpectrumFeed* f, std::function<void()> requestFrame) {
  requestFrame_ = std::move(requestFrame);
  if (f == feed_) return;
  release();
  feed_ = f;
  seq_ = 0;
  target_ = {};
  shown_ = {};
  if (feed_) {
    axisT_ = [f](float hz) { return f->axisT(hz); };
    points_ = feed_->config().points;
  }
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

void SpectrumPlot::setShownForTest(SpectrumSnapshot s, float pointCount) {
  shown_ = std::move(s);
  points_ = static_cast<uint32_t>(pointCount);
  axisT_ = [](float hz) {
    return std::clamp(std::log(std::max(hz, 1.0f) / SpectrumAxis::kLoHz) / kLogSpan, 0.0f, 1.0f);
  };
}

float SpectrumPlot::sampleAt(const std::vector<float>& v, float hz) const noexcept {
  const size_t P = v.size();
  if (P < 2 || !axisT_) return P == 1 ? v[0] : style_.floorDb - 1.0f;
  const float t = std::clamp(axisT_(hz), 0.0f, 1.0f) * static_cast<float>(P - 1);
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

// ---- painting ------------------------------------------------------------------------------------------------------------

void SpectrumPlot::paintBackdrop(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax) const {
  if (!style_.bands) return;
  const ui::ColorLut& lut = ui::colormapLut(ui::Colormap::Ice);
  const float edges[4] = {r.x0, std::max(r.x0, ax.x(250.0f)), std::min(r.x1, ax.x(4000.0f)), r.x1};
  // The tint ramps up toward the floor and back out over the last rows (no hard edge on the panel's last row).
  const float yTail = std::max(r.y0, r.y1 - std::min(10.0f, r.height() * 0.05f));
  for (int b = 0; b < 3; ++b) {
    if (edges[b + 1] <= edges[b] + 1.0f) continue;
    // The ramp at each third's place (PQ Meter: the palette at b/3 + 0.16, level 0.85), very faint.
    const ui::Rgba tint = ui::setAlpha(ui::lutAt(lut, 0.35f + 0.6f * (static_cast<float>(b) / 3.0f + 0.16f)), 0.055f);
    const ui::Rgba clear = ui::setAlpha(tint, 0.0f);
    dl.addRectFilledMultiColor({edges[b], r.y0, edges[b + 1], yTail}, clear, clear, tint, tint);
    dl.addRectFilledMultiColor({edges[b], yTail, edges[b + 1], r.y1}, tint, tint, clear, clear);
  }
}

void SpectrumPlot::paintGrid(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, const ui::Theme& th, float,
                             bool dbRules) const {
  if (!style_.grid) return;
  for (const float f : kGridHz) {
    const float x = std::round(ax.x(f));
    if (x <= r.x0 || x >= r.x1) continue;
    const bool major = f == 100.0f || f == 1000.0f || f == 10000.0f;
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
  const uint32_t steps = std::clamp<uint32_t>(static_cast<uint32_t>(r.width()), 32, 1024);
  poly_.resize(steps);
  cols_.resize(steps);
  lvl_.resize(steps);
  vis_.resize(steps);
  const float range = std::max(1.0f, style_.ceilDb - style_.floorDb);
  auto levelT = [&](float db) { return std::clamp((db - style_.floorDb) / range, 0.0f, 1.0f); };
  // A vertex at the floor fades out over the lowest 4 % of the range: silence draws no curve at all.
  auto visT = [&](float db) { return std::clamp((db - style_.floorDb) / std::max(1.0f, 0.04f * range), 0.0f, 1.0f); };
  auto xAt = [&](uint32_t s) { return r.x0 + r.width() * static_cast<float>(s) / static_cast<float>(steps - 1); };

  // 1. The input, faint and flat behind (no glow): what reached the device.
  if (style_.input && shown_.in.size() >= 2) {
    bool any = false;
    for (uint32_t s = 0; s < steps; ++s) {
      const float x = xAt(s), db = sampleAt(shown_.in, ax.hz(x));
      poly_[s] = {x, levelY(r, db)};
      cols_[s] = ui::setAlpha(kInputColour, 0.55f * visT(db));
      any = any || visT(db) > 0.0f;
    }
    if (any) {
      static thread_local std::vector<ui::Rgba> f;
      f.resize(steps);
      for (uint32_t s = 0; s < steps; ++s) f[s] = ui::mulAlpha(cols_[s], 0.18f);
      ui::gradientBand(dl, poly_.data(), f.data(), r.y1, 0.0f, steps);
      ui::gradientPolyline(dl, poly_.data(), cols_.data(), steps, std::max(1.0f, 1.1f * scale));
    }
  }

  // 2. The output: Ice by level, fill, bloom, body, core (ui::spectrumCurve).
  const ui::ColorLut& lut = ui::colormapLut(ui::Colormap::Ice);
  bool any = false;
  for (uint32_t s = 0; s < steps; ++s) {
    const float x = xAt(s), db = sampleAt(shown_.out, ax.hz(x));
    const float lt = levelT(db), vis = visT(db);
    any = any || vis > 0.0f;
    poly_[s] = {x, levelY(r, db)};
    lvl_[s] = lt;
    vis_[s] = vis;
    cols_[s] = ui::mulAlpha(ui::lutAt(lut, 0.25f + 0.75f * lt), vis);
  }
  if (any) {
    ui::SpectrumCurveStyle st;
    st.glowAmount = style_.glow;
    st.glow = style_.glow > 0.01f;
    st.fillAlpha = style_.fill;
    st.lineWidth = style_.lineWidth * scale;
    ui::spectrumCurve(dl, poly_.data(), cols_.data(), lvl_.data(), steps, r.y1, st);
  }

  // 3. The peak hold: through the knot spline, faded where it sits on the curve and where it reached the floor.
  if (style_.peaks && shown_.peaks.size() >= 2) {
    pk_.resize(steps);
    pkSm_.resize(steps);
    for (uint32_t s = 0; s < steps; ++s) pk_[s] = sampleAt(shown_.peaks, ax.hz(xAt(s)));
    ui::splineSmooth(pk_.data(), pkSm_.data(), steps, std::max(2, static_cast<int>(std::lround(8.0f * scale))));
    bool anyPeak = false;
    for (uint32_t s = 0; s < steps; ++s) {
      const float db = pkSm_[s];
      const float lead = std::clamp((db - sampleAt(shown_.out, ax.hz(xAt(s)))) / 6.0f, 0.0f, 1.0f);
      const float pv = visT(db);
      anyPeak = anyPeak || pv > 0.0f;
      poly_[s] = {xAt(s), levelY(r, db)};
      cols_[s] = ui::setAlpha(kPeakColour, pv * (70.0f + 150.0f * lead) / 255.0f);
    }
    if (anyPeak) ui::gradientPolyline(dl, poly_.data(), cols_.data(), steps, std::max(1.0f, 1.3f * scale));
  }
}

void SpectrumPlot::paintResponse(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, float scale) {
  if (!style_.response || shown_.response.size() < 2 || shown_.confidence.size() != shown_.response.size()) return;
  const uint32_t steps = std::clamp<uint32_t>(static_cast<uint32_t>(r.width() / std::max(1.0f, 1.5f * scale)), 16, 1024);
  poly_.resize(steps);
  cols_.resize(steps);
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
  static thread_local std::vector<ui::Rgba> halo;
  halo.resize(steps);
  for (uint32_t s = 0; s < steps; ++s) halo[s] = ui::mulAlpha(cols_[s], 0.22f);
  ui::gradientPolyline(dl, poly_.data(), halo.data(), steps, std::max(2.0f, 4.0f * scale));
  ui::smoothPolylineCols(dl, poly_.data(), cols_.data(), steps, std::max(1.0f, 1.4f * scale));
}

void SpectrumPlot::paintLabels(ui::DrawList& dl, const ui::Rect& r, const SpectrumAxis& ax, const ui::Theme& th,
                               float scale, const PlotText& text) const {
  if (!text.draw) return;
  const float lh = text.lineHeight;
  if (style_.freqLabels && r.height() > 2.0f * lh) {
    float usedL[std::size(kLabels)], usedR[std::size(kLabels)];
    size_t used = 0;
    const float ly = r.y1 - lh - std::round(1.0f * scale);
    for (const Label& l : kLabels) {
      const float x = std::round(ax.x(l.hz) + 3.0f * scale);
      const float w = textWidth(text, l.text);
      const float a = x - 2.0f * scale, b = x + w + 2.0f * scale;
      if (a < r.x0 || b > r.x1) continue;  // would overhang the plot
      bool clash = false;
      for (size_t u = 0; u < used && !clash; ++u) clash = !(b + 4.0f * scale <= usedL[u] || a >= usedR[u] + 4.0f * scale);
      if (clash) continue;
      usedL[used] = a;
      usedR[used] = b;
      ++used;
      dl.addRoundedRectFilled({a, ly - 1.0f, b, ly + lh + 1.0f}, 2.0f * scale, kLabelPad);
      text.draw({x, ly}, l.text, th.device.displayText);
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
  const char* lines[4] = {f, note, lv, rs};
  const ui::Rgba colours[4] = {kHoverText, kHoverNote,
                               ui::brighten(ui::lutAt(ui::colormapLut(ui::Colormap::Ice),
                                                      0.25f + 0.75f * std::clamp((levelAt(hz) - style_.floorDb) /
                                                                                         std::max(1.0f, style_.ceilDb - style_.floorDb),
                                                                                     0.0f, 1.0f)),
                                            0.35f),
                               kResponseColour};
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
  const SpectrumAxis ax{r.x0, r.x1};
  paintBackdrop(dl, r, ax);
  paintGrid(dl, r, ax, th, scale);
  paintSpectrum(dl, r, ax, scale);
  paintResponse(dl, r, ax, scale);
  paintLabels(dl, r, ax, th, scale, text);
  if (hoverX >= 0.0f) paintHover(dl, r, ax, hoverX, scale, text);
}

}  // namespace pq::views::devices
