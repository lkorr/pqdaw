#pragma once

// The note sequence the timeline plays (T3.16): plain-data descriptions (filled by T3.17's lowering from the
// document, without the engine depending on `document`) and the immutable SequenceImage built from them off the
// audio thread. The Timeline (timeline.h) publishes images by atomic swap and walks them with per-track cursors.
//
// ---- Clip semantics (pq/core/clip_time.h, shared with pq/document/clip_time.h) -----------------------------------
// A clip plays content from `contentStart` at arrangement tick `start`, for `length` ticks. With the loop off,
// content tick contentStart + o plays at start + o. With the loop on (loopEnabled and loopEnd > loopStart), playback
// runs from contentStart up to loopEnd and then repeats [loopStart, loopEnd) until the clip ends; a start marker at
// or after loopEnd is folded into the loop. Each run over the content is a PASS (pass 0 = the first, then one per
// loop repetition). A note sounds in a pass if its start lies in the pass's content range; it is cut at the end of
// the pass (the loop wrap or the clip end), as in Ableton. Notes that can never play (before the start marker,
// outside the loop) are dropped at build time. A seek into the middle of a note (a locate, a transport start, an
// arrangement loop wrap) starts it there: the timeline's chase (timeline.h "Chase"; covering() finds the notes).
// Disabled notes and inactive clips (`0`, deactivated: SPEC §9.3, Q15)
// emit nothing; muting a track is a gain param elsewhere (Q15), the sequencer keeps running.
//
// Clips on one track must not overlap (the document guarantees it, Q41); if they do, the later-starting clip wins
// and the earlier one is cut where it begins (equal starts: the later one in the description wins).
//
// ---- Chance and velocity range ----------------------------------------------------------------------------------
// Deterministic per note and pass (noteRandom): u1, u2 = uniform [0, 1) from (seed, note id, pass). The note plays
// iff u1 < chance; it plays with velocity clamp(velocity + u2 · velocityRange, 1, 127). The same note in the same
// pass always decides the same way (offline renders are reproducible); a new seed rerolls.
//
// ---- Pitch -------------------------------------------------------------------------------------------------------
// Notes carry resolved float32 Hz (SPEC §3.1) plus voice.ratio / voice.harmonic. PitchSource is tuning as a signal
// (T2.6): a nonzero `tuning` marks a note following a tuning signal; its note-on carries voice.tuning = `node` (which
// of the track patch's tuning nodes, T2.6-fix3) and, for kind Degree, degree/equave as voice.degree / voice.equave
// (note_events.h; NaN for kind Reference), from which that node (`tuning_hz` + `tune_voices`) derives the voice's Hz
// per sample. `hz` stays the static pitch (and the fallback where no tuning node is bound).
//
// ---- Per-note curves (T7.1) --------------------------------------------------------------------------------------
// Every note may carry a pitch curve and a gain curve: breakpoints (CurvePoint) in CONTENT ticks (the frame of
// NoteDesc::start, not relative to the note), piecewise linear between them, held at the first / last value outside
// them. A curve may begin before its note and end after it (several notes can share one curve: the note reads it from
// its own start on). Two points at one tick are a jump (the second is the value from that tick on).
//   pitchCurve   cents relative to `hz`: the note sounds at hz · 2^(cents / 1200), linear in cents between points
//   gainCurve    linear gain ≥ 0 (NoteEvent::gain): the voice's note gain; 0 parks the voice (it keeps running but
//                is invisible to the mix and the patch's reducers until the gain rises again)
// An empty curve is the constant 0 ¢ / gain 1 and costs nothing. The timeline (timeline.h "Note curves") starts the
// note at the curve's value at its note-on tick and sends one Pitch / Gain ramp per breakpoint: the voice
// interpolates per sample, linear in cents / linear in gain over SAMPLES, which equals linear over ticks while the
// tempo is constant between the two breakpoints (inside a tempo ramp the curve bends with the tempo: put breakpoints
// closer there). The identity signals (voice.ratio, voice.harmonic, degree) do not follow a curve; a note that follows
// a tuning node keeps doing so, with the curve on top (the node keeps glides: dsp/kernels_tuning.cpp).
// Storage: curves cost notes without them nothing (SequenceImage::curvesOf).
// Limits: at most kMaxCurvePoints points per curve after build-time clean-up (points sorted by tick, stably; of
// several points at one tick the first and the last are kept); values finite, |cents| ≤ kMaxCurveCents, gain ≥ 0.

#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"

namespace pq::engine {

// T2.6 (see above). All zero: the note plays its static hz.
enum class PitchFollowKind : uint8_t {
  Degree = 0,     // the node computes `degree` / `equave` (the note's, or its anchor chain's root Degree note's)
  Reference = 1,  // the node scales the static Hz by reference(t) / reference₀ (Ratio to the reference, Cents)
};
struct PitchSource {
  uint32_t tuning = 0;   // 0 = static Hz; otherwise the id of the tuning signal the note follows (T2.6)
  int32_t degree = 0;    // Degree: the degree and equave in that tuning
  int32_t equave = 0;
  uint16_t node = 0;     // T2.6-fix3: the tuning node of the track patch (1, 2, …; doc::tuningNodeIndex): voice.tuning
  PitchFollowKind kind = PitchFollowKind::Degree;
  uint8_t pad_ = 0;
  friend bool operator==(const PitchSource&, const PitchSource&) = default;
};

// T7.1: one breakpoint of a note curve (see "Per-note curves").
struct CurvePoint {
  Ticks tick = 0;      // content ticks
  float value = 0.0f;  // pitch: cents relative to the note's hz; gain: linear gain
  friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};
inline constexpr uint32_t kMaxCurvePoints = 65535;
inline constexpr float kMaxCurveCents = 24000.0f;

struct NoteDesc {
  Ticks start = 0;                // content ticks (relative to the clip's content origin, as doc::Note)
  Ticks length = kPpq;            // ≥ 1
  uint32_t id = 0;                // stable note id (doc::NoteId): the RNG key and the identity across image swaps
  float hz = 261.626f;            // resolved pitch, finite and > 0
  float velocity = 100.0f;        // (0, 127]
  float releaseVelocity = 64.0f;  // [0, 127]
  float chance = 1.0f;            // [0, 1] probability of playing (per pass)
  float velocityRange = 0.0f;     // [−127, 127] random velocity deviation (see above)
  float ratio = 1.0f;             // voice.ratio
  float harmonic = 0.0f;          // voice.harmonic
  PitchSource pitch;
  float keyDegree = std::numeric_limits<float>::quiet_NaN();  // voice.noteDegree (T4.17; NaN: unknown)
  // T7.3: voice.unisonCount / voice.index (NoteEvent): a JI object's voices that share one pitch at the note's start.
  uint16_t unisonCount = 1;
  uint16_t unisonIndex = 0;
  bool enabled = true;            // disabled notes are kept in the document but never play
  // T7.1 (see "Per-note curves"): content ticks; empty = constant (0 ¢ / gain 1).
  std::vector<CurvePoint> pitchCurve;  // cents relative to hz
  std::vector<CurvePoint> gainCurve;   // linear gain ≥ 0
};

struct ClipDesc {
  uint32_t id = 0;                // stable clip id (diagnostics only)
  Ticks start = 0;                // arrangement ticks
  Ticks length = kTicksPerWhole;  // ≥ 1
  Ticks contentStart = 0;         // content tick that plays at `start` (start marker)
  bool loopEnabled = false;
  Ticks loopStart = 0;            // content ticks; the loop is on iff loopEnabled && loopEnd > loopStart
  Ticks loopEnd = kTicksPerWhole;
  bool active = true;             // deactivated clips emit nothing
  std::vector<NoteDesc> notes;    // any order (sorted by (start, id) at build time)
};

struct TrackSequenceDesc {
  uint32_t id = 0;                // stable track id: sounding notes follow it across image swaps
  std::vector<ClipDesc> clips;    // any order
};

struct SequenceBuildOptions {
  uint64_t seed = 0;              // chance / velocity-range seed (noteRandom)
};

// Two independent uniforms in [0, 1) for (seed, note id, pass). RT-safe.
struct NoteRandom {
  float chance;
  float velocity;
};
NoteRandom noteRandom(uint64_t seed, uint32_t noteId, int64_t pass) noexcept;

class SequenceImage {
public:
  // A note as stored: 72 bytes. Its curves (T7.1) are not in it: see curvesOf().
  struct Note {
    Ticks start;            // content ticks
    Ticks length;
    uint32_t id;
    float hz;
    float velocity, releaseVelocity, chance, velocityRange;
    float ratio, harmonic;
    PitchSource pitch;
    float keyDegree;        // T4.17: voice.noteDegree (NaN: unknown)
    uint16_t unisonCount;   // T7.3: voice.unisonCount / voice.index (in what was padding)
    uint16_t unisonIndex;
  };
  static_assert(sizeof(Note) == 72);

  // T7.1: where a note's curves are in curves(): one header entry at `begin` (its `tick` holds the curve hash), then
  // pitchCount pitch points, then gainCount gain points. Both counts 0: no curves (`begin` is meaningless).
  struct NoteCurves {
    uint32_t begin = 0;
    uint16_t pitchCount = 0, gainCount = 0;
    bool any() const noexcept { return (pitchCount | gainCount) != 0; }
  };

  // A clip as stored (after trimming overlaps). Arrangement ticks unless noted.
  struct Clip {
    Ticks start, end;       // [start, end), end > start
    Ticks content0;         // content tick playing at `start` (start marker, folded into the loop)
    Ticks firstPassEnd;     // where pass 0 ends: the first loop wrap, or `end` (min of both)
    Ticks loopStart;        // content ticks
    Ticks loopLen;          // 0: not looped
    uint32_t pass0Begin, pass0End;  // note index range (into notes()) of pass 0: start ∈ [content0, pass-0 end)
    uint32_t loopBegin, loopEnd;    // note index range of the later passes: start ∈ [loopStart, loopStart + loopLen)
    uint32_t noteBegin, noteEnd;    // every stored note of the clip, sorted by (start, id)
    uint32_t id;
    uint32_t pad_;
  };

  struct Track {
    uint32_t id;
    uint32_t clipBegin, clipEnd;    // into clips(), sorted by start (so also by end)
  };

  // Off the audio thread (allocates). Errors: a clip with length < 1, a note with length < 1 or a non-finite /
  // non-positive Hz, a non-finite velocity / chance, more than 2^32 − 1 notes; a curve with a non-finite value, a
  // pitch beyond ±kMaxCurveCents, a negative gain or more than kMaxCurvePoints points (T7.1).
  static Result<std::unique_ptr<SequenceImage>> build(std::span<const TrackSequenceDesc> tracks,
                                                      const SequenceBuildOptions& options = {});

  // RT-safe accessors.
  std::span<const Track> tracks() const noexcept { return tracks_; }
  std::span<const Clip> clips() const noexcept { return clips_; }
  std::span<const Note> notes() const noexcept { return notes_; }
  uint32_t numTracks() const noexcept { return static_cast<uint32_t>(tracks_.size()); }
  // T7.1: the curve storage of every note, and one note's curves (`n` must be an element of notes()). Notes without
  // curves pay nothing: the note record has no curve field; a bit per note says whether it has curves (one load when
  // the image has any curve, none otherwise), and a rank over those bits finds the curved note's NoteCurves. O(1).
  std::span<const CurvePoint> curves() const noexcept { return curves_; }
  bool hasCurves() const noexcept { return !curveRefs_.empty(); }
  NoteCurves curvesOf(const Note& n) const noexcept {
    if (curveRefs_.empty()) return {};
    const size_t i = static_cast<size_t>(&n - notes_.data());
    const uint64_t word = curveBits_[i >> 6], bit = uint64_t{1} << (i & 63u);
    if ((word & bit) == 0) return {};
    return curveRefs_[curveRank_[i >> 6] + static_cast<uint32_t>(std::popcount(word & (bit - 1)))];
  }
  std::span<const CurvePoint> pitchCurve(const Note& n) const noexcept {
    const NoteCurves c = curvesOf(n);
    return {curves_.data() + c.begin + 1, c.pitchCount};
  }
  std::span<const CurvePoint> gainCurve(const Note& n) const noexcept {
    const NoteCurves c = curvesOf(n);
    return {curves_.data() + c.begin + 1 + c.pitchCount, c.gainCount};
  }
  // A hash of both curves relative to the note's start (0 without curves): equal for the same note in two images iff
  // its curves did not change (up to hash collisions), so a swap keeps the curve cursors of unchanged notes.
  uint64_t curveHash(const NoteCurves& c) const noexcept {
    return c.any() ? static_cast<uint64_t>(curves_[c.begin].tick) : 0u;
  }
  uint64_t curveHash(const Note& n) const noexcept { return curveHash(curvesOf(n)); }
  // The value of a curve at content tick t: linear between points, the first / last value outside them, the last
  // point of a tick at a jump; `empty` for a curve without points. `after` (optional) receives the index of the
  // first point with tick > t. O(log points). RT-safe.
  static float curveValue(std::span<const CurvePoint> curve, Ticks t, float empty, uint32_t* after = nullptr) noexcept;
  // The same at a fractional tick (the tick of a sample: TempoMap::samplesToTicks). RT-safe.
  static float curveValueAt(std::span<const CurvePoint> curve, double tick, float empty) noexcept;
  uint64_t seed() const noexcept { return seed_; }

  // T7.3 (chase): the notes of clip `c` with index in [lo, hi) (one of the clip's pass ranges) that started before
  // content tick `content` and still last beyond it (start < content < start + length), in (start, id) order: at
  // most `max` indices into notes() written to `out`; returns how many. A max-end tree over each clip's notes
  // (8 bytes per note, read only here) prunes every subtree that ended earlier: O((found + 1) · log notes), whatever
  // lies between. RT-safe.
  uint32_t covering(const Clip& c, uint32_t lo, uint32_t hi, Ticks content, uint32_t* out, uint32_t max) const noexcept;

  // The occurrence of note `noteId` that starts at arrangement tick `on` on track `track`: its stored note and the
  // end of the pass it plays in (the note-off tick is min(on + length, passEnd)). Null when there is none (the note
  // was removed, moved, or its clip deactivated). O(log clips + log notes). RT-safe.
  const Note* findOccurrence(uint32_t track, uint32_t noteId, Ticks on, Ticks* passEnd) const noexcept;

private:
  std::vector<Track> tracks_;
  std::vector<Clip> clips_;
  std::vector<Note> notes_;
  std::vector<Ticks> endMax_;           // T7.3: per note, the latest end of its subtree (implicit tree per clip)
  std::vector<CurvePoint> curves_;      // T7.1
  std::vector<NoteCurves> curveRefs_;   // per curved note, in note order
  std::vector<uint64_t> curveBits_;     // bit i: note i has curves (empty when no note has)
  std::vector<uint32_t> curveRank_;     // per 64 notes: curved notes before them
  uint64_t seed_ = 0;
};

}  // namespace pq::engine
