// TX.spectrogram: the reassigned spectrogram's columns (pq/dsp/spectrogram.h, PQ Meter's analysis) — a sine lands on its
// row at its level, a log sweep follows its instantaneous frequency column by column, a chord's partials are separate
// lines with dark space between them, the low chain resolves a 40 Hz sub, an impulse is a vertical line, silence leaves
// the columns empty, and the column sequence continues across reconfiguration (a new mapping is counted).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "pq/dsp/spectrogram.h"

using namespace pq::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;
constexpr uint32_t kRows = Spectrogram::kRows;

struct Collect final : Spectrogram::Sink {
  std::vector<std::vector<float>> cols;
  std::vector<double> centres;
  std::vector<uint64_t> index;
  void column(const float* rows, uint64_t i, double centre) override {
    cols.emplace_back(rows, rows + kRows);
    centres.push_back(centre);
    index.push_back(i);
  }
};

// The row a frequency lands on (row 0 = fMax).
float rowOf(const Spectrogram& g, float hz) {
  const SpectrogramConfig& c = g.config();
  return (1.0f - gramUnit(c.scale, hz, g.fMin(), g.fMax())) * static_cast<float>(kRows - 1);
}

uint32_t argmax(const std::vector<float>& c, uint32_t lo = 0, uint32_t hi = kRows) {
  uint32_t best = lo;
  for (uint32_t r = lo; r < hi; ++r)
    if (c[r] > c[best]) best = r;
  return best;
}

float db(float p) { return 10.0f * std::log10(std::max(p, 1e-30f)); }

void pushAll(Spectrogram& g, const std::vector<float>& x, Collect& out) {
  for (size_t i = 0; i < x.size(); i += 1000) g.push(x.data() + i, static_cast<uint32_t>(std::min<size_t>(1000, x.size() - i)), out);
}

}  // namespace

TEST_CASE("spectrogram: a sine lands on its row at its level; silence leaves the columns empty") {
  SpectrogramConfig cfg;
  cfg.hop = 512;
  Spectrogram g(cfg);
  CHECK(g.fftSize() == 2048);
  CHECK(g.lowFftSize() == 4096);  // 85 ms at 48 kHz
  CHECK(g.hop() == 512);
  Collect out;
  std::vector<float> x(static_cast<size_t>(kSr));
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kSr));
  pushAll(g, x, out);
  REQUIRE(out.cols.size() > 60);
  CHECK(g.columns() == out.cols.size());
  for (size_t i = 0; i < out.index.size(); ++i) CHECK(out.index[i] == i);  // a gapless sequence
  const float expect = rowOf(g, 1000.0f);
  int checked = 0;
  for (size_t c = 20; c + 20 < out.cols.size(); ++c) {
    const auto& col = out.cols[c];
    const uint32_t r = argmax(col);
    CHECK(std::fabs(static_cast<float>(r) - expect) <= 1.0f);
    CHECK(std::fabs(db(col[r]) + 6.02f) < 0.8f);  // amplitude 0.5: −6 dB
    // A hairline: 6 rows away nothing within 55 dB of it (the leakage mask keeps everything 58 dB under a strong bin's
    // skirt out; what is left PQ Meter leaves to the display floor and the visual gate).
    for (uint32_t k = 0; k < kRows; ++k)
      if (std::fabs(static_cast<float>(k) - expect) > 6.0f)
        CHECK_MESSAGE(db(col[k]) < db(col[r]) - 55.0f, "row " << k << " " << db(col[k]));
    ++checked;
  }
  CHECK(checked > 40);
  // Silence: empty columns.
  Collect quiet;
  std::vector<float> z(24000, 0.0f);
  pushAll(g, z, quiet);
  REQUIRE(quiet.cols.size() > 20);
  for (size_t c = 12; c < quiet.cols.size(); ++c) CHECK(*std::max_element(quiet.cols[c].begin(), quiet.cols[c].end()) == 0.0f);
}

TEST_CASE("spectrogram: a log sweep lands on the rows of its instantaneous frequency, column by column") {
  SpectrogramConfig cfg;
  cfg.hop = 480;  // 10 ms columns
  Spectrogram g(cfg);
  Collect out;
  // 100 Hz → 8 kHz over 3 s (exponential): f(t) = f0 · k^t.
  const double f0 = 100.0, f1 = 8000.0, dur = 3.0, k = std::pow(f1 / f0, 1.0 / dur);
  std::vector<float> x(static_cast<size_t>(dur * kSr));
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / kSr;
    const double phase = f0 * (std::pow(k, t) - 1.0) / std::log(k);
    x[i] = static_cast<float>(0.4 * std::sin(2.0 * kPi * phase));
  }
  pushAll(g, x, out);
  REQUIRE(out.cols.size() > 250);
  int checked = 0;
  double worst = 0.0;
  for (size_t c = 0; c < out.cols.size(); ++c) {
    const double t = out.centres[c] / kSr;
    if (t < 0.15 || t > dur - 0.15) continue;  // away from the ends (half a window of start-up / tail)
    const double hz = f0 * std::pow(k, t);
    const float expect = rowOf(g, static_cast<float>(hz));
    const uint32_t r = argmax(out.cols[c]);
    worst = std::max(worst, std::fabs(static_cast<double>(r) - expect));
    CHECK_MESSAGE(std::fabs(static_cast<float>(r) - expect) <= 3.0f, "t " << t << " hz " << hz << " row " << r << " want " << expect);
    CHECK(db(out.cols[c][r]) > -18.0f);  // the ridge stays bright (amplitude 0.4: −8 dB; a fast chirp spreads a little)
    ++checked;
  }
  MESSAGE("sweep: worst row error " << worst << " over " << checked << " columns");
  CHECK(checked > 200);
}

TEST_CASE("spectrogram: a chord's partials are separate lines with dark space between them") {
  SpectrogramConfig cfg;
  cfg.hop = 512;
  Spectrogram g(cfg);
  Collect out;
  // A major triad on A3 (220, 277.18, 329.63 Hz), four harmonics each, falling 1/h.
  const double roots[] = {220.0, 220.0 * std::pow(2.0, 4.0 / 12.0), 220.0 * std::pow(2.0, 7.0 / 12.0)};
  std::vector<float> x(static_cast<size_t>(kSr));
  std::vector<double> partials;
  for (const double f : roots)
    for (int h = 1; h <= 4; ++h) partials.push_back(f * h);
  for (size_t i = 0; i < x.size(); ++i) {
    double v = 0.0;
    for (const double f : roots)
      for (int h = 1; h <= 4; ++h) v += 0.15 / h * std::sin(2.0 * kPi * f * h * static_cast<double>(i) / kSr + h);
    x[i] = static_cast<float>(v);
  }
  pushAll(g, x, out);
  REQUIRE(out.cols.size() > 60);
  std::sort(partials.begin(), partials.end());
  int checked = 0;
  for (size_t c = 20; c + 20 < out.cols.size(); c += 5) {
    const auto& col = out.cols[c];
    for (size_t p = 0; p < partials.size(); ++p) {
      const auto f = static_cast<float>(partials[p]);
      const float expect = rowOf(g, f);
      const auto lo = static_cast<uint32_t>(std::max(0.0f, expect - 2.0f)), hi = static_cast<uint32_t>(expect + 3.0f);
      const uint32_t r = argmax(col, lo, hi);
      CAPTURE(f);
      // (≤ 2 rows: 831.5 Hz (C#'s 3rd) and 880 Hz (A's 4th) are two bins apart and pull each other a little.)
      CHECK(std::fabs(static_cast<float>(r) - expect) <= 2.0f);
      CHECK(db(col[r]) > -40.0f);
      // Between this partial and the next one up (at least 5 rows apart): a row 30 dB darker than the dimmer of the two.
      if (p + 1 < partials.size()) {
        const float next = rowOf(g, static_cast<float>(partials[p + 1]));
        if (expect - next >= 5.0f) {
          const auto mid = static_cast<uint32_t>(std::lround(0.5f * (expect + next)));
          const uint32_t rn = argmax(col, static_cast<uint32_t>(std::max(0.0f, next - 2.0f)), static_cast<uint32_t>(next + 3.0f));
          CHECK(db(col[mid]) < std::min(db(col[r]), db(col[rn])) - 30.0f);
        }
      }
    }
    ++checked;
  }
  CHECK(checked > 6);
}

TEST_CASE("spectrogram: the low chain resolves a 40 Hz sub on the hybrid axis; an impulse is a vertical line") {
  SpectrogramConfig cfg;
  cfg.hop = 512;
  Spectrogram g(cfg);
  Collect out;
  std::vector<float> x(static_cast<size_t>(kSr));
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 40.0 * static_cast<double>(i) / kSr));
  pushAll(g, x, out);
  const float expect = rowOf(g, 40.0f);
  for (size_t c = 20; c + 20 < out.cols.size(); ++c) {
    const uint32_t r = argmax(out.cols[c]);
    CHECK(std::fabs(static_cast<float>(r) - expect) <= 1.5f);
  }
  // An impulse in silence: one column (± the tent) lit across most of the spectrum.
  Spectrogram g2(cfg);
  Collect imp;
  std::vector<float> y(static_cast<size_t>(kSr / 2), 0.0f);
  y[12000] = 0.9f;
  pushAll(g2, y, imp);
  size_t best = 0, bestLit = 0;
  for (size_t c = 0; c < imp.cols.size(); ++c) {
    size_t lit = 0;
    for (uint32_t r = 0; r < kRows; ++r) lit += imp.cols[c][r] > 0.0f ? 1u : 0u;
    if (lit > bestLit) {
      bestLit = lit;
      best = c;
    }
  }
  CHECK(bestLit > kRows / 3);
  CHECK(std::fabs(imp.centres[best] - 12000.0) <= 512.0 * 1.5);  // at the impulse's time (reassigned)
  size_t litCols = 0;
  for (const auto& col : imp.cols) litCols += *std::max_element(col.begin(), col.end()) > 0.0f ? 1u : 0u;
  CHECK(litCols <= 4);
}

TEST_CASE("spectrogram: the column sequence continues across reconfiguration; a new mapping is counted") {
  SpectrogramConfig cfg;
  cfg.hop = 512;
  Spectrogram g(cfg);
  Collect out;
  std::vector<float> x(24000);
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 500.0 * static_cast<double>(i) / kSr));
  pushAll(g, x, out);
  const uint32_t m0 = g.mapping();
  cfg.hop = 256;  // a hop change (a display's span or width): the same mapping
  g.configure(cfg, out);
  CHECK(g.mapping() == m0);
  CHECK(g.hop() == 256);
  pushAll(g, x, out);
  cfg.scale = GramScale::Mel;  // a new row mapping
  g.configure(cfg, out);
  CHECK(g.mapping() == m0 + 1);
  pushAll(g, x, out);
  cfg.fftSize = 8192;  // a longer window: wider reach, the hop kept
  g.configure(cfg, out);
  CHECK(g.fftSize() == 8192);
  CHECK(g.lowFftSize() == 0);  // the main window already covers 85 ms
  pushAll(g, x, out);
  for (size_t i = 0; i < out.index.size(); ++i) REQUIRE(out.index[i] == i);
  CHECK(g.columns() == out.cols.size());
  CHECK(g.position() >= static_cast<double>(g.columns()));
  CHECK(g.position() <= static_cast<double>(g.columns()) + 1.0);
  // A long hop widens the window (≥ 2× overlap); a high rate keeps the window's duration.
  SpectrogramConfig wide;
  wide.hop = 3000;
  Spectrogram gw(wide);
  CHECK(gw.fftSize() == 8192);
  SpectrogramConfig hi;
  hi.sampleRate = 192000.0;
  Spectrogram gh(hi);
  CHECK(gh.fftSize() == 8192);
  // A tiny hop is raised until the open columns hold the reach.
  SpectrogramConfig tiny;
  tiny.hop = 32;
  Spectrogram gt(tiny);
  CHECK(2 * gt.ahead() + 2 <= Spectrogram::kOpen);
}
