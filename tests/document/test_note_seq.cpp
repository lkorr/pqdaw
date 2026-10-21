#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

#include "pq/document/note_seq.h"

using namespace pq::doc;

namespace {

Note note(uint32_t id, pq::Ticks start, int32_t degree = 0) {
  Note n;
  n.id = NoteId{id};
  n.start = start;
  n.pitch = pq::tuning::PitchId::fromDegree(degree);
  return n;
}

std::vector<Note> flatten(const NoteSeq& s) { return {s.begin(), s.end()}; }

// Structural invariants: sorted by key, chunk sizes within bounds, size() consistent.
void checkInvariants(const NoteSeq& s) {
  size_t total = 0;
  bool sizesOk = true;
  for (const NoteSeq::Chunk& c : s.chunks()) {
    sizesOk = sizesOk && c.count >= 1 && c.count <= kMaxNotesPerChunk;
    total += c.count;
  }
  REQUIRE(sizesOk);
  REQUIRE(total == s.size());
  const std::vector<Note> v = flatten(s);
  REQUIRE(std::is_sorted(v.begin(), v.end(), [](const Note& a, const Note& b) { return keyOf(a) < keyOf(b); }));
}

}  // namespace

TEST_CASE("note_seq: fromNotes sorts, chunks and rejects bad input") {
  std::vector<Note> in;
  for (uint32_t i = 0; i < 300; ++i) in.push_back(note(i + 1, (299 - i) * 10));
  auto s = NoteSeq::fromNotes(in);
  REQUIRE(s.ok());
  CHECK(s->size() == 300);
  CHECK(s->chunks().size() == 3);  // 128 + 128 + 44: bulk build fills chunks
  checkInvariants(*s);
  CHECK(s->find({0, NoteId{300}}) != nullptr);
  CHECK(s->find({1, NoteId{300}}) == nullptr);
  CHECK(s->locate({1, NoteId{300}}) != nullptr);  // stale key still finds the note by id
  CHECK(s->locate({0, NoteId{999}}) == nullptr);

  std::vector<Note> dup = {note(1, 0), note(1, 10)};
  CHECK_FALSE(NoteSeq::fromNotes(dup).ok());
  std::vector<Note> bad = {note(1, 0)};
  bad[0].length = 0;
  CHECK_FALSE(NoteSeq::fromNotes(bad).ok());
  bad[0] = note(1, 0);
  bad[0].chance = 1.5f;
  CHECK_FALSE(NoteSeq::fromNotes(bad).ok());
  bad[0] = note(0, 0);  // invalid id
  CHECK_FALSE(NoteSeq::fromNotes(bad).ok());
  CHECK(NoteSeq::fromNotes({}).ok());
}

TEST_CASE("note_seq: a one-note edit copies one chunk and shares the rest") {
  std::vector<Note> in;
  for (uint32_t i = 0; i < 1000; ++i) in.push_back(note(i + 1, i * 100));
  const NoteSeq a = NoteSeq::fromNotes(in).value();
  REQUIRE(a.chunks().size() == 8);

  // Move note 500 (chunk 3) a little: stays in its chunk.
  Note m = in[500];
  m.start += 50;
  const NoteSeq b = a.withReplaced(keyOf(in[500]), m);
  checkInvariants(b);
  REQUIRE(b.chunks().size() == a.chunks().size());
  int shared = 0;
  for (size_t i = 0; i < a.chunks().size(); ++i) shared += a.chunks()[i].notes == b.chunks()[i].notes;
  CHECK(shared == 7);
  CHECK(b.find(keyOf(m)) != nullptr);
  CHECK(b.find(keyOf(in[500])) == nullptr);
  CHECK(a.find(keyOf(in[500])) != nullptr);  // the old sequence is untouched

  // Move it far (chunk 3 → chunk 7): two chunks rebuilt, six shared.
  m.start = 99950;
  const NoteSeq c = a.withReplaced(keyOf(in[500]), m);
  checkInvariants(c);
  shared = 0;
  for (const NoteSeq::Chunk& x : c.chunks())
    for (const NoteSeq::Chunk& y : a.chunks()) shared += x.notes == y.notes;
  CHECK(shared == 6);
  CHECK(c.size() == 1000);
}

TEST_CASE("note_seq: inserting into a full chunk splits it in balanced halves") {
  std::vector<Note> in;
  for (uint32_t i = 0; i < 128; ++i) in.push_back(note(i + 1, i * 10));
  const NoteSeq a = NoteSeq::fromNotes(in).value();
  REQUIRE(a.chunks().size() == 1);
  const Note extra = note(1000, 5);
  const NoteSeq b = a.withInserted({&extra, 1});
  checkInvariants(b);
  REQUIRE(b.chunks().size() == 2);
  CHECK(b.chunks()[0].count == 64);
  CHECK(b.chunks()[1].count == 65);
}

TEST_CASE("note_seq: small chunks merge, empty chunks disappear") {
  std::vector<Note> in;
  for (uint32_t i = 0; i < 256; ++i) in.push_back(note(i + 1, i * 10));
  NoteSeq s = NoteSeq::fromNotes(in).value();
  REQUIRE(s.chunks().size() == 2);
  // Remove 100 notes of the first chunk: 28 remain, which merge with the second chunk's 128? No: 156 > 128, so
  // they stay separate.
  std::vector<NoteKey> rem;
  for (uint32_t i = 0; i < 100; ++i) rem.push_back(keyOf(in[i]));
  s = s.withRemoved(rem);
  checkInvariants(s);
  CHECK(s.chunks().size() == 2);
  // Remove 100 of the second chunk: 28 + 28 → merged into one.
  rem.clear();
  for (uint32_t i = 128; i < 228; ++i) rem.push_back(keyOf(in[i]));
  s = s.withRemoved(rem);
  checkInvariants(s);
  CHECK(s.chunks().size() == 1);
  CHECK(s.size() == 56);
  // Remove everything.
  rem.clear();
  for (const Note& n : s) rem.push_back(keyOf(n));
  s = s.withRemoved(rem);
  CHECK(s.empty());
  CHECK(s.chunks().empty());
  CHECK(s.begin() == s.end());
}

TEST_CASE("note_seq: random edits match a reference model" * doctest::test_suite("slow")) {
  std::mt19937 rng(1234);
  std::vector<Note> ref;
  NoteSeq s;
  uint32_t nextId = 1;
  auto refSort = [&] { std::sort(ref.begin(), ref.end(), [](const Note& a, const Note& b) { return keyOf(a) < keyOf(b); }); };
  for (int step = 0; step < 3000; ++step) {
    const int op = static_cast<int>(rng() % 10);
    if (op < 5 || ref.size() < 5) {  // insert a batch
      std::vector<Note> ins;
      const int k = 1 + static_cast<int>(rng() % 40);
      for (int i = 0; i < k; ++i) ins.push_back(note(nextId++, static_cast<pq::Ticks>(rng() % 20000), static_cast<int>(rng() % 50)));
      s = s.withInserted(ins);
      ref.insert(ref.end(), ins.begin(), ins.end());
    } else if (op < 8) {  // remove a random subset
      std::vector<NoteKey> rem;
      std::set<size_t> picks;
      const size_t k = 1 + rng() % std::min<size_t>(ref.size(), 30);
      while (picks.size() < k) picks.insert(rng() % ref.size());
      for (size_t i : picks) rem.push_back(keyOf(ref[i]));
      s = s.withRemoved(rem);
      for (auto it = picks.rbegin(); it != picks.rend(); ++it) ref.erase(ref.begin() + static_cast<std::ptrdiff_t>(*it));
    } else {  // replace (move) a few notes
      std::vector<NoteKey> rem;
      std::vector<Note> ins;
      std::set<size_t> picks;
      const size_t k = 1 + rng() % std::min<size_t>(ref.size(), 5);
      while (picks.size() < k) picks.insert(rng() % ref.size());
      for (size_t i : picks) {
        rem.push_back(keyOf(ref[i]));
        ref[i].start = static_cast<pq::Ticks>(rng() % 20000);
        ins.push_back(ref[i]);
      }
      s = s.edited(rem, ins);
    }
    REQUIRE(s.size() == ref.size());
    if (step % 10 == 0) {  // full checks are O(n); every 10th step keeps the dev (/Od) run short
      refSort();
      checkInvariants(s);
      REQUIRE(flatten(s) == ref);
    }
  }
  refSort();
  checkInvariants(s);
  CHECK(flatten(s) == ref);
  // Every note is found by key; range() agrees with a linear filter.
  for (const Note& n : ref) REQUIRE(s.find(keyOf(n)) != nullptr);
  for (int i = 0; i < 50; ++i) {
    const pq::Ticks a = static_cast<pq::Ticks>(rng() % 21000), b = a + static_cast<pq::Ticks>(rng() % 3000);
    auto [first, last] = s.range(a, b);
    std::vector<Note> got(first, last);
    std::vector<Note> want;
    for (const Note& n : ref)
      if (n.start >= a && n.start < b) want.push_back(n);
    REQUIRE(got == want);
  }
}

TEST_CASE("note_seq: equality is by content, not by chunking") {
  std::vector<Note> in;
  for (uint32_t i = 0; i < 200; ++i) in.push_back(note(i + 1, i * 10));
  const NoteSeq a = NoteSeq::fromNotes(in).value();  // 128 + 72
  NoteSeq b;
  for (const Note& n : in) b = b.withInserted({&n, 1});  // different chunk boundaries
  CHECK(a == b);
  Note m = in[3];
  m.velocity = 50.0f;
  CHECK_FALSE(a == a.withReplaced(keyOf(in[3]), m));
  CHECK(a == a);
}

TEST_CASE("note_seq: Note is 64 bytes and validated") {
  CHECK(sizeof(Note) == 64);
  Note n = note(1, 0);
  CHECK(validateNote(n).ok());
  n.velocity = 0.0f;
  CHECK_FALSE(validateNote(n).ok());
  n = note(1, 0);
  n.velocityRange = -128.0f;
  CHECK_FALSE(validateNote(n).ok());
  n = note(1, kMaxTicks + 1);
  CHECK_FALSE(validateNote(n).ok());
  n = note(1, 0);
  n.pitch = pq::tuning::PitchId::fromCents(std::numeric_limits<double>::infinity());
  CHECK_FALSE(validateNote(n).ok());
  n.pitch = pq::tuning::PitchId::fromRatio(pq::tuning::Rational(5, 4));
  CHECK(validateNote(n).ok());
  n.flags = 2;
  CHECK_FALSE(validateNote(n).ok());
}
