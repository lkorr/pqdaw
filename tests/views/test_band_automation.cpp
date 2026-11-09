// TL.6: automated controls in the project patch (band) view show the envelope at the playhead and move with it, as in
// the Device view (T4.18; one mechanism: pq/views/arrangement/automation_follow.h): the mixer face's volume, a meta
// face's control (an opened module), a closed group's panel control. A hand edit overrides; Re-enable brings the
// envelope back; the timer runs only while playing.

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "pq/document/meta.h"
#include "pq/dsp/curve.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "../../src/views/src/project_patch/internal.h"
#include "views_test_util.h"

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

// The shell over the sketch project (Keys: Operator, Saturator), the patch view shown.
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1280, 800};
  std::unique_ptr<Shell> shell;
  Ticks bar = 0;

  Ui() {
    ctx.setDeviceLibrary(&library());
    REQUIRE(buildSketchProject(ctx.store()).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    bar = ticksPerBar(ctx.doc()->timeline->sigMap.signatureAt(0));
    ctx.setDetailVisible(false);
    view().setShown(true, false);
    d.tree().layoutNow();
  }
  ~Ui() { d.tree().setRoot(std::make_unique<Widget>()); }

  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const doc::Track& track(std::string_view name) const {
    const doc::Track* found = nullptr;
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!found && t.name == name) found = &t;
    });
    REQUIRE_MESSAGE(found != nullptr, "no track " << std::string(name));
    return *found;
  }
  // A lane on `home` for `p` from `v0` at bar 1 to `v1` at bar 5 (one step).
  void addLane(doc::TrackId home, const doc::ParamRef& p, std::string name, float v0, float v1) {
    auto l = std::make_shared<doc::AutomationLane>();
    l->id = ctx.store().ids().lane();
    l->param = p;
    l->paramName = std::move(name);
    l->points.push_back({0, v0, 0.0f, ctx.store().ids().breakpoint()});
    l->points.push_back({4 * bar, v1, 0.0f, ctx.store().ids().breakpoint()});
    REQUIRE(ctx.store().dispatch(doc::AddAutomationLane{home, l}).ok());
    d.step();
  }
  void at(Ticks t) {
    port.setPlayheadTick(t);
    view().applyAutomation();
  }
  // The mixer face of `track` on the canvas.
  Face& mixer(doc::TrackId track) const {
    ProjectCanvas& c = view().canvas();
    for (size_t i = 0; i < c.panelCount(); ++i)
      if (mixerFaceTrack(c.panelAt(i)) == track) return c.panelAt(i);
    FAIL("no mixer face");
    return c.panelAt(0);
  }
  // The text its volume fader shows (its info: "Volume -12.0 dB: …"), read at the fader's cap.
  std::string volumeText(doc::TrackId track) const {
    Face& f = mixer(track);
    const doc::ParamRef vol = doc::ParamRef::track(doc::ParamKind::TrackVolume, track);
    const std::optional<ControlAnchor> a = mixerControlAnchor(f, vol);
    REQUIRE(a.has_value());
    std::string title, body;
    f.describe(f.hitTest(a->at, 1.0f), title, body);
    return body;
  }
};

}  // namespace

TEST_CASE("band automation (TL.6): the mixer face's automated volume follows the playhead; a hand edit overrides; Re-enable") {
  Ui u;
  ProjectPatchView& v = u.view();
  const doc::TrackId keys = u.track("Keys").id;
  CHECK(v.automatedShown() == 0);
  CHECK(u.volumeText(keys).find("Volume " + formatVolume(0.0) + " dB") == 0);
  u.addLane(keys, doc::ParamRef::track(doc::ParamKind::TrackVolume, keys), "", -24.0f, 0.0f);
  CHECK(v.automatedShown() >= 1);
  u.at(0);
  CHECK(u.volumeText(keys).find("Volume " + formatVolume(-24.0) + " dB") == 0);
  u.at(2 * u.bar);  // halfway (the lane is linear in dB)
  CHECK(u.volumeText(keys).find("Volume " + formatVolume(-12.0) + " dB") == 0);
  CHECK(doc::findTrackPtr(*u.ctx.doc(), keys)->mixer.volumeDb == doctest::Approx(0.0));  // the document is untouched
  // The timer runs while playing only.
  CHECK_FALSE(v.automationPolling());
  u.ctx.togglePlay();
  CHECK(v.automationPolling());
  u.ctx.togglePlay();
  u.at(2 * u.bar);
  CHECK_FALSE(v.automationPolling());
  // A hand edit overrides it: the fader shows the track's own value; Re-enable brings the envelope back.
  REQUIRE(u.ctx.store().dispatch(doc::SetTrackVolume{keys, -6.0f}).ok());
  u.d.step();
  CHECK(u.ctx.automationOverridden(doc::ParamRef::track(doc::ParamKind::TrackVolume, keys)));
  CHECK(u.volumeText(keys).find("Volume " + formatVolume(-6.0) + " dB") == 0);
  u.ctx.reenableAutomation();
  u.d.step();
  CHECK(u.volumeText(keys).find("Volume " + formatVolume(-12.0) + " dB") == 0);
  // The lane removed: its own value again.
  const doc::LaneId lane = doc::findTrackPtr(*u.ctx.doc(), keys)->lanes.back()->id;
  REQUIRE(u.ctx.store().dispatch(doc::RemoveAutomationLane{keys, lane}).ok());
  u.d.step();
  CHECK(u.volumeText(keys).find("Volume " + formatVolume(-6.0) + " dB") == 0);
  CHECK(v.automatedShown() == 0);
}

TEST_CASE("band automation (TL.6): a meta face's and a closed group panel's automated controls show the envelope at the playhead") {
  Ui u;
  ProjectPatchView& v = u.view();
  const doc::Track& keysTrack = u.track("Keys");
  const doc::TrackId keys = keysTrack.id;

  // A closed group's panel (Keys' modules are drawn as their panels): a control of it, automated.
  {
    ProjectCanvas& c = v.canvas();
    panel::GroupPanelFace* face = nullptr;
    std::optional<doc::Binding> ctl;
    for (size_t i = 0; i < c.panelCount() && !ctl; ++i) {
      auto* g = dynamic_cast<panel::GroupPanelFace*>(&c.panelAt(i));
      if (g == nullptr) continue;
      g->visitControls([&](const doc::Binding& b, const Rect&) {
        if (ctl || b.kind != doc::BindingKind::Control || !b.meta.empty()) return;
        const doc::DeviceLoc dl = doc::findDevice(*u.ctx.doc(), b.device);
        if (dl.track != nullptr && dl.track->id == keys && paramValid(*u.ctx.doc(), doc::ParamRef::device(b.device, b.name))) {
          ctl = b;
          face = g;
        }
      });
    }
    REQUIRE_MESSAGE(ctl.has_value(), "no panel control of Keys on the canvas");
    MESSAGE("panel control: " << ctl->name);
    u.addLane(keys, doc::ParamRef::device(ctl->device, ctl->name), ctl->name, 0.2f, 0.6f);
    // The face may be a new object after the rebuild: found again by its control.
    face = nullptr;
    for (size_t i = 0; i < c.panelCount() && face == nullptr; ++i)
      if (auto* g = dynamic_cast<panel::GroupPanelFace*>(&c.panelAt(i)))
        g->visitControls([&](const doc::Binding& b, const Rect&) {
          if (b == *ctl) face = g;
        });
    REQUIRE(face != nullptr);
    u.at(0);
    CHECK(face->target().position(*ctl) == doctest::Approx(0.2).epsilon(1e-5));
    u.at(u.bar);  // a quarter of the way
    CHECK(face->target().position(*ctl) == doctest::Approx(0.3).epsilon(1e-5));
    u.at(8 * u.bar);  // after the last point: its value
    CHECK(face->target().position(*ctl) == doctest::Approx(0.6).epsilon(1e-5));
  }

  // A meta face (the Operator's first meta block, its groups opened down to it): its control reads the envelope through
  // the param's curve.
  {
    doc::DeviceId meta;
    for (const doc::DevicePtr& d : u.track("Keys").devices)
      if (!meta.valid() && doc::isMetaDevice(*d)) meta = d->id;
    REQUIRE(meta.valid());
    for (doc::DeviceId g = doc::findDevice(*u.ctx.doc(), meta).device->group; g.valid(); g = doc::findDevice(*u.ctx.doc(), g).device->group) {
      v.setGroupOpen(g, true);
      u.d.step();
    }
    ProjectCanvas& c = v.canvas();
    MetaFace* face = nullptr;
    const DocTarget* dt = nullptr;
    for (size_t i = 0; i < c.faceCount() && face == nullptr; ++i) {
      const auto* t = dynamic_cast<const DocTarget*>(&c.faceAt(i).target());
      if (t != nullptr && t->device() == meta && t->path().empty() && !c.faceAt(i).type().params.empty()) {
        face = &c.faceAt(i);
        dt = t;
      }
    }
    REQUIRE_MESSAGE(face != nullptr, "no face of Keys' first meta block on the canvas (its groups opened)");
    const doc::DeviceId dev = dt->device();
    const meta::Type& type = face->type();
    const std::string name(type.params[0].name);
    MESSAGE("meta face: " << type.name << " " << name);
    const size_t before = v.automatedShown();
    u.addLane(keys, doc::ParamRef::device(dev, name), name, 0.0f, 1.0f);
    CHECK(v.automatedShown() == before + 1);
    MetaFace* now = nullptr;  // (after the rebuild)
    for (size_t i = 0; i < c.faceCount() && now == nullptr; ++i)
      if (const auto* t = dynamic_cast<const DocTarget*>(&c.faceAt(i).target()); t != nullptr && t->device() == dev && t->path().empty())
        now = &c.faceAt(i);
    REQUIRE(now != nullptr);
    const doc::Device* d = doc::findDevice(*u.ctx.doc(), dev).device;
    REQUIRE(d != nullptr);
    const dsp::Curve curve = doc::metaParamCurve(*d, "", type, 0);
    u.at(u.bar);  // a quarter: position 0.25 through the param's curve
    CHECK(now->target().value(0) == doctest::Approx(dsp::evalCurve(curve, 0.25)).epsilon(1e-6));
    u.at(3 * u.bar);
    CHECK(now->target().value(0) == doctest::Approx(dsp::evalCurve(curve, 0.75)).epsilon(1e-6));
    // Overridden by hand: its own (stored) value.
    REQUIRE(u.ctx.store().dispatch(doc::SetDeviceParam{dev, name, 0.5}).ok());
    u.d.step();
    CHECK(now->target().value(0) == doctest::Approx(doc::metaParamValue(*doc::findDevice(*u.ctx.doc(), dev).device, "", type, 0)));
    u.ctx.reenableAutomation();
    u.d.step();
    CHECK(now->target().value(0) == doctest::Approx(dsp::evalCurve(curve, 0.75)).epsilon(1e-6));
  }
}
