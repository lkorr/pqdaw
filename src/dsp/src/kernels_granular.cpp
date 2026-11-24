// granular (SM.6; semantics in pq/dsp/granular.h and pq/dsp/atoms.h "Tables"): the Sampler's granulator per lane
// over a live SampleSlot, 8 grains per AVX2 op.

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>

#include "kernels_common.h"
#include "pq/core/grain_tap.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/granular.h"
#include "pq/dsp/resample.h"
#include "pq/dsp/sample_slot.h"

namespace pq::dsp {

namespace {

struct ShapeTables {
  float t[5][kGrainShapePoints + 1];
  ShapeTables() noexcept {
    constexpr double N = kGrainShapePoints;
    const double gEdge = std::exp(-0.5 * (0.5 / 0.15) * (0.5 / 0.15));
    const double eTail = std::exp(-5.0);
    for (uint32_t i = 0; i <= kGrainShapePoints; ++i) {
      const double x = static_cast<double>(i) / N;
      t[0][i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * x));  // Hann
      // Tukey (α = 0.5: cosine tapers over the first and last quarter)
      const double a = 0.5;
      double tk = 1.0;
      if (x < a / 2) tk = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * x / a);
      else if (x > 1.0 - a / 2) tk = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * (1.0 - x) / a);
      t[1][i] = static_cast<float>(tk);
      const double g = std::exp(-0.5 * ((x - 0.5) / 0.15) * ((x - 0.5) / 0.15));  // Gauss (σ = 0.15), 0 at the ends
      t[2][i] = static_cast<float>(std::max(0.0, (g - gEdge) / (1.0 - gEdge)));
      t[3][i] = static_cast<float>(1.0 - std::abs(2.0 * x - 1.0));  // Triangle
      // Exp: a 5 % linear attack, then e^(−5 u) down to 0 at the end
      const double e = x < 0.05 ? x / 0.05 : (std::exp(-5.0 * (x - 0.05) / 0.95) - eTail) / (1.0 - eTail);
      t[4][i] = static_cast<float>(std::clamp(e, 0.0, 1.0));
    }
    for (auto& s : t) s[0] = s[kGrainShapePoints] = 0.0f;
  }
};
const ShapeTables kShapes;

}  // namespace

const float* grainShapeTable(GrainShape s) noexcept {  // RT-safe
  return kShapes.t[std::min<uint32_t>(static_cast<uint32_t>(s), 4u)];
}

namespace detail {
namespace {

using Entry = SampleSlot::Entry;

constexpr uint32_t kVoiceWords = (static_cast<uint32_t>(sizeof(GranVoice)) + 3u) / 4u;
static_assert(kVoiceWords <= kGranVoiceWords, "GranVoice outgrew GranularState");
constexpr uint32_t kActiveWord = static_cast<uint32_t>(offsetof(GranVoice, active)) / 4u;  // SM.14: the lane's stop
constexpr uint32_t kDefaultHarmonics[] = {1, 1, 2, 1, 3, 2, 3, 1, 4, 1, 5, 1};  // (n, d) pairs

void loadVoice(const GranularState& st, uint32_t l, GranVoice& v) noexcept {  // RT-safe
  uint32_t buf[kVoiceWords];
  for (uint32_t w = 0; w < kVoiceWords; ++w) buf[w] = st.v[w][l];
  std::memcpy(&v, buf, sizeof(GranVoice));
}
void storeVoice(GranularState& st, uint32_t l, const GranVoice& v) noexcept {  // RT-safe
  uint32_t buf[kVoiceWords] = {};
  std::memcpy(buf, &v, sizeof(GranVoice));
  for (uint32_t w = 0; w < kVoiceWords; ++w) st.v[w][l] = buf[w];
}

// The lane's param-rate inputs.
struct GranParams {
  double root = 261.63, ratio = 1.0;
  double startF = 0.0, endF = 0.0;  // frames
  double position = 0.0, scan = 1.0, size = 0.08, density = 30.0;
  double sprayTime = 0.0, sprayPitch = 0.0, sprayPan = 0.0;
  bool freeze = false, harmonic = false;
  GranularMode mode = GranularMode::Loop;
  GrainShape shape = GrainShape::Hann;
  uint32_t grains = 16;
  double sr = 48000.0;
};

double finiteOr(float x, double d) noexcept { return std::isfinite(x) ? static_cast<double>(x) : d; }

// P wrapped into [start, end) (32.32); an empty region holds start.
int64_t wrapPos(int64_t x, int64_t s, int64_t e) noexcept {
  const int64_t L = e - s;
  if (L <= 0) return s;
  int64_t d = (x - s) % L;
  if (d < 0) d += L;
  return s + d;
}

float hsum(__m256 v) noexcept {
  const __m128 a = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  const __m128 b = _mm_add_ps(a, _mm_movehl_ps(a, a));
  return _mm_cvtss_f32(_mm_add_ss(b, _mm_shuffle_ps(b, b, 1)));
}

// One batch: the grains of slots [q0, q0 + 8) of one lane whose bit is set in `include` (all of one mip level) over m
// frames, adding their sum into l / r (m floats each). Each slot keeps its vector position whatever else is live, so a
// frame's sum (the horizontal add's pairing) does not depend on which grains a run holds: renders are bit-identical
// whatever the call length.
void renderBatch(GranularState& st, uint32_t lane, uint32_t q0, uint32_t include, const SampleLevelView& lv, bool stereo,
                 const float* shape, uint32_t m, float* l, float* r) noexcept {  // RT-safe
  alignas(32) int32_t ip[8] = {}, si[8] = {}, age[8] = {}, len[8] = {};
  alignas(32) uint32_t fp[8] = {}, sf[8] = {};
  alignas(32) float wi[8] = {}, gl[8] = {}, gr[8] = {};
  for (uint32_t j = 0; j < 8; ++j) {
    if (((include >> j) & 1u) == 0) continue;  // (len 0: masked)
    const uint32_t q = q0 + j;
    ip[j] = st.gi[q][lane];
    fp[j] = st.gf[q][lane];
    si[j] = st.si[q][lane];
    sf[j] = st.sf[q][lane];
    age[j] = st.age[q][lane];
    len[j] = st.len[q][lane];
    wi[j] = st.winc[q][lane];
    gl[j] = st.gl[q][lane];
    gr[j] = st.gr[q][lane];
  }
  __m256i vip = _mm256_load_si256(reinterpret_cast<const __m256i*>(ip));
  __m256i vfp = _mm256_load_si256(reinterpret_cast<const __m256i*>(fp));
  const __m256i vsi = _mm256_load_si256(reinterpret_cast<const __m256i*>(si));
  const __m256i vsf = _mm256_load_si256(reinterpret_cast<const __m256i*>(sf));
  __m256i vage = _mm256_load_si256(reinterpret_cast<const __m256i*>(age));
  const __m256i vlen = _mm256_load_si256(reinterpret_cast<const __m256i*>(len));
  const __m256 vwi = _mm256_load_ps(wi), vgl = _mm256_load_ps(gl), vgr = _mm256_load_ps(gr);
  const __m256i one = _mm256_set1_epi32(1), sign = _mm256_set1_epi32(static_cast<int>(0x80000000u));
  const __m256i lo = _mm256_set1_epi32(-kSampleSlotGuard + 1), hi = _mm256_set1_epi32(lv.length + kSampleSlotGuard - 3);
  const __m256i tabMax = _mm256_set1_epi32(static_cast<int>(kGrainShapePoints) - 1);
  const __m256 k24 = _mm256_set1_ps(0x1p-24f);
  const float* L = lv.ch[0];
  const float* R = lv.ch[1];
  const __m256 zero = _mm256_setzero_ps();
  auto cubic = [&](const float* base, __m256i idx, __m256 fr, __m256 act) noexcept {
    const __m256 xm1 = _mm256_mask_i32gather_ps(zero, base - 1, idx, act, 4);
    const __m256 x0 = _mm256_mask_i32gather_ps(zero, base, idx, act, 4);
    const __m256 x1 = _mm256_mask_i32gather_ps(zero, base + 1, idx, act, 4);
    const __m256 x2 = _mm256_mask_i32gather_ps(zero, base + 2, idx, act, 4);
    return simd::cubic4<simd::S8>(xm1, x0, x1, x2, fr);  // simd.h: sample_read's cubic
  };
  for (uint32_t f = 0; f < m; ++f) {
    const __m256i actI = _mm256_andnot_si256(_mm256_cmpgt_epi32(_mm256_setzero_si256(), vage),
                                             _mm256_cmpgt_epi32(vlen, vage));  // 0 ≤ age < len
    const __m256 act = _mm256_castsi256_ps(actI);
    if (_mm256_movemask_ps(act) != 0) {
      const __m256 fr = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_srli_epi32(vfp, 8)), k24);
      const __m256i idx = _mm256_min_epi32(_mm256_max_epi32(vip, lo), hi);
      // The window: table[age · winc] (linear between points).
      const __m256 wx = _mm256_mul_ps(_mm256_cvtepi32_ps(vage), vwi);
      const __m256i wi0 = _mm256_min_epi32(_mm256_max_epi32(_mm256_cvttps_epi32(wx), _mm256_setzero_si256()), tabMax);
      const __m256 wf = _mm256_sub_ps(wx, _mm256_cvtepi32_ps(wi0));
      const __m256 w0 = _mm256_mask_i32gather_ps(zero, shape, wi0, act, 4);
      const __m256 w1 = _mm256_mask_i32gather_ps(zero, shape + 1, wi0, act, 4);
      const __m256 w = _mm256_and_ps(_mm256_fmadd_ps(wf, _mm256_sub_ps(w1, w0), w0), act);
      const __m256 yl = cubic(L, idx, fr, act);
      const __m256 yr = stereo ? cubic(R, idx, fr, act) : yl;
      l[f] += hsum(_mm256_mul_ps(_mm256_mul_ps(yl, w), vgl));
      r[f] += hsum(_mm256_mul_ps(_mm256_mul_ps(yr, w), vgr));
    }
    // Advance the started grains (age ≥ 0): frac += sf, carry into the integer part.
    const __m256i started = _mm256_cmpgt_epi32(vage, _mm256_set1_epi32(-1));
    const __m256i nfp = _mm256_add_epi32(vfp, vsf);
    const __m256i carry = _mm256_cmpgt_epi32(_mm256_xor_si256(vsf, sign), _mm256_xor_si256(nfp, sign));  // nfp < sf
    const __m256i nip = _mm256_sub_epi32(_mm256_add_epi32(vip, vsi), carry);
    vfp = _mm256_blendv_epi8(vfp, nfp, started);
    vip = _mm256_blendv_epi8(vip, nip, started);
    vage = _mm256_add_epi32(vage, one);
  }
  _mm256_store_si256(reinterpret_cast<__m256i*>(ip), vip);
  _mm256_store_si256(reinterpret_cast<__m256i*>(fp), vfp);
  _mm256_store_si256(reinterpret_cast<__m256i*>(age), vage);
  for (uint32_t j = 0; j < 8; ++j) {
    if (((include >> j) & 1u) == 0) continue;
    const uint32_t q = q0 + j;
    st.gi[q][lane] = ip[j];
    st.gf[q][lane] = fp[j];
    // A long-dead grain's age is held (no overflow over hours).
    st.age[q][lane] = age[j] > len[j] + (1 << 20) ? len[j] + (1 << 20) : age[j];
  }
}

struct GranularK {
  static constexpr uint32_t kSpec = 2;  // hz, reset
  template <class S, uint32_t SM>
  static void run(const KernelArgs& a) noexcept {  // RT-safe
    constexpr uint32_t W = S::kW;
    auto& st = *static_cast<GranularState*>(a.state);
    const auto& slot = *static_cast<const SampleSlot*>(a.resource);
    const Entry* newest = slot.acquire();
    const uint32_t frames = a.frames;
    auto hzAt = [&](uint32_t i, uint32_t lane) noexcept { return splBit(SM, 0) ? a.in[0][lane] : a.in[0][i * W + lane]; };
    auto resetAt = [&](uint32_t i, uint32_t lane) noexcept {
      return maskIsTrue(splBit(SM, 1) ? a.in[1][lane] : a.in[1][i * W + lane]);
    };
    GrainTap* tap = slot.grainTap();
    GrainTapRecord* rec = tap != nullptr ? tap->begin(st.group) : nullptr;
    uint32_t recCount = 0;
    alignas(32) float l[kGrainGrid], r[kGrainGrid];
    const uint32_t freeLanes = W == 8 ? ~a.laneMask & 0xFFu : 0u;  // SM.14 (kernel.h "Occupied lanes")
    for (uint32_t lane = 0; lane < W; ++lane) {
      if (((freeLanes >> lane) & 1u) != 0) {  // SM.14: a free lane: the voice and its grains stop
        st.v[kActiveWord][lane] = 0;
        for (uint32_t q = 0; q < kGrainsMax; ++q) st.len[q][lane] = 0;
        for (uint32_t i = 0; i < frames; ++i)
          a.out[0][i * W + lane] = a.out[1][i * W + lane] = a.out[2][i * W + lane] = a.out[3][i * W + lane] = 0.0f;
        continue;
      }
      GranVoice v;
      loadVoice(st, lane, v);
      const Entry* e = newest;
      if (v.active != 0 && v.gen != static_cast<uint32_t>(newest->generation)) {
        e = SampleSlot::find(newest, v.gen);
        if (e == nullptr) {  // its entry was retired: the voice and its grains stop until the next trigger
          v.active = 0;
          for (uint32_t q = 0; q < kGrainsMax; ++q) st.len[q][lane] = 0;
          e = newest;
        } else {
          e->touched.store(1, std::memory_order_relaxed);
        }
      }
      GranParams p;
      auto setParams = [&] {
        const double len0 = static_cast<double>(e->level[0].length);
        auto frac01 = [&](uint32_t k) noexcept { return std::clamp(finiteOr(a.in[k][lane], 0.0), 0.0, 1.0); };
        p.root = std::max(finiteOr(a.in[2][lane], 261.63), 1e-3);
        p.ratio = std::max(finiteOr(a.in[3][lane], 1.0), 0.0);
        p.startF = frac01(4) * len0;
        p.endF = frac01(5) * len0;
        p.position = frac01(6);
        p.scan = std::clamp(finiteOr(a.in[7][lane], 0.0), -64.0, 64.0);
        p.size = std::clamp(finiteOr(a.in[8][lane], 0.08), 0.001, 4.0);
        p.density = std::clamp(finiteOr(a.in[9][lane], 30.0), 0.01, 4000.0);
        p.sprayTime = std::clamp(finiteOr(a.in[10][lane], 0.0), 0.0, 10.0);
        p.sprayPitch = std::clamp(finiteOr(a.in[11][lane], 0.0), 0.0, 48.0);
        p.sprayPan = std::clamp(finiteOr(a.in[12][lane], 0.0), 0.0, 1.0);
        p.freeze = finiteOr(a.in[13][lane], 0.0) >= 0.5;
        p.shape = static_cast<GrainShape>(std::clamp<int>(static_cast<int>(std::lround(finiteOr(a.in[14][lane], 0.0))), 0, 4));
        p.mode = static_cast<GranularMode>(std::clamp<int>(static_cast<int>(std::lround(finiteOr(a.in[15][lane], 1.0))), 0, 2));
        p.grains = static_cast<uint32_t>(std::clamp<long>(std::lround(finiteOr(a.in[16][lane], 16.0)), 1, kGrainsMax));
        p.harmonic = finiteOr(a.in[17][lane], 0.0) >= 0.5;
        p.sr = std::clamp(finiteOr(a.in[18][lane], 48000.0), 1000.0, 768000.0);
      };
      setParams();
      const uint32_t voiceSlot = st.group * 8u + lane;
      // Live grains (silence: no voice, no grain → zeros at the cost of this scan).
      bool live = false;
      for (uint32_t q = 0; q < kGrainsMax && !live; ++q) live = st.age[q][lane] < st.len[q][lane];
      if (v.active == 0 && !live) {
        bool any = false;
        for (uint32_t i = 0; i < frames && !any; ++i) any = resetAt(i, lane);
        if (!any) {
          for (uint32_t i = 0; i < frames; ++i)
            a.out[0][i * W + lane] = a.out[1][i * W + lane] = a.out[2][i * W + lane] = a.out[3][i * W + lane] = 0.0f;
          continue;
        }
      }
      const float* shape = grainShapeTable(p.shape);
      uint32_t f = 0;
      while (f < frames) {
        if (resetAt(f, lane)) {  // a trigger at this sample, on the newest entry
          const bool other = static_cast<uint32_t>(newest->generation) != v.gen;
          e = newest;
          setParams();
          if (other)
            for (uint32_t q = 0; q < kGrainsMax; ++q) st.len[q][lane] = 0;  // grains of another sample stop
          v.t = 0;
          v.pos = toFixed(p.startF + p.position * (p.endF - p.startF));
          v.next = 0.0;
          v.count = 0;
          v.harm = 0;
          v.active = 1;
          v.gen = static_cast<uint32_t>(newest->generation);
        }
        // The run: to the voice's next grid boundary, the call's end or the next reset.
        const auto toGrid = static_cast<uint32_t>(kGrainGrid - static_cast<uint64_t>(v.t) % kGrainGrid);
        uint32_t f1 = std::min(frames, f + (v.active != 0 ? toGrid : kGrainGrid));
        for (uint32_t i = f + 1; i < f1; ++i)
          if (resetAt(i, lane)) {
            f1 = i;
            break;
          }
        const uint32_t m = f1 - f;
        const SampleLevelView& lv0 = e->level[0];
        const int64_t s32 = toFixed(p.startF), e32 = toFixed(p.endF);
        const int64_t scanStep = p.freeze ? 0 : toFixed(p.scan * static_cast<double>(e->rateScale));
        if (v.active != 0 && lv0.length > 0) {
          // Spawns in [t, t + m): slots whose grain ended at or before this grid cell's start are free.
          const int64_t cell = v.t - v.t % kGrainGrid;
          const int64_t sinceCell = v.t - cell;
          while (std::ceil(v.next) < static_cast<double>(v.t + m)) {
            const auto ts = std::max<int64_t>(static_cast<int64_t>(std::ceil(v.next)), v.t);
            const auto off = static_cast<uint32_t>(ts - v.t);
            const double hz = std::max(finiteOr(hzAt(f + off, lane), p.root), 1.0);
            const uint32_t k = v.count++;
            const uint32_t seed = st.seed;
            auto rnd = [&](uint32_t d) noexcept { return 2.0 * static_cast<double>(grainUniform(seed, voiceSlot, k, d)) - 1.0; };
            // The next onset.
            double interval = p.mode == GranularMode::Classic ? p.sr / hz : p.sr / p.density;
            if (p.mode == GranularMode::Cloud)
              interval *= -std::log(1.0 - 0.999999 * static_cast<double>(grainUniform(seed, voiceSlot, k, 0)));
            v.next = static_cast<double>(ts) + std::max(interval, 1.0);
            // A free slot (ended at or before the cell's start).
            uint32_t q = kGrainsMax;
            for (uint32_t j = 0; j < p.grains; ++j)
              if (st.len[j][lane] == 0 || st.age[j][lane] >= st.len[j][lane] + static_cast<int32_t>(sinceCell)) {
                q = j;
                break;
              }
            if (q == kGrainsMax) {
              ++v.skipped;
              continue;
            }
            double rate = (p.mode == GranularMode::Classic ? p.ratio : hz / p.root * p.ratio) * static_cast<double>(e->rateScale);
            if (p.harmonic) {
              const float* H = e->harmonics;
              const uint32_t nh = H != nullptr ? std::min(e->harmonicCount, kGrainHarmonicsMax) : 6u;
              const uint32_t hi = v.harm++ % nh;
              const auto n = H != nullptr ? static_cast<uint32_t>(H[2 * hi]) : kDefaultHarmonics[2 * hi];
              const auto d = H != nullptr ? static_cast<uint32_t>(H[2 * hi + 1]) : kDefaultHarmonics[2 * hi + 1];
              rate = rate * static_cast<double>(n) / static_cast<double>(std::max(d, 1u));
            }
            if (p.sprayPitch > 0.0) rate *= std::exp2(rnd(2) * p.sprayPitch / 12.0);
            rate = std::clamp(rate, 1.0 / 64.0, 64.0);
            const int64_t P = wrapPos(v.pos + static_cast<int64_t>(off) * scanStep, s32, e32);
            const double start = static_cast<double>(P) / 4294967296.0 + rnd(1) * p.sprayTime * e->fileRate;
            const uint32_t L = mipLevelFor(rate, e->levels);
            const double sc = std::ldexp(1.0, -static_cast<int>(L));
            const int64_t g0 = toFixed(start * sc), st32 = toFixed(rate * sc);
            const double pan = p.sprayPan > 0.0 ? rnd(3) * p.sprayPan : 0.0;
            const double th = (pan + 1.0) * std::numbers::pi / 4.0;
            const auto len = static_cast<int32_t>(std::clamp<int64_t>(std::llround(p.size * p.sr), 2, int64_t{1} << 24));
            st.gi[q][lane] = static_cast<int32_t>(std::clamp<int64_t>(g0 >> 32, -(int64_t{1} << 30), int64_t{1} << 30));
            st.gf[q][lane] = static_cast<uint32_t>(g0 & 0xFFFFFFFF);
            st.si[q][lane] = static_cast<int32_t>(st32 >> 32);
            st.sf[q][lane] = static_cast<uint32_t>(st32 & 0xFFFFFFFF);
            st.age[q][lane] = -static_cast<int32_t>(off);
            st.len[q][lane] = len;
            st.winc[q][lane] = static_cast<float>(kGrainShapePoints) / static_cast<float>(len);
            st.gl[q][lane] = static_cast<float>(std::cos(th) * std::numbers::sqrt2);
            st.gr[q][lane] = static_cast<float>(std::sin(th) * std::numbers::sqrt2);
            st.level[q][lane] = L;
          }
        }
        // Grains that will not render this run (over, or of a level beyond the entry's) still age: the slot grid
        // tests when each one ended.
        for (uint32_t q = 0; q < kGrainsMax; ++q) {
          const int32_t ln = st.len[q][lane];
          if (ln > 0 && (st.age[q][lane] >= ln || st.level[q][lane] > e->levels))
            st.age[q][lane] = std::min(st.age[q][lane] + static_cast<int32_t>(m), ln + (1 << 20));
        }
        // Render the live grains: per group of 8 slots and mip level present in it, one batch (only live grains
        // cost: a group or level without one is skipped).
        std::fill_n(l, m, 0.0f);
        std::fill_n(r, m, 0.0f);
        for (uint32_t q0 = 0; q0 < kGrainsMax; q0 += 8) {
          uint32_t levels = 0;  // bit L: a live grain of level L in the group
          for (uint32_t j = 0; j < 8; ++j) {
            const uint32_t q = q0 + j;
            if (st.len[q][lane] > 0 && st.age[q][lane] < st.len[q][lane] && st.level[q][lane] <= e->levels)
              levels |= 1u << st.level[q][lane];
          }
          for (; levels != 0; levels &= levels - 1) {
            const auto L = static_cast<uint32_t>(std::countr_zero(levels));
            uint32_t include = 0;
            for (uint32_t j = 0; j < 8; ++j) {
              const uint32_t q = q0 + j;
              if (st.len[q][lane] > 0 && st.age[q][lane] < st.len[q][lane] && st.level[q][lane] == L) include |= 1u << j;
            }
            renderBatch(st, lane, q0, include, e->level[L], e->channels > 1, shape, m, l, r);
          }
        }
        const float at = lv0.length > 0 ? static_cast<float>(static_cast<double>(v.pos) / 4294967296.0 / lv0.length) : 0.0f;
        for (uint32_t i = 0; i < m; ++i) {
          const uint32_t o = (f + i) * W + lane;
          a.out[0][o] = l[i];
          a.out[1][o] = boolMask(v.active != 0);
          a.out[2][o] = r[i];
          a.out[3][o] = v.active != 0 ? at : 0.0f;
        }
        if (v.active != 0) {
          v.pos = wrapPos(v.pos + static_cast<int64_t>(m) * scanStep, s32, e32);
          v.t += m;
        }
        f = f1;
      }
      // The grain tap: this lane's live grains now.
      if (rec != nullptr && e->level[0].length > 0) {
        for (uint32_t q = 0; q < kGrainsMax && recCount < kGrainTapGrains; ++q) {
          const int32_t ag = st.age[q][lane], ln = st.len[q][lane];
          if (ag < 0 || ag >= ln) continue;
          const double frame = (static_cast<double>(st.gi[q][lane]) + st.gf[q][lane] * 0x1p-32) *
                               std::ldexp(1.0, static_cast<int>(st.level[q][lane]));
          const float wx = static_cast<float>(ag) * st.winc[q][lane];
          const auto w0 = std::min<uint32_t>(static_cast<uint32_t>(wx), kGrainShapePoints - 1);
          GrainTapGrain& gt = rec->grain[recCount++];
          gt.position = static_cast<float>(std::clamp(frame / e->level[0].length, 0.0, 1.0));
          gt.amplitude = shape[w0] + (wx - static_cast<float>(w0)) * (shape[w0 + 1] - shape[w0]);
          gt.pan = (st.gr[q][lane] * st.gr[q][lane] - st.gl[q][lane] * st.gl[q][lane]) * 0.5f;  // sin² − cos² of θ
          gt.lane = lane;
        }
      }
      storeVoice(st, lane, v);
    }
    if (rec != nullptr) tap->commit(st.group, recCount);
  }
};

}  // namespace

KernelFn findGranularKernel(uint32_t lanes, uint32_t m) noexcept { return pick<GranularK>(lanes, m); }

}  // namespace detail

}  // namespace pq::dsp
