#include "pq/engine/sequence.h"

#include <algorithm>
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

bool before(const SequenceImage::Note& a, const SequenceImage::Note& b) noexcept {
  return a.start != b.start ? a.start < b.start : a.id < b.id;
}

Error invalid(const std::string& what) { return Error{ErrorCode::InvalidArgument, "SequenceImage: " + what, {}}; }

}  // namespace

NoteRandom noteRandom(uint64_t seed, uint32_t noteId, int64_t pass) noexcept {  // RT-safe
  const uint64_t h1 = splitmix64(seed ^ splitmix64((static_cast<uint64_t>(noteId) << 32) ^ static_cast<uint64_t>(pass)));
  const uint64_t h2 = splitmix64(h1);
  return {unitFloat(h1), unitFloat(h2)};
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

  std::vector<uint32_t> order;
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
      for (const NoteDesc& nd : cd.notes) {
        if (nd.length < 1) return invalid("note " + std::to_string(nd.id) + " has length < 1");
        if (!std::isfinite(nd.hz) || !(nd.hz > 0.0f)) return invalid("note " + std::to_string(nd.id) + " has a bad Hz");
        if (!std::isfinite(nd.velocity) || !std::isfinite(nd.releaseVelocity) || !std::isfinite(nd.chance) ||
            !std::isfinite(nd.velocityRange) || !std::isfinite(nd.ratio) || !std::isfinite(nd.harmonic))
          return invalid("note " + std::to_string(nd.id) + " has a non-finite field");
        if (!nd.enabled || !(nd.chance > 0.0f) || !playable(nd.start)) continue;
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
        img->notes_.push_back(n);
      }
      c.noteEnd = static_cast<uint32_t>(img->notes_.size());
      const auto nb = img->notes_.begin() + c.noteBegin, ne = img->notes_.begin() + c.noteEnd;
      if (!std::is_sorted(nb, ne, before)) std::sort(nb, ne, before);
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
