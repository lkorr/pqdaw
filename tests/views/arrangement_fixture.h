#pragma once

// Projects for the arrangement tests, goldens and benchmarks (T3.8), built through the document's edit operations
// (one history entry each, like a user would make them). No doctest here: the benchmark includes it too.
//
//   buildArrangementProject  → tests/views/fixtures/arrangement/project.pqd (8 tracks: a nested group and a folded
//                              group, looped / deactivated clips, Degree / Ratio / Cents notes, a 31-EDO track,
//                              tempo and signature changes, loop, locators). The fixture test keeps the committed file
//                              equal to what this builds.
//   buildLargeArrangement    → the T3.8 perf project (tracks × clips × notes), not saved.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/store.h"

namespace pq::views::test {

namespace fixture_detail {

inline void must(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "arrangement fixture: %s failed\n", what);
    std::abort();
  }
}

inline doc::ops::Edit run(doc::DocumentStore& store, const doc::ops::EditResult& e, const char* what) {
  must(e.ok(), what);
  must(store.dispatch(e->command).ok(), what);
  return *e;
}

inline void dispatch(doc::DocumentStore& store, const doc::Command& c, const char* what) {
  must(store.dispatch(c).ok(), what);
}

constexpr Ticks kBar = kTicksPerWhole;  // a 4/4 bar
constexpr Ticks kBeat = kPpq;
constexpr Ticks k16 = kTicksPerWhole / 16;

inline doc::Note note(Ticks start, Ticks length, tuning::PitchId pitch, float velocity = 100.0f) {
  doc::Note n;
  n.start = start;
  n.length = length;
  n.pitch = pitch;
  n.velocity = velocity;
  return n;
}
inline tuning::PitchId deg(int32_t d, int32_t equave = 0) { return tuning::PitchId::fromDegree(d, equave); }

// A clip on `track` over [start, end) bars with `notes` (content ticks), looped over [0, loopBars) bars or not.
inline doc::ClipRef clip(doc::DocumentStore& store, doc::TrackId track, double startBars, double endBars, const char* name,
                         const std::vector<doc::Note>& notes, double loopBars = 0.0) {
  const auto s = static_cast<Ticks>(startBars * static_cast<double>(kBar));
  const auto e = static_cast<Ticks>(endBars * static_cast<double>(kBar));
  const doc::ops::Edit c = run(store, doc::ops::insertEmptyClip(*store.current(), store.ids(), track, s, e, name), "insert clip");
  const doc::ClipRef ref = c.clips.at(0);
  if (!notes.empty()) run(store, doc::ops::addNotes(*store.current(), store.ids(), ref, notes), "add notes");
  doc::ClipLoop loop;
  loop.enabled = loopBars > 0.0;
  loop.start = 0;
  loop.end = loop.enabled ? static_cast<Ticks>(loopBars * static_cast<double>(kBar)) : e - s;
  dispatch(store, doc::SetClipLoop{ref, loop}, "clip loop");
  return ref;
}

inline doc::TrackId track(doc::DocumentStore& store, doc::TrackKind kind, doc::TrackId after, std::string name, uint32_t rgb) {
  const doc::ops::Edit t = run(store, doc::ops::createTrack(*store.current(), store.ids(), kind, after, std::move(name), doc::Colour{rgb}),
                               "create track");
  return t.tracks.at(0);
}

}  // namespace fixture_detail

// The `arrangement-basic` / `arrangement-zoomed-out` project. Colours are from the theme's clip palette.
inline void buildArrangementProject(doc::DocumentStore& store) {
  using namespace fixture_detail;
  using doc::TrackKind;
  // Timeline: 124 BPM, 128 from bar 9; 4/4, 3/4 from bar 17; loop bars 5–9 on; four locators.
  dispatch(store, doc::SetTempoMap{{{0, 124.0, TempoCurve::Step}, {8 * kBar, 128.0, TempoCurve::Step}}}, "tempo");
  dispatch(store, doc::SetTimeSignatures{{{0, {4, 4}}, {16, {3, 4}}}}, "signatures");
  dispatch(store, doc::SetLoop{{4 * kBar, 8 * kBar, true}}, "loop");
  const char* locNames[] = {"Intro", "Verse", "Chorus", "Outro"};
  const Ticks locTicks[] = {0, 4 * kBar, 8 * kBar, 16 * kBar};
  for (int i = 0; i < 4; ++i) dispatch(store, doc::AddLocator{{store.ids().locator(), locTicks[i], locNames[i]}}, "locator");

  const doc::TrackId drums = track(store, TrackKind::Midi, {}, "Drums", 0xEF7B7B);
  const doc::TrackId bass = track(store, TrackKind::Midi, drums, "Bass", 0xE68632);
  const doc::TrackId piano = track(store, TrackKind::Midi, bass, "Piano", 0x46A5E8);
  const doc::TrackId strings = track(store, TrackKind::Midi, piano, "Strings", 0xBB86F0);
  const doc::TrackId riser = track(store, TrackKind::Midi, strings, "Riser", 0x16B293);
  // Keys = [Piano, Pads = [Strings]] (nested), FX = [Riser] (folded).
  const doc::TrackId pads = run(store, doc::ops::groupTracks(*store.current(), store.ids(), std::vector{strings}, "Pads"), "group").tracks.at(0);
  const doc::TrackId keys =
      run(store, doc::ops::groupTracks(*store.current(), store.ids(), std::vector{piano, pads}, "Keys"), "group").tracks.at(0);
  const doc::TrackId fx = run(store, doc::ops::groupTracks(*store.current(), store.ids(), std::vector{riser}, "FX"), "group").tracks.at(0);
  run(store, doc::ops::setTracksColour(*store.current(), std::vector{keys}, doc::Colour{0x8499F0}), "colour");
  run(store, doc::ops::setTracksColour(*store.current(), std::vector{pads}, doc::Colour{0xDDC5F5}), "colour");
  run(store, doc::ops::setTracksColour(*store.current(), std::vector{fx}, doc::Colour{0x6FE697}), "colour");
  // Strings play in 31-EDO (a private tuning): their Degree notes resolve there.
  {
    auto edo = tuning::Edo::make(31);
    must(edo.ok(), "31-EDO");
    dispatch(store, doc::SetTrackTuning{strings, doc::TuningAssignment::privateTuning(doc::TuningDef{*edo, tuning::kDefaultReferenceHz})},
             "tuning");
  }

  // Drums: a one-bar beat looped over 8 bars, a 16th fill, then a two-bar beat looped to bar 17.
  std::vector<doc::Note> beat;
  for (int i = 0; i < 4; ++i) beat.push_back(note(i * kBeat, k16, deg(i % 2 == 0 ? 0 : 2), 110.0f));  // kick / snare
  for (int i = 0; i < 8; ++i) beat.push_back(note(i * kBeat / 2, k16, deg(6), i % 2 ? 70.0f : 95.0f));  // hats
  clip(store, drums, 0, 8, "Beat", beat, 1.0);
  std::vector<doc::Note> fill;
  for (int i = 0; i < 16; ++i) fill.push_back(note(i * k16, k16, deg(i < 8 ? 2 : (i < 12 ? 4 : 5)), 80.0f + 2.0f * static_cast<float>(i)));
  clip(store, drums, 8, 9, "Fill", fill);
  std::vector<doc::Note> beat2 = beat;
  for (const doc::Note& n : beat) beat2.push_back(note(n.start + kBar, n.length, n.pitch, n.velocity));
  beat2.push_back(note(kBar + 3 * kBeat + kBeat / 2, k16, deg(0), 100.0f));
  clip(store, drums, 9, 17, "Beat 2", beat2, 2.0);

  // Bass: walking eighths; the third clip is deactivated.
  auto bassLine = [&](int root) {
    std::vector<doc::Note> v;
    const int steps[] = {0, 0, 7, 0, 10, 0, 7, 5};
    for (int bar = 0; bar < 4; ++bar)
      for (int i = 0; i < 8; ++i) v.push_back(note(bar * kBar + i * kBeat / 2, kBeat / 2 - k16 / 2, deg(root + steps[i] + (bar == 2 ? 5 : 0), -2)));
    return v;
  };
  clip(store, bass, 0, 4, "Bass A", bassLine(0));
  clip(store, bass, 4, 8, "Bass B", bassLine(3));
  const doc::ClipRef off = clip(store, bass, 8, 12, "Bass A", bassLine(0));
  dispatch(store, doc::SetClipActive{off, false}, "deactivate");
  clip(store, bass, 12, 16, "Bass B", bassLine(3));

  // Piano: chords, partly as just ratios (5/4, 3/2) and one cents note.
  std::vector<doc::Note> chords;
  const int roots[] = {0, -4, 5, 3};
  for (int i = 0; i < 4; ++i) {
    const Ticks at = i * kBar;
    chords.push_back(note(at, kBar - kBeat / 2, deg(roots[i])));
    chords.push_back(note(at, kBar - kBeat / 2, deg(roots[i] + 4)));
    chords.push_back(note(at, kBar - kBeat / 2, deg(roots[i] + 7)));
    chords.push_back(note(at + 2 * kBeat, kBeat, deg(roots[i] + 12)));
  }
  clip(store, piano, 0, 4, "Chords", chords);
  std::vector<doc::Note> ji;
  for (int i = 0; i < 4; ++i) {
    const Ticks at = i * kBar;
    ji.push_back(note(at, kBar, tuning::PitchId::fromRatio(tuning::Rational(1, 1))));
    ji.push_back(note(at, kBar, tuning::PitchId::fromRatio(tuning::Rational(5, 4))));
    ji.push_back(note(at, kBar, tuning::PitchId::fromRatio(tuning::Rational(3, 2))));
    ji.push_back(note(at + kBeat, 2 * kBeat, tuning::PitchId::fromCents(968.8)));
  }
  clip(store, piano, 4, 12, "Just Chords", ji, 4.0);

  // Strings (31-EDO): long notes stepping by single 31-EDO degrees.
  std::vector<doc::Note> pad;
  for (int i = 0; i < 8; ++i) {
    pad.push_back(note(i * kBar, 2 * kBar, deg(i % 4)));
    pad.push_back(note(i * kBar, kBar, deg(10 + i)));
    pad.push_back(note(i * kBar + kBar / 2, kBar / 2, deg(18 + (i % 3))));
  }
  clip(store, strings, 4, 12, "Pad 31", pad);

  // Riser (inside the folded FX group): rising 16ths.
  std::vector<doc::Note> rise;
  for (int i = 0; i < 32; ++i) rise.push_back(note(i * k16, k16, deg(i)));
  clip(store, riser, 6, 8, "Riser", rise);
  clip(store, riser, 14, 16, "Riser", rise);

  doc::ops::setFolded(store.view(), fx, true);
  store.view().tracks[drums].height = 60;
}

// The perf project: `tracks` MIDI tracks, `clips` one-bar clips each (one bar apart: 2 bars per clip), `notes`
// notes per clip (16ths over four pitches + longer notes). Built with direct AddClip commands (fast).
inline void buildLargeArrangement(doc::DocumentStore& store, int tracks, int clips, int notes) {
  using namespace fixture_detail;
  doc::TrackId prev{};
  const uint32_t palette[] = {0xEF7B7B, 0xE68632, 0xB39E16, 0x4BB416, 0x16B293, 0x46A5E8, 0x8499F0, 0xBB86F0};
  std::vector<doc::Note> ns(static_cast<size_t>(notes));
  for (int t = 0; t < tracks; ++t) {
    const doc::TrackId id = track(store, doc::TrackKind::Midi, prev, {}, palette[t % 8]);
    prev = id;
    doc::Batch batch;
    for (int c = 0; c < clips; ++c) {
      doc::NoteClip clip;
      clip.id = store.ids().clip();
      clip.name = "Clip " + std::to_string(c + 1);
      clip.colour = doc::Colour{palette[(t + c) % 8]};
      clip.start = static_cast<Ticks>(c) * 2 * kBar;
      clip.length = kBar;
      for (int i = 0; i < notes; ++i) {
        doc::Note& n = ns[static_cast<size_t>(i)];
        n = doc::Note{};
        n.id = store.ids().note();
        n.start = (i * kBar) / notes;
        n.length = i % 5 == 0 ? kBeat : k16;
        n.pitch = deg((i * 7 + t) % 24, 0);
      }
      auto seq = doc::NoteSeq::fromNotes(ns);
      must(seq.ok(), "note seq");
      clip.notes = std::move(*seq);
      batch.commands.push_back(doc::AddClip{id, std::make_shared<const doc::NoteClip>(std::move(clip))});
    }
    dispatch(store, batch, "add clips");
  }
}

}  // namespace pq::views::test
