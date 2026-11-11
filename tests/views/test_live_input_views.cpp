// T6.4: the views' side of live input, through real dispatch — the computer MIDI keyboard (M toggles it; the letter rows
// send TransportPort::liveKey, a release ends what its press started, Z / X / C / V, repeats and command chords pass, a
// focused text field keeps its keys) and the mixer view's input section (MIDI From, channel, Keys / Hz, Monitor: one
// undo step each, never structural); T6.6: the strip's Track Delay and latency readout.
#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>

#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/computer_keyboard.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/piano_roll/row_model.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"
#include "../../src/views/src/mixer/internal.h"
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
  // KB.1: arms `name` exclusively (Live's exclusive arm: the fixture's armed Bass is disarmed).
  void armOnly(std::string_view name) {
    auto e = doc::ops::armTrack(*ctx.doc(), track(name).id, false);
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
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
  f.armOnly("Drums");  // KB.1: the keyboard plays the armed 12-EDO track: Live's piano layout
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
  f.armOnly("Drums");  // KB.1: the keyboard plays the armed 12-EDO track: Live's piano layout
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

TEST_CASE("mixer view (M6 review L21): sends, pan and volume line up across track, group, return and master strips; "
          "\"In: Keys\"; the latency row only while a latency is not zero") {
  Fixture f;
  MixerView* mixer = findMixer(*f.shell);
  REQUIRE(mixer != nullptr);
  mixer->setShown(true);
  f.d.tree().layoutNow();
  MixerStrip* piano = mixer->strip(f.track("Piano").id);
  MixerStrip* keys = mixer->strip(f.track("Keys").id);    // a group
  MixerStrip* ret = mixer->strip(f.track("B Delay").id);  // a return: no send to itself
  MixerStrip* master = mixer->strip(f.ctx.doc()->master->id);
  REQUIRE((piano && keys && ret && master));
  CHECK(static_cast<mixer::Chooser*>(piano->noteInput())->text() == "In: Keys");
  for (MixerStrip* s : {keys, ret, master}) {
    CHECK(s->pan()->rect().y == piano->pan()->rect().y);
    CHECK(s->volume()->rect().y == piano->volume()->rect().y);
  }
  // Send A: the same row on the track, the group and the B return.
  REQUIRE(piano->sendCount() == 2);
  for (MixerStrip* s : {keys, ret}) {
    bool found = false;
    for (size_t i = 0; i < s->sendCount(); ++i)
      if (s->sendTarget(i) == piano->sendTarget(0)) {
        CHECK(s->send(i)->rect().y == piano->send(0)->rect().y);
        found = true;
      }
    CHECK(found);
  }
  // No latency anywhere: no latency row.
  CHECK_FALSE(piano->latency()->visible());
  const int32_t panY = piano->pan()->rect().y;
  // A latency on one track: the row appears on every strip (the others' cell empty), everything below moves together.
  f.port.trackLatency = {{f.track("Piano").id.value, 2.5}};
  REQUIRE(f.ctx.store().dispatch(doc::SetTrackDelay{f.track("Piano").id, -1.0f}).ok());
  f.d.step();
  f.d.tree().layoutNow();
  CHECK(piano->latency()->visible());
  CHECK(piano->latency()->text() == "Lat 2.5 ms");
  CHECK(keys->latency()->text().empty());
  CHECK(piano->pan()->rect().y > panY);
  CHECK(master->pan()->rect().y == piano->pan()->rect().y);
  CHECK(ret->pan()->rect().y == piano->pan()->rect().y);
}

// ---- KB.1: the layout follows the keyboard track's tuning; arm on select ------------------------------------------

namespace {

using KN = ComputerKeyboard::KeyNote;

// Presses and releases `k` (real dispatch); the note it played (equave, step), or nullopt when it played none.
std::optional<KN> play(Fixture& f, Key k) {
  const size_t before = f.keys().size();
  f.d.keyDown(k);
  f.d.keyUp(k);
  const auto v = f.keys();
  if (v.size() == before) return std::nullopt;
  REQUIRE(v.size() == before + 2);  // its note-on and the note-off of the release
  CHECK(v[before].on);
  CHECK_FALSE(v[before + 1].on);
  CHECK(v[before + 1].equave == v[before].equave);
  CHECK(v[before + 1].step == v[before].step);
  return KN{v[before].equave, v[before].step};
}

// TU.3: the track on a new named tuning with `def`.
void setOwnTuning(Fixture& f, std::string_view track, doc::TuningDef def) {
  const doc::TuningId id = test::addTuning(f.ctx.store(), std::move(def));
  REQUIRE(f.ctx.store().dispatch(doc::SetTrackTuning{f.track(track).id, doc::TuningAssignment::mainTuning(id)}).ok());
}

}  // namespace

TEST_CASE("KB.1 computer keyboard: 12 degrees per equave play Live's piano layout on the rows the piano roll draws white") {
  Fixture f;
  f.armOnly("Drums");  // 12-EDO (the project's first main tuning, inherited)
  ComputerKeyboard& kb = f.ctx.computerKeyboard();
  CHECK(f.ctx.keyboardTrack() == f.track("Drums").id);
  CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{true, 12});
  // The white keys are exactly the degrees the piano roll marks natural for this tuning (kRowNatural), the upper row's
  // the others.
  const std::vector<uint8_t> natural = roll::defaultHighlightScale(tuning::Tuning{});  // 12-EDO
  REQUIRE(natural.size() == 12);
  f.d.press("M");
  REQUIRE(kb.active());
  int32_t last = -1;
  for (const char c : std::string_view("asdfghjkl;")) {
    const auto n = play(f, static_cast<Key>(c));
    REQUIRE(n.has_value());
    CHECK(n->equave == 0);
    CHECK(n->degree > last);  // ascending
    CHECK(natural[static_cast<size_t>(n->degree % 12)] == 1);
    last = n->degree;
  }
  for (const char c : std::string_view("wetyuop")) {
    const auto n = play(f, static_cast<Key>(c));
    REQUIRE(n.has_value());
    CHECK(natural[static_cast<size_t>(n->degree % 12)] == 0);
  }
  CHECK(play(f, 'f') == KN{0, 5});
  CHECK(play(f, 'k') == KN{0, 12});
  CHECK_FALSE(play(f, 'r').has_value());  // no black key between E and F
  CHECK_FALSE(play(f, 'q').has_value());
  // Z / X / C / V shift (no notes here), the navigation keys too.
  f.d.press("Z");
  CHECK(kb.equave() == -1);
  CHECK(play(f, 'a') == KN{-1, 0});
  f.d.press("PageUp");
  f.d.press("PageUp");
  CHECK(kb.equave() == 1);
  f.d.press("V");
  CHECK(kb.velocity() == 120);
  f.d.press("End");
  CHECK(kb.velocity() == 100);
}

TEST_CASE("KB.1 computer keyboard: other sizes play the mos-explorer layout (1Row default, Continuous): 22-EDO, 5L 2s, 31-EDO") {
  Fixture f;
  ComputerKeyboard& kb = f.ctx.computerKeyboard();
  f.armOnly("Pad");  // a private 22-EDO
  CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 22});
  f.d.press("M");
  REQUIRE(kb.active());
  CHECK(kb.scaleMode() == ComputerKeyboard::ScaleMode::OneRow);
  SUBCASE("1Row over 22-EDO: home row = degrees, the bottom row an equave down, the upper one up, the number row two up") {
    CHECK(play(f, 'a') == KN{0, 0});
    CHECK(play(f, 'f') == KN{0, 3});
    CHECK(play(f, '\'') == KN{0, 10});
    CHECK(play(f, 'z') == KN{-1, 0});
    CHECK(play(f, ',') == KN{-1, 7});  // M's place is the toggle
    CHECK(play(f, '/') == KN{-1, 9});
    CHECK(play(f, 'q') == KN{1, 0});
    CHECK(play(f, '\\') == KN{1, 12});
    CHECK(play(f, '1') == KN{2, 0});
    CHECK(play(f, '=') == KN{2, 11});
    // Every letter is a note: the navigation keys shift.
    f.d.press("PageDown");
    CHECK(kb.equave() == -1);
    CHECK(play(f, 'x') == KN{-2, 1});
    f.d.press("Home");
    CHECK(kb.velocity() == 120);
    CHECK(play(f, 'a') == KN{-1, 0});
    CHECK(f.keys()[f.keys().size() - 2].velocity == 120.0f);
    f.d.press("PageDown");
    f.d.press("PageDown");
    CHECK(kb.equave() == -3);
    CHECK_FALSE(play(f, 'z').has_value());  // equave −4: out of range, no note (and no view action)
    // M still toggles (off: every held note ends).
    f.d.keyDown('a');
    CHECK(kb.heldKeys() == 1);
    f.d.press("M");
    CHECK_FALSE(kb.active());
    CHECK(kb.heldKeys() == 0);
  }
  SUBCASE("Continuous over 22-EDO: 35 consecutive degrees from A up, the bottom row below A") {
    kb.setScaleMode(ComputerKeyboard::ScaleMode::Continuous);
    CHECK(play(f, 'a') == KN{0, 0});
    CHECK(play(f, '\'') == KN{0, 10});
    CHECK(play(f, 'q') == KN{0, 11});
    CHECK(play(f, '\\') == KN{0, 23});
    CHECK(play(f, '1') == KN{0, 24});
    CHECK(play(f, '=') == KN{0, 35});
    CHECK(play(f, '/') == KN{-1, 21});  // one below A
    CHECK(play(f, 'z') == KN{-1, 12});  // ten below A
  }
  SUBCASE("a 7-note MOS (5L 2s in 31-EDO): 1Row and Continuous over its 7 degrees") {
    tuning::MosParams m;
    m.n = 7;
    m.generator = 18.0 * 1200.0 / 31.0;
    doc::TuningDef def;
    def.system = m;
    setOwnTuning(f, "Pad", def);
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 7});
    CHECK(play(f, 'j') == KN{0, 6});
    CHECK(play(f, 'k') == KN{0, 7});  // past the equave: degree 7 = the next equave's tonic (equave · 7 + 7)
    CHECK(play(f, 'b') == KN{-1, 4});
    kb.setScaleMode(ComputerKeyboard::ScaleMode::Continuous);
    CHECK(play(f, '/') == KN{-1, 6});
    CHECK(play(f, 'z') == KN{-2, 4});  // ten below A = −2 equaves + 4
  }
  SUBCASE("31-EDO (Bass): Continuous below A wraps into the equave below") {
    f.armOnly("Bass");
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 31});
    kb.setScaleMode(ComputerKeyboard::ScaleMode::Continuous);
    CHECK(play(f, '/') == KN{-1, 30});
    CHECK(play(f, '=') == KN{0, 35});
  }
}

TEST_CASE("KB.1 computer keyboard: a 12-TET key map plays the piano layout; several armed tracks: the selected one's") {
  Fixture f;
  // Bass (31-EDO) and Drums (12-EDO) both armed: the selected armed track lays it out, else the first armed one.
  auto both = doc::ops::armTrack(*f.ctx.doc(), f.track("Drums").id, true);
  REQUIRE(both.ok());
  REQUIRE(f.ctx.store().dispatch(both->command).ok());
  CHECK(f.track("Bass").mixer.arm);
  CHECK(f.ctx.keyboardTrack() == f.track("Drums").id);  // first in document order
  CHECK(f.ctx.keyboardLayout().piano);
  f.ctx.setSelectedTrack(f.track("Bass").id);
  CHECK(f.ctx.keyboardTrack() == f.track("Bass").id);
  CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 31});
  doc::NoteInput in = f.track("Bass").noteInput;
  in.keyMap = doc::InputKeyMap::TwelveTetNearest;
  REQUIRE(f.ctx.store().dispatch(doc::SetNoteInput{f.track("Bass").id, in}).ok());
  CHECK(f.ctx.keyboardLayout().piano);
}

// KB.2: two note clips on the 12-EDO Drums track far right of the fixture's clips: A [100, 104) bars retuned to a private
// 22-EDO (as the clip box's tuning menu does: SetClipTuning), B [104, 108) inheriting the track's 12-EDO.
TEST_CASE("KB.2 computer keyboard: plays its track's live clip's tuning (the clip view's clip, else the one under the "
          "start marker / playhead, else the track's)") {
  Fixture f;
  f.armOnly("Drums");
  const doc::TrackId drums = f.track("Drums").id;
  constexpr Ticks kBar = 4 * kPpq;
  auto addClip = [&](Ticks start) {
    doc::NoteClip c;
    c.id = f.ctx.store().ids().clip();
    c.name = "live";
    c.start = start;
    c.length = 4 * kBar;
    const doc::ClipId id = c.id;
    REQUIRE(f.ctx.store().dispatch(doc::AddClip{drums, std::make_shared<const doc::NoteClip>(std::move(c))}).ok());
    return id;
  };
  const doc::ClipId a = addClip(100 * kBar), b = addClip(104 * kBar);
  doc::TuningDef edo22;
  edo22.system = tuning::Edo::make(22).value();
  const doc::TuningId t22 = test::addTuning(f.ctx.store(), edo22);  // TU.3: a named tuning
  REQUIRE(f.ctx.store().dispatch(doc::SetClipTuning{{drums, a}, doc::TuningAssignment::mainTuning(t22)}).ok());
  // Nothing open, the start marker over no clip of these: the track's 12-EDO.
  CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{true, 12});
  f.d.press("M");
  REQUIRE(f.ctx.computerKeyboard().active());
  CHECK(play(f, 'f') == KN{0, 5});  // the piano layout

  SUBCASE("(1) the clip open in the clip view, wherever the start marker is") {
    f.ctx.setStartMarker(105 * kBar);  // over B
    f.ctx.setDetailClip({drums, a});
    f.ctx.setDetailMode(DetailMode::Clip);
    CHECK(f.ctx.liveEditedClip().clip == a);
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 22});
    CHECK(play(f, 'f') == KN{0, 3});  // 1Row over 22 degrees
    CHECK(play(f, 'q') == KN{1, 0});
    // Closing the clip view: the start marker's clip (B, 12-EDO).
    f.ctx.setDetailVisible(false);
    CHECK_FALSE(f.ctx.liveEditedClip().clip.valid());
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{true, 12});
    CHECK(play(f, 'f') == KN{0, 5});
    // The device view shows no clip: the same.
    f.ctx.setDetailMode(DetailMode::Device);
    CHECK(f.ctx.keyboardLayout().piano);
    // Another track's clip open: this track's live clip is its own (B under the marker).
    f.ctx.setDetailMode(DetailMode::Clip);
    f.ctx.setDetailClip({f.track("Bass").id, a});
    CHECK(f.ctx.keyboardLayout().piano);
    // B open and retuned to 22-EDO: the keyboard follows the clip's tuning change.
    f.ctx.setDetailClip({drums, b});
    REQUIRE(f.ctx.store().dispatch(doc::SetClipTuning{{drums, b}, doc::TuningAssignment::mainTuning(t22)}).ok());
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 22});
  }
  SUBCASE("(2) closed: the clip under the start marker, stopped; under the playhead, playing") {
    f.ctx.setStartMarker(101 * kBar);  // over A
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 22});
    CHECK(play(f, 'f') == KN{0, 3});
    f.ctx.setStartMarker(104 * kBar);  // B's first tick: B
    CHECK(f.ctx.keyboardLayout().piano);
    f.ctx.setStartMarker(104 * kBar - 1);  // A's last tick
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 22});
    // Playing: the playhead decides, not the start marker.
    f.port.play(106 * kBar);
    CHECK(f.ctx.keyboardLayout().piano);
    f.port.locate(102 * kBar);
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 22});
    f.port.stop();
    // A deactivated clip plays nothing: the track's tuning.
    REQUIRE(f.ctx.store().dispatch(doc::SetClipActive{{drums, a}, false}).ok());
    CHECK(f.ctx.keyboardLayout().piano);
  }
  SUBCASE("(3) no live clip: the track's own tuning") {
    REQUIRE(f.ctx.store().dispatch(doc::SetTrackTuning{drums, doc::TuningAssignment::mainTuning(t22)}).ok());
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 22});
    f.ctx.setStartMarker(105 * kBar);  // B inherits it now
    CHECK(f.ctx.keyboardLayout() == ComputerKeyboard::Layout{false, 22});
  }
}

TEST_CASE("KB.1 arm on select: the selected MIDI track is auto-armed while no track is armed by hand") {
  Fixture f;
  // A click on a header's empty part (real dispatch) selects its track.
  auto clickHeader = [&](std::string_view name) {
    const doc::TrackId id = f.track(name).id;
    for (size_t i = 0; i < 16; ++i) {
      auto* h = dynamic_cast<TrackHeader*>(f.d.find("main/body/top/track-area/arrangement/tracks/track[" + std::to_string(i) + "]"));
      if (!h || h->trackId() != id) continue;
      const IRect r = h->rect();
      for (int y = r.y + 2; y < r.bottom() - 2; y += 2)
        for (int x = r.x + 2; x < r.right() - 2; x += 4)
          if (f.d.tree().hitTest({static_cast<float>(x), static_cast<float>(y)}) == h) {
            f.d.click({static_cast<float>(x), static_cast<float>(y)});
            REQUIRE(f.ctx.selectedTrack() == id);
            return;
          }
    }
    FAIL("no clickable header for " << name);
  };
  // The fixture's Bass is armed by hand: selecting another track arms nothing.
  clickHeader("Drums");
  CHECK_FALSE(f.ctx.autoArmTrack().valid());
  CHECK(f.port.autoArm == 0);
  // Disarm it (the strip's arm button): Drums, selected, is auto-armed now.
  MixerView* mixer = findMixer(*f.shell);
  REQUIRE(mixer != nullptr);
  mixer->setShown(true);
  f.d.tree().layoutNow();
  f.d.click(f.centre(*mixer->strip(f.track("Bass").id)->arm()));
  CHECK_FALSE(f.track("Bass").mixer.arm);
  CHECK(f.ctx.autoArmTrack() == f.track("Drums").id);
  CHECK(f.port.autoArm == f.track("Drums").id.value);
  f.d.step();
  CHECK(mixer->strip(f.track("Drums").id)->arm()->on());  // lit while auto-armed
  CHECK_FALSE(f.track("Drums").mixer.arm);                // not document state
  CHECK_FALSE(mixer->strip(f.track("Bass").id)->arm()->on());
  CHECK(f.ctx.keyboardTrack() == f.track("Drums").id);  // M + keys play it without arming by hand
  mixer->setShown(false);
  f.d.tree().layoutNow();
  // Another MIDI track selected: it follows; a group: none.
  clickHeader("Lead");
  CHECK(f.ctx.autoArmTrack() == f.track("Lead").id);
  CHECK(f.port.autoArm == f.track("Lead").id.value);
  clickHeader("Keys");
  CHECK_FALSE(f.ctx.autoArmTrack().valid());
  CHECK(f.port.autoArm == 0);
  clickHeader("Lead");
  // Clicking the lit arm button arms the track by hand; then selection arms nothing else.
  mixer->setShown(true);
  f.d.tree().layoutNow();
  f.d.click(f.centre(*mixer->strip(f.track("Lead").id)->arm()));
  CHECK(f.track("Lead").mixer.arm);
  CHECK_FALSE(f.ctx.autoArmTrack().valid());
  mixer->setShown(false);
  f.d.tree().layoutNow();
  clickHeader("Drums");
  CHECK_FALSE(f.ctx.autoArmTrack().valid());
  CHECK(f.ctx.keyboardTrack() == f.track("Lead").id);  // the armed track plays (and lays out the keys)
}
