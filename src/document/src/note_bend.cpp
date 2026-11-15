#include "pq/document/note_bend.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "pq/document/automation_curve.h"
#include "pq/document/pitch_resolve.h"

namespace pq::doc {

namespace {

std::optional<double> centsBetween(std::optional<double> from, std::optional<double> to) noexcept {
  if (!from || !to || !(*from > 0.0) || !(*to > 0.0)) return std::nullopt;
  const double c = 1200.0 * std::log2(*to / *from);
  return std::isfinite(c) ? std::optional<double>(c) : std::nullopt;
}

// The two bends of a note as spans of note ticks: in over [0, a], out over [L − b, L].
struct Bends {
  double in = 0.0, out = 0.0;  // cents; 0: none
  Ticks a = 0, b = 0, length = 1;
  const GlideShape* inShape = nullptr;
  const GlideShape* outShape = nullptr;

  Bends(const NoteCurves& c, Ticks noteLength, const NoteBendTargets& t) noexcept : length(std::max<Ticks>(noteLength, 1)) {
    if (c.bendIn.set() && t.in && c.bendIn.length > 0) {
      in = *t.in;
      a = std::min(c.bendIn.length, length);
      inShape = &c.bendIn.shape;
    }
    if (c.bendOut.set() && t.out && c.bendOut.length > 0) {
      out = *t.out;
      b = std::min(c.bendOut.length, length);
      outShape = &c.bendOut.shape;
    }
  }
  bool any() const noexcept { return a > 0 || b > 0; }
  bool none() const noexcept { return !any(); }
  double at(double t) const noexcept {
    double v = 0.0;
    if (a > 0 && t < static_cast<double>(a)) v += in * (1.0 - glideShapeValue(*inShape, t / static_cast<double>(a)));
    if (b > 0 && t > static_cast<double>(length - b))
      v += out * glideShapeValue(*outShape, (t - static_cast<double>(length - b)) / static_cast<double>(b));
    return v;
  }
};

const NoteCurves kNoCurves{};

// warp(t): how far the note is from where it started, through its group's warps.
struct Warp {
  const NoteWarp* w = nullptr;  // null: none
  double w0 = 0.0;              // the state the note starts in
  explicit Warp(const NoteWarp& warp) noexcept
      : w(warp.any() ? &warp : nullptr), w0(w ? jiWarpWritten(warp.warps, static_cast<double>(warp.offset)) : 0.0) {}
  bool any() const noexcept { return w != nullptr; }
  double at(double t, bool before = false) const noexcept {
    if (w == nullptr) return 0.0;
    const double s = jiWarpWritten(w->warps, static_cast<double>(w->offset) + t, before);
    return s == w0 ? 0.0 : (s - w0) * w->cents;
  }
};

}  // namespace

NotePitchIndex::NotePitchIndex(const ClipPitches& pitches) : warped_(pitches.warped) {
  byId_.reserve(pitches.notes.size());
  for (const NotePitch& np : pitches.notes) byId_.emplace_back(np.note.value, np.hz ? *np.hz : 0.0);
  std::sort(byId_.begin(), byId_.end());
}

const NoteWarpState* NotePitchIndex::warped(NoteId note) const noexcept {
  const auto it = std::lower_bound(warped_.begin(), warped_.end(), note,
                                   [](const NoteWarpState& s, NoteId id) noexcept { return s.note < id; });
  return it != warped_.end() && it->note == note ? &*it : nullptr;
}

NoteWarp NotePitchIndex::warp(const NoteClip& clip, NoteId note) const noexcept {
  const NoteWarpState* s = warped_.empty() ? nullptr : warped(note);
  if (s == nullptr) return {};
  const JiGroup* g = findJiGroup(clip, s->group);
  return g != nullptr ? NoteWarp{g->warps, s->offset, s->cents} : NoteWarp{};
}

std::optional<double> NotePitchIndex::hz(NoteId note) const noexcept {
  const auto it = std::lower_bound(byId_.begin(), byId_.end(), std::pair<uint32_t, double>{note.value, 0.0});
  if (it == byId_.end() || it->first != note.value || !(it->second > 0.0)) return std::nullopt;
  return it->second;
}

NoteBendTargets noteBendTargets(const NoteCurves& c, std::optional<double> noteHz, std::optional<double> inHz,
                                std::optional<double> outHz) noexcept {
  NoteBendTargets t;
  if (c.bendIn.set()) t.in = centsBetween(noteHz, inHz);
  if (c.bendOut.set()) t.out = centsBetween(noteHz, outHz);
  return t;
}

NoteBendTargets noteBendTargets(const NoteCurves& c, const NotePitchIndex& pitches) noexcept {
  if (!c.bendIn.set() && !c.bendOut.set()) return {};
  return noteBendTargets(c, pitches.hz(c.note), c.bendIn.set() ? pitches.hz(c.bendIn.other) : std::nullopt,
                         c.bendOut.set() ? pitches.hz(c.bendOut.other) : std::nullopt);
}

double noteCurveCents(const NoteCurves* c, Ticks noteLength, const NoteBendTargets& targets, const NoteWarp& warp,
                      double tick) noexcept {
  const NoteCurves& nc = c != nullptr ? *c : kNoCurves;
  const Bends bends(nc, noteLength, targets);
  tick = std::clamp(tick, 0.0, static_cast<double>(bends.length));
  return evalAutomation(std::span<const Breakpoint>(nc.pitch), tick, 0.0) + bends.at(tick) + Warp(warp).at(tick);
}

std::vector<Breakpoint> effectiveNoteCurve(const NoteCurves* record, Ticks noteLength, const NoteBendTargets& targets,
                                           const NoteWarp& noteWarp, double toleranceCents) {
  const NoteCurves& c = record != nullptr ? *record : kNoCurves;
  const Bends bends(c, noteLength, targets);
  const Warp warp(noteWarp);
  if (bends.none() && !warp.any()) return c.pitch;
  const std::span<const Breakpoint> free(c.pitch);
  const Ticks L = bends.length;
  // Knots: where a piece of the curve begins or ends. Between two neighbours it is smooth.
  std::vector<Ticks> knots;
  knots.reserve(c.pitch.size() + 4);
  knots.push_back(0);
  knots.push_back(L);
  if (bends.a > 0) knots.push_back(bends.a);
  if (bends.b > 0) knots.push_back(L - bends.b);
  for (const Breakpoint& p : c.pitch) knots.push_back(p.tick);
  if (warp.any())  // where a warp's glide begins and ends inside the note
    for (const JiWarp& w : noteWarp.warps)
      for (const Ticks t : {w.at - noteWarp.offset, w.at + w.glide - noteWarp.offset})
        if (t > 0 && t < L) knots.push_back(t);
  std::sort(knots.begin(), knots.end());
  knots.erase(std::unique(knots.begin(), knots.end()), knots.end());

  auto inside = [&](double t) { return evalAutomation(free, t, 0.0) + bends.at(t) + warp.at(t); };
  auto from = [&](Ticks t) {
    return automationValueAt(free, t, 0.0) + bends.at(static_cast<double>(t)) + warp.at(static_cast<double>(t));
  };
  auto upTo = [&](Ticks t) {
    return automationValueBefore(free, t, 0.0) + bends.at(static_cast<double>(t)) + warp.at(static_cast<double>(t), true);
  };
  const double tol = std::max(toleranceCents, 1e-6);
  std::vector<Breakpoint> out;
  auto push = [&](Ticks t, double v) {
    out.push_back(Breakpoint{t, static_cast<float>(std::clamp(v, -static_cast<double>(kMaxCurveCents),
                                                               static_cast<double>(kMaxCurveCents))),
                             0.0f, {}});
  };
  // Interior points of the smooth span (x0, x1) until the polyline is within the tolerance: bisection, the chord
  // checked at its quarter points (an S-curve's chord meets it at the middle).
  auto span = [&](auto&& self, Ticks x0, double v0, Ticks x1, double v1, int depth) -> void {
    if (x1 - x0 <= 1 || depth > 24) return;
    double worst = 0.0;
    for (int k = 1; k < 4; ++k) {
      const double t = static_cast<double>(x0) + static_cast<double>(x1 - x0) * k / 4.0;
      worst = std::max(worst, std::fabs(inside(t) - (v0 + (v1 - v0) * k / 4.0)));
    }
    if (worst <= tol) return;
    const Ticks mid = x0 + (x1 - x0) / 2;
    const double vm = inside(static_cast<double>(mid));
    self(self, x0, v0, mid, vm, depth + 1);
    push(mid, vm);
    self(self, mid, vm, x1, v1, depth + 1);
  };
  for (size_t i = 0; i < knots.size(); ++i) {
    const Ticks t = knots[i];
    const double before = upTo(t), at = from(t);
    if (i == 0 || before != at) {
      if (i > 0) push(t, before);
      push(t, at);
    } else {
      push(t, at);
    }
    if (i + 1 < knots.size()) span(span, t, at, knots[i + 1], upTo(knots[i + 1]), 0);
  }
  // A warp that does not reach the note (it starts and stays in one state): no curve at all.
  if (bends.none() && c.pitch.empty() && std::ranges::all_of(out, [](const Breakpoint& b) { return b.value == 0.0f; })) out.clear();
  return out;
}

std::vector<Breakpoint> effectiveNoteCurve(const NoteClip& clip, const Note& note, const NotePitchIndex& pitches,
                                           double toleranceCents) {
  const NoteCurves* c = findNoteCurves(clip, note.id);
  const NoteWarp warp = pitches.anyWarp() ? pitches.warp(clip, note.id) : NoteWarp{};
  if (!c && !warp.any()) return {};
  return effectiveNoteCurve(c, std::max<Ticks>(note.length, 1), c ? noteBendTargets(*c, pitches) : NoteBendTargets{}, warp,
                            toleranceCents);
}

}  // namespace pq::doc
