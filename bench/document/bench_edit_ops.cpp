// Edit-operation builders (T3.6), release. For each heavy operation: the builder alone (snapshot → Batch) and the
// dispatch of its Batch through DocumentStore (apply + one history entry), median over repeated runs (each run is
// undone before the next, so every run edits the same snapshot).
//   - insert / duplicate / delete time on 100 MIDI tracks × 100 clips × 100 notes (1M notes) + a 100-point volume
//     lane per track
//   - quantize 10,000 notes of one clip (Ctrl+U, all notes)
//   - transpose 1,000 selected notes (Up), 31-EDO
//   - split a 10,000-note clip, consolidate 100 clips × 100 notes on one track

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/store.h"
#include "pq_bench.h"

using namespace pq::doc;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kTracks = 100;
constexpr int kClipsPerTrack = 100;
constexpr int kNotesPerClip = 100;
constexpr pq::Ticks kBar = pq::kTicksPerWhole;

ClipPtr buildClip(IdSource& ids, std::mt19937& rng, pq::Ticks start, pq::Ticks length, int notes, bool offGrid) {
  NoteClip c;
  c.id = ids.clip();
  c.start = start;
  c.length = length;
  std::vector<Note> v;
  v.reserve(static_cast<size_t>(notes));
  const pq::Ticks step = length / notes;
  for (int i = 0; i < notes; ++i) {
    Note n;
    n.id = ids.note();
    n.start = i * step + (offGrid ? static_cast<pq::Ticks>(rng() % 2000) : 0);
    n.length = step / 2 + 1;
    n.pitch = pq::tuning::PitchId::fromDegree(static_cast<int32_t>(rng() % 31), 0);
    v.push_back(n);
  }
  c.notes = NoteSeq::fromNotes(std::move(v)).value();
  return std::make_shared<const NoteClip>(std::move(c));
}

Snapshot buildArrangement(IdSource& ids, std::vector<TrackId>& tracks) {
  std::mt19937 rng(7);
  auto p = std::make_shared<Project>(*makeDefaultProject(ids));
  for (int t = 0; t < kTracks; ++t) {
    Track tr = makeTrack(ids.track(), TrackKind::Midi, "Track");
    for (int c = 0; c < kClipsPerTrack; ++c) {
      ClipPtr clip = buildClip(ids, rng, c * 2 * kBar, 2 * kBar - kBar / 4, kNotesPerClip, false);
      tr.clips.push_back({clip->id, std::move(clip)});
    }
    AutomationLane lane;
    lane.id = ids.lane();
    lane.param = ParamRef::track(ParamKind::TrackVolume, tr.id);
    for (int i = 0; i < 100; ++i)
      lane.points.push_back(Breakpoint{i * 2 * kBar, -static_cast<float>(i % 30), i % 2 ? 0.3f : 0.0f, ids.breakpoint()});
    tr.lanes.push_back(std::make_shared<const AutomationLane>(std::move(lane)));
    tracks.push_back(tr.id);
    p->tracks.push_back(std::make_shared<const Track>(std::move(tr)));
  }
  return p;
}

struct Timing {
  double buildUs;
  double dispatchUs;
  size_t commands;
};

// Runs make() (a builder on the current snapshot) then dispatch, `reps` times, undoing after each.
template <class Make>
Timing run(DocumentStore& store, int reps, Make&& make) {
  std::vector<double> b, d;
  size_t commands = 0;
  for (int i = 0; i < reps; ++i) {
    const auto t0 = Clock::now();
    pq::Result<ops::Edit> e = make();
    const auto t1 = Clock::now();
    if (!e) {
      std::printf("builder failed: %s\n", e.error().message.c_str());
      return {};
    }
    commands = e->batch().commands.size();
    const pq::Status st = store.dispatch(e->command);
    const auto t2 = Clock::now();
    if (!st) {
      std::printf("dispatch failed: %s\n", st.error().message.c_str());
      return {};
    }
    store.undo();
    b.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    d.push_back(std::chrono::duration<double, std::micro>(t2 - t1).count());
  }
  std::sort(b.begin(), b.end());
  std::sort(d.begin(), d.end());
  return {b[b.size() / 2], d[d.size() / 2], commands};
}

void print(const char* name, const Timing& t) {
  std::printf("BENCH %-44s build %10.1f us   dispatch %10.1f us   (%zu commands)\n", name, t.buildUs, t.dispatchUs,
              t.commands);
}

}  // namespace

int main() {
  // ---- Arrangement-wide time edits ----
  {
    IdSource ids;
    std::vector<TrackId> tracks;
    Snapshot p = buildArrangement(ids, tracks);
    DocumentStore store(p);
    p.reset();
    std::printf("arrangement: %d tracks x %d clips x %d notes, 100-point lane per track\n", kTracks, kClipsPerTrack,
                kNotesPerClip);
    print("insert time (1 bar at bar 1.5, splits 100 clips)", run(store, 15, [&] {
            return ops::insertTime(*store.current(), store.ids(), kBar + kBar / 2, kBar);
          }));
    print("insert time (1 bar near the end: 100 clips move)", run(store, 15, [&] {
            return ops::insertTime(*store.current(), store.ids(), 198 * kBar, kBar);
          }));
    print("delete time (bars 1.5..3.5)", run(store, 15, [&] {
            return ops::deleteTime(*store.current(), store.ids(), kBar + kBar / 2, 3 * kBar + kBar / 2);
          }));
    print("duplicate time (bars 1.5..3.5)", run(store, 15, [&] {
            return ops::duplicateTime(*store.current(), store.ids(), kBar + kBar / 2, 3 * kBar + kBar / 2);
          }));
    std::vector<ClipRef> col;
    for (TrackId t : tracks) col.push_back({t, findTrackPtr(*store.current(), t)->clips[50].id});
    print("move 100 clips (one per track) +1 bar", run(store, 15, [&] {
            return ops::moveClipsBy(*store.current(), store.ids(), col, kBar);
          }));
    print("move 100 clips +1 bar, automation follows", run(store, 15, [&] {
            return ops::moveClipsBy(*store.current(), store.ids(), col, kBar, {.moveAutomation = true});
          }));
  }

  // ---- One big clip ----
  {
    DocumentStore store;
    IdSource& ids = store.ids();
    std::mt19937 rng(11);
    const TrackId t = ids.track();
    const TuningId t31 = ids.tuning();
    TuningSlot s31{t31, "31", {}, {}};
    s31.def.system = pq::tuning::Edo::make(31).value();
    ClipPtr big = buildClip(ids, rng, 0, 400 * kBar, 10000, true);
    std::vector<ClipRef> many;
    Batch setup{"setup", {AddTuning{s31}, AddTrack{std::make_shared<const Track>(makeTrack(t, TrackKind::Midi, "T")), {}, kAppend},
                          SetTrackTuning{t, TuningAssignment::mainTuning(t31)}, AddClip{t, big}}};
    const TrackId t2 = ids.track();
    setup.commands.push_back(AddTrack{std::make_shared<const Track>(makeTrack(t2, TrackKind::Midi, "T2")), {}, kAppend});
    for (int c = 0; c < 100; ++c) {
      ClipPtr cl = buildClip(ids, rng, c * kBar, kBar, 100, false);
      many.push_back({t2, cl->id});
      setup.commands.push_back(AddClip{t2, cl});
    }
    if (!store.dispatch(setup)) {
      std::printf("setup failed\n");
      return 1;
    }
    const ClipRef bigRef{t, big->id};
    std::printf("clip: 10,000 notes, off-grid; track of 100 clips x 100 notes\n");
    const pq::Grid g16(pq::GridSpec{pq::GridMode::Fixed, 0, 4, 1, true, false}, store.current()->timeline->sigMap, 0.0);
    print("quantize 10,000 notes (Ctrl+U, all)", run(store, 21, [&] {
            return ops::quantizeNotes(*store.current(), bigRef, {}, g16);
          }));
    std::vector<NoteKey> sel;
    int i = 0;
    for (const Note& n : big->notes)
      if (i++ % 10 == 0) sel.push_back(keyOf(n));
    print("transpose 1,000 selected notes (Up, 31-EDO)", run(store, 21, [&] {
            return ops::transposeNotes(*store.current(), bigRef, sel, 1, ops::StepUnit::Grid);
          }));
    const pq::Ticks cut[] = {200 * kBar + 7};
    const ClipRef one[] = {bigRef};
    print("split a 10,000-note clip in the middle", run(store, 21, [&] {
            return ops::splitClips(*store.current(), store.ids(), one, cut);
          }));
    print("consolidate 100 clips x 100 notes", run(store, 21, [&] {
            return ops::consolidateClips(*store.current(), store.ids(), many);
          }));
  }
  return 0;
}
