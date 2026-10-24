#include "pq/core/grid.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>

namespace pq {
namespace {

constexpr Ticks kSixteenth = kTicksPerWhole / 16;  // 55440
constexpr Ticks kEndOfTime = std::numeric_limits<Ticks>::max();

constexpr int64_t floorDiv(int64_t a, int64_t b) noexcept {  // RT-safe; b > 0
  const int64_t q = a / b;
  return (a % b != 0 && a < 0) ? q - 1 : q;
}
constexpr int64_t ceilDivPos(int64_t a, int64_t b) noexcept { return (a + b - 1) / b; }  // RT-safe; a ≥ 0, b > 0
// round(a / b), ties up; a ≥ 0, b > 0.
constexpr int64_t roundDivPos(int64_t a, int64_t b) noexcept { return (2 * a + b) / (2 * b); }  // RT-safe
// round(a / b), ties up; any sign of a, b > 0.
constexpr int64_t roundDiv(int64_t a, int64_t b) noexcept { return floorDiv(2 * a + b, 2 * b); }  // RT-safe

// Lines of the group starting at gs: gs + round(j · num / den).
// Every straight and 3/5/7/9/11-tuplet grid of PPQ 221760 has den == 1 (exact cells): skip the division there.
constexpr Ticks linePos(const detail::GridLayout& L, Ticks gs, int64_t j) noexcept {  // RT-safe
  return L.den == 1 ? gs + j * L.num : gs + roundDivPos(j * L.num, L.den);
}

// Appends text to out at pos; false if it does not fit.
bool put(std::span<char> out, std::size_t& pos, const char* s) noexcept {  // RT-safe
  for (; *s; ++s) {
    if (pos >= out.size()) return false;
    out[pos++] = *s;
  }
  return true;
}
bool put(std::span<char> out, std::size_t& pos, int64_t v) noexcept {  // RT-safe
  const auto r = std::to_chars(out.data() + pos, out.data() + out.size(), v);
  if (r.ec != std::errc{}) return false;
  pos = static_cast<std::size_t>(r.ptr - out.data());
  return true;
}

}  // namespace

// ---- GridSpec ---------------------------------------------------------------------------------------------------

GridSpec GridSpec::narrowed() const noexcept {
  GridSpec s = *this;
  if (mode == GridMode::Adaptive)
    s.level = static_cast<int8_t>(std::max(kGridLevelMin, level - 1));
  else
    s.fixedExp = static_cast<int8_t>(std::min(kGridFixedExpMax, fixedExp + 1));
  return s;
}

GridSpec GridSpec::widened() const noexcept {
  GridSpec s = *this;
  if (mode == GridMode::Adaptive)
    s.level = static_cast<int8_t>(std::min(kGridLevelMax, level + 1));
  else
    s.fixedExp = static_cast<int8_t>(std::max(kGridFixedExpMin, fixedExp - 1));
  return s;
}

GridSpec GridSpec::tripletToggled() const noexcept {
  GridSpec s = *this;
  s.tuplet = tuplet == 3 ? uint8_t{1} : uint8_t{3};
  return s;
}

GridSpec GridSpec::snapToggled() const noexcept {
  GridSpec s = *this;
  s.snap = !snap;
  return s;
}

GridSpec GridSpec::modeToggled() const noexcept {
  GridSpec s = *this;
  s.mode = mode == GridMode::Adaptive ? GridMode::Fixed : GridMode::Adaptive;
  s.off = false;
  return s;
}

double adaptiveMinCellPx(int level) noexcept {
  return std::ldexp(kGridAdaptiveBasePx, std::clamp(level, kGridLevelMin, kGridLevelMax));
}

const char* adaptiveLevelName(int level) noexcept {
  static constexpr const char* kNames[] = {"Narrowest", "Narrower", "Narrow", "Medium", "Wide", "Wider", "Widest"};
  return kNames[std::clamp(level, kGridLevelMin, kGridLevelMax) - kGridLevelMin];
}

const char* fixedValueName(int fixedExp) noexcept {
  static constexpr const char* kNames[] = {"8 Bars", "4 Bars", "2 Bars", "1/1",  "1/2",
                                           "1/4",    "1/8",    "1/16",   "1/32", "1/64"};
  return kNames[std::clamp(fixedExp, kGridFixedExpMin, kGridFixedExpMax) - kGridFixedExpMin];
}

std::size_t formatGridDivision(const GridDivision& d, std::span<char> out) noexcept {
  std::size_t pos = 0;
  bool ok = true;
  if (d.bars > 0) {
    ok = put(out, pos, d.bars) && put(out, pos, d.bars == 1 ? " Bar" : " Bars");
    if (ok && d.tuplet > 1)
      ok = put(out, pos, " ") && put(out, pos, d.tuplet) && put(out, pos, ":") &&
           put(out, pos, tupletBase(d.tuplet));
  } else if (d.denominator > 0) {
    ok = put(out, pos, "1/") && put(out, pos, d.denominator);
    if (ok && d.tuplet == 3)
      ok = put(out, pos, "T");
    else if (ok && d.tuplet > 1)
      ok = put(out, pos, " ") && put(out, pos, d.tuplet) && put(out, pos, ":") &&
           put(out, pos, tupletBase(d.tuplet));
  } else {
    ok = put(out, pos, "Off");
  }
  return ok ? pos : 0;
}

// ---- Ruler ------------------------------------------------------------------------------------------------------

namespace {
RulerPosition makeRulerPosition(int64_t bar, Ticks tickInBar, TimeSignature sig) noexcept {  // RT-safe
  const Ticks beatLen = ticksPerBeat(sig);
  const Ticks inBeat = tickInBar % beatLen;
  RulerPosition p;
  p.bar = bar >= 0 ? bar + 1 : bar;
  p.beat = static_cast<int32_t>(tickInBar / beatLen) + 1;
  p.sixteenth = static_cast<int32_t>(inBeat / kSixteenth) + 1;
  p.remainder = inBeat % kSixteenth;
  return p;
}
}  // namespace

RulerPosition rulerPosition(const TimeSigMap& sigs, Ticks t) noexcept {
  const std::size_t i = sigs.changeIndexAt(t);
  const TimeSignature sig = sigs.changeSignature(i);
  const Ticks barLen = ticksPerBar(sig);
  const int64_t bars = floorDiv(t - sigs.changeStartTick(i), barLen);
  return makeRulerPosition(sigs.changeBar(i) + bars, t - sigs.changeStartTick(i) - bars * barLen, sig);
}

RulerPosition rulerPosition(const GridLine& line) noexcept {
  return makeRulerPosition(line.bar, line.tickInBar, line.sig);
}

std::size_t formatRulerPosition(const RulerPosition& p, std::span<char> out) noexcept {
  std::size_t pos = 0;
  bool ok = put(out, pos, p.bar);
  const bool onBeat = p.sixteenth == 1 && p.remainder == 0;
  if (ok && !(onBeat && p.beat == 1)) ok = put(out, pos, ".") && put(out, pos, int64_t{p.beat});
  if (ok && !onBeat) ok = put(out, pos, ".") && put(out, pos, int64_t{p.sixteenth});
  return ok ? pos : 0;
}

// ---- Grid -------------------------------------------------------------------------------------------------------

Grid::Grid(const GridSpec& spec, const TimeSigMap& sigs, double pxPerTick) noexcept
    : sigs_(&sigs),
      pxPerTick_(pxPerTick),
      minCellPx_(adaptiveMinCellPx(spec.level)),
      kind_(spec.off ? Kind::Off : spec.mode == GridMode::Adaptive ? Kind::Adaptive : Kind::Fixed),
      fixedExp_(static_cast<int8_t>(std::clamp<int>(spec.fixedExp, kGridFixedExpMin, kGridFixedExpMax))),
      tuplet_(static_cast<uint8_t>(std::clamp<int>(spec.tuplet, 1, kGridMaxTuplet))),
      snap_(spec.snap) {}

Grid Grid::adaptive(double minCellPx, const TimeSigMap& sigs, double pxPerTick, int tuplet) noexcept {
  Grid g;
  g.sigs_ = &sigs;
  g.pxPerTick_ = pxPerTick;
  g.minCellPx_ = minCellPx;
  g.kind_ = Kind::Adaptive;
  g.tuplet_ = static_cast<uint8_t>(std::clamp(tuplet, 1, kGridMaxTuplet));
  g.snap_ = true;
  return g;
}

GridDivision Grid::divisionFor(TimeSignature sig) const noexcept {
  if (kind_ == Kind::Off) return {};
  const int32_t n = tuplet_;
  const int64_t base = tupletBase(n);
  const Ticks bar = ticksPerBar(sig);
  constexpr Ticks W = kTicksPerWhole;

  if (kind_ == Kind::Fixed) {
    if (fixedExp_ < 0) return {int32_t{1} << -fixedExp_, 0, n};
    const int64_t d = int64_t{1} << fixedExp_;
    if (W >= d * bar)  // the note cell is at least a bar: whole bars
      return {static_cast<int32_t>(std::bit_floor(static_cast<uint64_t>(W / (d * bar)))), 0, n};
    return {0, static_cast<int32_t>(d), n};
  }

  // Adaptive: the finest cell (incl. the tuplet) that is at least minCellPx_ wide.
  const double minTicks = (pxPerTick_ > 0.0 && std::isfinite(pxPerTick_)) ? minCellPx_ / pxPerTick_
                                                                          : std::numeric_limits<double>::infinity();
  const Ticks beat = ticksPerBeat(sig);
  for (int e = kGridFixedExpMax; e >= 0; --e) {
    const int64_t d = int64_t{1} << e;
    if (W >= d * bar) break;                        // cell ≥ bar: continue with whole bars
    if (W > d * beat && (bar * d) % W != 0) continue;  // longer than a beat but does not tile the bar
    if (static_cast<double>(base * W) >= minTicks * static_cast<double>(d * n)) return {0, static_cast<int32_t>(d), n};
  }
  for (int64_t k = 1; k < kGridMaxBarsPerCell; k *= 2)
    if (static_cast<double>(k * base * bar) >= minTicks * static_cast<double>(n)) return {static_cast<int32_t>(k), 0, n};
  return {static_cast<int32_t>(kGridMaxBarsPerCell), 0, n};
}

GridDivision Grid::divisionAt(Ticks t) const noexcept {
  return divisionFor(sigs_->signatureAt(t));
}

detail::GridLayout Grid::layoutFor(std::size_t seg) const noexcept {
  detail::GridLayout L;
  L.seg = seg;
  L.segBar = sigs_->changeBar(seg);
  L.segTick = sigs_->changeStartTick(seg);
  L.segEndTick = seg + 1 < sigs_->changeCount() ? sigs_->changeStartTick(seg + 1) : kEndOfTime;
  L.sig = sigs_->changeSignature(seg);
  L.barLen = ticksPerBar(L.sig);
  L.beatLen = ticksPerBeat(L.sig);
  const GridDivision d = divisionFor(L.sig);
  const int64_t n = d.tuplet;
  const int64_t base = tupletBase(n);
  int64_t num = 0;
  int64_t den = 0;
  if (d.denominator > 0) {  // note cells restart at each bar
    L.groupBars = 1;
    num = base * kTicksPerWhole;
    den = d.denominator * n;
  } else {                  // k-bar cells; a tuplet group spans base · k bars
    L.groupBars = base * d.bars;
    num = L.groupBars * L.barLen;
    den = n;
  }
  const int64_t g = std::gcd(num, den);
  L.num = num / g;
  L.den = den / g;
  L.groupLen = L.groupBars * L.barLen;
  L.tuplet = n;
  return L;
}

Grid::Bracket Grid::bracket(Ticks t) const noexcept {
  if (kind_ == Kind::Off) return {t, t};
  const detail::GridLayout L = layoutFor(sigs_->changeIndexAt(t));
  const int64_t bar = L.segBar + floorDiv(t - L.segTick, L.barLen);
  const int64_t gb = L.segBar + floorDiv(bar - L.segBar, L.groupBars) * L.groupBars;
  const Ticks gs = L.segTick + (gb - L.segBar) * L.barLen;
  const Ticks ge = std::min(gs + L.groupLen, L.segEndTick);
  const Ticks off = t - gs;  // 0 ≤ off < ge − gs
  if (off == 0) return {t, t};
  // Smallest j with round(j·num/den) ≥ off  ⇔  2·j·num ≥ (2·off − 1)·den.
  const int64_t j = ceilDivPos((2 * off - 1) * L.den, 2 * L.num);
  const Ticks lineJ = linePos(L, gs, j);
  const Ticks after = lineJ < ge ? lineJ : ge;  // the next group (bar / segment) always starts with a line
  if (after == t) return {t, t};
  return {linePos(L, gs, j - 1), after};
}

Ticks Grid::lineAtOrBefore(Ticks t) const noexcept { return bracket(t).before; }
Ticks Grid::lineAtOrAfter(Ticks t) const noexcept { return bracket(t).after; }
Ticks Grid::lineAfter(Ticks t) const noexcept { return kind_ == Kind::Off ? t + 1 : bracket(t + 1).after; }

Ticks Grid::nearestLine(Ticks t) const noexcept {
  const Bracket b = bracket(t);
  return (t - b.before < b.after - t) ? b.before : b.after;
}

TickRange Grid::cellAt(Ticks t) const noexcept { return {lineAtOrBefore(t), lineAfter(t)}; }

Ticks Grid::snap(Ticks t) const noexcept { return snapping() ? nearestLine(t) : t; }

Ticks Grid::snapDelta(Ticks origin, Ticks rawDelta) const noexcept {
  if (!snapping()) return rawDelta;
  const Ticks anchor = lineAtOrBefore(origin);
  return nearestLine(anchor + rawDelta) - anchor;
}

GridLineCursor Grid::lines(Ticks from, Ticks to) const noexcept {
  GridLineCursor c;
  c.grid_ = *this;
  if (kind_ == Kind::Off || from >= to) return c;  // done_ stays true
  c.L_ = layoutFor(sigs_->changeIndexAt(from));
  const detail::GridLayout& L = c.L_;
  const int64_t bar = L.segBar + floorDiv(from - L.segTick, L.barLen);
  c.groupBar_ = L.segBar + floorDiv(bar - L.segBar, L.groupBars) * L.groupBars;
  c.groupStart_ = L.segTick + (c.groupBar_ - L.segBar) * L.barLen;
  c.groupEnd_ = std::min(c.groupStart_ + L.groupLen, L.segEndTick);
  const Ticks off = from - c.groupStart_;
  c.j_ = off == 0 ? 0 : ceilDivPos((2 * off - 1) * L.den, 2 * L.num);
  c.jInTuplet_ = c.j_ % L.tuplet;
  c.to_ = to;
  c.done_ = false;
  return c;
}

// ---- Cursor -----------------------------------------------------------------------------------------------------

void GridLineCursor::advanceGroup() noexcept {
  if (groupEnd_ == L_.segEndTick) {  // next time-signature segment
    L_ = grid_.layoutFor(L_.seg + 1);
    groupBar_ = L_.segBar;
    groupStart_ = L_.segTick;
  } else {
    groupBar_ += L_.groupBars;
    groupStart_ = groupEnd_;
  }
  groupEnd_ = std::min(groupStart_ + L_.groupLen, L_.segEndTick);
  j_ = 0;
  jInTuplet_ = 0;
}

bool GridLineCursor::next(GridLine& out) noexcept {
  if (done_) return false;
  Ticks pos = linePos(L_, groupStart_, j_);
  if (pos >= groupEnd_) {  // at most once: a fresh group's first line is its start, < its end
    advanceGroup();
    pos = groupStart_;
  }
  if (pos >= to_) {
    done_ = true;
    return false;
  }
  const Ticks off = pos - groupStart_;
  int64_t barsIn = 0;
  Ticks inBar = off;
  if (L_.groupBars > 1) {
    barsIn = off / L_.barLen;
    inBar = off - barsIn * L_.barLen;
  }
  out.tick = pos;
  out.bar = groupBar_ + barsIn;
  out.tickInBar = inBar;
  out.sig = L_.sig;
  // 0 ≤ inBar < barLen ≤ 1024 whole notes = 9.1·10⁸ ticks < 2^32: a 32-bit modulo is enough (and cheaper).
  const bool onBeat = static_cast<uint32_t>(inBar) % static_cast<uint32_t>(L_.beatLen) == 0;
  out.strength = inBar == 0                ? GridStrength::Bar
                 : onBeat                  ? GridStrength::Beat
                 : jInTuplet_ == 0         ? GridStrength::Subdivision
                                           : GridStrength::Tuplet;
  ++j_;
  if (++jInTuplet_ == L_.tuplet) jInTuplet_ = 0;
  return true;
}

// ---- Quantize ---------------------------------------------------------------------------------------------------

TickRange quantize(const Grid& grid, Ticks start, Ticks end, int amountPercent, QuantizeMode mode) noexcept {
  if (!grid.enabled()) return {start, end};
  const int64_t a = std::clamp(amountPercent, 0, 100);
  const Ticks qs = grid.nearestLine(start);
  const Ticks ns = start + roundDiv((qs - start) * a, 100);
  if (mode == QuantizeMode::StartOnly) return {ns, ns + (end - start)};
  Ticks qe = grid.nearestLine(end);
  if (qe <= qs) qe = grid.lineAfter(qs);
  Ticks ne = end + roundDiv((qe - end) * a, 100);
  if (ne <= ns) ne = ns + 1;
  return {ns, ne};
}

}  // namespace pq
