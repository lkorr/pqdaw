// MF.3: the clip menu's Export MIDI submenu by real dispatch: a right-click on a MIDI clip, Export MIDI > Degrees… asks
// for the file (suggested "<clip>.mid"), writes the clip there, says so in the status line and remembers the mode (the
// next menu checks it; drag-out uses it); a cancelled dialog writes nothing; on an audio clip the submenu is disabled.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/document/midi_file.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
namespace fs = std::filesystem;

namespace {

constexpr const char* kArr = "main/body/top/track-area/arrangement";

struct Fx {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  test::ScriptedDialogs dialogs;
  doc::ClipRef clip, wav;

  Fx() {
    doc::DocumentStore& st = ctx.store();
    auto run = [&](const doc::ops::EditResult& e) {
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      REQUIRE(st.dispatch(e->command).ok());
      return *e;
    };
    const doc::TrackId midi = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    std::vector<doc::Note> notes;
    for (int i = 0; i < 3; ++i) {
      doc::Note n;
      n.id = st.ids().note();
      n.start = i * kPpq;
      n.length = kPpq;
      n.pitch = tuning::PitchId::fromDegree(i);
      notes.push_back(n);
    }
    doc::ops::CapturedClip cc{midi, 0, kTicksPerWhole, false, notes, "Riff"};
    clip = run(doc::ops::captureClip(*st.current(), st.ids(), std::span<const doc::ops::CapturedClip>(&cc, 1))).clips.at(0);
    const doc::TrackId audio = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Audio, midi, "Vox")).tracks[0];
    doc::SampleRef s;
    s.id = st.ids().sample();
    s.path = "samples/vox.wav";
    s.frames = 4 * 48000;
    s.displayName = "vox";
    const doc::ops::NewAudioClip nc{audio, 0, s.id, "Vox"};
    wav = run(doc::ops::addAudioClips(*st.current(), st.ids(), std::span<const doc::SampleRef>(&s, 1),
                                      std::span<const doc::ops::NewAudioClip>(&nc, 1))).clips.at(0);
    ctx.setFileDialogs(&dialogs);
    auto sh = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(sh.ok(), (sh.ok() ? "" : sh.error().message));
    shell = std::move(*sh);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    arr->setTimeView(0.0, 400.0 / static_cast<double>(kTicksPerWhole));
    d.tree().layoutNow();
    d.step();
  }
  ~Fx() {
    ctx.setFileDialogs(nullptr);
    d.tree().setRoot(std::make_unique<Widget>());
  }
  OverlayLayer& layer() {
    OverlayLayer* l = OverlayLayer::of(*arr);
    REQUIRE(l != nullptr);
    return *l;
  }
  // A right-click on the clip: selects it (when it is not) and opens the clip menu.
  Menu& menuOn(const doc::ClipRef& c) {
    d.tree().layoutNow();
    const IRect box = arr->clipRect(c);
    REQUIRE_FALSE(box.empty());
    const Vec2 at{static_cast<float>(box.x) + 0.3f * static_cast<float>(box.w), static_cast<float>(box.y) + 0.7f * static_cast<float>(box.h)};
    d.click(at, mod::kNone, PointerButton::Right);
    d.step();
    d.tree().layoutNow();
    auto* m = dynamic_cast<Menu*>(layer().top());
    REQUIRE(m != nullptr);
    return *m;
  }
  static const MenuItem* item(const Menu& m, std::string_view label) {
    for (const MenuItem& it : m.items())
      if (it.label == label) return &it;
    return nullptr;
  }
};

}  // namespace

TEST_CASE("Export MIDI menu: Degrees… asks for the file, writes the clip, remembers the mode; cancel writes nothing; "
          "disabled on an audio clip") {
  Fx f;
  CHECK(f.ctx.viewPrefs().midiExportMode == doc::MidiExportMode::Mpe);  // the default
  const fs::path dir = fs::path(test::tempDir("midi_export_menu"));
  const fs::path out = dir / "riff.mid";
  f.dialogs.midiPath = out.string();
  Menu& m = f.menuOn(f.clip);
  const MenuItem* sub = Fx::item(m, "Export MIDI");
  REQUIRE(sub != nullptr);
  CHECK(sub->enabled);
  REQUIRE(sub->submenu.size() == 3);
  CHECK(sub->submenu[0].checked);  // MPE…
  test::clickMenuItem(f.d, f.layer(), "Export MIDI");
  test::clickMenuItem(f.d, f.layer(), "Degrees");
  CHECK(f.dialogs.midis == 1);
  CHECK(f.dialogs.midiSuggested == "Riff.mid");
  CHECK(f.ctx.viewPrefs().midiExportMode == doc::MidiExportMode::Degrees);
  CHECK(f.ctx.status().rfind("Exported 3 notes (Degrees)", 0) == 0);
  {
    std::ifstream in(out, std::ios::binary);
    REQUIRE(in);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto file = doc::parseMidiFile(bytes);
    REQUIRE(file.ok());
    const doc::MidiNotes n = doc::resolveNotes(*file);
    REQUIRE(n.notes.size() == 3);
    for (size_t i = 0; i < 3; ++i) CHECK(n.notes[i].key == 60 + i);
  }
  // The next menu checks Degrees; a cancelled dialog writes nothing.
  std::error_code ec;
  fs::remove(out, ec);
  f.dialogs.midiPath.clear();
  Menu& again = f.menuOn(f.clip);
  const MenuItem* sub2 = Fx::item(again, "Export MIDI");
  REQUIRE(sub2 != nullptr);
  CHECK(sub2->submenu[2].checked);
  CHECK_FALSE(sub2->submenu[0].checked);
  test::clickMenuItem(f.d, f.layer(), "Export MIDI");
  test::clickMenuItem(f.d, f.layer(), "12edo");
  CHECK(f.dialogs.midis == 2);
  CHECK_FALSE(fs::exists(out));
  CHECK(f.ctx.viewPrefs().midiExportMode == doc::MidiExportMode::TwelveEdo);
  // An audio clip: the submenu is there but disabled.
  Menu& onAudio = f.menuOn(f.wav);
  const MenuItem* sub3 = Fx::item(onAudio, "Export MIDI");
  REQUIRE(sub3 != nullptr);
  CHECK_FALSE(sub3->enabled);
  fs::remove_all(dir, ec);
}
