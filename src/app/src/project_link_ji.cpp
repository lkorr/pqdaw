#include "project_link_ji.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <span>

#include "pq/core/automation_curve.h"
#include "pq/document/lower.h"
#include "pq/document/pitch_resolve.h"

namespace pq::app {

namespace {

using Point = ClipCurveLowering::Point;

// Drops the points of curved spans that follow the last kept one by less than `spacing`. Corners stay: the first and
// last point, both points of a jump, the ends of a flat stretch.
void thin(std::vector<Point>& p, Ticks spacing) {
  const size_t n = p.size();
  if (n < 3 || spacing <= 1) return;
  size_t kept = 1;
  for (size_t i = 1; i < n; ++i) {
    const bool corner = i + 1 == n || p[i].tick == p[i - 1].tick || p[i].tick == p[i + 1].tick ||
                        p[i].value == p[i - 1].value || p[i].value == p[i + 1].value;
    if (corner || p[i].tick - p[kept - 1].tick >= spacing) p[kept++] = p[i];
  }
  p.resize(kept);
}

// A note curve (the automation curve law between breakpoints, ticks from the note's start) as lines within
// kJiPitchTolerance, in content ticks.
void flatten(const std::vector<doc::Breakpoint>& pts, Ticks origin, std::vector<Point>& out) {
  out.clear();
  for (size_t i = 0; i < pts.size(); ++i) {
    const doc::Breakpoint& p = pts[i];
    out.push_back({origin + p.tick, static_cast<double>(p.value)});
    if (i + 1 == pts.size()) break;
    const doc::Breakpoint& q = pts[i + 1];
    const double k = kAutomationCurveK * static_cast<double>(p.curve);
    const double delta = std::fabs(static_cast<double>(q.value) - static_cast<double>(p.value));
    if (q.tick <= p.tick + 1 || std::fabs(k) < kAutomationLinearK || delta == 0.0) continue;
    // |s''| ≤ k² · max(1, e^k) / |e^k − 1|; a chord of n equal steps is within delta · |s''| / (8 n²).
    const double bend = k * k * std::max(1.0, std::exp(k)) / std::fabs(std::expm1(k));
    const double want = std::ceil(std::sqrt(delta * bend / (8.0 * kJiPitchTolerance)));
    const auto n = static_cast<Ticks>(std::clamp(want, 1.0, static_cast<double>(std::min<Ticks>(q.tick - p.tick, 512))));
    for (Ticks s = 1; s < n; ++s) {
      const Ticks t = p.tick + (q.tick - p.tick) * s / n;
      if (origin + t <= out.back().tick) continue;
      out.push_back({origin + t, automationSegmentValue(p, q, static_cast<double>(t))});
    }
  }
}

}  // namespace

ClipCurveLowering::ClipCurveLowering(const doc::NoteClip& clip, const TempoMap& tempo, bool tempoMoves,
                                     const doc::ClipPitches& pitches)
    : clip_(clip), tempo_(tempo), tempoMoves_(tempoMoves), pitches_(pitches) {
  // voice.harmonic: every numbered note of every group, once per clip. One pass over the notes for all groups
  // (T7.16b, review M17; was doc::groupHarmonics per group: a pass over the clip each). The same arithmetic as
  // model.cpp's deriveGroup, in the same note order per group: the group's m is the lcm of its members' reduced
  // denominators over gcd(d, N), capped (a member too fine for any numbering leaves m as it was); anchor = N · m, a
  // member's number = N · m · its ratio when that is whole.
  const std::span<const doc::JiGroup> groups = doc::jiGroupsOf(clip);
  if (groups.empty()) return;
  struct G {
    uint32_t anchor;
    uint32_t index;  // into groups
  };
  std::vector<G> byAnchor;
  byAnchor.reserve(groups.size());
  for (uint32_t i = 0; i < groups.size(); ++i)
    if (groups[i].index >= 1 && clip.notes.findById(groups[i].anchor) != nullptr) byAnchor.push_back({groups[i].anchor.value, i});
  std::stable_sort(byAnchor.begin(), byAnchor.end(), [](const G& a, const G& b) { return a.anchor < b.anchor; });
  auto groupOf = [&](doc::NoteId anchor) -> const G* {
    const auto it = std::lower_bound(byAnchor.begin(), byAnchor.end(), anchor.value, [](const G& g, uint32_t id) { return g.anchor < id; });
    return it != byAnchor.end() && it->anchor == anchor.value ? &*it : nullptr;
  };
  std::vector<int64_t> m(groups.size(), 1);
  std::vector<std::pair<const doc::Note*, uint32_t>> members;  // (member, group index), in note order
  for (const doc::Note& n : clip.notes) {
    const doc::NoteId a = doc::noteAnchorOf(n.pitch);
    if (!a.valid() || a == n.id) continue;
    const G* g = groupOf(a);
    if (g == nullptr) continue;
    const int64_t N = groups[g->index].index;
    const tuning::Rational q = n.pitch.ratio();
    if (q.isPositive()) {
      const int64_t need = q.den() / std::gcd(q.den(), N);
      const int64_t next = doc::lcmCapped(m[g->index], need, doc::kMaxFundamentalDenominator / N);
      if (next > 0) m[g->index] = next;
    }
    members.emplace_back(&n, g->index);
  }
  harmonics_.reserve(byAnchor.size() + members.size());
  for (const G& g : byAnchor) harmonics_.emplace_back(groups[g.index].anchor, static_cast<int64_t>(groups[g.index].index) * m[g.index]);
  for (const auto& [n, gi] : members) {
    const std::optional<tuning::Rational> h = tuning::mul(tuning::Rational(static_cast<int64_t>(groups[gi].index) * m[gi]), n->pitch.ratio());
    if (h && h->den() == 1 && h->num() >= 1) harmonics_.emplace_back(n->id, h->num());
  }
  std::sort(harmonics_.begin(), harmonics_.end());
}

double ClipCurveLowering::bpmAt(Ticks contentTick) const noexcept {
  return tempo_.bpmAt(std::max<Ticks>(clip_.start + (contentTick - clip_.contentStart), 0));
}

Ticks ClipCurveLowering::ticksOf(double seconds, Ticks contentTick) const noexcept {
  return std::max<Ticks>(1, std::llround(seconds * bpmAt(contentTick) / 60.0 * static_cast<double>(kPpq)));
}

void ClipCurveLowering::emit(const std::vector<Point>& pts, std::vector<engine::CurvePoint>& out) const {
  out.clear();
  bool any = false;
  for (const Point& p : pts) any |= std::fabs(p.value) > 1e-9;
  if (!any) return;  // the note's own pitch throughout
  const double lim = static_cast<double>(engine::kMaxCurveCents);
  const Ticks cut = kPpq / 4;
  out.reserve(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) {
    if (tempoMoves_ && i > 0 && pts[i].tick - pts[i - 1].tick > cut && pts[i].value != pts[i - 1].value) {
      // Inside a tempo ramp a line over ticks is not a line over samples: shorter pieces.
      const Point a = pts[i - 1], b = pts[i];
      const Ticks n = (b.tick - a.tick + cut - 1) / cut;
      for (Ticks s = 1; s < n; ++s) {
        const Ticks t = a.tick + (b.tick - a.tick) * s / n;
        const double v = a.value + (b.value - a.value) * static_cast<double>(t - a.tick) / static_cast<double>(b.tick - a.tick);
        out.push_back({t, static_cast<float>(std::clamp(v, -lim, lim))});
      }
    }
    out.push_back({pts[i].tick, static_cast<float>(std::clamp(pts[i].value, -lim, lim))});
  }
}

void ClipCurveLowering::note(const doc::Note& n, engine::NoteDesc& nd) {
  if (!harmonics_.empty()) {
    const auto it = std::lower_bound(harmonics_.begin(), harmonics_.end(), std::pair<doc::NoteId, int64_t>{n.id, 0});
    if (it != harmonics_.end() && it->first == n.id) nd.harmonic = static_cast<float>(it->second);
  }
  const doc::NoteCurves* own = doc::findNoteCurves(clip_, n.id);
  const bool bends = own != nullptr && (own->bendIn.set() || own->bendOut.set() || own->vibrato.set());  // T7.14
  if (own == nullptr && pitches_.warped.empty()) return;
  // The stored free curve as it is, unless a bend or a warp moves the note (then the one evaluation, flattened: an
  // allocation per such note; the pitch index is built for the first of them).
  std::vector<doc::Breakpoint> effective;
  const std::vector<doc::Breakpoint>* points = own != nullptr ? &own->pitch : nullptr;
  if (bends || !pitches_.warped.empty()) {
    if (!pitchIndex_) pitchIndex_.emplace(pitches_);
    const doc::NoteWarp warp = pitchIndex_->warp(clip_, n.id);
    const doc::NoteBendTargets targets = bends ? doc::noteBendTargets(*own, *pitchIndex_) : doc::NoteBendTargets{};
    if (targets.any() || warp.any()) {
      effective = doc::effectiveNoteCurve(own, std::max<Ticks>(n.length, 1), targets, warp, kJiPitchTolerance);
      points = &effective;
    }
  }
  if (points == nullptr || points->empty()) return;
  flatten(*points, n.start, a_);
  thin(a_, ticksOf(kJiMinSpacingSeconds, n.start));
  emit(a_, nd.pitchCurve);
}

engine::ClipDesc lowerClipDesc(const doc::Project& p, const doc::NoteClip& c, const doc::ClipPitches& cp,
                               std::span<const doc::TuningId> signals, bool tempoMoves) {
  // How a note follows its tuning node (PitchSource), from what the document resolved.
  auto setFollow = [&](engine::NoteDesc& nd, const doc::NotePitch& np) {
    if (np.follow == doc::PitchFollow::Static) return;
    const uint32_t node = doc::tuningNodeIndex(signals, np.signal);
    if (node == 0) return;
    nd.pitch.tuning = np.signal.value;
    nd.pitch.node = static_cast<uint16_t>(node);
    nd.pitch.kind = np.follow == doc::PitchFollow::Degree ? engine::PitchFollowKind::Degree
                                                           : engine::PitchFollowKind::Reference;
    if (np.follow == doc::PitchFollow::Degree) {
      nd.pitch.degree = np.degree;
      nd.pitch.equave = np.equave;
    }
  };
  engine::ClipDesc cd;
  cd.id = c.id.value;
  cd.start = c.start;
  cd.length = std::max<Ticks>(c.length, 1);
  cd.contentStart = c.contentStart;
  cd.loopEnabled = c.loop.enabled;
  cd.loopStart = c.loop.start;
  cd.loopEnd = c.loop.end;
  cd.active = c.active;
  // T7.13: the clip's note curves, bends, warps and harmonic numbers (above).
  std::optional<ClipCurveLowering> curves;
  if (ClipCurveLowering::needed(c)) curves.emplace(c, p.timeline->tempoMap, tempoMoves, cp);
  cd.notes.reserve(c.notes.size());
  size_t ni = 0;
  for (const doc::Note& n : c.notes) {
    const doc::NotePitch& np = cp.notes[ni++];
    if (!np.hz) continue;
    const double hz = *np.hz;
    if (!(hz > 0.0) || !std::isfinite(static_cast<float>(hz))) continue;
    engine::NoteDesc nd;
    setFollow(nd, np);
    nd.start = n.start;
    nd.length = std::max<Ticks>(n.length, 1);
    nd.id = n.id.value;
    nd.hz = static_cast<float>(hz);
    if (np.keyDegree) nd.keyDegree = static_cast<float>(*np.keyDegree);
    nd.velocity = std::clamp(n.velocity, 1.0f, 127.0f);
    nd.releaseVelocity = std::clamp(n.releaseVelocity, 0.0f, 127.0f);
    nd.chance = std::clamp(n.chance, 0.0f, 1.0f);
    nd.velocityRange = std::clamp(n.velocityRange, -127.0f, 127.0f);
    if (n.pitch.kind() == tuning::PitchKind::Ratio && n.pitch.rawDen() != 0)
      nd.ratio = static_cast<float>(static_cast<double>(n.pitch.rawNum()) / static_cast<double>(n.pitch.rawDen()));
    nd.enabled = n.enabled();
    if (curves) curves->note(n, nd);
    cd.notes.push_back(std::move(nd));
  }
  return cd;
}

}  // namespace pq::app
