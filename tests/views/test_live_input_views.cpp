// T6.4: the views' side of live input, through real dispatch — the computer MIDI keyboard (M toggles it; the letter rows
// send TransportPort::liveKey, a release ends what its press started, Z / X / C / V, repeats and command chords pass, a
// focused text field keeps its keys) and the mixer view's input section (MIDI From, channel, Keys / Hz, Monitor: one
// undo step each, never structural); T6.6: the strip's Track Delay and latency readout.
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/computer_keyboard.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  FakeAudioPrefsPort prefs = FakeAudioPrefsPort::studio();
  Fixture() {
    REQUIRE(ctx.openProject(views::test::fixtureDir("mixer")).ok());
    ctx.setAudioPrefs(&prefs);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE_MESSAGE(found != nullptr, name);
    return *found;
  }
  std::vector<FakeTransport::Record> keys() const {
    std::vector<FakeTransport::Record> v;
    for (const auto& r : port.calls)
      if (r.call == FakeTransport::Call::LiveKey) v.push_back(r);
    return v;
  }
  Vec2 centre(const Widget& w) const {
    const IRect r = w.rect();
    return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
  }
  Menu* openMenu(Control* c) {
    REQUIRE(c != nullptr);
    d.click(centre(*c));
    auto* menu = dynamic_cast<Menu*>(shell->overlay().top());
    REQUIRE(menu != nullptr);
    return menu;
  }
  int32_t itemOf(Menu* m, std::string_view label) {
    for (size_t i = 0; i < m->items().size(); ++i)
      if (m->items()[i].label == label) return static_cast<int32_t>(i);
    FAIL("no item " << label);
    return 0;
  }
};

}  // namespace

TEST_CASE("computer keyboard: M toggles it; letters play degrees, a release ends its own note, Z X C V move it") {
  Fixture f;
  ComputerKeyboard& kb = f.ctx.computerKeyboard();
  f.d.keyDown('a');  // off: no note
  f.d.keyUp('a');
  CHECK(f.keys().empty());
  f.d.keyDown('m');
  f.d.keyUp('m');
  CHECK(kb.active());
  CHECK(f.ctx.status().find("Computer MIDI Keyboard on") != std::string::npos);
  f.d.keyDown('a');
  f.d.keyDown('a', mod::kNone, true);  // auto-repeat: nothing more
  REQUIRE(f.keys().size() == 1);
  CHECK(f.keys()[0].on);
  CHECK(f.keys()[0].equave == 0);
  CHECK(f.keys()[0].step == 0);
  CHECK(f.keys()[0].velocity == 100.0f);
  f.d.keyDown('x');  // an equave up while A is held
  f.d.keyUp('x');
  CHECK(kb.equave() == 1);
  f.d.keyDown(';');  // step 16 at equave 1
  f.d.keyUp('a');    // A ends its own note (equave 0, step 0)
  REQUIRE(f.keys().size() == 3);
  CHECK(f.keys()[1].step == 16);
  CHECK(f.keys()[1].equave == 1);
  CHECK_FALSE(f.keys()[2].on);
  CHECK(f.keys()[2].equave == 0);
  CHECK(f.keys()[2].step == 0);
  f.d.keyDown('c');  // velocity −20
  f.d.keyUp('c');
  CHECK(kb.velocity() == 80);
  f.d.keyDown('w');
  CHECK(f.keys().back().velocity == 80.0f);
  CHECK(f.keys().back().step == 1);
  // Ctrl+S is not a note.
  const size_t before = f.keys().size();
  f.d.keyDown('s', mod::kCtrl);
  f.d.keyUp('s', mod::kCtrl);
  CHECK(f.keys().size() == before);
  // M again: off, and every held note (; and W) ends.
  f.d.keyDown('m');
  CHECK_FALSE(kb.active());
  CHECK(kb.heldKeys() == 0);
  const auto k = f.keys();
  CHECK(std::count_if(k.begin(), k.end(), [](const FakeTransport::Record& r) { return !r.on; }) == 3);
  f.d.keyUp('w');  // the release after the note already ended: nothing
  f.d.keyUp(';');
  CHECK(f.keys().size() == k.size());
}

TEST_CASE("computer keyboard: a focused text field keeps its letters; the release of a held key still ends its note") {
  Fixture f;
  ComputerKeyboard& kb = f.ctx.computerKeyboard();
  kb.setActive(true);
  f.d.keyDown('d');  // step 4, before the field has the focus
  f.d.press("Ctrl+F");  // the browser's search field takes the focus
  f.d.step();
  REQUIRE(f.d.tree().focus() != nullptr);
  REQUIRE(f.d.tree().focus()->acceptsTextInput());
  f.d.keyDown('a');  // the field types it
  CHECK(f.keys().size() == 1);
  f.d.keyUp('d');  // ends the note even though the field has the focus
  REQUIRE(f.keys().size() == 2);
  CHECK_FALSE(f.keys()[1].on);
  CHECK(f.keys()[1].step == 4);
}

TEST_CASE("mixer view: a MIDI strip's input section edits MIDI From, channel, note input and monitor (one step each)") {
  Fixture f;
  MixerView* mixer = findMixer(*f.shell);
  REQUIRE(mixer != nullptr);
  mixer->setShown(true);
  f.d.tree().layoutNow();
  const doc::TrackId piano = f.track("Piano").id;
  MixerStrip* s = mixer->strip(piano);
  REQUIRE(s != nullptr);
  REQUIRE(s->midiIn() != nullptr);
  CHECK(mixer->strip(f.track("A Reverb").id)->midiIn() == nullptr);  // returns have none
  const size_t h0 = f.ctx.store().history().cursor();

  Menu* m = f.openMenu(s->midiIn());
  // All Ins, Computer Keyboard, the connected or enabled ports (KeyStep, loopMIDI, Launchkey), ─, No Input.
  CHECK(m->items()[0].label == "All Ins");
  CHECK(m->items()[0].checked);
  m->choose(f.itemOf(m, "Arturia KeyStep 37"));
  f.d.step();
  CHECK(f.track("Piano").mixer.midiIn.kind == doc::MidiInputKind::Port);
  CHECK(f.track("Piano").mixer.midiIn.port == "Arturia KeyStep 37");
  CHECK_FALSE(f.ctx.store().history().entries().back().changes.structural());

  m = f.openMenu(s->midiChannel());
  m->choose(f.itemOf(m, "Channel 3"));
  f.d.step();
  CHECK(f.track("Piano").mixer.midiIn.channel == 3);
  CHECK(f.track("Piano").mixer.midiIn.port == "Arturia KeyStep 37");  // the port kept

  m = f.openMenu(s->noteInput());
  m->choose(f.itemOf(m, "Hz (raw pitch)"));
  f.d.step();
  CHECK(f.track("Piano").noteInput.mode == doc::NoteInputMode::Hz);

  m = f.openMenu(s->monitor());
  m->choose(f.itemOf(m, "Monitor In (always)"));
  f.d.step();
  CHECK(f.track("Piano").mixer.monitor == doc::MonitorMode::In);
  CHECK(f.ctx.store().history().cursor() == h0 + 4);
  CHECK_FALSE(f.ctx.store().history().entries().back().changes.structural());
}

TEST_CASE("mixer view + T6.6: Track Delay (type-in, drag, Delete) and the strip's latency readout; none on the master") {
  Fixture f;
  MixerView* mixer = findMixer(*f.shell);
  REQUIRE(mixer != nullptr);
  mixer->setShown(true);
  f.d.tree().layoutNow();
  const doc::TrackId piano = f.track("Piano").id;
  f.port.trackLatency = {{piano.value, 2.5}};
  MixerStrip* s = mixer->strip(piano);
  REQUIRE(s != nullptr);
  REQUIRE(s->trackDelay() != nullptr);
  REQUIRE(s->latency() != nullptr);
  CHECK(mixer->strip(f.track("A Reverb").id)->trackDelay() != nullptr);  // returns have one too
  CHECK(mixer->strip(f.ctx.doc()->master->id)->trackDelay() == nullptr);
  CHECK(s->trackDelay()->valueText() == "0.0 ms");
  // The readout reads the transport when the strip is bound (and on the meter poll).
  REQUIRE(f.ctx.store().dispatch(doc::SetTrackDelay{piano, -12.5f}).ok());
  f.d.step();
  CHECK(s->trackDelay()->valueText() == "-12.5 ms");
  CHECK(s->latency()->text() == "Lat 2.5 ms");
  // A drag: one undo step, Meta (no compile).
  const size_t h0 = f.ctx.store().history().cursor();
  const Vec2 c = f.centre(*s->trackDelay());
  f.d.drag(c, {c.x, c.y - 20.0f});
  CHECK(f.track("Piano").mixer.trackDelayMs > -12.5f);
  CHECK(f.ctx.store().history().cursor() == h0 + 1);
  CHECK_FALSE(f.ctx.store().history().entries().back().changes.structural());
  // Delete: back to 0.
  f.d.click(c);
  f.d.press("Delete");
  f.d.step();
  CHECK(f.track("Piano").mixer.trackDelayMs == 0.0f);
}
