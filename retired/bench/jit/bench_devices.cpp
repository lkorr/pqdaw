// T4.9 / T4.10 / T4.11: the stock devices of stdlib/devices/ (Utility, Saturator, EQ Eight, Auto Filter) compiled by
// the JIT (and the interpreter, for reference) against their hand-written C++ implementations (tests/stdlib/device_reference.h: the
// same float32 formulas, state in locals, one loop over the samples with every stage interleaved, param-rate values
// once per block). Release / ship build.
//
// Every case is one stereo instance on a global track: 64-frame blocks, ns per stereo sample frame. JIT and C++ are
// measured alternately, three rounds each; the table prints each one's best median and the ratio.
//   pq_bench_devices [--block N] [filter]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../tests/stdlib/device_reference.h"
#include "../../tests/stdlib/stdlib_harness.h"
#include "pq/core/denormal_guard.h"
#include "pq_bench.h"

namespace st = stdlib_test;
namespace eng = pq::engine;

namespace {

uint32_t kBlock = 64;  // --block N (≤ 1024)
constexpr float kSr = 48000.0f;
std::string gFilter;

// Stereo input (two different noise-like signals at about −6 dBFS) and an audio-rate control.
struct Signals {
  std::vector<float> ch[2], ctl;
  Signals() {
    for (uint32_t i = 0; i < 1024; ++i) {
      const float t = static_cast<float>(i);
      ch[0].push_back(0.4f * std::sin(0.37f * t) + 0.25f * std::sin(1.91f * t) + 0.1f * std::sin(2.71f * t));
      ch[1].push_back(0.35f * std::sin(0.23f * t) + 0.3f * std::sin(1.37f * t) + 0.1f * std::sin(2.93f * t));
      ctl.push_back(800.0f + 600.0f * std::sin(0.05f * t));
    }
  }
};
const Signals& sig() {
  static const Signals s;
  return s;
}

// A compiled image driven by blocks: root input 0 = the stereo signal, input 1 (if any) = the control.
class Rig {
public:
  Rig(const st::Program& p, st::Backend backend, const std::map<std::string, double>& params) {
    auto built = st::build(p, backend, kBlock, 1);
    if (!built) {
      std::fprintf(stderr, "%s\n", built.error().message.c_str());
      std::exit(2);
    }
    img_ = std::move(built.value());
    const eng::ImageInterface& io = img_->io();
    gIn_.resize(io.globalIn.size());
    for (size_t s = 0; s < io.globalIn.size(); ++s) {
      const auto& slot = io.globalIn[s];
      const std::vector<float>& src = slot.ordinal == 0 ? sig().ch[slot.channel & 1] : sig().ctl;
      gIn_[s].assign(src.begin(), src.begin() + kBlock);
    }
    gOut_.assign(io.globalOut.size(), std::vector<float>(kBlock));
    for (auto& b : gIn_) gInP_.push_back(b.data());
    for (auto& b : gOut_) gOutP_.push_back(b.data());
    params_.assign(io.params.size(), NAN);
    for (size_t i = 0; i < io.params.size(); ++i)
      if (auto it = params.find(p.typed.patch.nodeName(io.params[i].node)); it != params.end())
        params_[i] = static_cast<float>(it->second);
    ctx_.frames = kBlock;
    ctx_.sampleRate = kSr;
    ctx_.in = gInP_.data();
    ctx_.numIn = static_cast<uint32_t>(gInP_.size());
    ctx_.out = gOutP_.data();
    ctx_.numOut = static_cast<uint32_t>(gOutP_.size());
    ctx_.params = params_.data();
    ctx_.numParams = static_cast<uint32_t>(params_.size());
    ctx_.transport.beatsPerFrame = 2.0 / kSr;
  }
  void process() noexcept { img_->process(ctx_); }
  float last() const { return gOut_[0].back(); }

private:
  std::unique_ptr<eng::ProgramImage> img_;
  std::vector<std::vector<float>> gIn_, gOut_;
  std::vector<const float*> gInP_;
  std::vector<float*> gOutP_;
  std::vector<float> params_;
  eng::ProcessContext ctx_;
};

struct Case {
  std::string name;
  std::string text;  // the test patch: `in x : audio stereo`, optional `in c : audio`, first out = the device out
  std::map<std::string, double> params;
  std::function<void()> ref;  // the C++ reference for one block
};

double gWorstRatio = 0.0;
std::vector<float> gOut(2048);

void runCase(const Case& c) {
  if (!gFilter.empty() && c.name.find(gFilter) == std::string::npos) return;
  const st::Program p = st::compile(c.text);
  Rig jit(p, st::Backend::Jit, c.params);
  Rig interp(p, st::Backend::Interp, c.params);
  pq::DenormalGuard dg;
  double bestJ = 1e30, bestR = 1e30, minJ = 1e30, minR = 1e30;
  for (int round = 0; round < 3; ++round) {
    const auto sj = pq::bench::measure([&] { jit.process(); }, kBlock, 9, 2, 1.5e6);
    const auto sr = pq::bench::measure(c.ref, kBlock, 9, 2, 1.5e6);
    bestJ = std::min(bestJ, sj.medianNs);
    bestR = std::min(bestR, sr.medianNs);
    minJ = std::min(minJ, sj.minNs);
    minR = std::min(minR, sr.minNs);
  }
  const auto si = pq::bench::measure([&] { interp.process(); }, kBlock, 9, 2, 1.5e6);
  pq::bench::doNotOptimize(jit.last());
  pq::bench::doNotOptimize(gOut[0]);
  pq::bench::report(("devices-jit/" + c.name).c_str(), {bestJ, minJ}, "ns/stereo-sample");
  pq::bench::report(("devices-c++/" + c.name).c_str(), {bestR, minR}, "ns/stereo-sample");
  pq::bench::report(("devices-interp/" + c.name).c_str(), {si.medianNs, si.minNs}, "ns/stereo-sample");
  const double ratio = bestJ / bestR;
  gWorstRatio = std::max(gWorstRatio, ratio);
  std::printf("      %-34s jit %7.3f  c++ %7.3f  jit/c++ %5.2f   interp %8.3f  (%zu ops, %zu regions)\n", c.name.c_str(), bestJ, bestR,
              ratio, si.medianNs, p.sched.ops.size(), p.sched.regions.size());
}

// Runs the stereo reference `F(x[2]) -> void` over one block (writes gOut interleaved).
template <class F>
inline void stereoBlock(F&& f) {
  const Signals& s = sig();
  for (uint32_t t = 0; t < kBlock; ++t) {
    const float x[2] = {s.ch[0][t], s.ch[1][t]};
    f(x, t);
  }
}

// ---- Saturator, block-wise: how a hand-written oversampled saturator is structured --------------------------------
// Per block: one loop over the frames for the up-sampling (both channels, every allpass of every stage interleaved,
// so the IIR recursions overlap), the curve vectorised over the oversampled block (8 samples per AVX2 op), one loop
// for the down-sampling and the dry/wet mix. States in locals for the loops. Curves 0, 3, 6, 7 (the bench cases).
inline __m256 curve8(int type, __m256 v, const devref::SatParams& p) {
  switch (type) {
    case 0: {
      const __m256 c = _mm256_min_ps(_mm256_max_ps(v, _mm256_set1_ps(-1.5f)), _mm256_set1_ps(1.5f));
      return _mm256_sub_ps(c, _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(0.14814814814814814f), c), c), c));
    }
    case 3: return pq::fastTanh(v);
    case 6: return _mm256_min_ps(_mm256_max_ps(v, _mm256_set1_ps(-1.0f)), _mm256_set1_ps(1.0f));
    default: {
      const __m256 t = _mm256_min_ps(_mm256_max_ps(v, _mm256_set1_ps(-1.0f)), _mm256_set1_ps(1.0f));
      const __m256 wf = _mm256_set1_ps(std::fma(7.5f, p.wsPeriod, 0.5f));
      const __m256 wg = _mm256_set1_ps(std::fma(0.01f * p.wsDamp, p.wsDamp, 1e-30f));
      const __m256 ph = _mm256_mul_ps(wf, t);
      const __m256 r = _mm256_sub_ps(ph, _mm256_round_ps(ph, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
      const __m256 sn = pq::fastSin(_mm256_mul_ps(r, _mm256_set1_ps(6.283185307179586f)));
      const __m256 cub = _mm256_sub_ps(_mm256_mul_ps(_mm256_set1_ps(1.5f), t),
                                       _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(0.5f), t), t), t));
      __m256 shape = _mm256_fmadd_ps(_mm256_set1_ps(p.wsLin), v, _mm256_mul_ps(_mm256_set1_ps(p.wsCurve), cub));
      shape = _mm256_fmadd_ps(_mm256_set1_ps(p.wsDepth), sn, shape);
      const __m256 v2 = _mm256_mul_ps(v, v);
      const __m256 gate = _mm256_div_ps(_mm256_add_ps(v2, _mm256_set1_ps(1e-30f)), _mm256_add_ps(v2, wg));
      return _mm256_fmadd_ps(_mm256_set1_ps(p.wsDrive), _mm256_sub_ps(_mm256_mul_ps(shape, gate), v), v);
    }
  }
}

struct SatBlock {
  int type = 3, hq = 1;
  devref::HbUp<3, 3> up1[2];
  devref::HbDown<3, 3> dn1[2];
  devref::HbUp<2, 1> up2[2];
  devref::HbDown<2, 1> dn2[2];
  devref::HbUp<1, 1> up3[2];
  devref::HbDown<1, 1> dn3[2];
  alignas(32) float buf[2][8 * 1024 + 8];

  void run(const devref::SatParams& p) {
    using namespace devref;
    const Signals& s = sig();
    const uint32_t n = kBlock, f = static_cast<uint32_t>(hq);
    auto u1 = up1[0], u1r = up1[1];
    auto u2 = up2[0], u2r = up2[1];
    auto u3 = up3[0], u3r = up3[1];
    for (uint32_t t = 0; t < n; ++t) {
      for (int ch = 0; ch < 2; ++ch) {
        auto& a = ch ? u1r : u1;
        auto& b = ch ? u2r : u2;
        auto& c = ch ? u3r : u3;
        float* o = buf[ch] + static_cast<size_t>(t) * f;
        const float u = s.ch[ch][t] * p.drive;
        if (f == 1) {
          o[0] = u;
          continue;
        }
        float s0, s1;
        a.tick(u, kHb1P0, kHb1P1, s0, s1);
        if (f == 2) {
          o[0] = s0;
          o[1] = s1;
          continue;
        }
        float q[4];
        b.tick(s0, kHb2P0, kHb2P1, q[0], q[1]);
        b.tick(s1, kHb2P0, kHb2P1, q[2], q[3]);
        if (f == 4) {
          for (int i = 0; i < 4; ++i) o[i] = q[i];
          continue;
        }
        for (int i = 0; i < 4; ++i) c.tick(q[i], kHb3P0, kHb3P1, o[2 * i], o[2 * i + 1]);
      }
    }
    up1[0] = u1, up1[1] = u1r, up2[0] = u2, up2[1] = u2r, up3[0] = u3, up3[1] = u3r;
    const __m256 og = _mm256_set1_ps(p.output);
    for (int ch = 0; ch < 2; ++ch)
      for (uint32_t i = 0; i < n * f; i += 8)
        _mm256_store_ps(buf[ch] + i, _mm256_mul_ps(curve8(type, _mm256_load_ps(buf[ch] + i), p), og));
    auto d1 = dn1[0], d1r = dn1[1];
    auto d2 = dn2[0], d2r = dn2[1];
    auto d3 = dn3[0], d3r = dn3[1];
    for (uint32_t t = 0; t < n; ++t) {
      for (int ch = 0; ch < 2; ++ch) {
        auto& a = ch ? d1r : d1;
        auto& b = ch ? d2r : d2;
        auto& c = ch ? d3r : d3;
        const float* o = buf[ch] + static_cast<size_t>(t) * f;
        float w;
        if (f == 1) {
          w = o[0];
        } else if (f == 2) {
          w = a.tick(o[0], o[1], kHb1P0, kHb1P1);
        } else {
          float r[4];
          if (f == 4) {
            for (int i = 0; i < 4; ++i) r[i] = o[i];
          } else {
            for (int i = 0; i < 4; ++i) r[i] = c.tick(o[2 * i], o[2 * i + 1], kHb3P0, kHb3P1);
          }
          const float e0 = b.tick(r[0], r[1], kHb2P0, kHb2P1), e1 = b.tick(r[2], r[3], kHb2P0, kHb2P1);
          w = a.tick(e0, e1, kHb1P0, kHb1P1);
        }
        const float x = s.ch[ch][t];
        gOut[2 * t + ch] = std::fma(p.dryWet, w - x, x);
      }
    }
    dn1[0] = d1, dn1[1] = d1r, dn2[0] = d2, dn2[1] = d2r, dn3[0] = d3, dn3[1] = d3r;
  }
};

// ---- EQ Eight, block-wise: how a hand-written SVF EQ is structured ------------------------------------------------
// Coefficients once per block (or, for an audio-rate cutoff, vectorised over the block first); then one loop per
// filter section over the block with its two states in registers, both channels in the same loop when they share
// the coefficients (stereo mode). HQ: the half-band up loop, the sections over the 2× block, the down loop.
struct EqBlock {
  struct Sec {
    devref::FastSvfCore::Coef c;
    bool m0One;
    float m0, m1, m2;
    int band;
  };
  int mode = 0, hq = 1;
  std::vector<Sec> secs[2];
  float st[2][2][32][2] = {};  // [chain][channel][section] (ic1, ic2)
  devref::HbUp<3, 3> up[2];
  devref::HbDown<3, 3> dn[2];
  int audioBand = -1;          // the band whose freq comes from the control signal
  devref::EqBand audio{};
  alignas(32) float buf[2][2048 + 8];
  alignas(32) float cv[3 * 1024];  // per-sample d1, b1, b2 of the audio-rate band

  void prepare(const devref::EqConf& conf, const devref::EqParams& p) {
    for (int chain = 0; chain < 2; ++chain) {
      const int set = mode == 0 ? 0 : chain;
      secs[chain].clear();
      for (int b = 0; b < 8; ++b) {
        if (!conf.on[set][b]) continue;
        const int type = conf.type[set][b];
        const devref::EqCoef c = devref::eqCoef(type, static_cast<float>(hq), p.band[set][b], p.adaptive, p.scale, kSr);
        const bool one = type != devref::kHS && type != devref::kHC12 && type != devref::kHC48;
        secs[chain].push_back({devref::FastSvfCore::coef(c.g, c.k), one, c.m0, c.m1, c.m2, b});
        if (type == devref::kLC48 || type == devref::kHC48) {
          const bool hp = type == devref::kLC48;
          const float ks[3] = {c.k2, c.k3, c.k4};
          for (float k : ks) secs[chain].push_back({devref::FastSvfCore::coef(c.g, k), hp, hp ? 1.0f : 0.0f, hp ? -k : 0.0f, hp ? -1.0f : 1.0f, b});
        }
      }
    }
  }
  // One section over n samples of up to two channels (in place).
  template <int kCh>
  static void runSec(float* const (&x)[2], uint32_t n, const Sec& s, float (&state)[2][32][2], int si, const float* d1v = nullptr) {
    float ic1[kCh], ic2[kCh];
    for (int c = 0; c < kCh; ++c) ic1[c] = state[c][si][0], ic2[c] = state[c][si][1];
    for (uint32_t t = 0; t < n; ++t) {
      const float d1 = d1v ? d1v[t] : s.c.d1, b1 = d1v ? d1v[1024 + t] : s.c.b1, b2 = d1v ? d1v[2048 + t] : s.c.b2;
      for (int c = 0; c < kCh; ++c) {
        const float v = x[c][t] - ic2[c];
        const float n1 = std::fma(b1, v, std::fma(d1, ic1[c], ic1[c]));
        const float n2 = std::fma(b2, v, std::fma(b1, ic1[c], ic2[c]));
        const float band = 0.5f * (ic1[c] + n1), low = 0.5f * (ic2[c] + n2);
        const float w = s.m0One ? std::fma(s.m1, band, x[c][t]) : std::fma(s.m0, x[c][t], s.m1 * band);
        x[c][t] = std::fma(s.m2, low, w);
        ic1[c] = n1;
        ic2[c] = n2;
      }
    }
    for (int c = 0; c < kCh; ++c) state[c][si][0] = ic1[c], state[c][si][1] = ic2[c];
  }
  void run(const devref::EqParams& p) {
    const Signals& sg = sig();
    const uint32_t n = kBlock * static_cast<uint32_t>(hq);
    for (int ch = 0; ch < 2; ++ch) {
      auto u = up[ch];
      for (uint32_t t = 0; t < kBlock; ++t) {
        const float L = sg.ch[0][t], R = sg.ch[1][t];
        const float x = mode == 2 ? (ch ? 0.5f * (L - R) : 0.5f * (L + R)) : (ch ? R : L);
        if (hq == 1) buf[ch][t] = x;
        else u.tick(x, devref::kHb1P0, devref::kHb1P1, buf[ch][2 * t], buf[ch][2 * t + 1]);
      }
      up[ch] = u;
    }
    if (audioBand >= 0) {  // per-sample coefficients of the modulated band (a bell): vectorised over the block
      const devref::EqCoef c0 = devref::eqCoef(devref::kBell, 1.0f, audio, p.adaptive, p.scale, kSr);
      const __m256 k = _mm256_set1_ps(c0.k), one = _mm256_set1_ps(1.0f);
      for (uint32_t t = 0; t < kBlock; t += 8) {
        const __m256 f = _mm256_min_ps(_mm256_max_ps(_mm256_loadu_ps(sg.ctl.data() + t), _mm256_setzero_ps()), _mm256_set1_ps(0.49f * kSr));
        const __m256 g = _mm256_tan_ps(_mm256_div_ps(_mm256_mul_ps(_mm256_set1_ps(3.141592653589793f), f), _mm256_set1_ps(kSr)));
        const __m256 gk = _mm256_add_ps(g, k);
        const __m256 a1 = _mm256_div_ps(one, _mm256_fmadd_ps(g, gk, one));
        _mm256_store_ps(cv + t, _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(-2.0f), g), gk), a1));
        const __m256 b1 = _mm256_mul_ps(_mm256_mul_ps(_mm256_set1_ps(2.0f), g), a1);
        _mm256_store_ps(cv + 1024 + t, b1);
        _mm256_store_ps(cv + 2048 + t, _mm256_mul_ps(b1, g));
      }
    }
    float* const xs[2] = {buf[0], buf[1]};
    if (mode == 0) {
      for (size_t i = 0; i < secs[0].size(); ++i)
        runSec<2>(xs, n, secs[0][i], st[0], static_cast<int>(i), secs[0][i].band == audioBand ? cv : nullptr);
    } else {
      for (int chain = 0; chain < 2; ++chain) {
        float* const one[2] = {buf[chain], buf[chain]};
        for (size_t i = 0; i < secs[chain].size(); ++i) runSec<1>(one, n, secs[chain][i], st[chain], static_cast<int>(i));
      }
    }
    for (int ch = 0; ch < 2; ++ch) {
      auto d = dn[ch];
      for (uint32_t t = 0; t < kBlock; ++t) {
        const float y = hq == 1 ? buf[ch][t] : d.tick(buf[ch][2 * t], buf[ch][2 * t + 1], devref::kHb1P0, devref::kHb1P1);
        buf[ch][t] = y;
      }
      dn[ch] = d;
    }
    for (uint32_t t = 0; t < kBlock; ++t) {
      const float a = buf[0][t], b = buf[1][t];
      gOut[2 * t] = (mode == 2 ? a + b : a) * p.outGain;
      gOut[2 * t + 1] = (mode == 2 ? a - b : b) * p.outGain;
    }
  }
};

// ---- Auto Filter -----------------------------------------------------------------------------------------------------
devref::AfParams autoFilterParams() {
  devref::AfParams p;
  p.freq = 900.0f;
  p.res = 0.6f;
  p.drive = 2.0f;
  p.morph = 0.4f;
  p.lfoAmount = 0.5f;
  p.lfoRate = 3.0f;
  p.envAmount = 0.4f;
  return p;
}

template <class Conf>
void addAutoFilter(std::vector<Case>& cases, const std::string& name, int type, int circuit, int slope, bool lfo, bool env) {
  static devref::AutoFilter<Conf> ref;
  static const devref::AfParams p = autoFilterParams();
  const std::string text = "import \"devices/auto_filter.pqp\"\npatch T { in x : audio stereo\n  out y : audio stereo\n  a = auto_filter(x, type: " +
                           std::to_string(type) + ", circuit: " + std::to_string(circuit) + ", slope: " + std::to_string(slope) +
                           ", lfo: " + (lfo ? "1" : "0") + ", env: " + (env ? "1" : "0") + ")\n  y = a }";
  const std::map<std::string, double> params = {{"a__freq", p.freq},           {"a__res", p.res},         {"a__drive", p.drive},
                                                {"a__morph", p.morph},         {"a__lfoAmount", p.lfoAmount}, {"a__lfoRate", p.lfoRate},
                                                {"a__envAmount", p.envAmount}};
  cases.push_back({name, text, params, [] {
                     ref.prepare(p);
                     ref.processBlock(sig().ch[0].data(), sig().ch[1].data(), kBlock, gOut.data());
                   }});
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--block" && i + 1 < argc) kBlock = std::min<uint32_t>(1024, static_cast<uint32_t>(std::atoi(argv[++i])));
    else gFilter = a;
  }
  pq::bench::SystemLoad load;
  std::printf("T4.9-T4.11 stock devices: JIT vs hand-written C++ (device_reference.h), stereo, block %u, 48 kHz; JIT target %s\n",
              kBlock, st::jitCompiler().hostCpu().c_str());
  std::vector<Case> cases;

  // ---- Utility -------------------------------------------------------------------------------------------------
  {
    static devref::Utility u;
    static devref::UtilityParams up;
    up.width = 1.4f;
    up.balance = 0.2f;
    up.gain = 0.8f;
    cases.push_back({"utility", R"(import "devices/utility.pqp"
patch T { in x : audio stereo
  out y : audio stereo
  u = utility(x)
  y = u })", {{"u__width", 1.4}, {"u__balance", 0.2}, {"u__gain", 0.8}},
                     [] {
                       u.prepare(up);
                       stereoBlock([&](const float (&x)[2], uint32_t t) {
                         u.process(x[0], x[1]);
                         gOut[2 * t] = u.out[0];
                         gOut[2 * t + 1] = u.out[1];
                       });
                     }});
    static devref::Utility b;
    b.conf.bassMono = true;
    b.wantBass = false;
    cases.push_back({"utility bass mono", R"(import "devices/utility.pqp"
patch T { in x : audio stereo
  out y : audio stereo
  u = utility(x, bassMono: 1)
  y = u })", {{"u__width", 1.4}, {"u__balance", 0.2}, {"u__gain", 0.8}},
                     [] {
                       b.prepare(up);
                       stereoBlock([&](const float (&x)[2], uint32_t t) {
                         b.process(x[0], x[1]);
                         gOut[2 * t] = b.out[0];
                         gOut[2 * t + 1] = b.out[1];
                       });
                     }});
  }

  // ---- Saturator -------------------------------------------------------------------------------------------------
  {
    struct SatCase {
      int type, hq;
    };
    static devref::SatParams sp;
    sp.drive = 3.981f;
    sp.output = 0.7f;
    const SatCase list[] = {{3, 1}, {3, 2}, {3, 4}, {3, 8}, {0, 2}, {6, 4}, {7, 1}, {7, 2}};
    static std::vector<SatBlock> blocks(std::size(list));
    for (size_t i = 0; i < std::size(list); ++i) {
      blocks[i].type = list[i].type;
      blocks[i].hq = list[i].hq;
      const std::string text = "import \"devices/saturator.pqp\"\npatch T { in x : audio stereo\n  out y : audio stereo\n  s = saturator(x, type: " +
                               std::to_string(list[i].type) + ", hq: " + std::to_string(list[i].hq) + ")\n  y = s }";
      cases.push_back({"saturator type " + std::to_string(list[i].type) + " hq " + std::to_string(list[i].hq), text,
                       {{"s__drive", 3.981}, {"s__output", 0.7}}, [i] { blocks[i].run(sp); }});
    }
  }

  // ---- EQ Eight --------------------------------------------------------------------------------------------------
  {
    struct EqCase {
      std::string name;
      int mode, hq;
      int types[8];
      bool on[8];
      bool audioFreq;  // band 4's freq from the control input (per-sample coefficients)
    };
    const EqCase list[] = {
        {"eq_eight defaults (6 bands)", 0, 1, {0, 2, 3, 3, 3, 3, 5, 7}, {false, true, true, true, true, true, true, false}, false},
        {"eq_eight 8 bells", 0, 1, {3, 3, 3, 3, 3, 3, 3, 3}, {true, true, true, true, true, true, true, true}, false},
        {"eq_eight 1 bell", 0, 1, {3, 3, 3, 3, 3, 3, 3, 3}, {false, false, false, true, false, false, false, false}, false},
        {"eq_eight LC48 + HC48", 0, 1, {0, 3, 3, 3, 3, 3, 3, 7}, {true, false, false, false, false, false, false, true}, false},
        {"eq_eight 8 bands L/R", 1, 1, {0, 2, 3, 3, 3, 3, 5, 7}, {true, true, true, true, true, true, true, true}, false},
        // T1b.x-perf (Q122): the same eight bands in stereo and in M/S mode, so L/R and M/S compare with stereo.
        {"eq_eight 8 bands stereo", 0, 1, {0, 2, 3, 3, 3, 3, 5, 7}, {true, true, true, true, true, true, true, true}, false},
        {"eq_eight 8 bands M/S", 2, 1, {0, 2, 3, 3, 3, 3, 5, 7}, {true, true, true, true, true, true, true, true}, false},
        {"eq_eight 8 bells HQ", 0, 2, {3, 3, 3, 3, 3, 3, 3, 3}, {true, true, true, true, true, true, true, true}, false},
        {"eq_eight 1 bell audio-rate freq", 0, 1, {3, 3, 3, 3, 3, 3, 3, 3}, {false, false, false, true, false, false, false, false}, true},
    };
    static std::vector<EqBlock> eqs(std::size(list));
    static std::vector<devref::EqParams> eqp(std::size(list));
    static std::vector<devref::EqConf> eqc(std::size(list));
    for (size_t i = 0; i < std::size(list); ++i) {
      const EqCase& e = list[i];
      devref::EqConf& r = eqc[i];
      eqs[i].mode = r.mode = e.mode;
      eqs[i].hq = r.hq = e.hq;
      std::string args = "mode: " + std::to_string(e.mode) + ", hq: " + std::to_string(e.hq);
      std::map<std::string, double> params;
      const float f[8] = {60, 150, 400, 900, 2000, 4500, 9000, 15000};
      for (int s = 0; s < 2; ++s)
        for (int b = 0; b < 8; ++b) {
          r.type[s][b] = e.types[b];
          r.on[s][b] = e.on[b];
          const std::string n = (s ? "B" : "") + std::to_string(b + 1);
          args += ", type" + n + ": " + std::to_string(e.types[b]) + ", on" + n + ": " + (e.on[b] ? "1" : "0");
          eqp[i].band[s][b] = {f[b] * (s ? 1.1f : 1.0f), b % 2 ? 2.0f : 0.5f, 1.2f};
          if (!(e.audioFreq && s == 0 && b == 3)) params["e__freq" + n] = eqp[i].band[s][b].freq;
          params["e__gain" + n] = eqp[i].band[s][b].gain;
          params["e__q" + n] = eqp[i].band[s][b].q;
        }
      if (e.audioFreq) args += ", freq4: c";
      const std::string text = "import \"devices/eq_eight.pqp\"\npatch T { in x : audio stereo\n  in c : audio\n  out y : audio stereo\n  e = eq_eight(x, " +
                               args + ")\n  y = e }";
      if (e.audioFreq) {
        eqs[i].audioBand = 3;
        eqs[i].audio = eqp[i].band[0][3];
      }
      cases.push_back({e.name, text, params, [i] {
                         eqs[i].prepare(eqc[i], eqp[i]);
                         eqs[i].run(eqp[i]);
                       }});
    }
  }

  // ---- Auto Filter (T4.11) ---------------------------------------------------------------------------------------
  // The C++ side is devref::AutoFilter specialised at compile time (AfFixed: every branch on the configuration folds
  // away): param-rate values once per block, then one loop over the frames with both channels interleaved.
  addAutoFilter<devref::AfFixed<0, 0, 0, false, false>>(cases, "auto_filter LP12 Clean", 0, 0, 0, false, false);
  addAutoFilter<devref::AfFixed<0, 0, 1, true, false>>(cases, "auto_filter LP24 Clean + LFO", 0, 0, 1, true, false);
  addAutoFilter<devref::AfFixed<4, 0, 0, true, true>>(cases, "auto_filter Morph12 Clean + LFO + env", 4, 0, 0, true, true);
  addAutoFilter<devref::AfFixed<0, 1, 0, false, false>>(cases, "auto_filter LP12 OSR", 0, 1, 0, false, false);
  addAutoFilter<devref::AfFixed<0, 1, 1, true, false>>(cases, "auto_filter LP24 OSR + LFO", 0, 1, 1, true, false);
  addAutoFilter<devref::AfFixed<0, 2, 0, false, false>>(cases, "auto_filter LP12 MS2", 0, 2, 0, false, false);
  addAutoFilter<devref::AfFixed<2, 3, 0, true, false>>(cases, "auto_filter BP12 SMP + LFO", 2, 3, 0, true, false);
  addAutoFilter<devref::AfFixed<0, 4, 1, false, false>>(cases, "auto_filter LP24 PRD", 0, 4, 1, false, false);
  addAutoFilter<devref::AfFixed<0, 4, 1, true, true>>(cases, "auto_filter LP24 PRD + LFO + env", 0, 4, 1, true, true);
  addAutoFilter<devref::AfFixed<6, 0, 0, true, false>>(cases, "auto_filter Comb + LFO", 6, 0, 0, true, false);
  addAutoFilter<devref::AfFixed<9, 0, 0, true, false>>(cases, "auto_filter Vowel + LFO", 9, 0, 0, true, false);

  for (const Case& c : cases) runCase(c);
  std::printf("worst jit/c++ ratio %.2f; system load during the run %.0f %%\n", gWorstRatio, load.busyPercent());
  return 0;
}
