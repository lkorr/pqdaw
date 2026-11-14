#include "pq/engine/sequence.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <string>

#include "pq/core/clip_time.h"

namespace pq::engine {

namespace {

constexpr uint64_t splitmix64(uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// Top 24 bits → [0, 1) exactly representable in float32.
constexpr float unitFloat(uint64_t h) noexcept { return static_cast<float>(h >> 40) * (1.0f / 16777216.0f); }

Error invalid(const std::string& what) { return Error{ErrorCode::InvalidArgument, "SequenceImage: " + what, {}}; }

// T7.1: appends a cleaned copy of `in` to `out`: sorted by tick (stably), and of several points at one tick only
// the first and the last (a jump needs no more). Returns the number of points appended.
size_t appendCurve(const std::vector<CurvePoint>& in, std::vector<CurvePoint>& out) {
  const size_t begin = out.size();
  out.insert(out.end(), in.begin(), in.end());
  const auto first = out.begin() + static_cast<std::ptrdiff_t>(begin);
  const auto byTick = [](const CurvePoint& a, const CurvePoint& b) noexcept { return a.tick < b.tick; };
  if (!std::is_sorted(first, out.end(), byTick)) std::stable_sort(first, out.end(), byTick);
  size_t w = begin;
  for (size_t r = begin; r < out.size(); ++r) {
    if (w >= begin + 2 && out[w - 1].tick == out[r].tick && out[w - 2].tick == out[r].tick) out[w - 1] = out[r];
    else out[w++] = out[r];
  }
  out.resize(w);
  return w - begin;
}

}  // namespace

NoteRandom noteRandom(uint64_t seed, uint32_t noteId, int64_t pass) noexcept {  // RT-safe
  const uint64_t h1 = splitmix64(seed ^ splitmix64((static_cast<uint64_t>(noteId) << 32) ^ static_cast<uint64_t>(pass)));
  const uint64_t h2 = splitmix64(h1);
  return {unitFloat(h1), unitFloat(h2)};
}

namespace {

// T = Ticks or double (a fractional tick; ticks are exact in a double far beyond any project length).
template <class T>
float curveValueImpl(std::span<const CurvePoint> curve, T t, float empty, uint32_t* after) noexcept {  // RT-safe
  const CurvePoint* b = curve.data();
  const CurvePoint* e = b + curve.size();
  const CurvePoint* it =
      std::upper_bound(b, e, t, [](T v, const CurvePoint& p) noexcept { return v < static_cast<T>(p.tick); });
  if (after != nullptr) *after = static_cast<uint32_t>(it - b);
  if (b == e) return empty;
  if (it == b) return b->value;
  const CurvePoint& p0 = *(it - 1);
  if (it == e || static_cast<T>(p0.tick) == t) return p0.value;
  const double x = static_cast<double>(t - static_cast<T>(p0.tick)) / static_cast<double>(it->tick - p0.tick);
  return static_cast<float>(static_cast<double>(p0.value) + (static_cast<double>(it->value) - p0.value) * x);
}

}  // namespace

float SequenceImage::curveValue(std::span<const CurvePoint> curve, Ticks t, float empty,
                                uint32_t* after) noexcept {  // RT-safe
  return curveValueImpl<Ticks>(curve, t, empty, after);
}

float SequenceImage::curveValueAt(std::span<const CurvePoint> curve, double tick, float empty) noexcept {  // RT-safe
  return curveValueImpl<double>(curve, tick, empty, nullptr);
}

Result<std::unique_ptr<SequenceImage>> SequenceImage::build(std::span<const TrackSequenceDesc> tracks,
                                                            const SequenceBuildOptions& options) {
  auto img = std::unique_ptr<SequenceImage>(new SequenceImage());
  img->seed_ = options.seed;
  size_t totalClips = 0, totalNotes = 0;
  for (const TrackSequenceDesc& t : tracks) {
    totalClips += t.clips.size();
    for (const ClipDesc& c : t.clips) totalNotes += c.notes.size();
  }
  if (totalNotes >= 0xFFFFFFFFull || totalClips >= 0xFFFFFFFFull) return invalid("too many notes or clips");
  img->tracks_.reserve(tracks.size());
  img->clips_.reserve(totalClips);
  img->notes_.reserve(totalNotes);

  std::vector<uint32_t> order, play, curved;  // clip order; a clip's playable notes; the curved notes' indices
  for (const TrackSequenceDesc& td : tracks) {
    Track tr{td.id, static_cast<uint32_t>(img->clips_.size()), 0};
    // Active clips by start (stable: at equal starts the later one in the description comes last and wins).
    order.clear();
    for (uint32_t i = 0; i < td.clips.size(); ++i) {
      const ClipDesc& c = td.clips[i];
      if (c.length < 1) return invalid("clip " + std::to_string(c.id) + " has length < 1");
      if (c.active) order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return td.clips[a].start < td.clips[b].start; });
    for (size_t k = 0; k < order.size(); ++k) {
      const ClipDesc& cd = td.clips[order[k]];
      Clip c{};
      c.id = cd.id;
      c.start = cd.start;
      c.end = cd.start + cd.length;
      if (k + 1 < order.size()) c.end = std::min(c.end, td.clips[order[k + 1]].start);  // overwritten by the next
      if (c.end <= c.start) continue;
      // The clip-time rules are pq/core/clip_time.h, shared with the document (M3 review L16).
      const ContentLoop loop{cd.loopEnabled, cd.loopStart, cd.loopEnd};
      const bool looped = contentLooped(loop);
      const Ticks loopLen = looped ? cd.loopEnd - cd.loopStart : 0;
      c.content0 = clipContentAt(cd.contentStart, loop, 0);
      c.firstPassEnd = looped ? std::min(c.end, c.start + clipFirstPassLength(cd.contentStart, loop)) : c.end;
      const bool loopPasses = looped && c.firstPassEnd < c.end;
      c.loopStart = looped ? cd.loopStart : 0;
      c.loopLen = loopPasses ? loopLen : 0;
      // Content ranges that can play: pass 0 [content0, pass0To) and, with later passes, [loopStart, loopEnd).
      const Ticks pass0To = looped ? cd.loopEnd : c.content0 + (c.end - c.start);
      auto playable = [&](Ticks s) noexcept {
        return (s >= c.content0 && s < pass0To) || (loopPasses && s >= cd.loopStart && s < cd.loopEnd);
      };
      c.noteBegin = static_cast<uint32_t>(img->notes_.size());
      // The playable notes in (start, id) order (T7.1: emitted in that order, so curved notes are appended to the
      // curve tables in note order).
      play.clear();
      for (uint32_t ni = 0; ni < cd.notes.size(); ++ni) {
        const NoteDesc& nd = cd.notes[ni];
        if (nd.length < 1) return invalid("note " + std::to_string(nd.id) + " has length < 1");
        if (!std::isfinite(nd.hz) || !(nd.hz > 0.0f)) return invalid("note " + std::to_string(nd.id) + " has a bad Hz");
        if (!std::isfinite(nd.velocity) || !std::isfinite(nd.releaseVelocity) || !std::isfinite(nd.chance) ||
            !std::isfinite(nd.velocityRange) || !std::isfinite(nd.ratio) || !std::isfinite(nd.harmonic))
          return invalid("note " + std::to_string(nd.id) + " has a non-finite field");
        for (const CurvePoint& p : nd.pitchCurve)
          if (!std::isfinite(p.value) || std::fabs(p.value) > kMaxCurveCents)
            return invalid("note " + std::to_string(nd.id) + " has a bad pitch-curve value");
        for (const CurvePoint& p : nd.gainCurve)
          if (!std::isfinite(p.value) || p.value < 0.0f)
            return invalid("note " + std::to_string(nd.id) + " has a bad gain-curve value");
        if (!nd.enabled || !(nd.chance > 0.0f) || !playable(nd.start)) continue;
        play.push_back(ni);
      }
      const auto earlier = [&](uint32_t a, uint32_t b) noexcept {
        const NoteDesc& x = cd.notes[a];
        const NoteDesc& y = cd.notes[b];
        return x.start != y.start ? x.start < y.start : x.id < y.id;
      };
      if (!std::is_sorted(play.begin(), play.end(), earlier)) std::sort(play.begin(), play.end(), earlier);
      for (const uint32_t ni : play) {
        const NoteDesc& nd = cd.notes[ni];
        Note n{};
        n.start = nd.start;
        n.length = nd.length;
        n.id = nd.id;
        n.hz = nd.hz;
        n.velocity = nd.velocity;
        n.releaseVelocity = nd.releaseVelocity;
        n.chance = nd.chance;
        n.velocityRange = nd.velocityRange;
        n.ratio = nd.ratio;
        n.harmonic = nd.harmonic;
        n.pitch = nd.pitch;
        n.keyDegree = nd.keyDegree;
        if (!nd.pitchCurve.empty() || !nd.gainCurve.empty()) {  // T7.1: header (hash), pitch points, gain points
          std::vector<CurvePoint>& cv = img->curves_;
          if (cv.size() + 1 + nd.pitchCurve.size() + nd.gainCurve.size() >= 0xFFFFFFFFull)
            return invalid("too many curve points");
          NoteCurves ref;
          ref.begin = static_cast<uint32_t>(cv.size());
          cv.push_back(CurvePoint{});
          const size_t np = appendCurve(nd.pitchCurve, cv);
          const size_t ng = appendCurve(nd.gainCurve, cv);
          if (np > kMaxCurvePoints || ng > kMaxCurvePoints)
            return invalid("note " + std::to_string(nd.id) + " has a curve with more than " +
                           std::to_string(kMaxCurvePoints) + " points");
          ref.pitchCount = static_cast<uint16_t>(np);
          ref.gainCount = static_cast<uint16_t>(ng);
          uint64_t h = splitmix64((static_cast<uint64_t>(np) << 32) | ng);
          for (size_t i = ref.begin + 1; i < cv.size(); ++i)
            h = splitmix64(h ^ splitmix64(static_cast<uint64_t>(cv[i].tick - nd.start)) ^
                           (static_cast<uint64_t>(std::bit_cast<uint32_t>(cv[i].value)) << 17));
          cv[ref.begin].tick = static_cast<Ticks>(h);
          img->curveRefs_.push_back(ref);
          curved.push_back(static_cast<uint32_t>(img->notes_.size()));
        }
        img->notes_.push_back(n);
      }
      c.noteEnd = static_cast<uint32_t>(img->notes_.size());
      const auto nb = img->notes_.begin() + c.noteBegin, ne = img->notes_.begin() + c.noteEnd;
      auto firstAtOrAfter = [&](Ticks s) {
        return static_cast<uint32_t>(
            std::lower_bound(nb, ne, s, [](const Note& n, Ticks v) noexcept { return n.start < v; }) -
            img->notes_.begin());
      };
      c.pass0Begin = firstAtOrAfter(c.content0);
      c.pass0End = firstAtOrAfter(pass0To);
      c.loopBegin = loopPasses ? firstAtOrAfter(cd.loopStart) : c.noteEnd;
      c.loopEnd = loopPasses ? firstAtOrAfter(cd.loopEnd) : c.noteEnd;
      img->clips_.push_back(c);
    }
    tr.clipEnd = static_cast<uint32_t>(img->clips_.size());
    img->tracks_.push_back(tr);
  }
  if (!curved.empty()) {  // T7.1: the bit per note and the rank per 64 notes (curvesOf)
    const size_t words = img->notes_.size() / 64 + 1;
    img->curveBits_.assign(words, 0);
    img->curveRank_.assign(words, 0);
    for (const uint32_t i : curved) img->curveBits_[i >> 6] |= uint64_t{1} << (i & 63u);
    uint32_t before = 0;
    for (size_t w = 0; w < words; ++w) {
      img->curveRank_[w] = before;
      before += static_cast<uint32_t>(std::popcount(img->curveBits_[w]));
    }
  }
  return img;
}

const SequenceImage::Note* SequenceImage::findOccurrence(uint32_t track, uint32_t noteId, Ticks on,
                                                         Ticks* passEnd) const noexcept {  // RT-safe
  if (track >= tracks_.size()) return nullptr;
  const Track& t = tracks_[track];
  const Clip* cb = clips_.data() + t.clipBegin;
  const Clip* ce = clips_.data() + t.clipEnd;
  // The clip containing `on`: the last one starting at or before it.
  const Clip* it = std::upper_bound(cb, ce, on, [](Ticks v, const Clip& c) noexcept { return v < c.start; });
  if (it == cb) return nullptr;
  const Clip& c = *(it - 1);
  if (on >= c.end) return nullptr;
  Ticks content, end;
  uint32_t lo, hi;
  if (c.loopLen == 0 || on < c.firstPassEnd) {
    content = c.content0 + (on - c.start);
    end = c.firstPassEnd;
    lo = c.pass0Begin;
    hi = c.pass0End;
  } else {
    const Ticks o = on - c.firstPassEnd;
    const Ticks k = o / c.loopLen;
    content = c.loopStart + (o - k * c.loopLen);
    end = std::min(c.end, c.firstPassEnd + (k + 1) * c.loopLen);
    lo = c.loopBegin;
    hi = c.loopEnd;
  }
  const Note* nb = notes_.data() + lo;
  const Note* ne = notes_.data() + hi;
  const Note* n = std::lower_bound(nb, ne, content, [noteId](const Note& x, Ticks v) noexcept {
    return x.start != v ? x.start < v : x.id < noteId;
  });
  if (n == ne || n->start != content || n->id != noteId) return nullptr;
  if (passEnd != nullptr) *passEnd = end;
  return n;
}

}  // namespace pq::engine
