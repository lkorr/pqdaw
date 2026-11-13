// MF.3: MIDI clips dragged out of the window (ShellApp::onDragOut → pq/app/clip_export.h dragOutClips, the part before
// the OLE drag): each note clip becomes "<folder>/<clip>.mid" in the given mode (the app passes ViewPrefs::
// midiExportMode), a taken name gets " 2"; an audio clip without a playback engine is refused with a reason.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pq/app/clip_export.h"
#include "pq/document/edit_ops.h"
#include "pq/document/midi_file.h"
#include "pq/document/store.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

fs::path pathOf(const std::string& u) { return fs::path(std::u8string(u.begin(), u.end())); }

MidiFile readMid(const std::string& path) {
  std::ifstream in(pathOf(path), std::ios::binary);
  REQUIRE(in);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto f = parseMidiFile(bytes);
  REQUIRE_MESSAGE(f.ok(), (f.ok() ? std::string() : f.error().message));
  return std::move(*f);
}

ops::Edit apply(DocumentStore& s, const Result<ops::Edit>& e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  return *e;
}

}  // namespace

TEST_CASE("drag-out: a MIDI clip becomes <clip>.mid in the mode given (a taken name: \"<clip> 2.mid\"); an audio clip "
          "without an engine is refused") {
  DocumentStore store;
  const TrackId midi = apply(store, ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
  std::vector<Note> notes;
  for (int i = 0; i < 3; ++i) {
    Note n;
    n.id = store.ids().note();
    n.start = i * kPpq;
    n.length = kPpq;
    n.pitch = tuning::PitchId::fromDegree(i * 2);
    notes.push_back(n);
  }
  ops::CapturedClip cc{midi, 0, kTicksPerWhole, false, notes, "Riff: A/B"};
  const ClipRef clip = apply(store, ops::captureClip(*store.current(), store.ids(), std::span<const ops::CapturedClip>(&cc, 1))).clips.at(0);
  const TrackId audio = apply(store, ops::createTrack(*store.current(), store.ids(), TrackKind::Audio, midi, "Vox")).tracks.at(0);
  SampleRef s;
  s.id = store.ids().sample();
  s.path = "samples/vox.wav";
  s.frames = 48000;
  s.displayName = "vox";
  const ops::NewAudioClip nc{audio, 0, s.id, "Vox"};
  const ClipRef wav = apply(store, ops::addAudioClips(*store.current(), store.ids(), std::span<const SampleRef>(&s, 1),
                                                       std::span<const ops::NewAudioClip>(&nc, 1))).clips.at(0);

  const fs::path dir = fs::temp_directory_path() / "pq_app_midi_dragout";
  std::error_code ec;
  fs::remove_all(dir, ec);
  const std::vector<ClipRef> clips{clip, wav};
  app::ClipDragOut a = app::dragOutClips(nullptr, *store.current(), clips, dir, false, MidiExportMode::Mpe);
  REQUIRE(a.files.size() == 1);
  CHECK(a.lazy.empty());
  CHECK(a.refused == "no playback engine to render the clip");
  CHECK(pathOf(a.files[0]) == dir / "Riff_ A_B.mid");
  const MidiFile mpe = readMid(a.files[0]);
  const MidiNotes mn = resolveNotes(mpe);
  REQUIRE(mn.notes.size() == 3);
  bool bends = false;
  for (const MidiEvent& e : mpe.tracks.at(1).events) bends = bends || e.kind() == 0xE0;
  CHECK(bends);  // MPE: a bend before each note
  for (size_t i = 0; i < 3; ++i) {
    CHECK(mn.notes[i].key == 60 + 2 * i);
    CHECK(mn.notes[i].channel >= 1);
  }
  // Degrees: no bends, channel 1; the first file stays, the new one is " 2".
  app::ClipDragOut b = app::dragOutClips(nullptr, *store.current(), std::vector<ClipRef>{clip}, dir, false, MidiExportMode::Degrees);
  REQUIRE(b.files.size() == 1);
  CHECK(b.refused.empty());
  CHECK(pathOf(b.files[0]) == dir / "Riff_ A_B 2.mid");
  const MidiFile deg = readMid(b.files[0]);
  for (const MidiEvent& e : deg.tracks.at(1).events) CHECK(e.kind() != 0xE0);
  const MidiNotes dn = resolveNotes(deg);
  REQUIRE(dn.notes.size() == 3);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(dn.notes[i].key == 60 + 2 * i);
    CHECK(dn.notes[i].channel == 0);
  }
  fs::remove_all(dir, ec);
}
