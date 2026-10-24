#pragma once

// The T3.10 project (tests/views/fixtures/mixer): track headers with their mixer sections, a group, two returns and
// the master, every mixer state the header shows (volume, pan, activator off, solo, arm, sends, Pre/Post, an output
// routed to a group, Main / inherited / private tunings). Built through the document's edit operations; the fixture
// test keeps the committed file equal to what this builds. No doctest here (benchmarks include it too).

#include <string>
#include <vector>

#include "arrangement_fixture.h"

namespace pq::views::test {

inline void buildMixerProject(doc::DocumentStore& store) {
  using namespace fixture_detail;
  using doc::TrackKind;
  dispatch(store, doc::SetTempoMap{{{0, 122.0, TempoCurve::Step}}}, "tempo");

  // A second main tuning (31-EDO, its own colour) next to the default 12-EDO.
  auto edo31 = tuning::Edo::make(31);
  must(edo31.ok(), "31-EDO");
  const doc::TuningId t31 = store.ids().tuning();
  dispatch(store, doc::AddTuning{{t31, "31-EDO", doc::Colour{0xE0A63A}, doc::TuningDef{*edo31, tuning::kDefaultReferenceHz}}}, "tuning");

  const doc::TrackId drums = track(store, TrackKind::Midi, {}, "Drums", 0xEF7B7B);
  const doc::TrackId bass = track(store, TrackKind::Midi, drums, "Bass", 0xE68632);
  const doc::TrackId piano = track(store, TrackKind::Midi, bass, "Piano", 0x46A5E8);
  const doc::TrackId pad = track(store, TrackKind::Midi, piano, "Pad", 0xBB86F0);
  const doc::TrackId lead = track(store, TrackKind::Midi, pad, "Lead", 0x16B293);
  const doc::TrackId keys =
      run(store, doc::ops::groupTracks(*store.current(), store.ids(), std::vector{piano, pad}, "Keys"), "group").tracks.at(0);
  run(store, doc::ops::setTracksColour(*store.current(), std::vector{keys}, doc::Colour{0x8499F0}), "colour");
  const doc::TrackId reverb = track(store, TrackKind::Return, {}, "A Reverb", 0x92A7FF);
  const doc::TrackId delay = track(store, TrackKind::Return, reverb, "B Delay", 0x10A4EE);

  // Mixer states.
  dispatch(store, doc::SetTrackVolume{drums, -3.5f}, "volume");
  dispatch(store, doc::SetTrackVolume{bass, -6.0f}, "volume");
  dispatch(store, doc::SetTrackVolume{lead, 2.0f}, "volume");
  dispatch(store, doc::SetTrackVolume{reverb, -9.0f}, "volume");
  dispatch(store, doc::SetTrackVolume{store.current()->master->id, -1.5f}, "volume");
  dispatch(store, doc::SetTrackPan{piano, -0.4f}, "pan");
  dispatch(store, doc::SetTrackPan{lead, 0.24f}, "pan");
  dispatch(store, doc::SetTrackMute{pad, true}, "mute");
  dispatch(store, doc::SetTrackSolo{lead, true}, "solo");
  dispatch(store, doc::SetTrackArm{bass, true}, "arm");
  dispatch(store, doc::SetSendLevel{drums, reverb, -12.0f}, "send");
  dispatch(store, doc::SetSendLevel{piano, reverb, -6.0f}, "send");
  dispatch(store, doc::SetSendLevel{lead, delay, -3.0f}, "send");
  dispatch(store, doc::SetSendLevel{lead, reverb, -20.0f}, "send");
  run(store, doc::ops::setReturnSendsPreFader(*store.current(), delay, true), "pre");
  dispatch(store, doc::SetTrackOutput{lead, doc::Output{doc::OutputKind::Track, keys}}, "output");
  // Tunings: Bass on the 31-EDO main tuning, Pad a private 22-EDO, the rest inherit 12-EDO.
  dispatch(store, doc::SetTrackTuning{bass, doc::TuningAssignment::mainTuning(t31)}, "tuning");
  {
    auto edo22 = tuning::Edo::make(22);
    must(edo22.ok(), "22-EDO");
    dispatch(store, doc::SetTrackTuning{pad, doc::TuningAssignment::privateTuning(doc::TuningDef{*edo22, tuning::kDefaultReferenceHz})},
             "tuning");
  }

  // A few clips so the lanes are not empty.
  std::vector<doc::Note> beat;
  for (int i = 0; i < 8; ++i) beat.push_back(note(i * kBeat / 2, k16, deg(i % 2 ? 6 : (i % 4 ? 2 : 0)), 100.0f));
  clip(store, drums, 0, 8, "Beat", beat, 1.0);
  std::vector<doc::Note> line;
  for (int i = 0; i < 8; ++i) line.push_back(note(i * kBeat / 2, kBeat / 2, deg((i * 5) % 9, -1)));
  clip(store, bass, 0, 4, "Bass", line, 1.0);
  std::vector<doc::Note> chords;
  for (int i = 0; i < 4; ++i)
    for (int d : {0, 4, 7}) chords.push_back(note(i * kBar, kBar - kBeat / 2, deg(d + (i % 2) * 5)));
  clip(store, piano, 0, 4, "Chords", chords);
  std::vector<doc::Note> pads;
  for (int d : {0, 7, 13}) pads.push_back(note(0, 2 * kBar, deg(d)));
  clip(store, pad, 2, 6, "Pad 22", pads, 2.0);
  std::vector<doc::Note> mel;
  for (int i = 0; i < 12; ++i) mel.push_back(note(i * kBeat, kBeat - k16, deg((i * 3) % 12, 1)));
  clip(store, lead, 4, 7, "Hook", mel);

  doc::ViewState& v = store.view();
  v.tracks[drums].height = 64;  // three lines: sends shown
  v.tracks[piano].height = 64;
  v.tracks[lead].height = 64;
  v.tracks[keys].height = 44;
  v.tracks[reverb].height = 40;
  v.tracks[store.current()->master->id].height = 40;
}

}  // namespace pq::views::test
