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
//   - MX.4 JACKS: In A, In B and Out are the closed block's pins on the face's "jack" elements (present unwired, no
//     orbit, sockets in the scene); a cable dragged onto In B wires B's input (one undo step), one from Out wires another
//     block; the scene rebuilt under a held cable (the op swapped mid-drag) neither crashes nor loses the drop.
//   - GOLDENS (signals flowing: A a sine, B a slower sine, Out the op's result, through a manual feed): math-face-gt
//     (a compare: the gate trace), math-face-clip (a range op), math-face-eq (Tolerance shown), each at 1x and 2x.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../support/blocks.h"
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
#include "pq/views/panel/trace_scope.h"
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
// MX.4: the jacks' centres (modules.cpp mathBlocks: In A 8, 8; In B 282, 8; Out 145, 173; 10 × 10).
constexpr Vec2 kJackA{13.0f, 13.0f}, kJackB{287.0f, 13.0f}, kJackOut{150.0f, 178.0f};

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

  doc::DeviceId vca;  // MX.4: a VCA off the path (withVca)

  explicit MathUi(bool withVca = false) {
    ctx.setDeviceLibrary(&devLib());
    g = buildMath(ctx.store(), doc::kMathDefaultOp);
    if (withVca) vca = test_blocks::addModule(ctx.store(), doc::findDevice(*ctx.doc(), g).track->id, "vca", doc::ops::kOffPath);
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
  // MX.4: where block `d`'s main input pin is now (device px), and whether it is on the canvas.
  std::optional<Vec2> mainInput(doc::DeviceId dev, bool& docked) const {
    const uint32_t i = view().patch().nodeOf(dev);
    if (i == kNone || view().patch().nodes[i].mainIn < 0) return std::nullopt;
    const Vec2 p = view().canvas().toDevice(view().canvas().portWorld(i, static_cast<uint32_t>(view().patch().nodes[i].mainIn), true));
    const ui::IRect r = view().canvas().rect();
    docked = p.x >= static_cast<float>(r.x) && p.y >= static_cast<float>(r.y) && p.x <= static_cast<float>(r.right()) &&
             p.y <= static_cast<float>(r.bottom());
    return p;
  }
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

TEST_CASE("BR.A: a trace scope shows −1 … 1; a value beyond it widens the range slowly to what it spanned over the last "
          "~5 s, and the range relaxes back to ±1 once those extremes age out") {
  FaceRig r("add", 1.0f);
  (void)r.render();  // the first paint acquires the feed
  std::vector<const panel::Element*> scopes;
  for (size_t i = 0; i < r.face->elementCount(); ++i)
    if (panel::test::traceRange(r.face->element(i))) scopes.push_back(&r.face->element(i));
  REQUIRE(scopes.size() >= 3);  // (A, B, Out; an "all" scope too)
  for (const panel::Element* e : scopes) CHECK(*panel::test::traceRange(*e) == std::pair<float, float>{-1.0f, 1.0f});
  // A = ampA · sin(2π·110 t), B = 0.4 · sin, Out = A + B: `seconds` of it in 50 ms steps, the face painted at each.
  auto run = [&](double ampA, double seconds) {
    constexpr uint32_t kStep = 2400;  // 50 ms
    std::vector<float> beat(kStep), a(kStep), b(kStep), out(kStep);
    for (double done = 0.0; done < seconds - 1e-9; done += 0.05) {
      for (uint32_t j = 0; j < kStep; ++j) {
        const double t = static_cast<double>(r.at + j) / kSr;
        a[j] = static_cast<float>(ampA * std::sin(2.0 * std::numbers::pi * 110.0 * t));
        b[j] = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 27.5 * t));
        beat[j] = static_cast<float>(t * 2.0);
        out[j] = a[j] + b[j];
      }
      const float* ch[4] = {beat.data(), a.data(), b.data(), out.data()};
      r.tap.write(ch, 4, kStep);
      r.at += kStep;
      (void)r.feed.step(r.clock += 0.05);
      Clock::now() += 0.05;
      (void)r.face->pollVoices();  // (the host's frame tick: the feed read, a repaint asked)
      (void)r.render();
    }
  };
  auto widest = [&] {
    std::pair<float, float> w{0.0f, 0.0f};
    for (const panel::Element* e : scopes) {
      const auto x = *panel::test::traceRange(*e);
      w = {std::min(w.first, x.first), std::max(w.second, x.second)};
    }
    return w;
  };
  auto narrowest = [&] {
    float h = 1e9f;
    for (const panel::Element* e : scopes) h = std::min(h, panel::test::traceRange(*e)->second);
    return h;
  };
  run(4.0, 0.1);  // slowly: after 0.1 s well short of the peak
  CHECK(widest().second > 1.2f);
  CHECK(widest().second < 3.0f);
  run(4.0, 2.0);  // settled on the span (A's ±4, Out's ±4.4)
  CHECK(widest().second > 3.9f);
  CHECK(widest().second < 4.45f);
  CHECK(widest().first < -3.9f);
  CHECK(widest().first > -4.45f);
  CHECK(narrowest() == doctest::Approx(1.0));  // B (±0.4) never left ±1
  run(0.5, 3.0);  // quiet, but the loud part is still inside the 5 s window: held
  CHECK(widest().second > 4.0f);
  run(0.5, 14.0);  // aged out (after ~5 s), then eased back to ±1
  CHECK(widest().second == doctest::Approx(1.0).epsilon(0.01));
  CHECK(widest().first == doctest::Approx(-1.0).epsilon(0.01));
}

// ---- MX.4: the face's jacks are the closed block's pins (Q?-MX.3-a option 2) ---------------------------------------

namespace {
size_t wiresInto(const doc::Project& p, doc::DeviceId to, std::string_view param) {
  size_t n = 0;
  for (const doc::Wire& w : doc::wiresOf(p)) n += !w.toMixer() && w.to == to && w.param == param ? 1 : 0;
  return n;
}
const doc::Wire* wireFromTo(const doc::Project& p, doc::DeviceId from, doc::DeviceId to) {
  for (const doc::Wire& w : doc::wiresOf(p))
    if (!w.fromNoteSource() && !w.toMixer() && w.from == from && w.to == to) return &w;
  return nullptr;
}
}  // namespace

TEST_CASE("MX.4 math face: In A, In B and Out are the block's pins on its jacks — present unwired, no orbit, sockets in "
          "the scene where the jacks are") {
  MathUi u(true);
  const doc::DeviceId inB = doc::mathInfo(*u.ctx.doc(), u.g)->roles.inB;
  const Node& n = u.node();
  REQUIRE(n.groupPanel);
  const int32_t b = n.input("@" + std::to_string(inB.value));  // B's main input, unwired
  REQUIRE(b >= 0);
  CHECK(n.inputs[static_cast<size_t>(b)].jack);
  CHECK_FALSE(n.inputs[static_cast<size_t>(b)].param);  // a signal input (drawn solid)
  REQUIRE(n.mainIn >= 0);
  CHECK(n.inputs[static_cast<size_t>(n.mainIn)].jack);
  REQUIRE_FALSE(n.outputs.empty());
  CHECK(n.outputs[0].jack);
  CHECK_FALSE(n.orbitIn.on);
  CHECK_FALSE(n.orbitOut.on);
  auto near = [](graph::Point2 a, graph::Point2 e) { return std::fabs(a.x - e.x) < 0.01f && std::fabs(a.y - e.y) < 0.01f; };
  const graph::Point2 jb{n.x + kJackB.x * n.scale, n.y + kJackB.y * n.scale};
  CHECK(near(inPortPoint(n, static_cast<size_t>(b)), jb));
  CHECK(near(inPortPoint(n, static_cast<size_t>(n.mainIn)), {n.x + kJackA.x * n.scale, n.y + kJackA.y * n.scale}));
  CHECK(near(outPortPoint(n, 0), {n.x + kJackOut.x * n.scale, n.y + kJackOut.y * n.scale}));
  // The scene draws them as sockets there (the canvas's port look: hover, the plugged cable's colour).
  const ProjectScene& ps = u.view().scene();
  const uint32_t sn = ps.sceneNode[u.view().patch().nodeOf(u.g)];
  REQUIRE(sn != kNone);
  const circuit::SceneNode& node = ps.scene->nodes[sn];
  const circuit::ScenePin& pin = ps.scene->pins[node.firstPin + static_cast<uint32_t>(b)];
  CHECK(pin.socket == circuit::ScenePin::kSocketIn);
  CHECK(pin.visible);
  CHECK(near({pin.x, pin.y}, jb));
  CHECK(ps.scene->pins[node.firstPin + n.inputs.size()].socket == circuit::ScenePin::kSocketOut);
  // A press there is the pin's (not the face's frame): the canvas finds it.
  const ProjectCanvas::PinHit hit = u.view().canvas().pinAt(u.at(kJackB));
  CHECK(hit.valid());
  CHECK(hit.input);
  CHECK(hit.port == static_cast<uint32_t>(b));
}

TEST_CASE("MX.4 math face: a cable dragged onto In B on the closed face is a wire into B's input (one undo step); one "
          "from Out onto another block's input — real dispatch") {
  MathUi u(true);
  const doc::DeviceId inB = doc::mathInfo(*u.ctx.doc(), u.g)->roles.inB;
  bool docked = false;
  std::optional<Vec2> vcaOut = u.view().outputPoint(u.vca, "y", &docked);
  if (!vcaOut || !docked) {
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    u.d.step();
    vcaOut = u.view().outputPoint(u.vca, "y", &docked);
  }
  REQUIRE(vcaOut);
  REQUIRE(docked);
  const size_t h0 = u.history();
  REQUIRE(wiresInto(*u.ctx.doc(), inB, doc::kMainPort) == 0);
  u.d.drag(*vcaOut, u.at(kJackB), 8);
  u.d.step();
  CHECK(wiresInto(*u.ctx.doc(), inB, doc::kMainPort) == 1);
  const doc::Wire* w = wireFromTo(*u.ctx.doc(), u.vca, inB);
  REQUIRE(w != nullptr);
  CHECK(u.history() == h0 + 1);
  // The wire draws to the jack: its edge ends there, and the scene's cable plugs into the jack's socket.
  const doc::WireId wid = w->id;
  const ProjectPatch& pp = u.view().patch();
  const Node& n = u.node();
  bool drawn = false;
  for (const Edge& e : pp.edges)
    if (e.wire == wid && !e.points.empty()) {
      drawn = true;
      CHECK(std::fabs(e.points.back().x - (n.x + kJackB.x * n.scale)) < 0.01f);
      CHECK(std::fabs(e.points.back().y - (n.y + kJackB.y * n.scale)) < 0.01f);
    }
  CHECK(drawn);
  REQUIRE(u.ctx.store().undo());
  u.d.step();
  CHECK(wiresInto(*u.ctx.doc(), inB, doc::kMainPort) == 0);
  CHECK(u.history() == h0);

  // From Out onto the VCA's main input.
  std::optional<Vec2> vcaIn = u.mainInput(u.vca, docked);
  REQUIRE(vcaIn);
  REQUIRE(docked);
  REQUIRE(u.view().canvas().pinAt(u.at(kJackOut)).valid());
  CHECK_FALSE(u.view().canvas().pinAt(u.at(kJackOut)).input);
  REQUIRE(wireFromTo(*u.ctx.doc(), u.g, u.vca) == nullptr);
  u.d.drag(u.at(kJackOut), *vcaIn, 8);
  u.d.step();
  const doc::Wire* o = wireFromTo(*u.ctx.doc(), u.g, u.vca);
  REQUIRE(o != nullptr);
  CHECK(o->port == doc::kMainPort);
  CHECK(o->param == doc::kMainPort);
  CHECK(u.history() == h0 + 1);
}

TEST_CASE("MX.4 math face: the scene rebuilt while a cable is drawn out of a jack (the op swapped mid-drag) — no crash, "
          "the drop still wires — real dispatch") {
  MathUi u(true);
  bool docked = false;
  std::optional<Vec2> vcaIn = u.mainInput(u.vca, docked);
  if (!vcaIn || !docked) {
    u.view().fitAll(false);
    u.d.tree().layoutNow();
    u.d.step();
    vcaIn = u.mainInput(u.vca, docked);
  }
  REQUIRE(vcaIn);
  REQUIRE(docked);
  const Vec2 from = u.at(kJackOut);
  u.d.move(from);
  u.d.down(from);
  u.d.move({from.x + 10.0f, from.y + 10.0f});
  u.d.step();
  // A structural edit under the held cable: the op's blocks replaced, the patch and scene rebuilt (deferred past paint).
  const doc::ops::EditResult swap = doc::ops::setMathOp(*u.ctx.doc(), u.ctx.store().ids(), u.g, "clip");
  REQUIRE(swap.ok());
  REQUIRE(u.ctx.store().dispatch(swap->command).ok());
  for (int i = 0; i < 3; ++i) {
    u.d.move({from.x + 20.0f + 10.0f * static_cast<float>(i), from.y + 20.0f});
    u.d.step();
  }
  vcaIn = u.mainInput(u.vca, docked);
  REQUIRE(vcaIn);
  u.d.move(*vcaIn);
  u.d.up(*vcaIn);
  u.d.step();
  CHECK(u.op() == "clip");
  CHECK(wireFromTo(*u.ctx.doc(), u.g, u.vca) != nullptr);
}
