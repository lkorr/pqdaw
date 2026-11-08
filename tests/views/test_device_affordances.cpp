// T4.16c Device view affordances through the real shell: Live's arrow keys (Left / Right walk the chain into and out of
// rack chains, Shift extends the selection — then Ctrl+G groups it —, Home / End, Ctrl+Left / Right move a device one
// step = one undo step, Up / Down step through a rack's chains), the hint a multi-selection shows, and the mouse cursor
// over every interactive part of a device and a rack frame (each checked against what a press there does). Renaming a
// chain in place.

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/panel/block_frame.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

const doc::DeviceLibrary& library() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

Vec2 mid(const IRect& r) { return {static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}; }

// One MIDI track: its instrument group (Basic Synth) → Utility → Saturator (modules: groups), in the Device view.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2600, 900};  // (GU.11: the instrument group's frame is wide)
  std::unique_ptr<Shell> shell;
  doc::TrackId keys;
  doc::DeviceId inst, util, sat;

  Ui() {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    keys = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    inst = doc::mainPath(*st.current(), track()).blocks.at(0)->id;
    util = test_blocks::addModule(st, keys, "utility");
    sat = test_blocks::addModule(st, keys, "saturator");
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(keys);
    ctx.setDetailVisible(true);
    ctx.setDetailMode(DetailMode::Device);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  doc::ops::Edit run(const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(ctx.store().dispatch(e->command).ok());
    return *e;
  }
  DeviceChainView& view() const {
    DeviceChainView* v = findDeviceChain(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const doc::Track& track() const { return *doc::findTrackPtr(*ctx.doc(), keys); }
  // TX.all-free: the track's main path (what the Device view lists), in signal order.
  std::vector<doc::DeviceId> chain() const {
    std::vector<doc::DeviceId> v;
    for (const doc::Device* dev : doc::mainPath(*ctx.doc(), track()).blocks) v.push_back(dev->id);
    return v;
  }
  size_t history() const { return ctx.store().history().cursor(); }
  IRect frame(doc::DeviceId id) {
    d.tree().layoutNow();
    Widget* f = view().frameOf(id);
    REQUIRE(f != nullptr);
    return f->rect();
  }
  Widget* in(doc::DeviceId id, std::string_view path) {
    d.tree().layoutNow();
    Widget* f = view().frameOf(id);
    REQUIRE(f != nullptr);
    return f->find(path);
  }
  Vec2 title(doc::DeviceId id) {
    const IRect r = frame(id);
    if (const panel::BlockFrame* p = view().panelOf(id)) {  // GU.13: below its jack strip
      const IRect t = p->titleRect();
      return {static_cast<float>(t.x + t.w / 2), static_cast<float>(t.y + t.h / 2)};
    }
    return {static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + 10)};
  }
  std::vector<doc::DeviceId> selection() const { return {view().selection().begin(), view().selection().end()}; }
  // The cursor with the pointer at p (a move there first).
  Cursor at(Vec2 p) {
    d.move(p);
    return d.tree().cursor();
  }
};

using Ids = std::vector<doc::DeviceId>;

}  // namespace

TEST_CASE("device keys: the selection scrolls into view") {
  Ui u;
  DeviceChainView& v = u.view();
  for (int k = 0; k < 6; ++k) u.run(doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.keys, "module:saturator", doc::DeviceKind::AudioEffect));
  u.d.tree().layoutNow();
  const doc::DeviceId last = u.chain().back();
  const IRect vp = v.chainViewport();
  REQUIRE(u.frame(last).x > vp.right());  // scrolled away
  u.d.click(u.title(u.inst));
  u.d.press("End");
  REQUIRE(u.selection() == Ids{last});
  const IRect f = u.frame(last);
  CHECK(f.x >= vp.x);
  CHECK(f.right() <= vp.right());
  u.d.press("Home");
  const IRect fi = u.frame(u.inst);
  CHECK(fi.x >= vp.x);
}

TEST_CASE("device panel knobs: the wheel steps them (a run = one undo step), a double-click returns the default (T4.16-fix1)") {
  // GU.13: a rack's macro knob (the Saturator grouped into an Audio Effect Rack, macro 1 mapped onto its Drive) — a knob
  // element of the one panel system, as a module's (test_panel_gestures.cpp: the same on a leaf's and a group's).
  Ui u;
  const doc::DeviceId rack =
      u.run(doc::ops::groupDevices(*u.ctx.doc(), u.ctx.store().ids(), std::vector<doc::DeviceId>{u.sat})).devices.at(0);
  u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), rack, 0, test_blocks::member(*u.ctx.doc(), u.sat, "Drive"),
                           std::string(doc::kValueParam)));
  u.d.tree().layoutNow();
  panel::BlockFrame* p = u.view().panelOf(rack);
  REQUIRE(p != nullptr);
  const doc::Binding mb = doc::controlBinding(rack, "macro1");
  const std::optional<IRect> knob = u.view().controlRect(rack, "macro1");
  REQUIRE(knob.has_value());
  const double v0 = p->target().position(mb);
  const Vec2 c = mid(*knob);
  const size_t h0 = u.history();
  u.d.move(c);
  u.d.wheel(c, -1.0f);
  u.d.wheel(c, -1.0f);
  CHECK(p->target().position(mb) < v0);
  u.d.advance(panel::PanelHost::kWheelRunSeconds + 0.1);
  CHECK(u.history() == h0 + 1);
  u.d.doubleClick(c);
  CHECK(p->target().position(mb) == doctest::Approx(0.0));  // a macro's default: 0 (mapping it took the Drive's position)
  CHECK(u.history() == h0 + 2);
}
