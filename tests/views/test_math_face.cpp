// MX.3 (docs/plans/MX.md "MX.3"): the Math module's face.
//   - REAL DISPATCH in the shell's project patch view (the module spawns closed as its panel): a click on the op circle
//     opens the radial menu (current op opened to); a pick by hovering a category then clicking an op, and a pick by
//     typing and Enter, each swap the op (ops::setMathOp: one undo step; undo restores the op and its blocks); the scene
//     rebuilds inside the pick handler and the rebuilt circle opens the menu again. A value typed into the A box is one
//     undo step, a param write (no recompile).
//   - VISIBILITY: Tolerance only while == or ≠; the output scope's gate element while the op gives a gate, the auto one
//     otherwise.
//   - TRACES: every scope of the face is one consumer of the one trace tap (a shared window); the feed is acquired at
//     the first paint, never while the face is not painted, and let go when painting stops.
//   - GOLDENS (signals flowing: A a sine, B a slower sine, Out the op's result, through a manual feed): math-face-gt
//     (a compare: the gate trace), math-face-clip (a range op), math-face-eq (Tolerance shown), each at 1x and 2x.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/core/tap_ring.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/dsp/curve.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/radial_menu.h"
#include "pq/ui/test_driver.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/fake_transport.h"
#include "pq/views/panel/element.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;
using namespace pq::views::project_patch;

namespace {

constexpr double kSr = 48000.0;
constexpr Vec2 kOpCentre{150.0f, 112.0f};  // the op circle (modules.cpp mathBlocks: 110, 72, 80 × 80)
constexpr Vec2 kValueA{44.0f, 31.0f};      // the A box (8, 22, 72 × 18)

const doc::DeviceLibrary& devLib() {
  static std::unique_ptr<doc::DeviceLibrary> lib = [] {
    auto r = doc::loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

void checkImage(const std::string& name, const Image& img) {
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + name + ".png");
  if (!ref) {
    (void)writePng(name + ".actual.png", img);
    FAIL_CHECK("no golden " << name << ": wrote " << name << ".actual.png");
    return;
  }
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(name, img, *ref, true, &d));
  MESSAGE(name << ": " << pq::ui::test::describe(d));
}

// A MIDI track with a Math module pasted (as the node menu does); its op `op`.
doc::DeviceId buildMath(doc::DocumentStore& st, std::string_view op) {
  auto run = [&](const doc::ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(st.dispatch(e->command).ok());
    return *e;
  };
  const doc::TrackId track = run(doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Math")).tracks.at(0);
  doc::IdSource local;
  doc::MathRoles l;
  const doc::ops::Edit e = run(doc::ops::pasteBlocks(*st.current(), st.ids(), track, doc::mathBlocks(local, {}, &l)));
  const doc::DeviceId g = e.devices.at(l.group.value - 1);
  if (op != doc::kMathDefaultOp) (void)run(doc::ops::setMathOp(*st.current(), st.ids(), g, op));
  return g;
}

std::string opOf(const doc::Project& p, doc::DeviceId g) {
  const std::optional<doc::MathInfo> mi = doc::mathInfo(p, g);
  return mi && mi->op != nullptr ? std::string(mi->op->key) : std::string();
}

// The Math module in the shell's project patch view, the view at zoom 1 on it.
struct MathUi {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d{1600, 900};
  std::unique_ptr<Shell> shell;
  doc::DeviceId g;
  int structural = 0;
  ViewContext::Subscription sub;

  MathUi() {
    ctx.setDeviceLibrary(&devLib());
    g = buildMath(ctx.store(), doc::kMathDefaultOp);
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    d.tree().layoutNow();
    view().setPushAnimated(false);
    view().setShown(true, false);
    d.tree().layoutNow();
    frame();
    d.tree().setFocus(&view().canvas());
    sub = ctx.subscribe([this](const doc::StoreEvent& e) { structural += e.before != e.after && e.changes.structural() ? 1 : 0; });
  }
  ~MathUi() {
    sub.reset();
    d.tree().setRoot(std::make_unique<Widget>());
  }
  void frame() {
    const Node& n = node();
    circuit::CircuitCanvas::View v;
    v.zoom = 1.0;
    v.x = n.x - 40.0;
    v.y = n.y - 40.0;
    view().canvas().setView(v);
    d.tree().layoutNow();
    d.step();
  }
  ProjectPatchView& view() const {
    ProjectPatchView* v = findProjectPatch(*shell);
    REQUIRE(v != nullptr);
    return *v;
  }
  const Node& node() const {
    const uint32_t n = view().patch().nodeOf(g);
    REQUIRE(n != kNone);
    return view().patch().nodes[n];
  }
  Vec2 at(Vec2 local) const {
    const Node& n = node();
    return view().canvas().toDevice({n.x + local.x * n.scale, n.y + local.y * n.scale});
  }
  size_t history() const { return ctx.store().history().cursor(); }
  std::string op() const { return opOf(*ctx.doc(), g); }
  RadialMenu* menu() const {
    OverlayLayer* o = OverlayLayer::of(view().canvas());
    return o != nullptr ? dynamic_cast<RadialMenu*>(o->top()) : nullptr;
  }
  RadialMenu& open() {
    d.click(at(kOpCentre));
    RadialMenu* m = menu();
    REQUIRE(m != nullptr);
    d.advance(0.2);  // (past the open animation)
    return *m;
  }
  Vec2 bubble(std::string_view keyOrLabel) {
    RadialMenu* m = menu();
    REQUIRE(m != nullptr);
    const int32_t i = m->find(keyOrLabel);
    REQUIRE_MESSAGE(i >= 0, keyOrLabel);
    REQUIRE_MESSAGE(m->shown(i), keyOrLabel);
    return m->centreOf(i);
  }
};

// The panel target with the trace scopes' feed (as the app's lowering tap gives one).
class FeedTarget final : public panel::PanelTarget {
public:
  FeedTarget(std::unique_ptr<panel::PanelTarget> inner, devices::SpectrumFeed* feed) : in_(std::move(inner)), feed_(feed) {}
  const panel::BoundInfo& info(const doc::Binding& b) const override { return in_->info(b); }
  double position(const doc::Binding& b) const override { return in_->position(b); }
  std::string format(const doc::Binding& b, double p) const override { return in_->format(b, p); }
  std::optional<double> parse(const doc::Binding& b, std::string_view t) const override { return in_->parse(b, t); }
  void begin(const doc::Binding& b, const char* l) override { in_->begin(b, l); }
  void setPosition(const doc::Binding& b, double p) override { in_->setPosition(b, p); }
  void setCurve(const doc::Binding& b, const dsp::Curve& c) override { in_->setCurve(b, c); }
  void end() override { in_->end(); }
  void cancel() override { in_->cancel(); }
  void setOnce(const doc::Binding& b, double p, const char* l) override { in_->setOnce(b, p, l); }
  void reset(const doc::Binding& b) override { in_->reset(b); }
  uint64_t revision() const override { return in_->revision(); }
  const doc::Project* project() const override { return in_->project(); }
  devices::SpectrumFeed* traceFeed(const doc::PanelElement& e) override { return doc::elementTraces(e).empty() ? nullptr : feed_; }

private:
  std::unique_ptr<panel::PanelTarget> in_;
  devices::SpectrumFeed* feed_;
};

struct Clock {
  static double& now() {
    static double c = 100.0;
    return c;
  }
};

// The face alone in a plain tree (as Standalone does a panel), its scopes on a manual feed of the trace tap's four
// channels (beat, A, B, Out).
struct FaceRig {
  FakeTransport port;
  ViewContext ctx{port};
  doc::DeviceId g;
  TapRing tap{4, 1u << 16, 4096};
  devices::SpectrumFeed feed{tap, devices::AnalysisHub::config(kSr), devices::SpectrumFeed::Manual{}};
  std::unique_ptr<TestDriver> d;
  panel::GroupPanelFace* face = nullptr;
  uint64_t at = 0;
  double clock = 1.0;

  FaceRig(std::string_view op, float scale) {
    MetaFace::setClock(+[]() -> double { return Clock::now(); });
    ctx.setDeviceLibrary(&devLib());
    g = buildMath(ctx.store(), op);
    const doc::Panel* p = doc::findDevice(*ctx.doc(), g).device->panel.get();
    REQUIRE(p != nullptr);
    d = std::make_unique<TestDriver>(static_cast<uint32_t>(p->width * scale), static_cast<uint32_t>(p->height * scale), scale);
    panel::PanelHost* host = nullptr;
    auto f = std::make_unique<panel::GroupPanelFace>(
        panel::docPanelSource(ctx, g),
        std::make_unique<FeedTarget>(panel::makeDocPanelTarget(ctx, g, [&host]() -> Widget* { return host; }), &feed));
    face = f.get();
    host = &d->setRoot<panel::PanelHost>("panel", std::move(f));
    d->tree().layoutNow();
  }
  ~FaceRig() {
    d->tree().setRoot(std::make_unique<Widget>());
    MetaFace::setClock(nullptr);
  }
  // `seconds` of A = 0.8·sin(2π·110 t), B = 0.4·sin(2π·27.5 t), Out = the op's reference.
  void stream(std::string_view op, double seconds) {
    const auto n = static_cast<uint32_t>(seconds * kSr);
    std::vector<float> beat(4096), a(4096), b(4096), out(4096);
    for (uint32_t i = 0; i < n; i += 4096) {
      const uint32_t k = std::min<uint32_t>(4096, n - i);
      for (uint32_t j = 0; j < k; ++j) {
        const double t = static_cast<double>(at + j) / kSr;
        a[j] = static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 110.0 * t));
        b[j] = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 27.5 * t));
        beat[j] = static_cast<float>(t * 2.0);
        out[j] = static_cast<float>(doc::mathOpReference(op, a[j], b[j], doc::kMathDefaultTolerance));
      }
      const float* ch[4] = {beat.data(), a.data(), b.data(), out.data()};
      tap.write(ch, 4, k);
      at += k;
      (void)feed.step(clock += static_cast<double>(k) / kSr);
    }
  }
  bool gpu = false;
  Image render() {
    if (!gpu) REQUIRE(d->enableGpu().ok());
    gpu = true;
    d->advance(0.05);
    auto img = d->render();
    REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
    return std::move(*img);
  }
  int32_t index(uint32_t id) const { return face->indexOf(id); }
  bool shown(uint32_t id) const { return face->shown(static_cast<size_t>(index(id))); }
};

Image faceImage(std::string_view op, float scale) {
  FaceRig r(op, scale);
  (void)r.render();     // the first paint acquires the feed (the tap gets a reader)
  r.stream(op, 0.5);    // then the signals flow
  CHECK(r.face->pollVoices());
  return r.render();
}

}  // namespace

TEST_CASE("math face: a click on the op circle opens the radial menu; hover a category, click an op — one undo step, "
          "undo restores the op — real dispatch") {
  MathUi u;
  CHECK_FALSE(u.view().groupOpen(u.g));
  CHECK(u.node().groupPanel);
  REQUIRE(u.op() == "gt");
  const std::optional<doc::MathInfo> before = doc::mathInfo(*u.ctx.doc(), u.g);
  REQUIRE(before);
  const size_t h0 = u.history();
  RadialMenu& m = u.open();
  CHECK(m.labelOf(m.openItem(0)) == "Compare");  // the current op opened to
  CHECK(m.find("hypot") >= 0);
  CHECK(m.find("Geometry") >= 0);
  u.d.move(u.bubble("Range"));
  CHECK(m.labelOf(m.openItem(0)) == "Range");
  const Vec2 max = u.bubble("max");
  u.d.move(max);
  u.d.click(max);
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK(u.op() == "max");
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural >= 1);
  const std::optional<doc::MathInfo> after = doc::mathInfo(*u.ctx.doc(), u.g);
  REQUIRE(after);
  CHECK(after->roles.op != before->roles.op);
  CHECK(after->roles.a == before->roles.a);  // A, B and Tolerance kept
  CHECK(after->roles.tolerance == before->roles.tolerance);
  REQUIRE(u.ctx.store().undo());
  u.d.step();
  CHECK(u.op() == "gt");
  CHECK(doc::mathInfo(*u.ctx.doc(), u.g)->roles.op == before->roles.op);
  CHECK(u.history() == h0);
}

TEST_CASE("math face: typing filters the menu and Enter picks; the scene rebuilds inside the pick handler and the "
          "rebuilt circle opens the menu again — real dispatch") {
  MathUi u;
  const size_t h0 = u.history();
  RadialMenu& m = u.open();
  u.d.type("hyp");
  REQUIRE(m.resultCount() == 1);
  CHECK(m.keyOf(m.result(0)) == "hypot");
  u.d.press("Enter");
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK(u.op() == "hypot");
  CHECK(u.history() == h0 + 1);
  // The scene was rebuilt (the op's blocks changed): the circle answers again, now marking hypot.
  RadialMenu& m2 = u.open();
  CHECK(m2.labelOf(m2.openItem(0)) == "Geometry");
  u.d.type("!=");
  REQUIRE(m2.resultCount() >= 1);
  CHECK(m2.keyOf(m2.result(0)) == "ne");
  u.d.press("Enter");
  u.d.step();
  CHECK(u.op() == "ne");
  CHECK(u.history() == h0 + 2);
  // Esc goes out of the current op's ring, then closes — without an edit.
  (void)u.open();
  u.d.press("Escape");
  u.d.step();
  CHECK(u.menu() != nullptr);
  u.d.press("Escape");
  u.d.step();
  CHECK(u.menu() == nullptr);
  CHECK(u.history() == h0 + 2);
}

TEST_CASE("math face: a value typed into the A box is one undo step, a param write (no recompile) — real dispatch") {
  MathUi u;
  const doc::DeviceId a = doc::mathInfo(*u.ctx.doc(), u.g)->roles.a;
  const size_t h0 = u.history();
  u.d.doubleClick(u.at(kValueA));
  u.d.type("0.5");
  u.d.press("Enter");
  u.d.step();
  CHECK(u.history() == h0 + 1);
  CHECK(u.structural == 0);
  const doc::Device& dev = *doc::findDevice(*u.ctx.doc(), a).device;
  const doc::DeviceValue* v = dev.param(doc::kValueParam);
  REQUIRE(v != nullptr);
  const dsp::Curve* c = dev.curve(doc::kValueParam);
  REQUIRE(c != nullptr);
  CHECK(dsp::evalCurveUnits(*c, v->value) == doctest::Approx(0.5).epsilon(1e-3));
}

TEST_CASE("math face: Tolerance only while == or ≠; the output scope draws a gate while the op gives one") {
  for (const doc::MathOpInfo& op : doc::mathOpInfos()) {
    CAPTURE(op.key);
    FaceRig r(op.key, 1.0f);
    (void)r.face->worldSize();
    CHECK(r.shown(4) == op.tolerance);  // Tolerance
    CHECK(r.shown(2) == !op.gate);      // the output scope, auto
    CHECK(r.shown(8) == op.gate);       // … as a 0 / 1 gate
    CHECK(r.shown(9));                  // A, B
    CHECK(r.shown(10));
    CHECK(r.shown(7));                  // the op circle
  }
}

TEST_CASE("math face: the scopes are ONE consumer of the trace tap — acquired at the first paint, never while unpainted, "
          "let go when painting stops") {
  FaceRig r("gt", 1.0f);
  for (int k = 0; k < 3; ++k) (void)r.face->pollVoices();
  CHECK(r.feed.consumers() == 0);  // never painted: nothing read
  CHECK_FALSE(r.tap.wanted());
  (void)r.render();
  CHECK(r.feed.consumers() == 1);  // three scopes shown (A, B, the gate Out): one shared window
  CHECK(r.tap.wanted());
  r.stream("gt", 0.1);
  CHECK(r.face->pollVoices());
  // Not painted any more (the host hidden): a poll after 0.5 s asks for one repaint; none comes — let go.
  Clock::now() += 1.0;
  CHECK(r.face->pollVoices());
  Clock::now() += 0.1;
  CHECK_FALSE(r.face->pollVoices());
  CHECK(r.feed.consumers() == 0);
  CHECK_FALSE(r.tap.wanted());
}

TEST_CASE("render: math-face-gt / -clip / -eq (MX.3: a compare's gate, a range op, == with Tolerance; signals flowing, "
          "the three traces aligned), 1x and 2x") {
  for (const float s : {1.0f, 2.0f}) {
    const std::string sfx = s == 1.0f ? "-1x" : "-2x";
    checkImage("math-face-gt" + sfx, faceImage("gt", s));
    checkImage("math-face-clip" + sfx, faceImage("clip", s));
    checkImage("math-face-eq" + sfx, faceImage("eq", s));
  }
}
