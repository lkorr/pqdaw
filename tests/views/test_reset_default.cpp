// TX.reset-default: a reset (Delete, double-click) puts a value block's knob at its DEFAULT knob position
// (doc::Device::defaultPosition: the module's default when its builder made it), not at knob position 0 — the Phase
// Accumulator's Amplitude ×1 and Pitch 0 c, an EQ band's Gain 0 dB, a value block the user placed at its declaration's
// 0, a rack macro where it was made; one undo step each, through real window dispatch in the device view. And two detents
// (the curves' points split where none was: the function unchanged): the VCA's ×0, ×2 and ×−2, the EQ band's Q 0.7071.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/document/racks.h"
#include "pq/ui/controls.h"
#include "pq/ui/test_driver.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/block_frame.h"
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

// One track in the device view holding the module under test (its frame's panel shows the controls).
struct Ui {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{2600, 900};
  std::unique_ptr<Shell> shell;
  doc::TrackId track;
  doc::DeviceId group;

  explicit Ui(std::string_view module) {
    ctx.setDeviceLibrary(&library());
    doc::DocumentStore& st = ctx.store();
    track = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys")).tracks[0];
    if (!module.empty()) group = test_blocks::addModule(st, track, module);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    dynamic_cast<Splitter&>(*d.find("main/body")).setPosition(300.0f);
    ctx.setSelectedTrack(track);
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
  size_t history() const { return ctx.store().history().cursor(); }
  doc::DeviceId member(std::string_view name) const { return test_blocks::member(*ctx.doc(), group, name); }
  const doc::Device& dev(doc::DeviceId id) const { return *doc::findDevice(*ctx.doc(), id).device; }
  Vec2 at(doc::DeviceId block, std::string_view param = doc::kValueParam) {
    d.tree().layoutNow();
    const std::optional<IRect> r = view().controlRect(block, param);
    REQUIRE(r.has_value());
    return mid(*r);
  }
  // A point of a 120-unit element (the PA's arcs, the VCA's dial: their panel's full size) at local units `l`.
  Vec2 local(doc::DeviceId block, Vec2 l) {
    d.tree().layoutNow();
    const std::optional<IRect> r = view().controlRect(block, doc::kValueParam);
    REQUIRE(r.has_value());
    return {static_cast<float>(r->x) + l.x * static_cast<float>(r->w) / 120.0f, static_cast<float>(r->y) + l.y * static_cast<float>(r->h) / 120.0f};
  }
  // An arc's handle at knob position t (rich_elements.cpp Arc: round (60, 64) at radius 39; the angle start + sweep·t).
  Vec2 arcAt(doc::DeviceId block, double start, double sweep, double t) {
    const double a = (start + sweep * t) * std::numbers::pi / 180.0;
    return local(block, {60.0f + 39.0f * static_cast<float>(std::cos(a)), 64.0f + 39.0f * static_cast<float>(std::sin(a))});
  }
  // The knob position its panel shows (through the panel's own target).
  double pos(doc::DeviceId block, std::string_view param = doc::kValueParam) {
    panel::PanelHost* h = view().controlHost(block, param);
    REQUIRE(h != nullptr);
    return h->panelFace()->target().position(doc::controlBinding(block, std::string(param)));
  }
  double stored(doc::DeviceId block) const {
    const doc::DeviceValue* v = dev(block).param(doc::kValueParam);
    return v != nullptr ? v->value : 0.0;
  }
  // The block's value in its curve's units at its stored position.
  double units(doc::DeviceId block) const {
    const dsp::Curve* c = dev(block).curve(doc::kValueParam);
    return c != nullptr ? dsp::evalCurveUnits(*c, stored(block)) : stored(block);
  }
  // Moves the block off its default (one step), then resets it by Delete (a click gives the knob the keys) and by a
  // double-click: each one undo step, back at `want` (its units).
  void resetBoth(doc::DeviceId block, double want, std::optional<Vec2> where = std::nullopt) {
    const std::string name = dev(block).name;
    CAPTURE(name);
    CHECK(units(block) == doctest::Approx(want).epsilon(1e-9).scale(1.0));
    const double home = stored(block);
    const Vec2 c = where ? *where : at(block);
    for (const bool key : {true, false}) {
      CAPTURE(key);
      (void)run(doc::ops::setDeviceParam(*ctx.doc(), block, std::string(doc::kValueParam), home < 0.5 ? 0.9 : 0.1));
      d.step();
      REQUIRE(pos(block) != doctest::Approx(home));
      size_t h0 = history();
      if (key) {
        d.click(c);
        d.step();
        h0 = history();
        d.press("Delete");
      } else {
        d.doubleClick(c);
      }
      d.step();
      CHECK(history() == h0 + 1);
      CHECK(stored(block) == home);
      CHECK(pos(block) == home);
      CHECK(units(block) == doctest::Approx(want).epsilon(1e-9).scale(1.0));
    }
  }
};

}  // namespace

TEST_CASE("reset default: the Phase Accumulator's Amplitude (x1) and Pitch (0 c) — Delete and double-click, one undo "
          "step each — real dispatch") {
  Ui u("phase_acc");
  const doc::DeviceId amp = u.member(doc::kPaAmplitude), pitch = u.member(doc::kPaPitch);
  REQUIRE(amp.valid());
  REQUIRE(pitch.valid());
  // (the arcs' handles at the default: modules.cpp phaseAccumulatorBlocks' panel; a press on the arc's ring anywhere)
  u.resetBoth(amp, 1.0, u.arcAt(amp, 252.0, 40.0, 0.5));
  u.resetBoth(pitch, 0.0, u.arcAt(pitch, 36.0, -72.0, 0.5));
  CHECK(u.stored(amp) == 0.5);
}

TEST_CASE("reset default: an EQ band's Gain (0 dB) — Delete and double-click, one undo step each — real dispatch") {
  Ui u("eq_band");
  const doc::DeviceId gain = u.member("Gain");
  REQUIRE(gain.valid());
  u.resetBoth(gain, 0.0);
}

TEST_CASE("reset default: a value block the user placed (its declaration's 0) and a rack macro (where it was made) — "
          "real dispatch") {
  Ui u("utility");
  // A value block the user places in the module's group (made at 0; no default of its own), given a panel knob.
  const doc::DeviceId v = u.run(doc::ops::addDevice(*u.ctx.doc(), u.ctx.store().ids(), u.track, std::string(doc::kValueDevice),
                                                    doc::DeviceKind::AudioEffect, doc::ops::kOffPath))
                              .devices.at(0);
  CHECK_FALSE(u.dev(v).defaultPosition.has_value());
  (void)u.run(doc::ops::setDeviceGroup(*u.ctx.doc(), u.ctx.store().ids(), &library(), v, u.group));
  {
    doc::PanelElement k;
    k.kind = "knob";
    k.x = u.dev(u.group).panel->width;
    k.y = 8.0f;
    k.w = k.h = 40.0f;
    k.bindings = {doc::controlBinding(v, std::string(doc::kValueParam))};
    (void)u.run(doc::ops::addPanelElements(*u.ctx.doc(), u.group, {k}));
  }
  u.d.tree().layoutNow();
  u.resetBoth(v, 0.0);
  // A rack macro: made at the mapped knob's position (Utility's Gain at 0.5: 0 dB), a reset goes back there.
  const doc::DeviceId gain = u.member("Gain");
  REQUIRE(gain.valid());
  const doc::DeviceId rack = u.run(doc::ops::groupDevices(*u.ctx.doc(), u.ctx.store().ids(), std::vector<doc::DeviceId>{u.group})).devices[0];
  (void)u.run(doc::ops::mapMacro(*u.ctx.doc(), u.ctx.store().ids(), rack, 0, gain, std::string(doc::kValueParam)));
  const std::optional<doc::RackInfo> ri = doc::rackOf(*u.ctx.doc(), rack);
  REQUIRE(ri.has_value());
  const doc::DeviceId block = ri->macros[0].block;
  REQUIRE(block.valid());
  const double home = u.stored(block);
  CHECK(u.dev(block).defaultPosition == std::optional<double>(home));
  u.d.tree().layoutNow();
  const Vec2 m = u.at(rack, "macro1");
  (void)u.run(doc::ops::setMacroValue(*u.ctx.doc(), rack, 0, 0.9 * doc::kMacroMax));
  u.d.step();
  REQUIRE(u.stored(block) != doctest::Approx(home));
  const size_t h0 = u.history();
  u.d.doubleClick(m);
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.stored(block) == home);
}

// TX.reset-default's detents: a slow drag from the control passes each listed value and holds there (several pointer
// steps at exactly its position), never jumping.
namespace {
void dragHolds(Ui& u, doc::DeviceId block, float dy, int steps, std::vector<double> values, std::optional<Vec2> where = std::nullopt) {
  REQUIRE(u.dev(block).curve(doc::kValueParam) != nullptr);
  const dsp::Curve curve = *u.dev(block).curve(doc::kValueParam);
  const dsp::Curve* c = &curve;
  const Vec2 at = where ? *where : u.at(block);
  u.d.down(at);
  std::vector<double> seen;
  double prev = u.pos(block);
  for (int px = 1; px <= steps; ++px) {
    u.d.move({at.x, at.y + dy * static_cast<float>(px)});
    const double p = u.pos(block);
    CHECK(std::abs(p - prev) <= 0.02);  // never a jump
    seen.push_back(p);
    prev = p;
  }
  u.d.up({at.x, at.y + dy * static_cast<float>(steps)});
  u.d.step();
  for (const double v : values) {
    CAPTURE(v);
    const double t = dsp::curvePosition(*c, dsp::curveOutput(*c, v), 0.5);
    std::array<double, dsp::kMaxCurveSegments + 1> det{};
    const uint32_t n = dsp::curveDetents(*c, det);
    const auto it = std::find_if(det.begin(), det.begin() + n, [&](double x) { return std::abs(x - t) < 1e-9; });
    REQUIRE(it != det.begin() + n);  // a detent there
    CHECK(std::count(seen.begin(), seen.end(), *it) >= 3);  // held over several pixels
  }
  REQUIRE(u.ctx.undo());
}
}  // namespace

TEST_CASE("reset default: detents — the VCA's knob holds at x2 and x0 / x-1 / x-2 on its way, the EQ band's Q at "
          "0.7071 (its default) — real dispatch drags") {
  {
    Ui u("vca");
    const doc::DeviceId v = u.member(doc::kVcaValueName);
    REQUIRE(v.valid());
    CHECK(u.units(v) == doctest::Approx(1.0));  // the default x1
    const Vec2 knob = u.local(v, {60.0f + 25.0f, 61.0f});  // the dial's knob (rich_elements.cpp Dial, centre (60, 61))
    dragHolds(u, v, -1.0f, 120, {2.0}, knob);                // up: through x2
    dragHolds(u, v, 1.0f, 260, {0.0, -1.0, -2.0}, knob);     // down: x0, x-1, x-2
  }
  {
    Ui u("eq_band");
    const doc::DeviceId q = u.member("Q");
    REQUIRE(q.valid());
    CHECK(u.units(q) == doctest::Approx(0.70710678118654752).epsilon(1e-12));
    dragHolds(u, q, -1.0f, 40, {0.70710678118654752});  // starts on it: held, then free
  }
}
