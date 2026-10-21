#pragma once

// Notes and the immutable chunked note sequence of a clip (T3.4).
//
// A clip's notes are sorted by (start, id) and stored in chunks of at most kMaxNotesPerChunk notes. Each chunk is
// one immutable heap array (`shared_ptr<const Note[]>`, a single allocation via make_shared), shared between all
// snapshots that contain it. An edit rebuilds only the chunks it touches; every other chunk is shared by pointer.
// Moving one note therefore copies one chunk (≤ 128 × 64 B = 8 KB, two chunks if it crosses a chunk boundary)
// plus the chunk directory (24 B per chunk).
//
// Why 128 and not 256 (the T3.4 upper bound): a 256-note chunk alone is 16 KB, the whole per-edit memory budget
// (T3.4 perf criterion). 128 halves the copy while the per-chunk overhead (one directory entry + one allocation
// header ≈ 40 B per 8 KB) stays below 1 %. Iteration is unaffected: one pointer chase per 128 notes.
//
// Not RT code: the audio thread never sees the document (T3.16 builds flat SequenceImages from it).

#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/ids.h"
#include "pq/tuning/pitch_id.h"

namespace pq::doc {

inline constexpr uint32_t kMaxNotesPerChunk = 128;
// A rebuilt chunk smaller than this is merged with its successor when both fit in one chunk, so repeated deletes
// cannot leave a long tail of tiny chunks.
inline constexpr uint32_t kMinNotesPerChunk = 32;

// Largest |tick| any document position may have. Leaves headroom for start + length and for TempoMap queries,
// which are valid up to 2^53.
inline constexpr Ticks kMaxTicks = Ticks{1} << 52;

enum NoteFlags : uint32_t {
  kNoteDisabled = 1u << 0,  // deactivated note (Ableton: `0` on a note): kept, drawn grey, not played
};

// 64 bytes, trivially copyable: notes stay dense and copying a chunk is a memcpy.
// Times are clip-content ticks (relative to the clip's content origin, see NoteClip).
struct Note {
  Ticks start = 0;
  Ticks length = kPpq;
  tuning::PitchId pitch;
  NoteId id;
  float velocity = 100.0f;        // (0, 127]
  float releaseVelocity = 64.0f;  // [0, 127]
  float chance = 1.0f;            // [0, 1] probability of playing (SPEC §9.4)
  float velocityRange = 0.0f;     // [−127, 127] random velocity deviation (Ableton "velocity range")
  uint32_t flags = 0;             // NoteFlags
  // Per-note expression curves (pitch/gain/pan/timbre, SPEC §3.1) are reserved for M7 (Q15): they will live in a
  // side table keyed by NoteId so the dense note record does not grow.

  bool enabled() const noexcept { return (flags & kNoteDisabled) == 0; }
  friend bool operator==(const Note&, const Note&) noexcept = default;
};
static_assert(sizeof(Note) == 64, "Note layout: keep chunks dense (8 KB per full chunk)");
static_assert(std::is_trivially_copyable_v<Note>);

// The sort key of a note: (start, id). Unique within a clip; commands address notes by key so a lookup is a
// binary search (a stale key falls back to a linear search by id, see NoteSeq::locate).
struct NoteKey {
  Ticks start = 0;
  NoteId id;
  friend constexpr auto operator<=>(const NoteKey&, const NoteKey&) noexcept = default;
};
inline NoteKey keyOf(const Note& n) noexcept { return {n.start, n.id}; }

// Checks one note's fields (length ≥ 1, bounded start, value ranges, finite floats, valid id).
Status validateNote(const Note& n);

class NoteSeq {
public:
  struct Chunk {
    std::shared_ptr<const Note[]> notes;
    uint32_t count = 0;
    std::span<const Note> span() const noexcept { return {notes.get(), count}; }
  };

  // Forward iterator over all notes in order.
  class Iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Note;
    using difference_type = std::ptrdiff_t;
    using pointer = const Note*;
    using reference = const Note&;

    Iterator() noexcept = default;
    reference operator*() const noexcept { return chunks_[chunk_].notes[index_]; }
    pointer operator->() const noexcept { return &chunks_[chunk_].notes[index_]; }
    Iterator& operator++() noexcept {
      if (++index_ == chunks_[chunk_].count) {
        ++chunk_;
        index_ = 0;
      }
      return *this;
    }
    Iterator operator++(int) noexcept {
      Iterator t = *this;
      ++*this;
      return t;
    }
    friend bool operator==(const Iterator& a, const Iterator& b) noexcept {
      return a.chunk_ == b.chunk_ && a.index_ == b.index_;
    }

  private:
    friend class NoteSeq;
    Iterator(const Chunk* chunks, size_t chunk, uint32_t index) noexcept
        : chunks_(chunks), chunk_(chunk), index_(index) {}
    const Chunk* chunks_ = nullptr;
    size_t chunk_ = 0;
    uint32_t index_ = 0;
  };

  NoteSeq() noexcept = default;

  // Builds from notes in any order. Errors: an invalid note, or two notes with the same id.
  static Result<NoteSeq> fromNotes(std::vector<Note> notes);

  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  std::span<const Chunk> chunks() const noexcept { return chunks_; }
  Iterator begin() const noexcept { return Iterator(chunks_.data(), 0, 0); }
  Iterator end() const noexcept { return Iterator(chunks_.data(), chunks_.size(), 0); }

  // The note with exactly this key, or nullptr. O(log chunks + log 128).
  const Note* find(NoteKey key) const noexcept;
  // Linear search by id (for keys that went stale). O(n).
  const Note* findById(NoteId id) const noexcept;
  // find(key), falling back to findById(key.id). nullptr if the id is not in the sequence.
  const Note* locate(NoteKey key) const noexcept {
    if (const Note* n = find(key)) return n;
    return findById(key.id);
  }
  // Notes whose start lies in [from, to), as a pair of iterators (for views and sequence building).
  std::pair<Iterator, Iterator> range(Ticks from, Ticks to) const noexcept;

  // Edits. Each returns a new sequence that shares every untouched chunk with *this.
  // Preconditions (checked by the commands, asserted here): `remove` keys exist, `insert` keys do not exist after
  // the removals, no key appears twice. Both spans may be in any order.
  NoteSeq edited(std::span<const NoteKey> remove, std::span<const Note> insert) const;
  NoteSeq withInserted(std::span<const Note> insert) const { return edited({}, insert); }
  NoteSeq withRemoved(std::span<const NoteKey> remove) const { return edited(remove, {}); }
  NoteSeq withReplaced(NoteKey key, const Note& note) const { return edited({&key, 1}, {&note, 1}); }

  // Content equality (pointer-equal chunks are skipped without comparing notes).
  friend bool operator==(const NoteSeq& a, const NoteSeq& b) noexcept;

private:
  // Index of the chunk a note with `key` belongs to: the first chunk whose last key ≥ key (the last chunk for keys
  // beyond everything). chunks_ must not be empty.
  size_t chunkFor(NoteKey key) const noexcept;

  std::vector<Chunk> chunks_;
  size_t size_ = 0;
};

}  // namespace pq::doc
