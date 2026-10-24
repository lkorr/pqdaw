#pragma once

// The T3.15 project (tests/views/fixtures/automation): automation lanes of every kind the arrangement draws —
// straight and curved segments, a jump, shapes (triangle, sine, square), a send, the activator, a main tuning's MOS
// generator automated on a track, a group's lane — plus tracks without automation (their lanes show the dashed
// current value). Built through the document's commands and edit operations; the fixture test keeps the committed
// file equal to what this builds. No doctest here (benchmarks include it too).
//
// Lane visibility, the chosen parameters and automation mode are view state (not in the file): the scenes and tests
// set them.
//
// buildAutomationBench: `tracks` MIDI tracks with one clip each and a volume lane of `points` breakpoints (every
// fourth segment curved), for the lane-paint and drag benchmarks.

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "arrangement_fixture.h"

namespace pq::views::test {

namespace automation_detail {

using fixture_detail::kBar;
using fixture_detail::kBeat;

// A lane on `track` automating `param` with `points` ({bar position, value, curve}); ids from the store.
struct Pt {
  double bars;
  double value;
  float curve = 0.0f;
};
inline doc::LaneId lane(doc::DocumentStore& store, doc::TrackId track, doc::ParamRef param, const std::vector<Pt>& points) {
  auto l = std::make_shared<doc::AutomationLane>();
  l->id = store.ids().lane();
  l->param = param;
  for (const Pt& p : points)
    l->points.push_back({static_cast<Ticks>(p.bars * static_cast<double>(kBar)), static_cast<float>(p.value), p.curve, store.ids().breakpoint()});
  fixture_detail::dispatch(store, doc::AddAutomationLane{track, l}, "add lane");
  return l->id;
}

inline void shape(doc::DocumentStore& store, doc::TrackId track, doc::LaneId lane, double fromBars, double toBars,
                  doc::ops::ShapeKind kind, double low, double high, int32_t cycles) {
  doc::ops::ShapeParams sp;
  sp.kind = kind;
  sp.low = low;
  sp.high = high;
  sp.cycles = cycles;
  fixture_detail::run(store,
                      doc::ops::insertShape(*store.current(), store.ids(), track, lane, static_cast<Ticks>(fromBars * static_cast<double>(kBar)),
                                            static_cast<Ticks>(toBars * static_cast<double>(kBar)), sp),
                      "shape");
}

}  // namespace automation_detail

inline void buildAutomationProject(doc::DocumentStore& store) {
  using namespace fixture_detail;
  using namespace automation_detail;
  using doc::ParamKind;
  using doc::ParamRef;
  using doc::TrackKind;
  dispatch(store, doc::SetTempoMap{{{0, 120.0, TempoCurve::Step}}}, "tempo");

  // A second main tuning: 5L 2s (meantone generator), whose generator is automated on the Piano track.
  const doc::TuningId mos = store.ids().tuning();
  dispatch(store, doc::AddTuning{{mos, "5L 2s", doc::Colour{0xE0A63A}, doc::TuningDef{tuning::MosParams{}, tuning::kDefaultReferenceHz}}},
           "tuning");

  const doc::TrackId drums = track(store, TrackKind::Midi, {}, "Drums", 0xEF7B7B);
  const doc::TrackId bass = track(store, TrackKind::Midi, drums, "Bass", 0xE68632);
  const doc::TrackId piano = track(store, TrackKind::Midi, bass, "Piano", 0x46A5E8);
  const doc::TrackId pad = track(store, TrackKind::Midi, piano, "Pad", 0xBB86F0);
  const doc::TrackId lead = track(store, TrackKind::Midi, pad, "Lead", 0x16B293);
  const doc::TrackId keys =
      run(store, doc::ops::groupTracks(*store.current(), store.ids(), std::vector{piano, pad}, "Keys"), "group").tracks.at(0);
  run(store, doc::ops::setTracksColour(*store.current(), std::vector{keys}, doc::Colour{0x8499F0}), "colour");
  const doc::TrackId reverb = track(store, TrackKind::Return, {}, "A Reverb", 0x92A7FF);
  dispatch(store, doc::SetTrackTuning{piano, doc::TuningAssignment::mainTuning(mos)}, "tuning");
  dispatch(store, doc::SetTrackVolume{pad, -4.0f}, "volume");
  dispatch(store, doc::SetSendLevel{lead, reverb, -12.0f}, "send");

  // Clips.
  std::vector<doc::Note> beat;
  for (int i = 0; i < 8; ++i) beat.push_back(note(i * kBeat / 2, k16, deg(i % 2 ? 6 : (i % 4 ? 2 : 0)), 100.0f));
  clip(store, drums, 0, 8, "Beat", beat, 1.0);
  std::vector<doc::Note> line;
  for (int i = 0; i < 8; ++i) line.push_back(note(i * kBeat / 2, kBeat / 2, deg((i * 5) % 9, -1)));
  clip(store, bass, 0, 8, "Bass", line, 1.0);
  std::vector<doc::Note> chords;
  for (int i = 0; i < 4; ++i)
    for (int d : {0, 2, 4}) chords.push_back(note(i * kBar, kBar - kBeat / 2, deg(d + (i % 2) * 3)));
  clip(store, piano, 0, 4, "Chords", chords);
  clip(store, piano, 4, 8, "Chords", chords);
  std::vector<doc::Note> pads;
  for (int d : {0, 4, 7}) pads.push_back(note(0, 2 * kBar, deg(d)));
  clip(store, pad, 0, 8, "Pad", pads, 2.0);
  std::vector<doc::Note> mel;
  for (int i = 0; i < 12; ++i) mel.push_back(note(i * kBeat, kBeat - k16, deg((i * 3) % 12, 1)));
  clip(store, lead, 2, 5, "Hook", mel);
  clip(store, lead, 5, 8, "Hook", mel);

  // Drums volume: straight and curved segments and a jump at bar 6.
  lane(store, drums, ParamRef::track(ParamKind::TrackVolume, drums),
       {{0.0, 0.0}, {2.0, -12.0, 0.6f}, {4.0, -3.0, -0.5f}, {6.0, -3.0}, {6.0, -20.0, 0.8f}, {8.0, -6.0}});
  // Bass pan: a triangle LFO over the 8 bars.
  const doc::LaneId bassPan = lane(store, bass, ParamRef::track(ParamKind::TrackPan, bass), {});
  shape(store, bass, bassPan, 0.0, 8.0, doc::ops::ShapeKind::Triangle, -0.8, 0.8, 4);
  // Keys (group) pan: a sine.
  const doc::LaneId keysPan = lane(store, keys, ParamRef::track(ParamKind::TrackPan, keys), {});
  shape(store, keys, keysPan, 1.0, 7.0, doc::ops::ShapeKind::Sine, -0.6, 0.6, 2);
  // The 5L 2s generator, automated on the Piano track: 696 → 705 ¢ (curved) → 690 ¢.
  lane(store, piano, ParamRef::tuning(ParamKind::MosGenerator, mos), {{0.0, 696.0}, {4.0, 705.0, 0.4f}, {8.0, 690.0}});
  // Lead: a volume ramp in, the reverb send, the activator gating the second half.
  lane(store, lead, ParamRef::track(ParamKind::TrackVolume, lead), {{2.0, -30.0, -0.7f}, {4.0, 0.0}, {7.0, -2.0, 0.5f}, {8.0, -18.0}});
  lane(store, lead, ParamRef::track(ParamKind::TrackSend, lead, reverb), {{2.0, -40.0}, {5.0, -6.0}, {8.0, -24.0}});
  const doc::LaneId gate = lane(store, lead, ParamRef::track(ParamKind::TrackMute, lead), {});
  shape(store, lead, gate, 4.0, 8.0, doc::ops::ShapeKind::Square, 0.0, 1.0, 4);

  doc::ViewState& v = store.view();
  v.tracks[drums].height = 56;
  v.tracks[lead].height = 56;
}

// Benchmark project: `tracks` MIDI tracks, one 32-bar clip each, a volume lane of `points` breakpoints over 64 bars
// (every fourth segment curved).
inline void buildAutomationBench(doc::DocumentStore& store, int tracks, int points) {
  using namespace fixture_detail;
  doc::TrackId prev{};
  for (int t = 0; t < tracks; ++t) {
    const doc::TrackId id = track(store, doc::TrackKind::Midi, prev, "T" + std::to_string(t + 1), 0x46A5E8);
    prev = id;
    clip(store, id, 0, 32, "Clip", {note(0, kBeat, deg(0))});
    auto l = std::make_shared<doc::AutomationLane>();
    l->id = store.ids().lane();
    l->param = doc::ParamRef::track(doc::ParamKind::TrackVolume, id);
    const Ticks span = 64 * kBar;
    for (int i = 0; i < points; ++i) {
      const Ticks at = span * i / points;
      const double v = -30.0 + 30.0 * (0.5 + 0.5 * std::sin(0.05 * i + t));
      l->points.push_back({at, static_cast<float>(v), (i % 4 == 0) ? 0.5f : 0.0f, store.ids().breakpoint()});
    }
    dispatch(store, doc::AddAutomationLane{id, l}, "add lane");
  }
}

}  // namespace pq::views::test
