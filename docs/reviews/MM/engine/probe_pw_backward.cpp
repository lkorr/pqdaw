// MM review, loose end (c) / Q190 item 4: "backward with PW modulated at 2 kHz full depth aliases 5.4 dB more than
// forward (-89.7 vs -95.1 dBFS)". Is that a systematic direction bias in wavetable_live, or the worst-bin statistic of
// two error terms that interfere differently in the two renders?
// Uses the TMM.8 test rig (tests/dsp/wave_live_rig.h) unchanged; doctest disabled (its REQUIREs compile to nothing).
//   E1  the test's case (k0 601, modulator bin 683, phase0 0.3): forward / backward alias vs the 64x reference
//   E2  the same with the modulator's start phase varied (the mirror relation holds for every phase0)
//   E3  neighbouring modulator bins
//   E4  corrections off (does the gap come from the corrections or from the read / level choice?)
//   E5  backward vs forward spectra directly (how far the backward render is from the mirror of the forward one)
//   E6  where the error sits: worst bins and an RSS over all bins (a total, not one bin)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "wave_live_rig.h"

using namespace pq::dsp;
using namespace wlrig;

namespace {
constexpr double kSr = 48000;
constexpr uint32_t N = 1u << 14;

struct Err {
  double worst, worstHz, rssDb;
};
Err errOf(const std::vector<double>& live, const std::vector<double>& ref) {
  const Compare c = compare(live, ref, kSr, N);
  double e = 0;
  for (uint32_t b = 1; b < live.size() && b * kSr / N < 20000.0; ++b) e += (live[b] - ref[b]) * (live[b] - ref[b]);
  return {c.worstDb, c.worstHz, 10.0 * std::log10(std::max(e, 1e-60))};
}

void run(const TablePtr& t, uint32_t k0, double km, double phase0, bool corr, uint32_t os, bool direct, bool detail = false) {
  const double fm = km / N;
  const Ctl wf = sine(0.8, 0.0, fm, phase0), wb = sine(0.8, 0.0, -fm, phase0);
  FrameTableSlot slot(t);
  LiveRegion fwd(os, kSr, WarpMode::PulseWidth, corr, wf, constant(0), constant(0));
  LiveRegion bwd(os, kSr, WarpMode::PulseWidth, corr, wb, constant(0), constant(0));
  const auto af = amplitudes(renderLive(fwd, slot, k0, N));
  const auto ab = amplitudes(renderLive(bwd, slot, N - k0, N));
  if (direct) {
    const Err d = errOf(ab, af);
    std::printf("  k0 %u km %.0f ph0 %.2f corr %d os %u: |bwd|-|fwd| worst %.1f dB @ %.0f Hz, rss %.1f dB\n", k0, km,
                phase0, corr ? 1 : 0, os, d.worst, d.worstHz, d.rssDb);
    return;
  }
  Reference ref;
  ref.os = os;
  ref.sampleRate = kSr;
  ref.corrections = corr;
  const auto want = ref.amplitudes(t->view(), WarpMode::PulseWidth, wf, constant(0), constant(0), k0, N);
  const Err ef = errOf(af, want), eb = errOf(ab, want);
  std::printf("  k0 %u km %.0f ph0 %.2f corr %d os %u: fwd %.1f dBFS @ %.0f Hz (rss %.1f) | bwd %.1f dBFS @ %.0f Hz "
              "(rss %.1f) | gap worst %+.1f dB, rss %+.1f dB\n",
              k0, km, phase0, corr ? 1 : 0, os, ef.worst, ef.worstHz, ef.rssDb, eb.worst, eb.worstHz, eb.rssDb,
              eb.worst - ef.worst, eb.rssDb - ef.rssDb);
  std::fflush(stdout);
  if (detail) {
    // E6: the worst bin's line (h·f0 + m·fm, a real sideband of the PW modulation, not a folded alias) and the
    // pure-alias floor: the loudest bin where the reference is empty (< -140 dBFS), and the error RSS over those bins.
    const uint32_t wb = static_cast<uint32_t>(std::lround(eb.worstHz * N / kSr));
    std::printf("    worst bin %u: ref %.3e  fwd %.3e (%+.3e)  bwd %.3e (%+.3e)\n", wb, want[wb], af[wb], af[wb] - want[wb],
                ab[wb], ab[wb] - want[wb]);
    double ef0 = 0, eb0 = 0, lf = -400, lb = -400, sf = 0, sb = 0;
    uint32_t nEmpty = 0;
    for (uint32_t b = 1; b < want.size() && b * kSr / N < 20000.0; ++b) {
      if (want[b] < 1e-7) {
        ++nEmpty;
        ef0 += af[b] * af[b];
        eb0 += ab[b] * ab[b];
        lf = std::max(lf, 20 * std::log10(std::max(af[b], 1e-30)));
        lb = std::max(lb, 20 * std::log10(std::max(ab[b], 1e-30)));
      } else {
        sf += (af[b] - want[b]) * (af[b] - want[b]);
        sb += (ab[b] - want[b]) * (ab[b] - want[b]);
      }
    }
    std::printf("    empty bins %u: loudest fwd %.1f / bwd %.1f dBFS, rss fwd %.1f / bwd %.1f dB; line-bin error rss fwd "
                "%.1f / bwd %.1f dB\n",
                nEmpty, lf, lb, 10 * std::log10(std::max(ef0, 1e-60)), 10 * std::log10(std::max(eb0, 1e-60)),
                10 * std::log10(std::max(sf, 1e-60)), 10 * std::log10(std::max(sb, 1e-60)));
  }
}
}  // namespace

int main(int argc, char** argv) {
  const std::string which = argc > 1 ? argv[1] : "all";
  const TablePtr t = build({ftfix::basic(BasicCycle::Saw)}, kSr);
  auto want = [&](const char* e) { return which == "all" || which == e; };
  if (want("E1")) {
    std::printf("E1 the test's case\n");
    run(t, 601, 683, 0.3, true, 8, false);
  }
  if (want("E2")) {
    std::printf("E2 modulator start phase\n");
    for (const double p : {0.0, 1.0, 1.9, 3.0, 4.5}) run(t, 601, 683, p, true, 8, false);
  }
  if (want("E3")) {
    std::printf("E3 neighbouring modulator bins / pitches\n");
    for (const double km : {681.0, 685.0, 671.0}) run(t, 601, km, 0.3, true, 8, false);
    run(t, 597, 683, 0.3, true, 8, false);
    run(t, 605, 683, 0.3, true, 8, false);
  }
  if (want("E4")) {
    std::printf("E4 corrections off\n");
    run(t, 601, 683, 0.3, false, 8, false);
  }
  if (want("E5")) {
    std::printf("E5 backward vs forward directly (0 = exact mirror)\n");
    run(t, 601, 683, 0.3, true, 8, true);
    run(t, 601, 683, 0.3, false, 8, true);
    run(t, 601, 41, 0.3, true, 8, true);
  }
  if (want("E6")) {
    std::printf("E6 where the error sits\n");
    run(t, 601, 683, 0.3, true, 8, false, true);
    run(t, 601, 683, 0.3, false, 8, false, true);
    run(t, 601, 41, 0.3, true, 8, false, true);
  }
  return 0;
}
