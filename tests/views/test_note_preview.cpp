// AP.7 (docs/plans/AP.md "Roll preview + Flatten"; pq/views/note_preview.h): the piano roll's preview of the track's
// note devices and Flatten to Clip, through the real shell, pointer and key dispatch:
//   - with an Arpeggiator on the clip's track the clip box shows its Arp row; the roll asks for the preview when it
//     paints, the worker computes it OFF the UI thread (debounced; the source records the thread it runs on) and the
//     ghost notes appear; an edit asks again (one compute per pause, not one per edit);
//   - the Ghosts switch (one view setting, also the arpeggiator panel's ROLL chip) hides and shows them;
//   - Ctrl+Alt+F flattens: the generated notes become the clip's (one undo step), the device is switched off, Ctrl+Z
//     restores both.
// The views cannot run the engine (the app installs renderTrackNoteStages: its equality with playback is
// tests/app/test_note_stage_render.cpp); here the source is a test function that "arpeggiates" each source note into
// two notes (itself and an octave up, a sixteenth each) — enough to see what the roll does with what it gets.
// Golden: pianoroll-arp-preview (view_scenes.cpp).

#include <doctest/doctest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/note_devices.h"
#include "pq/document/note_flatten.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/test_driver.h"
#include "pq/views/fake_transport.h"
#include "pq/views/note_preview.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/piano_roll/roll_state.h"
#include "../ui/golden_check.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using pq::views::roll::RollState;

namespace {

constexpr Ticks kQ = kPpq;

struct Rig {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  ClipView* view = nullptr;
  doc::ClipRef ref{};
  doc::DeviceId arp;
  std::mutex m;
  std::vector<std::thread::id> threads;  // where the source ran
  std::atomic<int> calls{0};

  Rig() {
    REQUIRE(ctx.openProject(views::test::fixtureDir("empty")).ok());
    ctx.notePreviews().setSource([this](const doc::Project& p, doc::TrackId t, Ticks) -> Result<NotePreview> {
      {
        std::lock_guard lock(m);
        threads.push_back(std::this_thread::get_id());
      }
      ++calls;
      return arpeggiate(p, t);
    });
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(480.0f);
    doc::DocumentStore& st = ctx.store();
    auto t = doc::ops::createTrack(*ctx.doc(), st.ids(), doc::TrackKind::Midi, {}, "Arp", doc::Colour{0x5FA8D8});
    REQUIRE(t.ok());
    REQUIRE(st.dispatch(t->command).ok());
    auto c = doc::ops::insertEmptyClip(*ctx.doc(), st.ids(), t->tracks.at(0), 0, kTicksPerWhole, "Chord");
    REQUIRE(c.ok());
    REQUIRE(st.dispatch(c->command).ok());
    ref = c->clips.at(0);
    std::vector<doc::Note> chord;
    for (const int32_t deg : {0, 4, 7}) {
      doc::Note n;
      n.start = 0;
      n.length = kQ;
      n.pitch = tuning::PitchId::fromDegree(deg, 0);
      chord.push_back(n);
    }
    auto a = doc::ops::addNotes(*ctx.doc(), st.ids(), ref, chord);
    REQUIRE(a.ok());
    REQUIRE(st.dispatch(a->command).ok());
    auto dv = doc::ops::addDevice(*ctx.doc(), st.ids(), ref.track, doc::noteDevicePatch("arpeggiator"), doc::DeviceKind::AudioEffect);
    REQUIRE(dv.ok());
    REQUIRE(st.dispatch(dv->command).ok());
    arp = dv->devices.at(0);
    ctx.setDetailClip(ref);
    d.tree().layoutNow();
    view = dynamic_cast<ClipView*>(d.find("main/body/bottom/detail/clip/clip-view"));
    REQUIRE(view != nullptr);
    view->setRowHeight(14);
    view->centreOnDegree(6);
    view->setZoom(60.0 / static_cast<double>(kQ), 0.0);
    view->scrollToTick(-0.5 * static_cast<double>(kQ));
    d.tree().layoutNow();
  }
  ~Rig() { d.tree().setRoot(std::make_unique<Widget>()); }

  // The test's "note device": every playing source note → itself, then an octave up, a sixteenth each.
  static NotePreview arpeggiate(const doc::Project& p, doc::TrackId t) {
    NotePreview out;
    const doc::Track* tr = doc::findTrackPtr(p, t);
    if (tr == nullptr) return out;
    bool on = false;
    for (const doc::DevicePtr& d : tr->devices) on = on || (d->active && doc::isNoteDevice(*d));
    if (!on) return out;
    out.any = true;
    for (const doc::ClipSlot& s : doc::playedClips(*tr)) {
      if (!s.clip) continue;
      doc::ClipGenerated cg{s.clip->id, {}};
      Ticks at = 0;
      for (const doc::Note& n : s.clip->notes) {
        if (!n.enabled() || n.pitch.kind() != tuning::PitchKind::Degree) continue;
        for (int32_t up = 0; up < 2; ++up) {
          doc::GeneratedNote g;
          g.start = at;
          g.length = kQ / 4;
          g.pitch = tuning::PitchId::fromDegree(n.pitch.degree(), n.pitch.equave() + up);
          g.source = n.id;
          g.velocity = 90.0f;
          if (up == 1) g.curve = {{0, -300.0f, 0.0f, {}}, {kQ / 16, 0.0f, 0.0f, {}}};  // a glide in
          cg.notes.push_back(g);
          at += kQ / 4;
        }
      }
      out.clips.push_back(std::move(cg));
    }
    return out;
  }

  RollState& st() { return view->state(); }
  const doc::NoteClip& clip() const {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    REQUIRE(t != nullptr);
    return *t->clips[static_cast<size_t>(doc::findClipIndex(*t, ref.clip))].clip;
  }
  size_t notes() const {
    size_t k = 0;
    for (const doc::Note& n : clip().notes) k += n.enabled() ? 1u : 0u;
    return k;
  }
  // A frame (the roll paints: it asks), the worker done, its result taken on this (the UI) thread, a frame.
  void settle() {
    d.step();
    REQUIRE(ctx.notePreviews().waitIdle());
    (void)ctx.notePreviews().pump();
    d.tree().layoutNow();
    d.step();
  }
  bool arpDeviceActive() const { return doc::findDevice(*ctx.doc(), arp).device->active; }
};

}  // namespace

TEST_CASE("roll preview (AP.7): the arpeggiator's ghosts — computed off the UI thread, debounced, switched by Ghosts") {
  Rig r;
  REQUIRE(r.st().hasNoteDevices());
  CHECK(r.d.find("main/body/bottom/detail/clip/clip-view/clip-box/arp-row")->visible());
  CHECK(r.ctx.notePreview());  // on by default
  r.settle();
  const doc::ClipGenerated* pv = r.st().preview();
  REQUIRE(pv != nullptr);
  CHECK(pv->notes.size() == 6);
  CHECK(r.calls.load() >= 1);
  {
    std::lock_guard lock(r.m);
    for (const std::thread::id id : r.threads) {
      CHECK(id != std::this_thread::get_id());  // never on the UI thread
      CHECK(id == r.ctx.notePreviews().workerThread());
    }
  }
  // Edits in a row (one undo step each) ask each time; the worker computes once they pause.
  const int before = r.calls.load();
  for (int i = 0; i < 5; ++i) {
    doc::Note n;
    n.start = 2 * kQ;
    n.length = kQ;
    n.pitch = tuning::PitchId::fromDegree(i, 1);
    auto a = doc::ops::addNotes(*r.ctx.doc(), r.ctx.store().ids(), r.ref, std::vector<doc::Note>{n});
    REQUIRE(a.ok());
    REQUIRE(r.ctx.store().dispatch(a->command).ok());
    r.d.step();  // the roll repaints and asks
  }
  r.settle();
  CHECK(r.calls.load() - before <= 2);  // debounced (not one per edit)
  REQUIRE(r.st().preview() != nullptr);
  CHECK(r.st().preview()->notes.size() == 16);  // 8 source notes, two each
  // The Ghosts switch (real dispatch): off hides them, on shows them.
  const Vec2 sw = r.d.center("main/body/bottom/detail/clip/clip-view/clip-box/arp-row/arp-preview");
  r.d.click(sw);
  r.d.step();
  CHECK_FALSE(r.ctx.notePreview());
  CHECK(r.st().preview() == nullptr);
  r.d.click(sw);
  r.settle();
  CHECK(r.ctx.notePreview());
  CHECK(r.st().preview() != nullptr);
}

TEST_CASE("roll preview (AP.7): Ctrl+Alt+F flattens into the clip (one undo step), the device off; Ctrl+Z restores") {
  Rig r;
  r.settle();
  const IRect a = r.d.rect("main/body/bottom/detail/clip/clip-view/notes");
  REQUIRE_FALSE(a.empty());
  r.d.click({static_cast<float>(a.right()) - 6.0f, static_cast<float>(a.y) + 6.0f});  // the roll has the keys
  r.d.step();
  REQUIRE(r.notes() == 3);
  CHECK(r.d.press("Ctrl+Alt+F") == KeyResult::Action);
  r.d.step();
  CHECK(r.notes() == 6);  // the generated notes, the sources gone (nothing anchors to them)
  CHECK_FALSE(r.arpDeviceActive());
  CHECK_FALSE(r.st().hasNoteDevices());
  CHECK_FALSE(r.d.find("main/body/bottom/detail/clip/clip-view/clip-box/arp-row")->visible());
  // The glides became the notes' curves.
  size_t curved = 0;
  for (const doc::Note& n : r.clip().notes) curved += doc::findNoteCurves(r.clip(), n.id) != nullptr ? 1u : 0u;
  CHECK(curved == 3);
  CHECK(r.d.press("Ctrl+Z") == KeyResult::Action);
  r.d.step();
  CHECK(r.notes() == 3);
  CHECK(r.arpDeviceActive());
}

TEST_CASE("roll preview (AP.7): golden pianoroll-arp-preview") {
  const char* scene = "pianoroll-arp-preview";
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir("empty"), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff diff;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &diff));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(diff));
}
