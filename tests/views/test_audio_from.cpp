// RT.3 (docs/plans/RT.md): recording from a track, Live's "Audio From", through real window / overlay dispatch — the
// mixer strip's and the arrangement header's Audio From + channel choosers, the circuit's Audio In terminal (a cable
// dragged onto it from a mixer's out or a block's output, its chip, its menu), and the isomorphism between them: a
// choice is a cable in the circuit, a cable reads back as "<Track> | <block>". Goldens: project-patch-audio-in,
// mixer-audio-in, tracks-audio-in.
#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "../support/blocks.h"
#include "../ui/golden_check.h"
#include "pq/document/audio_in.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/audio_from.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"
#include "../../src/views/src/mixer/internal.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

// The audio fixture (Drums, Vox, Lost: audio; Keys: MIDI), a tanh block on Drums' path.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  doc::TrackId drums, vox, lost, keys;
  doc::DeviceId tanh;

  Ui() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(ctx.openProject(views::test::fixtureDir("audio")).ok());
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (t.name == "Drums") drums = t.id;
      if (t.name == "Vox") vox = t.id;
      if (t.name == "Lost") lost = t.id;
      if (t.name == "Keys") keys = t.id;
    });
    REQUIRE(drums.valid());
    REQUIRE(vox.valid());
    tanh = test_blocks::appendAfter(ctx.store(), drums, "atom:tanh");
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  size_t history() const { return ctx.store().history().cursor(); }
  doc::AudioFrom from(doc::TrackId t) const { return doc::audioFromOf(*ctx.doc(), t); }
  size_t audioInWires() const {
    size_t n = 0;
    for (const doc::Wire& w : doc::wiresOf(*ctx.doc())) n += w.toRecordInput() ? 1u : 0u;
    return n;
  }
  static Vec2 centre(const Widget& w) {
    const IRect r = w.rect();
    return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
  }
  Menu* topMenu() const {
    for (size_t i = shell->overlay().childCount(); i-- > 0;)
      if (auto* m = dynamic_cast<Menu*>(&shell->overlay().child(i))) return m;
    return nullptr;
  }
  Menu& clickOpen(Control* c) {
    REQUIRE(c != nullptr);
    REQUIRE(c->visible());
    d.click(centre(*c));
    Menu* m = topMenu();
    REQUIRE(m != nullptr);
    return *m;
  }
  static int32_t itemOf(const Menu& m, std::string_view label) {
    for (size_t i = 0; i < m.items().size(); ++i)
      if (m.items()[i].label == label) return static_cast<int32_t>(i);
    return -1;
  }
  void choose(Menu& m, std::string_view label) {
    const int32_t i = itemOf(m, label);
    REQUIRE_MESSAGE(i >= 0, "no menu item " << std::string(label));
    m.choose(i);
    d.step();
  }
  // The mixer view (Ctrl+Alt+M) and a track's strip in it.
  MixerStrip& mixerStrip(doc::TrackId t) {
    MixerView* m = findMixer(*shell);
    REQUIRE(m != nullptr);
    if (!m->shown()) REQUIRE(d.press("Ctrl+Alt+M") == KeyResult::Action);
    d.tree().layoutNow();
    d.step();
    MixerStrip* s = m->strip(t);
    REQUIRE(s != nullptr);
    return *s;
  }
  // The arrangement header's strip of a track.
  MixerStrip& headerStrip(doc::TrackId t) {
    auto* a = dynamic_cast<ArrangementView*>(shell->slot(ShellSlot::TrackArea).find("arrangement"));
    REQUIRE(a != nullptr);
    d.tree().layoutNow();
    d.step();
    TrackHeader* h = a->header(t);
    REQUIRE(h != nullptr);
    REQUIRE(h->mixerSection().childCount() == 1);
    return static_cast<MixerStrip&>(h->mixerSection().child(0));
  }

  // The circuit, framed on Drums' mixer and Vox's Audio In at a readable zoom.
  ProjectPatchView& patch() {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    if (!v->shown()) {
      v->setShown(true, false);
      d.tree().layoutNow();
    }
    return *v;
  }
  void frame(ProjectPatchView& v, doc::TrackId top) {
    const ProjectPatch& pp = v.patch();
    const uint32_t b = pp.bandOf(top);
    REQUIRE(b != kNone);
    circuit::CircuitCanvas::View cv;
    cv.zoom = 0.85;
    cv.x = static_cast<double>(pp.laneX0) - 40.0;
    cv.y = static_cast<double>(pp.bands[b].y) - 8.0;
    v.canvas().setView(cv);
    d.tree().layoutNow();
    d.step();
  }
  Vec2 dev(ProjectPatchView& v, float x, float y) const { return v.canvas().toDevice({x, y}); }
  // Vox's Audio In terminal's chip (its middle) and a block's / mixer's out pin, in device px.
  Vec2 audioIn(ProjectPatchView& v, doc::TrackId t) const {
    const ProjectPatch& pp = v.patch();
    const uint32_t n = pp.bands[pp.bandOf(t)].audioIn;
    REQUIRE(n != kNone);
    const Node& a = pp.nodes[n];
    return dev(v, a.x + a.w * 0.5f, a.y + a.h * 0.5f);
  }
  Vec2 outPin(ProjectPatchView& v, uint32_t node, std::string_view name) const {
    const int32_t k = v.patch().nodes[node].output(name);
    REQUIRE(k >= 0);
    return v.canvas().toDevice(v.canvas().portWorld(node, static_cast<uint32_t>(k), false));
  }
  uint32_t mixerNode(ProjectPatchView& v, doc::TrackId t) const { return v.patch().bands[v.patch().bandOf(t)].mixer; }
  // The model edge drawn into `t`'s Audio In (kNone: none) and where it starts.
  uint32_t audioInEdge(ProjectPatchView& v, doc::TrackId t) const {
    const ProjectPatch& pp = v.patch();
    const uint32_t n = pp.bands[pp.bandOf(t)].audioIn;
    for (uint32_t k = 0; k < pp.edges.size(); ++k)
      if (pp.edges[k].to == n) return k;
    return kNone;
  }
};

std::string chooserText(Control* c) {
  REQUIRE(c != nullptr);
  return static_cast<mixer::Chooser*>(c)->text();
}

}  // namespace

TEST_CASE("RT.3 mixer strip: Audio From lists Ext. In / Resampling / No Input / the other tracks; picking a track and then "
          "a device writes the cable the circuit draws (each one undo step)") {
  Ui u;
  MixerStrip& s = u.mixerStrip(u.vox);
  REQUIRE(s.audioIn() != nullptr);
  REQUIRE(s.audioChannel() != nullptr);
  REQUIRE(s.monitor() != nullptr);
  CHECK(chooserText(s.audioIn()) == "No Input");
  CHECK_FALSE(s.audioChannel()->visible());  // No Input: no channel
  Menu& m = u.clickOpen(s.audioIn());
  REQUIRE(m.items().size() >= 5);
  CHECK(m.items()[0].label == "Ext. In");
  CHECK(m.items()[1].label == "Resampling");
  CHECK(m.items()[2].label == "No Input");
  CHECK(m.items()[2].checked);
  CHECK(m.items()[3].label.empty());  // the separator
  CHECK(Ui::itemOf(m, "Vox") < 0);    // never itself
  REQUIRE(Ui::itemOf(m, "Drums") > 3);
  CHECK(Ui::itemOf(m, "Lost") > 3);
  CHECK(m.items()[static_cast<size_t>(Ui::itemOf(m, "Drums"))].swatch != 0);  // its colour chip
  const size_t h0 = u.history();
  u.choose(m, "Drums");
  // The default: Drums' last block before its mixer (Live's Post FX).
  const doc::AudioFrom f = u.from(u.vox);
  REQUIRE(f.kind == doc::AudioFromKind::Output);
  CHECK(f.start.from == u.tanh);
  CHECK(u.history() == h0 + 1);
  CHECK(u.audioInWires() == 1);
  CHECK(chooserText(s.audioIn()) == "Drums");
  CHECK(s.audioChannel()->visible());
  // The channel lists Drums' audio outputs in signal order: its clips, its block, its mixer.
  Menu& c = u.clickOpen(s.audioChannel());
  REQUIRE(c.items().size() == 3);
  CHECK(c.items()[0].label == "Clips");
  CHECK(c.items()[1].checked);
  CHECK(c.items()[2].label == "Mixer");
  const std::string block = c.items()[1].label;
  CHECK(chooserText(s.audioChannel()) == block);
  (void)u.d.press("Escape");
  // The circuit draws the cable from that block's output into Vox's Audio In, under the same name.
  ProjectPatchView& v = u.patch();
  const uint32_t e = u.audioInEdge(v, u.vox);
  REQUIRE(e != kNone);
  CHECK(v.patch().edges[e].from == v.patch().nodeOf(u.tanh));
  CHECK(v.patch().nodes[v.patch().nodeOf(u.tanh)].title == block);
  CHECK(v.patch().nodes[v.patch().bands[v.patch().bandOf(u.vox)].audioIn].inputs[0].label.empty());  // a cable: no chip
  v.setShown(false, false);
  // Back in the mixer: Mixer (Live's Post Mixer) replaces the cable in one step.
  MixerStrip& s2 = u.mixerStrip(u.vox);
  u.choose(u.clickOpen(s2.audioChannel()), "Mixer");
  CHECK(u.from(u.vox).start.mixerOut == u.drums);
  CHECK(u.audioInWires() == 1);
  CHECK(u.history() == h0 + 2);
  CHECK(chooserText(s2.audioChannel()) == "Mixer");
  // Resampling hides the channel; Ext. In shows the device's channels.
  u.choose(u.clickOpen(s2.audioIn()), "Resampling");
  CHECK(u.from(u.vox).kind == doc::AudioFromKind::Resampling);
  u.d.tree().layoutNow();
  CHECK_FALSE(s2.audioChannel()->visible());
  u.port.inputs = 4;
  u.choose(u.clickOpen(s2.audioIn()), "Ext. In");
  CHECK(u.from(u.vox) == doc::AudioFrom::extIn(doc::AudioInput{0, 2}));
  CHECK(u.audioInWires() == 0);
  u.d.tree().layoutNow();
  CHECK(chooserText(s2.audioChannel()) == "1/2");
  u.choose(u.clickOpen(s2.audioChannel()), "3/4");
  CHECK(u.from(u.vox) == doc::AudioFrom::extIn(doc::AudioInput{2, 2}));
  u.port.inputs = 2;
  CHECK(Ui::itemOf(u.clickOpen(s2.audioChannel()), "3/4 (not available)") >= 0);
}

TEST_CASE("RT.4 arrangement header: volume and pan under the name, Audio From under them at the default height (real "
          "dispatch); a two-line row hides the input line; automation mode's chooser takes it") {
  Ui u;
  auto* a = dynamic_cast<ArrangementView*>(u.shell->slot(ShellSlot::TrackArea).find("arrangement"));
  REQUIRE(a != nullptr);
  namespace am = arrangement_metrics;
  REQUIRE(a->trackHeight(u.lost) == am::kTrackHeight);  // Lost has no height of its own: the default
  MixerStrip& s = u.headerStrip(u.lost);
  TrackHeader& h = *a->header(u.lost);
  const IRect title = h.titleRect();
  const int32_t line = static_cast<int32_t>(mixer_metrics::kLine);  // scale 1
  auto lineOf = [&](const Widget* w) { return (w->rect().y - h.rect().y) / line; };
  auto inNameColumn = [&](const Widget* w) { return w->rect().x >= title.x && w->rect().right() <= title.right(); };
  // Line 1: volume + pan side by side in the name column; line 2: Audio From + channel (Drums: shown below), Monitor
  // and the output in the side column, the toggles beside the title.
  for (Widget* w : {static_cast<Widget*>(s.volume()), static_cast<Widget*>(s.pan()), static_cast<Widget*>(s.audioIn()),
                    static_cast<Widget*>(s.monitor()), static_cast<Widget*>(s.output()), static_cast<Widget*>(s.activator())}) {
    REQUIRE(w != nullptr);
    CHECK(w->visible());
  }
  CHECK(lineOf(s.volume()) == 1);
  CHECK(lineOf(s.pan()) == 1);
  CHECK(s.pan()->rect().x > s.volume()->rect().right());
  CHECK(inNameColumn(s.volume()));
  CHECK(inNameColumn(s.pan()));
  CHECK(lineOf(s.audioIn()) == 2);
  CHECK(inNameColumn(s.audioIn()));
  CHECK(lineOf(s.activator()) == 0);
  CHECK(lineOf(s.output()) == 1);
  CHECK(lineOf(s.monitor()) == 2);
  CHECK(s.activator()->rect().x >= title.right());
  CHECK(s.output()->rect().x >= title.right());
  CHECK(s.monitor()->rect().x >= title.right());

  // A drag up on the volume box raises the volume: one undo step.
  const float vol0 = doc::findTrackPtr(*u.ctx.doc(), u.lost)->mixer.volumeDb;
  size_t h0 = u.history();
  const Vec2 vc = Ui::centre(*s.volume());
  u.d.drag(vc, {vc.x, vc.y - 30.0f}, 10);
  CHECK(doc::findTrackPtr(*u.ctx.doc(), u.lost)->mixer.volumeDb > vol0);
  CHECK(u.history() == h0 + 1);
  // The title over the strip still takes its clicks: it selects the track.
  u.d.click({static_cast<float>(title.x + 30), static_cast<float>(title.y + title.h / 2)});
  CHECK(a->selection().tracks == std::vector<doc::TrackId>{u.lost});
  // Audio From on line 2 opens its menu; picking Drums is one step and the channel appears beside it.
  h0 = u.history();
  u.choose(u.clickOpen(s.audioIn()), "Drums");
  CHECK(u.from(u.lost).kind == doc::AudioFromKind::Output);
  CHECK(u.history() == h0 + 1);
  u.d.tree().layoutNow();
  CHECK(chooserText(s.audioIn()) == "Drums");
  REQUIRE(s.audioChannel()->visible());
  CHECK(lineOf(s.audioChannel()) == 2);
  CHECK(s.audioChannel()->rect().x > s.audioIn()->rect().right());
  CHECK(inNameColumn(s.audioChannel()));

  // Automation mode: the parameter chooser takes the name column's line 2; the input choosers give way (Monitor stays).
  u.d.tree().setFocus(a);
  REQUIRE(u.d.press("A") == KeyResult::Action);
  u.d.tree().layoutNow();
  CHECK(h.paramChooser().visible());
  CHECK((h.paramChooser().rect().y - h.rect().y) / line == 2);
  CHECK_FALSE(s.audioIn()->visible());
  CHECK_FALSE(s.audioChannel()->visible());
  CHECK(s.monitor()->visible());
  REQUIRE(u.d.press("A") == KeyResult::Action);
  u.d.tree().layoutNow();
  CHECK(s.audioIn()->visible());
  CHECK(s.audioChannel()->visible());

  // Dragging the header's bottom edge up to two lines hides the input line (and Monitor); volume, pan and the output
  // stay.
  const Vec2 edge{static_cast<float>(title.x + 30), static_cast<float>(h.rect().bottom() - 2)};
  u.d.drag(edge, {edge.x, edge.y - (am::kTrackHeight - 2.0f * am::kHeaderLine)}, 6);
  u.d.tree().layoutNow();
  CHECK(a->trackHeight(u.lost) == doctest::Approx(2.0f * am::kHeaderLine));
  CHECK(s.volume()->visible());
  CHECK(s.pan()->visible());
  CHECK(s.output()->visible());
  CHECK_FALSE(s.audioIn()->visible());
  CHECK_FALSE(s.audioChannel()->visible());
  CHECK_FALSE(s.monitor()->visible());
}

TEST_CASE("RT.3 circuit: a cable dragged from a mixer's out or a block's output onto an Audio In is its source; the "
          "arrangement's choosers read it back as <Track> | <block>") {
  Ui u;
  ProjectPatchView& v = u.patch();
  u.frame(v, u.drums);
  const size_t h0 = u.history();
  // Drums' mixer out (Live's Post Mixer) onto Vox's Audio In.
  u.d.drag(u.outPin(v, u.mixerNode(v, u.drums), "out"), u.audioIn(v, u.vox), 8);
  u.d.step();
  REQUIRE(u.from(u.vox).kind == doc::AudioFromKind::Output);
  CHECK(u.from(u.vox).start.mixerOut == u.drums);
  CHECK(u.history() == h0 + 1);
  REQUIRE(u.audioInEdge(v, u.vox) != kNone);
  CHECK(v.patch().edges[u.audioInEdge(v, u.vox)].from == u.mixerNode(v, u.drums));
  CHECK(v.patch().edges[u.audioInEdge(v, u.vox)].cable);
  CHECK(audio_from::sourceLabel(u.ctx, u.from(u.vox)) == "Drums | Mixer");
  // RT.2: lowered — drawn as its route (Drums' `post` into Vox's m<wire>), not dim.
  REQUIRE(v.lowered() != nullptr);
  CHECK(v.patch().edges[u.audioInEdge(v, u.vox)].status == doc::WireStatus::Lowered);
  CHECK(v.patch().edges[u.audioInEdge(v, u.vox)].graphWire != kNone);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");
  // A block's output: the same Audio In, the old cable replaced (one source, one step).
  const uint32_t tn = v.patch().nodeOf(u.tanh);
  REQUIRE(tn != kNone);
  const std::string title = v.patch().nodes[tn].title;
  REQUIRE_FALSE(v.patch().nodes[tn].outputs.empty());
  u.d.drag(u.outPin(v, tn, v.patch().nodes[tn].outputs[0].name), u.audioIn(v, u.vox), 8);
  u.d.step();
  CHECK(u.from(u.vox).start.from == u.tanh);
  CHECK(u.audioInWires() == 1);
  CHECK(u.history() == h0 + 2);
  CHECK(audio_from::sourceLabel(u.ctx, u.from(u.vox)) == "Drums | " + title);
  CHECK(checkAgainstGraph(v.patch(), *u.ctx.doc(), *v.lowered()) == "");  // RT.2: the source's m<wire> route
  // RT.2: Resampling (the master's shadow; drawn from the master's mixer).
  REQUIRE(audio_from::set(u.ctx, u.vox, doc::AudioFrom::resampling()));
  u.d.tree().layoutNow();
  u.d.step();
  {
    ProjectPatchView& vr = u.patch();
    REQUIRE(vr.lowered() != nullptr);
    CHECK(vr.lowered()->ownTracks < vr.lowered()->graph.tracks.size());
    CHECK(checkAgainstGraph(vr.patch(), *u.ctx.doc(), *vr.lowered()) == "");
  }
  REQUIRE(audio_from::set(u.ctx, u.vox, doc::AudioFrom::output(doc::Wire{.from = u.tanh})));
  u.d.tree().layoutNow();
  u.d.step();
  // The arrangement header's choosers read it (the same model, two views).
  v.setShown(false, false);
  MixerStrip& h = u.headerStrip(u.vox);
  REQUIRE(h.audioIn() != nullptr);
  CHECK(chooserText(h.audioIn()) == "Drums");
  CHECK(chooserText(h.audioChannel()) == title);
  // Picking Mixer there moves the circuit's cable to Drums' mixer.
  u.choose(u.clickOpen(h.audioChannel()), "Mixer");
  ProjectPatchView& v2 = u.patch();
  REQUIRE(u.audioInEdge(v2, u.vox) != kNone);
  CHECK(v2.patch().edges[u.audioInEdge(v2, u.vox)].from == u.mixerNode(v2, u.drums));
}

TEST_CASE("RT.3 circuit: the Audio In's chip (Ext. In / No Input), its menu (the choosers' entries), a refused loop "
          "drag says why, a deleted cable is No Input") {
  Ui u;
  u.port.inputs = 2;
  ProjectPatchView& v = u.patch();
  u.frame(v, u.drums);
  const uint32_t ai = v.patch().bands[v.patch().bandOf(u.drums)].audioIn;
  REQUIRE(ai != kNone);
  CHECK(v.patch().nodes[ai].title == "Audio In");
  CHECK(v.patch().nodes[ai].inputs[0].label == "No Input");
  // A right-click on the terminal opens the shared menu: Ext. In opens its channels.
  u.d.click(u.audioIn(v, u.drums), mod::kNone, PointerButton::Right);
  Menu* m = u.topMenu();
  REQUIRE(m != nullptr);
  CHECK(m->items()[0].label == "Ext. In");
  CHECK_FALSE(m->items()[0].submenu.empty());
  CHECK(Ui::itemOf(*m, "No Input") >= 0);
  const int32_t voxItem = Ui::itemOf(*m, "Vox");
  REQUIRE(voxItem > 0);
  CHECK(Ui::itemOf(*m, "Drums") < 0);  // never itself
  // Vox's outputs as its submenu (its clips, its mixer).
  CHECK(m->items()[static_cast<size_t>(voxItem)].submenu.size() == 2);
  m->choose(0);
  Menu* sub = m->submenu();
  REQUIRE(sub != nullptr);
  u.choose(*sub, "1/2");
  CHECK(u.from(u.drums) == doc::AudioFrom::extIn(doc::AudioInput{0, 2}));
  u.d.tree().layoutNow();
  ProjectPatchView& v2 = u.patch();
  const uint32_t ai2 = v2.patch().bands[v2.patch().bandOf(u.drums)].audioIn;
  CHECK(v2.patch().nodes[ai2].inputs[0].label == "Ext. In 1/2");
  // Vox records Drums; a cable from Vox's mixer back onto Drums' Audio In would be a loop: refused, the status says so.
  doc::Wire mx;
  mx.mixerOut = u.drums;
  REQUIRE(audio_from::set(u.ctx, u.vox, doc::AudioFrom::output(mx)));
  u.d.tree().layoutNow();
  u.d.step();
  ProjectPatchView& v3 = u.patch();
  u.frame(v3, u.drums);
  const size_t h0 = u.history();
  u.d.drag(u.outPin(v3, u.mixerNode(v3, u.vox), "out"), u.audioIn(v3, u.drums), 8);
  u.d.step();
  CHECK(u.history() == h0);
  CHECK(u.ctx.status().find(std::string(doc::kAudioInLoop)) != std::string::npos);
  CHECK(u.from(u.drums) == doc::AudioFrom::extIn(doc::AudioInput{0, 2}));
  // Deleting the cable is No Input (the chip reads it).
  const doc::Wire* w = doc::audioInWireOf(*u.ctx.doc(), u.vox);
  REQUIRE(w != nullptr);
  const std::vector<doc::WireId> one{w->id};
  const auto del = doc::ops::deleteWires(*u.ctx.doc(), one);
  REQUIRE(del.ok());
  REQUIRE(u.ctx.store().dispatch(del->command).ok());
  CHECK(u.from(u.vox).kind == doc::AudioFromKind::None);
  u.d.tree().layoutNow();
  ProjectPatchView& v4 = u.patch();
  CHECK(v4.patch().nodes[v4.patch().bands[v4.patch().bandOf(u.vox)].audioIn].inputs[0].label == "No Input");
}

namespace {
void golden(const char* name) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(name, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(name, views::test::fixtureDir("audio"), w, h, {}, o, &library());
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(name) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  CHECK(pq::ui::test::matchesGolden(name, *img, *ref, true));
}
}  // namespace

TEST_CASE("render: project-patch-audio-in matches the golden image") { golden("project-patch-audio-in"); }
TEST_CASE("render: mixer-audio-in matches the golden image") { golden("mixer-audio-in"); }
TEST_CASE("render: tracks-audio-in matches the golden image") { golden("tracks-audio-in"); }
