#include "pq/document/note_seq.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>

namespace pq::doc {

namespace {

Error invalid(std::string msg) { return Error{ErrorCode::InvalidArgument, std::move(msg), {}}; }

bool inRange(float v, float lo, float hi) noexcept { return std::isfinite(v) && v >= lo && v <= hi; }

NoteKey lastKey(const NoteSeq::Chunk& c) noexcept { return keyOf(c.notes[c.count - 1]); }

bool keyLess(const Note& a, const Note& b) noexcept { return keyOf(a) < keyOf(b); }

// One immutable chunk holding exactly `count` notes copied from `src`.
NoteSeq::Chunk makeChunk(const Note* src, uint32_t count) {
  std::shared_ptr<Note[]> buf = std::make_shared_for_overwrite<Note[]>(count);
  std::memcpy(buf.get(), src, sizeof(Note) * count);
  return NoteSeq::Chunk{std::move(buf), count};
}

// Appends `total` notes from `src` as ceil(total / max) chunks of balanced size (so a split of a full chunk
// yields two half-full chunks with room for further inserts).
void appendSplit(std::vector<NoteSeq::Chunk>& out, const Note* src, size_t total) {
  if (total == 0) return;
  const size_t pieces = (total + kMaxNotesPerChunk - 1) / kMaxNotesPerChunk;
  size_t done = 0;
  for (size_t p = 0; p < pieces; ++p) {
    const size_t end = total * (p + 1) / pieces;
    out.push_back(makeChunk(src + done, static_cast<uint32_t>(end - done)));
    done = end;
  }
}

// Sorted view of a span: the span itself when already sorted (the common one-element case allocates nothing),
// otherwise a sorted copy held in `storage`.
template <class T, class Less>
std::span<const T> sortedView(std::span<const T> in, std::vector<T>& storage, Less less) {
  if (std::is_sorted(in.begin(), in.end(), less)) return in;
  storage.assign(in.begin(), in.end());
  std::sort(storage.begin(), storage.end(), less);
  return storage;
}

}  // namespace

Status validateNote(const Note& n) {
  if (!n.id.valid()) return invalid("note has no id");
  if (n.start < -kMaxTicks || n.start > kMaxTicks) return invalid("note start out of range");
  if (n.length < 1 || n.length > kMaxTicks) return invalid("note length must be at least one tick");
  if (!inRange(n.velocity, 0.0f, 127.0f) || n.velocity == 0.0f) return invalid("note velocity must be in (0, 127]");
  if (!inRange(n.releaseVelocity, 0.0f, 127.0f)) return invalid("release velocity must be in [0, 127]");
  if (!inRange(n.chance, 0.0f, 1.0f)) return invalid("note chance must be in [0, 1]");
  if (!inRange(n.velocityRange, -127.0f, 127.0f)) return invalid("velocity range must be in [-127, 127]");
  if ((n.flags & ~static_cast<uint32_t>(kNoteDisabled)) != 0) return invalid("unknown note flags");
  switch (n.pitch.kind()) {
    case tuning::PitchKind::Cents:
      if (!std::isfinite(n.pitch.cents())) return invalid("cents pitch must be finite");
      break;
    case tuning::PitchKind::Ratio:
      if (!(n.pitch.rawNum() > 0 && n.pitch.rawDen() > 0)) return invalid("ratio pitch must be positive");
      break;
    case tuning::PitchKind::Degree:
    case tuning::PitchKind::Harmonic:
      break;
  }
  return Ok{};
}

Result<NoteSeq> NoteSeq::fromNotes(std::vector<Note> notes) {
  for (const Note& n : notes)
    if (Status s = validateNote(n); !s) return std::move(s).error();
  std::vector<uint32_t> ids(notes.size());
  for (size_t i = 0; i < notes.size(); ++i) ids[i] = notes[i].id.value;
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return invalid("duplicate note id");

  std::sort(notes.begin(), notes.end(), keyLess);
  NoteSeq seq;
  // Bulk build fills chunks completely (the compact layout for loading); edits split them as needed.
  seq.chunks_.reserve((notes.size() + kMaxNotesPerChunk - 1) / kMaxNotesPerChunk);
  for (size_t i = 0; i < notes.size(); i += kMaxNotesPerChunk) {
    const size_t n = std::min<size_t>(kMaxNotesPerChunk, notes.size() - i);
    seq.chunks_.push_back(makeChunk(notes.data() + i, static_cast<uint32_t>(n)));
  }
  seq.size_ = notes.size();
  return seq;
}

size_t NoteSeq::chunkFor(NoteKey key) const noexcept {
  assert(!chunks_.empty());
  size_t lo = 0, hi = chunks_.size() - 1;  // the answer is in [lo, hi]; hi = last chunk is the fallback
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (lastKey(chunks_[mid]) < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

const Note* NoteSeq::find(NoteKey key) const noexcept {
  if (chunks_.empty()) return nullptr;
  const Chunk& c = chunks_[chunkFor(key)];
  const Note* first = c.notes.get();
  const Note* last = first + c.count;
  const Note* it = std::lower_bound(first, last, key, [](const Note& n, NoteKey k) { return keyOf(n) < k; });
  return (it != last && keyOf(*it) == key) ? it : nullptr;
}

const Note* NoteSeq::findById(NoteId id) const noexcept {
  for (const Chunk& c : chunks_)
    for (uint32_t i = 0; i < c.count; ++i)
      if (c.notes[i].id == id) return &c.notes[i];
  return nullptr;
}

std::pair<NoteSeq::Iterator, NoteSeq::Iterator> NoteSeq::range(Ticks from, Ticks to) const noexcept {
  auto lower = [&](Ticks t) -> Iterator {
    if (chunks_.empty()) return end();
    const NoteKey key{t, NoteId{0}};
    const size_t ci = chunkFor(key);
    const Chunk& c = chunks_[ci];
    const Note* it = std::lower_bound(c.notes.get(), c.notes.get() + c.count, key,
                                      [](const Note& n, NoteKey k) { return keyOf(n) < k; });
    const auto idx = static_cast<uint32_t>(it - c.notes.get());
    if (idx == c.count) return Iterator(chunks_.data(), ci + 1, 0);  // past this chunk: start of the next
    return Iterator(chunks_.data(), ci, idx);
  };
  if (to <= from) {
    const Iterator a = lower(from);
    return {a, a};
  }
  return {lower(from), lower(to)};
}

NoteSeq NoteSeq::edited(std::span<const NoteKey> removeIn, std::span<const Note> insertIn) const {
  std::vector<NoteKey> removeStore;
  std::vector<Note> insertStore;
  const std::span<const NoteKey> rem = sortedView(removeIn, removeStore, std::less<NoteKey>{});
  const std::span<const Note> ins = sortedView(insertIn, insertStore, keyLess);

  NoteSeq out;
  out.size_ = size_ + ins.size() - rem.size();
  if (chunks_.empty()) {
    assert(rem.empty());
    appendSplit(out.chunks_, ins.data(), ins.size());
    return out;
  }
  out.chunks_.reserve(chunks_.size() + 2 + ins.size() / kMaxNotesPerChunk);

  std::vector<Note> scratch;  // only used when a rebuilt range overflows one chunk
  size_t ci = 0, ri = 0, ii = 0;
  const size_t nChunks = chunks_.size();
  // Number of keys from `keys[from..]` assigned to chunk c: those ≤ lastKey(c), or all of them for the last chunk.
  auto countFor = [&](size_t c, auto keys, size_t from, size_t n, auto keyAt) {
    if (c + 1 == nChunks) return n - from;
    const NoteKey last = lastKey(chunks_[c]);
    size_t k = from;
    while (k < n && keyAt(keys, k) <= last) ++k;
    return k - from;
  };
  auto remKey = [](std::span<const NoteKey> s, size_t k) { return s[k]; };
  auto insKey = [](std::span<const Note> s, size_t k) { return keyOf(s[k]); };

  while (ri < rem.size() || ii < ins.size()) {
    NoteKey next;
    if (ri < rem.size() && (ii >= ins.size() || rem[ri] < keyOf(ins[ii])))
      next = rem[ri];
    else
      next = keyOf(ins[ii]);
    const size_t a = chunkFor(next);
    assert(a >= ci);
    out.chunks_.insert(out.chunks_.end(), chunks_.begin() + static_cast<std::ptrdiff_t>(ci),
                       chunks_.begin() + static_cast<std::ptrdiff_t>(a));

    // Edits assigned to chunk a.
    size_t nRem = countFor(a, rem, ri, rem.size(), remKey);
    size_t nIns = countFor(a, ins, ii, ins.size(), insKey);
    size_t oldCount = chunks_[a].count;
    size_t b = a;
    size_t total = oldCount - nRem + nIns;
    // A small result absorbs the following chunk (and its edits) when both fit in one chunk.
    if (total < kMinNotesPerChunk && a + 1 < nChunks) {
      const size_t r2 = countFor(a + 1, rem, ri + nRem, rem.size(), remKey);
      const size_t i2 = countFor(a + 1, ins, ii + nIns, ins.size(), insKey);
      const size_t t2 = total + chunks_[a + 1].count - r2 + i2;
      if (t2 <= kMaxNotesPerChunk) {
        b = a + 1;
        nRem += r2;
        nIns += i2;
        oldCount += chunks_[a + 1].count;
        total = t2;
      }
    }
    // ...or is appended to the previous output chunk when that one has room (keeps the tail compact).
    const Note* prevSrc = nullptr;
    uint32_t prevCount = 0;
    if (total > 0 && total < kMinNotesPerChunk && b + 1 == nChunks && !out.chunks_.empty() &&
        out.chunks_.back().count + total <= kMaxNotesPerChunk) {
      prevCount = out.chunks_.back().count;
      total += prevCount;
    }

    // Merge old notes of chunks [a, b] (minus removals) with the insertions into `dst`.
    std::shared_ptr<Note[]> single;
    Note* dst = nullptr;
    if (total <= kMaxNotesPerChunk) {
      if (total > 0) {
        single = std::make_shared_for_overwrite<Note[]>(total);
        dst = single.get();
      }
    } else {
      scratch.resize(total);
      dst = scratch.data();
    }
    Chunk prevChunk;
    if (prevCount > 0) {
      prevChunk = std::move(out.chunks_.back());  // keep it alive while copying out of it
      out.chunks_.pop_back();
      prevSrc = prevChunk.notes.get();
      std::memcpy(dst, prevSrc, sizeof(Note) * prevCount);
    }
    Note* w = dst + prevCount;
    const size_t remEnd = ri + nRem, insEnd = ii + nIns;
    for (size_t c = a; c <= b; ++c) {
      const Chunk& ch = chunks_[c];
      for (uint32_t k = 0; k < ch.count; ++k) {
        const Note& n = ch.notes[k];
        const NoteKey key = keyOf(n);
        while (ii < insEnd && keyOf(ins[ii]) < key) *w++ = ins[ii++];
        if (ri < remEnd && rem[ri] == key) {
          ++ri;
          continue;
        }
        *w++ = n;
      }
    }
    while (ii < insEnd) *w++ = ins[ii++];
    assert(ri == remEnd && "NoteSeq::edited: a removed key was not in the sequence");
    assert(static_cast<size_t>(w - dst) == total);

    if (total > 0) {
      if (total <= kMaxNotesPerChunk)
        out.chunks_.push_back(Chunk{std::move(single), static_cast<uint32_t>(total)});
      else
        appendSplit(out.chunks_, dst, total);
    }
    ci = b + 1;
  }
  out.chunks_.insert(out.chunks_.end(), chunks_.begin() + static_cast<std::ptrdiff_t>(ci), chunks_.end());
  return out;
}

bool operator==(const NoteSeq& a, const NoteSeq& b) noexcept {
  if (a.size_ != b.size_) return false;
  if (a.chunks_.size() == b.chunks_.size()) {
    bool sameShape = true;
    for (size_t i = 0; i < a.chunks_.size() && sameShape; ++i) sameShape = a.chunks_[i].count == b.chunks_[i].count;
    if (sameShape) {
      for (size_t i = 0; i < a.chunks_.size(); ++i) {
        const NoteSeq::Chunk& x = a.chunks_[i];
        const NoteSeq::Chunk& y = b.chunks_[i];
        if (x.notes == y.notes) continue;
        if (!std::equal(x.notes.get(), x.notes.get() + x.count, y.notes.get())) return false;
      }
      return true;
    }
  }
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

}  // namespace pq::doc
