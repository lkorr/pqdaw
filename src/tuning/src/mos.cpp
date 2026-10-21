// Port of the mos-explorer MOS math (index.html <script>, mos-calc.js). Each function names the JS function it
// reproduces; the comments only note where the C++ shape differs. Keep the operation order identical to the JS:
// the fixtures compare floating-point results to 1e-6 cents and strings exactly, and step sizes are compared after
// 0.01-cent rounding, so a reordered sum could flip a comparison.

#include "pq/tuning/mos.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "pq/tuning/js_number.h"

namespace pq::tuning {

namespace {

// Steps between sorted pitches; the last wraps to the period.
std::vector<double> stepsOf(const std::vector<double>& pitches, double period) {
  const size_t n = pitches.size();
  std::vector<double> steps(n);
  for (size_t i = 0; i < n; ++i) {
    const size_t next = (i + 1) % n;
    steps[i] = next == 0 ? period - pitches[n - 1] + pitches[0] : pitches[next] - pitches[i];
  }
  return steps;
}

// [...new Set(steps.map(round2))].sort(desc): largest and smallest rounded step, and whether they differ.
struct RoundedExtremes {
  double largest = 0.0;
  double smallest = 0.0;
  bool any = false;
  bool distinct = false;
};
RoundedExtremes roundedExtremes(const std::vector<double>& steps) {
  RoundedExtremes r;
  for (double st : steps) {
    const double v = js::round2(st);
    if (!r.any) {
      r.largest = r.smallest = v;
      r.any = true;
    } else {
      r.largest = std::max(r.largest, v);
      r.smallest = std::min(r.smallest, v);
    }
  }
  r.distinct = r.any && r.largest != r.smallest;
  return r;
}

// `x || 0` for a number.
double orZero(double x) { return (x != 0.0 && !std::isnan(x)) ? x : 0.0; }

// JS `a % n` for integers (sign of a).
int32_t imod(int32_t a, int32_t n) { return a % n; }
// ((a % n) + n) % n
int32_t iwrap(int32_t a, int32_t n) { return ((a % n) + n) % n; }

// Chain pitches i·g reduced into the period, i = 0..n−1.
std::vector<double> chainPositions(double g, int32_t n, double period) {
  std::vector<double> pos(static_cast<size_t>(std::max(n, 0)));
  for (int32_t i = 0; i < n; ++i) pos[static_cast<size_t>(i)] = js::wrap(static_cast<double>(i) * g, period);
  return pos;
}

int32_t indexOf(const std::vector<int32_t>& v, int32_t x) {
  const auto it = std::find(v.begin(), v.end(), x);
  return it == v.end() ? -1 : static_cast<int32_t>(it - v.begin());
}

}  // namespace

// ---- Keys and tree navigation ------------------------------------------------------------------------------------

std::string mosKeyString(MosKey k) { return std::to_string(k.nL) + "L_" + std::to_string(k.nS) + "s"; }

std::string mosDisplayLabel(MosKey k) { return std::to_string(k.nL) + "L " + std::to_string(k.nS) + "s"; }

std::optional<MosKey> parseMosKey(std::string_view t) {
  // Leftmost match of /(\d+)L_(\d+)s/ with greedy digit runs (a match starting at i uses the maximal digit run
  // from i, so trying each start position in order finds the regex's leftmost match).
  auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
  for (size_t i = 0; i < t.size(); ++i) {
    if (!isDigit(t[i])) continue;
    size_t j = i;
    while (j < t.size() && isDigit(t[j])) ++j;
    if (j + 1 >= t.size() || t[j] != 'L' || t[j + 1] != '_') continue;
    size_t k = j + 2;
    const size_t k0 = k;
    while (k < t.size() && isDigit(t[k])) ++k;
    if (k == k0 || k >= t.size() || t[k] != 's') continue;
    return MosKey{std::atoi(std::string(t.substr(i, j - i)).c_str()), std::atoi(std::string(t.substr(k0, k - k0)).c_str())};
  }
  return std::nullopt;
}

std::optional<MosKey> findParentMos(int32_t nL, int32_t nS) noexcept {  // RT-safe
  if (nL == nS || (nL <= 1 && nS <= 1)) return std::nullopt;
  if (nL > nS) return MosKey{nS, nL - nS};
  return MosKey{nL, nS - nL};
}

std::array<MosKey, 2> childMoses(MosKey k) noexcept {  // RT-safe
  return {MosKey{k.nL + k.nS, k.nL}, MosKey{k.nL, k.nL + k.nS}};
}

bool isDescendantMos(MosKey child, MosKey ancestor) noexcept {  // RT-safe
  MosKey cur = child;
  for (int i = 0; i < 30; ++i) {
    const auto parent = findParentMos(cur.nL, cur.nS);
    if (!parent) return false;
    if (*parent == ancestor) return true;
    cur = *parent;
  }
  return false;
}

std::vector<MosKey> ancestorChain(MosKey k) {
  std::vector<MosKey> chain{k};
  MosKey cur = k;
  for (int i = 0; i < 50; ++i) {
    const auto p = findParentMos(cur.nL, cur.nS);
    if (!p) break;
    chain.push_back(*p);
    cur = *p;
  }
  return chain;
}

// ---- Scales ------------------------------------------------------------------------------------------------------

double chainSafeGenerator(double genCents, int32_t n, double period, const RegionHint* sel) {
  if (!(n > 1)) return genCents;
  bool degenerate = false;
  for (int32_t i = 1; i < n; ++i) {
    const double r = js::wrap(static_cast<double>(i) * genCents, period);
    if (r < 1e-6 || r > period - 1e-6) {
      degenerate = true;
      break;
    }
  }
  if (!degenerate) return genCents;
  constexpr double kNudge = 1e-7;
  if (sel != nullptr && sel->perPeriodSize == n && genCents >= sel->lowCents - 1e-6 &&
      genCents <= sel->highCents + 1e-6) {
    const double mid = (sel->lowCents + sel->highCents) / 2.0;
    return genCents + (mid >= genCents ? kNudge : -kNudge);
  }
  auto stepSizeCount = [&](double g) {
    std::vector<double> pos = chainPositions(g, n, period);
    std::sort(pos.begin(), pos.end());
    std::vector<double> sizes;
    for (int32_t i = 0; i < n; ++i) {
      const size_t u = static_cast<size_t>(i);
      const double st = i < n - 1 ? pos[u + 1] - pos[u] : period - pos[static_cast<size_t>(n - 1)] + pos[0];
      if (std::none_of(sizes.begin(), sizes.end(), [&](double v) { return std::fabs(v - st) < 1e-6; }))
        sizes.push_back(st);
    }
    return sizes.size();
  };
  constexpr double kProbe = 1e-4;
  const size_t up = stepSizeCount(genCents + kProbe);
  const size_t down = stepSizeCount(genCents - kProbe);
  if (down == up) return genCents;
  return down < up ? genCents - kNudge : genCents + kNudge;
}

MosScale buildMosScale(double genCents, int32_t n, double period, const RegionHint* sel) {
  genCents = chainSafeGenerator(genCents, n, period, sel);
  MosScale sc;
  sc.pitches.push_back(0.0);
  for (int32_t i = 1; i < n; ++i) sc.pitches.push_back(js::wrap(static_cast<double>(i) * genCents, period));
  std::sort(sc.pitches.begin(), sc.pitches.end());
  sc.steps = stepsOf(sc.pitches, period);
  const RoundedExtremes r = roundedExtremes(sc.steps);
  sc.L = orZero(r.largest);
  sc.s = r.distinct ? r.smallest : sc.L;
  return sc;
}

Hardness computeHardness(double genCents, int32_t mosSize, double period) {
  std::vector<double> pos = chainPositions(genCents, mosSize, period);
  std::sort(pos.begin(), pos.end());
  std::vector<double> steps(pos.size());
  const int32_t N = mosSize;
  for (int32_t i = 0; i < N; ++i) {
    const size_t u = static_cast<size_t>(i);
    steps[u] = i < N - 1 ? pos[u + 1] - pos[u] : period - pos[static_cast<size_t>(N - 1)] + pos[0];
  }
  const RoundedExtremes r = roundedExtremes(steps);
  if (!r.distinct) {
    const double v = r.any ? orZero(r.largest) : 0.0;
    return {v, v, 1.0, "1.00"};
  }
  const double L = r.largest, s = r.smallest;
  if (s < 0.01) return {L, 0.0, INFINITY, "\xE2\x88\x9E:1"};  // "∞:1"
  const double ratio = L / s;
  return {L, s, ratio, js::toFixed(ratio, 2)};
}

std::vector<int32_t> generatorChain(double genCents, int32_t n, double period, const RegionHint* sel) {
  genCents = chainSafeGenerator(genCents, n, period, sel);
  struct Entry {
    int32_t genIndex;
    double cents;
  };
  std::vector<Entry> raw;
  raw.reserve(static_cast<size_t>(std::max(n, 0)));
  for (int32_t i = 0; i < n; ++i) raw.push_back({i, js::wrap(static_cast<double>(i) * genCents, period)});
  // Array.prototype.sort is stable: equal positions keep chain order.
  std::stable_sort(raw.begin(), raw.end(), [](const Entry& a, const Entry& b) { return a.cents < b.cents; });
  std::vector<int32_t> chain(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) chain[i] = raw[i].genIndex;
  return chain;
}

bool isBrightGenerator(double genCents, int32_t n, double period, const RegionHint* sel) {
  genCents = chainSafeGenerator(genCents, n, period, sel);
  std::vector<double> pos = chainPositions(genCents, n, period);
  std::sort(pos.begin(), pos.end());
  std::vector<double> steps(pos.size());
  for (int32_t i = 0; i < n; ++i) {
    const size_t u = static_cast<size_t>(i);
    steps[u] = i < n - 1 ? pos[u + 1] - pos[u] : period - pos[static_cast<size_t>(n - 1)] + pos[0];
  }
  for (int32_t r = 1; r < n; ++r) {
    for (int32_t i = 0; i < n; ++i) {
      const double d = steps[static_cast<size_t>((r + i) % n)] - steps[static_cast<size_t>(i)];
      if (d > 0.01) return false;
      if (d < -0.01) break;
    }
  }
  return true;
}

std::optional<std::string> computeUdp(double genCents, int32_t n, int32_t modeRotation, double period,
                                      const RegionHint* sel) {
  if (!(n > 1)) return std::nullopt;
  const std::vector<int32_t> chain = generatorChain(genCents, n, period, sel);
  const int32_t root = chain[static_cast<size_t>(iwrap(modeRotation, n))];
  const bool dark = !isBrightGenerator(genCents, n, period, sel);
  const int32_t u = dark ? root : (n - 1 - root);
  const int32_t d = dark ? (n - 1 - root) : root;
  return std::to_string(u) + "|" + std::to_string(d);
}

double gmnGenerator(double genCents, double period) noexcept {  // RT-safe
  const double g = js::wrap(genCents, period);
  const double other = js::wrap(period - g, period);
  auto isFifthish = [&](double x) { return x > period * 0.5 + 1e-9 && x <= period * (3.0 / 5.0) + 1e-9; };
  if (isFifthish(g) && !isFifthish(other)) return g;
  if (isFifthish(other) && !isFifthish(g)) return other;
  return std::min(g, other);
}

std::optional<int32_t> computeGmn(double genCents, int32_t n, int32_t modeRotation, double period,
                                  const RegionHint* sel) {
  if (!(n > 1)) return std::nullopt;
  const double g = js::wrap(genCents, period);
  const std::vector<int32_t> chain = generatorChain(g, n, period, sel);
  const int32_t pos = chain[static_cast<size_t>(iwrap(modeRotation, n))];
  const bool mirrored = std::fabs(g - gmnGenerator(genCents, period)) > 1e-9;
  return (mirrored ? (n - 1 - pos) : pos) + 1;
}

std::string gmnLabel(int32_t gmn) { return "Mode " + std::to_string(gmn); }

std::string formatModmosAlterations(std::span<const double> modmos) {
  std::string out;
  bool first = true;
  for (size_t i = 0; i < modmos.size(); ++i) {
    const double v = orZero(modmos[i]);
    if (std::fabs(v) <= 0.01) continue;
    const char* sign = v > 0 ? "+" : "-";
    const double mag = std::fabs(v);
    const bool isInt = std::fabs(mag - js::round(mag)) < 0.01;
    std::string label = isInt ? (js::round(mag) == 1.0 ? std::string(sign) : sign + js::toString(js::round(mag)) + "\xC3\x97")
                              : sign + js::toString(js::round(mag * 100.0) / 100.0) + "\xC3\x97";  // "×"
    if (!first) out += ' ';
    first = false;
    out += label + std::to_string(i + 1);
  }
  return out;
}

bool hasModmos(std::span<const double> modmos) noexcept {  // RT-safe
  return std::any_of(modmos.begin(), modmos.end(), [](double v) { return std::fabs(v) > 0.01; });
}

std::string computeStepPattern(double genCents, int32_t n, int32_t modeRotation, double period,
                               const RegionHint* sel) {
  const MosScale scale = rotateMosScale(buildMosScale(genCents, n, period, sel), modeRotation, period);
  std::string out;
  out.reserve(scale.steps.size());
  for (double st : scale.steps) out.push_back(std::fabs(st - scale.L) < 0.5 ? 'L' : 's');
  return out;
}

int32_t transferModeRotation(double oldGenCents, int32_t oldN, double oldRotation, double newGenCents, int32_t newN,
                             double period, const RegionHint* sel) {
  if (!(oldN > 1) || !(newN > 1)) return 0;
  if (!std::isfinite(oldRotation) || oldRotation < 0 || oldRotation >= oldN) return 0;
  // oldChain[x] is undefined for a non-integer x, and the JS then returns 0.
  if (oldRotation != std::floor(oldRotation)) return 0;
  const std::vector<int32_t> oldChain = generatorChain(oldGenCents, oldN, period, sel);
  const int32_t oldPos = oldChain[static_cast<size_t>(oldRotation)];
  const int32_t targetPos =
      oldN == newN ? oldPos
                   : static_cast<int32_t>(js::round((static_cast<double>(oldPos) / (oldN - 1)) * (newN - 1)));
  const int32_t newRotation = indexOf(generatorChain(newGenCents, newN, period, sel), targetPos);
  return newRotation >= 0 ? newRotation : 0;
}

int32_t rotateByGenerator(double genCents, int32_t n, int32_t currentRotation, int32_t direction, double period,
                          const RegionHint* sel) {
  const std::vector<int32_t> chain = generatorChain(genCents, n, period, sel);
  if (currentRotation < 0 || currentRotation >= n) return -1;  // chain[x] undefined -> NaN -> indexOf -1
  const int32_t current = chain[static_cast<size_t>(currentRotation)];
  return indexOf(chain, iwrap(current - direction, n));
}

MosScale rotateMosScale(const MosScale& scale, int32_t rotation, double period) {
  const int32_t n = static_cast<int32_t>(scale.pitches.size());
  if (rotation == 0) return scale;
  assert(rotation > 0 && n > 0);
  const double root = scale.pitches[static_cast<size_t>(imod(rotation, n))];
  MosScale out;
  out.pitches.resize(static_cast<size_t>(n));
  for (int32_t i = 0; i < n; ++i)
    out.pitches[static_cast<size_t>(i)] = js::wrap(scale.pitches[static_cast<size_t>((i + rotation) % n)] - root, period);
  std::sort(out.pitches.begin(), out.pitches.end());
  out.steps = stepsOf(out.pitches, period);
  const RoundedExtremes r = roundedExtremes(out.steps);
  out.L = orZero(r.largest);
  out.s = r.distinct ? r.smallest : 0.0;
  return out;
}

ModmosScale applyModmos(const MosScale& scale, std::span<const double> modmos, double period) {
  ModmosScale out;
  if (modmos.empty()) {
    out.pitches = scale.pitches;
    out.steps = scale.steps;
    out.L = scale.L;
    out.s = scale.s;
    return out;
  }
  const double chroma = scale.L - scale.s;
  const size_t n = scale.pitches.size();
  struct Tagged {
    double cents;
    double alt;
    int32_t origDeg;
  };
  std::vector<Tagged> tagged(n);
  for (size_t i = 0; i < n; ++i) {
    const double raw = i < modmos.size() ? orZero(modmos[i]) : 0.0;
    const double offset = raw * chroma;
    const double p = scale.pitches[i];
    tagged[i] = {offset != 0.0 ? js::wrap(p + offset, period) : p, offset, static_cast<int32_t>(i)};
  }
  std::stable_sort(tagged.begin(), tagged.end(), [](const Tagged& a, const Tagged& b) { return a.cents < b.cents; });
  out.pitches.resize(n);
  out.alts.resize(n);
  out.origDegs.resize(n);
  for (size_t i = 0; i < n; ++i) {
    out.pitches[i] = tagged[i].cents;
    out.alts[i] = tagged[i].alt;
    out.origDegs[i] = tagged[i].origDeg;
  }
  out.steps = stepsOf(out.pitches, period);
  out.L = scale.L;
  out.s = scale.s;
  return out;
}

std::vector<int32_t> rotationDegreeMap(double genCents, int32_t n, int32_t rotation, double period,
                                       const RegionHint* sel) {
  const MosScale base = buildMosScale(genCents, n, period, sel);
  const double root = base.pitches[static_cast<size_t>(imod(rotation, n))];
  struct Tagged {
    double cents;
    int32_t baseIdx;
  };
  std::vector<Tagged> tagged(base.pitches.size());
  for (size_t i = 0; i < tagged.size(); ++i) tagged[i] = {js::wrap(base.pitches[i] - root, period), static_cast<int32_t>(i)};
  std::stable_sort(tagged.begin(), tagged.end(), [](const Tagged& a, const Tagged& b) { return a.cents < b.cents; });
  std::vector<int32_t> map(tagged.size());
  for (size_t i = 0; i < tagged.size(); ++i) map[i] = tagged[i].baseIdx;
  return map;
}

ModeChange applyModeAction(ModeAction action, double genCents, int32_t n, int32_t rotation,
                           std::span<const double> modmos, double period, const RegionHint* sel) {
  ModeChange out;
  out.modmos.assign(modmos.begin(), modmos.end());
  switch (action) {
    case ModeAction::StepUp: out.rotation = (rotation + 1) % n; break;
    case ModeAction::StepDown: out.rotation = (rotation - 1 + n) % n; break;
    case ModeAction::GenUp: out.rotation = rotateByGenerator(genCents, n, rotation, 1, period, sel); break;
    case ModeAction::GenDown: out.rotation = rotateByGenerator(genCents, n, rotation, -1, period, sel); break;
  }
  if (hasModmos(modmos) && rotation != out.rotation) {
    const std::vector<int32_t> oldMap = rotationDegreeMap(genCents, n, rotation, period, sel);
    const std::vector<int32_t> newMap = rotationDegreeMap(genCents, n, out.rotation, period, sel);
    std::vector<int32_t> baseToNew(static_cast<size_t>(n), -1);
    for (int32_t i = 0; i < n; ++i) baseToNew[static_cast<size_t>(newMap[static_cast<size_t>(i)])] = i;
    out.modmos.assign(static_cast<size_t>(n), 0.0);
    const size_t m = std::min(modmos.size(), static_cast<size_t>(n));
    for (size_t i = 0; i < m; ++i) {
      if (std::fabs(modmos[i]) > 0.01) {
        const int32_t newIdx = baseToNew[static_cast<size_t>(oldMap[i])];
        if (newIdx >= 0) out.modmos[static_cast<size_t>(newIdx)] = modmos[i];
      }
    }
  }
  return out;
}

MosScale buildGenSequenceScale(std::span<const double> genSequence, int32_t n, double period) {
  const size_t k = genSequence.size();
  MosScale sc;
  sc.pitches.push_back(0.0);
  // The JS re-sums seq[0..i) from 0 for every i; a running sum performs the identical additions in the same order.
  double cum = 0.0;
  for (int32_t i = 1; i < n; ++i) {
    cum += genSequence[static_cast<size_t>(i - 1) % k];
    sc.pitches.push_back(js::wrap(cum, period));
  }
  std::sort(sc.pitches.begin(), sc.pitches.end());
  sc.steps = stepsOf(sc.pitches, period);
  const RoundedExtremes r = roundedExtremes(sc.steps);
  sc.L = orZero(r.largest);
  sc.s = r.distinct ? r.smallest : sc.L;
  return sc;
}

void updateGenSequenceEntry(std::vector<double>& seq, size_t idx, double newCents) {
  if (seq.size() < 2) return;
  const size_t k = seq.size();
  const double delta = newCents - seq[idx];
  seq[idx] = newCents;
  const size_t next = (idx + 1) % k;
  seq[next] = std::max(1.0, seq[next] - delta);
}

int32_t periodRepetitions(double viewLimit, double period) noexcept {  // RT-safe
  return static_cast<int32_t>(js::round(viewLimit / period));
}

FullScale buildFullScale(const MosParams& p) {
  const RegionHint* sel = p.region ? &*p.region : nullptr;
  const MosScale current = !p.genSequence.empty() ? buildGenSequenceScale(p.genSequence, p.n, p.period)
                                                  : buildMosScale(p.generator, p.n, p.period, sel);
  const MosScale rotated = rotateMosScale(current, p.rotation, p.period);
  std::vector<double> base = hasModmos(p.modmos) ? applyModmos(rotated, p.modmos, p.period).pitches : rotated.pitches;
  FullScale full;
  if (p.periodsPerEquave <= 1) {
    full.pitches = std::move(base);
    full.size = p.n;
    full.equaveCents = p.period;
    return full;
  }
  full.pitches.reserve(base.size() * static_cast<size_t>(p.periodsPerEquave));
  for (int32_t k = 0; k < p.periodsPerEquave; ++k)
    for (double c : base) full.pitches.push_back(c + k * p.period);
  full.size = p.n * p.periodsPerEquave;
  full.equaveCents = p.period * p.periodsPerEquave;
  return full;
}

std::vector<DegreeCandidate> findNearestDegrees(double targetCents, std::span<const double> pitches, int32_t n,
                                                double octave) {
  std::vector<DegreeCandidate> c;
  c.reserve(static_cast<size_t>(std::max(n, 0)) * 3);
  for (int32_t d = 0; d < n; ++d) {
    const double base = pitches[static_cast<size_t>(d)];
    const int32_t octEst = static_cast<int32_t>(js::round((targetCents - base) / octave));
    for (int32_t o = octEst - 1; o <= octEst + 1; ++o) {
      const double cents = base + o * octave;
      c.push_back({d, o, cents, std::fabs(cents - targetCents)});
    }
  }
  std::stable_sort(c.begin(), c.end(), [](const DegreeCandidate& a, const DegreeCandidate& b) { return a.dist < b.dist; });
  return c;
}

void updateHeldNotes(std::span<HeldNote> notes, const FullScale& full, int32_t& heldScaleSize) {
  const int32_t n = full.size;
  const double octave = full.equaveCents;
  if (notes.empty()) {
    heldScaleSize = n;
    return;
  }
  // Same degrees: every note follows its own degree.
  if (heldScaleSize == n) {
    for (HeldNote& h : notes)
      h.currentCents = full.pitches[static_cast<size_t>(imod(h.degree, n))] + h.octave * octave;
    return;
  }
  heldScaleSize = n;

  struct Entry {
    HeldNote* note;
    std::vector<DegreeCandidate> candidates;
    DegreeCandidate chosen;
  };
  std::vector<Entry> entries;
  entries.reserve(notes.size());
  for (HeldNote& h : notes) {
    if (!h.currentCents) h.currentCents = full.pitches[static_cast<size_t>(imod(h.degree, n))] + h.octave * octave;
    entries.push_back({&h, findNearestDegrees(*h.currentCents, full.pitches, n, octave), {}});
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& a, const Entry& b) { return a.candidates[0].dist < b.candidates[0].dist; });

  std::vector<std::pair<int32_t, int32_t>> taken;
  for (Entry& e : entries) {
    bool found = false;
    for (const DegreeCandidate& c : e.candidates) {
      const std::pair<int32_t, int32_t> slot{c.degree, c.octave};
      if (std::find(taken.begin(), taken.end(), slot) == taken.end()) {
        taken.push_back(slot);
        e.chosen = c;
        found = true;
        break;
      }
    }
    if (!found) {
      const DegreeCandidate& c = e.candidates[0];
      e.chosen = {c.degree, c.octave + 1, c.cents + octave, c.dist};
    }
  }
  for (Entry& e : entries) {
    e.note->degree = e.chosen.degree;
    e.note->octave = e.chosen.octave;
    e.note->currentCents = full.pitches[static_cast<size_t>(e.chosen.degree)] + e.chosen.octave * octave;
  }
}

std::vector<EdoGenerator> edoGenerators(int32_t maxEdo, double period) {
  std::vector<EdoGenerator> gens;
  for (int32_t n = 2; n <= maxEdo; ++n)
    for (int32_t m = 1; m < n; ++m) {
      int32_t a = m, b = n;
      while (b != 0) a = std::exchange(b, a % b);
      if (a != 1) continue;
      gens.push_back({m, n, (static_cast<double>(m) / n) * period});
    }
  return gens;
}

std::optional<std::string> edoStepRatio(double genCents, int32_t mosSize, double period,
                                        std::span<const EdoGenerator> edos) {
  const auto it = std::find_if(edos.begin(), edos.end(),
                               [&](const EdoGenerator& g) { return std::fabs(g.cents - genCents) < 0.001; });
  if (it == edos.end()) return std::nullopt;
  const double step = period / it->edo;
  const Hardness hr = computeHardness(genCents, mosSize, period);
  return js::toString(js::round(hr.L / step)) + ":" + js::toString(js::round(hr.s / step));
}

// ---- mos-calc.js -------------------------------------------------------------------------------------------------

namespace mos_calc {

MosScale buildMosScale(double genCents, int32_t n, double period) {
  MosScale sc;
  sc.pitches.push_back(0.0);
  for (int32_t i = 1; i < n; ++i) sc.pitches.push_back(js::wrap(static_cast<double>(i) * genCents, period));
  std::sort(sc.pitches.begin(), sc.pitches.end());
  sc.steps = stepsOf(sc.pitches, period);
  const RoundedExtremes r = roundedExtremes(sc.steps);
  sc.L = orZero(r.largest);
  sc.s = r.distinct ? r.smallest : 0.0;
  return sc;
}

Hardness computeHardness(double genCents, int32_t mosSize, double period) {
  const MosScale scale = buildMosScale(genCents, mosSize, period);
  if (scale.s < 0.01) return {scale.L, 0.0, INFINITY, "\xE2\x88\x9E:1"};
  const double ratio = scale.L / scale.s;
  return {scale.L, scale.s, ratio, js::toFixed(ratio, 4)};
}

std::optional<RegionBounds> mosRegionBounds(int32_t nL, int32_t nS, double period) {
  // The CLI builds the whole region list for maxSize = nL + nS and returns the first match; walking in the same
  // order and stopping at the first match gives the same answer.
  struct Found {
    bool hit = false;
    std::array<int32_t, 2> lowFrac{}, highFrac{};
    bool mirror = false;
  } found;
  auto addRegion = [&](int32_t rL, int32_t rS, int32_t fp1, int32_t fq1, int32_t fp2, int32_t fq2) {
    if (found.hit || rL != nL || rS != nS) return;
    found = {true, {fp1, fq1}, {fp2, fq2}, false};  // the direct region precedes its mirror, so it always wins
  };
  const int32_t maxSize = nL + nS;
  auto traverse = [&](auto&& self, int32_t lp, int32_t lq, int32_t rp, int32_t rq, int32_t depth) -> void {
    if (depth <= 0 || found.hit) return;
    const int32_t mp = lp + rp, mq = lq + rq;
    if (mq > maxSize) return;
    addRegion(lq, rq, lp, lq, mp, mq);
    addRegion(rq, lq, mp, mq, rp, rq);
    self(self, lp, lq, mp, mq, depth - 1);
    self(self, mp, mq, rp, rq, depth - 1);
  };
  addRegion(1, 1, 0, 1, 1, 2);
  traverse(traverse, 0, 1, 1, 2, maxSize);
  if (!found.hit) return std::nullopt;
  RegionBounds b;
  b.lowCents = (static_cast<double>(found.lowFrac[0]) / found.lowFrac[1]) * period;
  b.highCents = (static_cast<double>(found.highFrac[0]) / found.highFrac[1]) * period;
  b.lowFrac = found.lowFrac;
  b.highFrac = found.highFrac;
  b.mirror = found.mirror;
  return b;
}

std::optional<HardnessSolution> generatorForHardness(int32_t nL, int32_t nS, double targetRatio, double period) {
  const auto bounds = mosRegionBounds(nL, nS, period);
  if (!bounds) return std::nullopt;
  double lo = bounds->lowCents + 0.001;
  double hi = bounds->highCents - 0.001;
  const Hardness hrLo = computeHardness(lo, nL + nS, period);
  const Hardness hrHi = computeHardness(hi, nL + nS, period);
  const bool eqLow = hrLo.ratio < hrHi.ratio;
  const double minRatio = std::min(hrLo.ratio, hrHi.ratio);
  if (targetRatio < minRatio) targetRatio = minRatio;
  for (int iter = 0; iter < 100; ++iter) {
    const double mid = (lo + hi) / 2.0;
    const Hardness hr = computeHardness(mid, nL + nS, period);
    if (eqLow) {
      if (hr.ratio < targetRatio) lo = mid;
      else hi = mid;
    } else {
      if (hr.ratio < targetRatio) hi = mid;
      else lo = mid;
    }
  }
  const double gen = (lo + hi) / 2.0;
  const Hardness hr = computeHardness(gen, nL + nS, period);
  // +x.toFixed(d): round to d decimals through the exact decimal string, then parse.
  auto fixed = [](double x, int d) { return std::isfinite(x) ? std::strtod(js::toFixed(x, d).c_str(), nullptr) : x; };
  return HardnessSolution{fixed(gen, 6), fixed(hr.L, 4), fixed(hr.s, 4), fixed(hr.ratio, 6)};
}

}  // namespace mos_calc

}  // namespace pq::tuning
