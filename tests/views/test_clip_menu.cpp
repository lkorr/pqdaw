// SX.2 / SX.5: the clip menu (pq/views/audio_clip/clip_menu.h: a compact popup in bands around the clip) on the audio
// fixture (tests/views/fixtures/audio): the button on a selected audio clip, Ctrl+Space and the context menu's
// "Clip Menu…" open it around the clip, centred on the press (real dispatch: window → arrangement → overlay → menu); its
// controls edit the clip, one undo step each (knob drags, typed transpose, unit chips, warp chips with the chosen mode's
// settings row, toggles, :2); disabled where the mode ignores a control; the document changing under it refreshes it;
// the clip deleted / undone away closes it; clips near the window's top / bottom and narrow clips keep it on screen and
// off the clip. Parity: every control of the clip panel (AudioClipView's cards) but Gain and the fades (the clip's own
// handles) has a menu counterpart that makes the identical clip. Goldens: the button on a selected clip
// (arrangement-audio), the menu on an Off clip and on a Beats clip (its settings row).

#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../ui/golden_check.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/image.h"
#include "pq/ui/overlay.h"
#include "pq/ui/radial_menu.h"
#include "pq/ui/test_driver.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/audio_clip/audio_clip_view.h"
#include "pq/views/audio_clip/clip_controls.h"
#include "pq/views/audio_clip/clip_edits.h"
#include "pq/views/audio_clip/clip_menu.h"
#include "pq/views/fake_transport.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_scenes.h"
#include "views_test_util.h"

using namespace pq;
using namespace pq::ui;
using namespace pq::views;

namespace {

constexpr Ticks kBar = kTicksPerWhole;
constexpr const char* kArr = "main/body/top/track-area/arrangement";
constexpr double kPxPerBar = 300.0;

struct Fixture {
  FakeTransport port;
  ViewContext ctx{port};
  TestDriver d;
  std::unique_ptr<Shell> shell;
  ArrangementView* arr = nullptr;
  explicit Fixture(uint32_t w = 1280, uint32_t h = 800) : d(w, h) {
    REQUIRE(ctx.openProject(views::test::fixtureDir("audio")).ok());
    auto s = Shell::create(d.tree(), ctx);
    REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
    shell = std::move(*s);
    arr = dynamic_cast<ArrangementView*>(d.find(kArr));
    REQUIRE(arr != nullptr);
    arr->setTimeView(-0.05 * static_cast<double>(kBar), kPxPerBar / static_cast<double>(kBar));
    d.tree().layoutNow();
    d.step();
  }
  ~Fixture() { d.tree().setRoot(std::make_unique<Widget>()); }

  size_t history() const { return ctx.store().history().cursor(); }
  doc::ClipRef clip(std::string_view trackName, size_t i) const {
    doc::ClipRef r{};
    doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
      if (!r.clip.valid() && t.name == trackName && i < t.clips.size()) r = {t.id, t.clips[i].id};
    });
    REQUIRE(r.clip.valid());
    return r;
  }
  const doc::AudioClip* find(const doc::ClipRef& r) const { return clip_edits::findAudioClip(*ctx.doc(), r); }
  const doc::AudioClip& get(const doc::ClipRef& r) const {
    const doc::AudioClip* c = find(r);
    REQUIRE(c != nullptr);
    return *c;
  }
  Vec2 title(const doc::ClipRef& r, Ticks dt) {
    const IRect box = arr->clipRect(r);
    return {arr->xOfTick(static_cast<double>(get(r).start + dt)) + 0.5f, static_cast<float>(box.y + 7)};
  }
  Vec2 button(const doc::ClipRef& r) {
    const Rect b = arr->clipMenuButtonRect(r);
    REQUIRE(b.x1 > b.x0);
    return {0.5f * (b.x0 + b.x1), 0.5f * (b.y0 + b.y1)};
  }
  void settle() {
    d.advance(0.3);  // the open animation
    d.tree().layoutNow();
    d.step();
  }
  RadialMenu& menu() {
    RadialMenu* m = arr->clipMenu();
    REQUIRE(m != nullptr);
    return *m;
  }
  // Where item `key` is drawn (its bubble shown).
  Vec2 at(std::string_view key) {
    RadialMenu& m = menu();
    const int32_t i = m.find(key);
    REQUIRE_MESSAGE(i >= 0, "no item " << key);
    REQUIRE_MESSAGE(m.shown(i), key << " not shown");
    return m.centreOf(i);
  }
  int32_t item(std::string_view key) {
    const int32_t i = menu().find(key);
    REQUIRE_MESSAGE(i >= 0, "no item " << key);
    return i;
  }
  // The open menu hugs the clip: its bands clear of the clip's visible box, inside the window.
  void checkAround(const doc::ClipRef& r) {
    RadialMenu& m = menu();
    const IRect box = arr->clipRect(r);
    const IRect win = d.root()->rect();
    for (int32_t b = 0; b < 2; ++b) {
      const IRect band = m.bandRect(b);
      if (band.empty()) continue;
      CHECK(band.x >= win.x);
      CHECK(band.y >= win.y);
      CHECK(band.right() <= win.right());
      CHECK(band.bottom() <= win.bottom());
      const bool clear = band.bottom() <= box.y || band.y >= box.bottom() || band.right() <= box.x || band.x >= box.right();
      CHECK_MESSAGE(clear, "band " << b << " covers the clip");
    }
    CHECK(m.bounds().x >= 0);
    CHECK(m.bounds().y >= 0);
    CHECK(m.bounds().right() <= win.right());
    CHECK(m.bounds().bottom() <= win.bottom());
  }
  void dispatch(const doc::ops::EditResult& e) {
    REQUIRE(e.ok());
    REQUIRE(ctx.store().dispatch(e->command).ok());
  }
};

}  // namespace

TEST_CASE("clip menu (SX.2, real dispatch): the button on a selected audio clip, Ctrl+Space and the context menu open it; "
          "narrow or unselected clips have no button") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0), keys = f.clip("Keys", 0);
  // Unselected: no button.
  CHECK(f.arr->clipMenuButtonRect(beat).x1 <= f.arr->clipMenuButtonRect(beat).x0);
  f.d.click(f.title(beat, kBar / 4));
  REQUIRE(f.arr->selection().clips.size() == 1);
  const size_t h0 = f.history();
  const doc::Snapshot before = f.ctx.doc();
  const Vec2 b = f.button(beat);
  f.d.move(b);
  CHECK(f.d.tree().cursor() == Cursor::Pointer);
  CHECK(f.arr->clipHover().zone == 6);
  // A press on it opens the menu (no move, no edit; the selection stays).
  f.d.down(b);
  f.d.move({b.x - 30.0f, b.y});  // the pointer now belongs to the menu: the clip does not move
  f.d.up({b.x - 30.0f, b.y});
  RadialMenu& m = f.menu();
  CHECK(f.ctx.doc() == before);
  CHECK(f.history() == h0);
  CHECK(f.arr->selection().clips.size() == 1);
  f.settle();
  // Bands around the clip, centred on the button (where it was clicked), never over the clip, on screen.
  const IRect box = f.arr->clipRect(beat);
  CHECK(m.bands());
  f.checkAround(beat);
  for (int32_t k = 0; k < 2; ++k) {  // centred on the press, unless clamped at the window's edge
    const IRect band = m.bandRect(k);
    CHECK((std::abs(0.5f * static_cast<float>(band.x + band.right()) - b.x) <= 1.0f || band.x <= 5));
  }
  CHECK(m.find("gain") < 0);  // the clip's handles edit gain and the fades
  CHECK(m.find("fade-in") < 0);
  CHECK(m.find("fade-out") < 0);
  CHECK(f.d.press("Escape") != KeyResult::Unhandled);
  f.d.step();
  CHECK(f.arr->clipMenu() == nullptr);

  // Ctrl+Space: the selected audio clip's menu.
  f.d.tree().setFocus(f.arr);
  CHECK(f.d.press("Ctrl+Space") == KeyResult::Action);
  REQUIRE(f.arr->clipMenu() != nullptr);
  f.settle();
  f.checkAround(beat);
  f.d.press("Escape");
  f.d.step();
  CHECK(f.arr->clipMenu() == nullptr);

  // The context menu's entry ("Clip Menu…", Ctrl+Space) on the clip under the pointer.
  const Vec2 rc{f.arr->xOfTick(static_cast<double>(kBar) * 0.5), static_cast<float>(box.bottom() - 8)};
  f.d.click(rc, mod::kNone, PointerButton::Right);
  f.d.step();
  f.d.tree().layoutNow();
  OverlayLayer* layer = OverlayLayer::of(*f.arr);
  REQUIRE(layer != nullptr);
  auto* cm = dynamic_cast<Menu*>(layer->top());
  REQUIRE(cm != nullptr);
  int32_t entry = -1;
  for (size_t i = 0; i < cm->items().size(); ++i)
    if (cm->items()[i].label == "Clip Menu\xE2\x80\xA6") entry = static_cast<int32_t>(i);
  REQUIRE(entry >= 0);
  CHECK(cm->items()[static_cast<size_t>(entry)].enabled);
  CHECK(cm->items()[static_cast<size_t>(entry)].shortcut == "Ctrl+Space");
  const IRect er = cm->itemRect(entry);
  f.d.click({static_cast<float>(er.x + er.w / 2), static_cast<float>(er.y + er.h / 2)});
  f.d.step();
  REQUIRE(f.arr->clipMenu() != nullptr);
  f.settle();
  f.checkAround(beat);
  {  // centred on the right-click (clamped on screen)
    const IRect band = f.menu().bandRect(1);
    CHECK((std::abs(0.5f * static_cast<float>(band.x + band.right()) - rc.x) <= 1.0f || band.x <= 8));
  }
  f.d.press("Escape");
  f.d.step();

  // A MIDI clip has no button (and Ctrl+Space with only it selected does nothing).
  const IRect kb = f.arr->clipRect(keys);
  f.d.click({static_cast<float>(kb.x + kb.w / 4), static_cast<float>(kb.y + 7)});
  REQUIRE(f.arr->selection().clips.size() == 1);
  CHECK(f.arr->clipMenuButtonRect(keys).x1 <= f.arr->clipMenuButtonRect(keys).x0);
  CHECK(f.d.press("Ctrl+Space") != KeyResult::Action);
  CHECK(f.arr->clipMenu() == nullptr);

  // Narrow (zoomed out: the clip under 3 buttons wide): no button, and a press there is a plain title press.
  f.d.click(f.title(beat, kBar / 4));
  f.arr->setTimeView(-0.05 * static_cast<double>(kBar), 24.0 / static_cast<double>(kBar));
  f.d.tree().layoutNow();
  f.d.step();
  CHECK(f.arr->clipMenuButtonRect(beat).x1 <= f.arr->clipMenuButtonRect(beat).x0);
}

TEST_CASE("clip menu (SX.2 / SX.5, real dispatch): its controls edit the clip, one undo step each; the chosen mode's "
          "settings row; disabled where the mode ignores them; refreshed under it; closed when the clip goes") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0), vox = f.clip("Vox", 0);
  f.d.click(f.title(beat, kBar / 4));
  f.d.click(f.button(beat));
  f.settle();
  RadialMenu& m = f.menu();

  // Off: transpose and detune apply, the tempo controls do not; no settings row.
  CHECK(m.enabledOf(f.item("transpose")));
  CHECK(m.enabledOf(f.item("detune")));
  CHECK_FALSE(m.enabledOf(f.item("bpm")));
  CHECK_FALSE(m.enabledOf(f.item("loop")));
  CHECK_FALSE(m.enabledOf(f.item("tempo/:2")));
  for (const char* k : {"preserve/1/8", "beats-env", "tones-grain", "flux", "formants"}) CHECK_FALSE(m.shown(f.item(k)));
  // Above the clip: the seven modes in one row; below: Transpose, its units, Detune, Reverse, BPM, :2 / ×2, Loop.
  const IRect box = f.arr->clipRect(beat);
  const float modeY = f.at("warp/Off").y;
  for (const char* k : {"warp/Off", "warp/Re-Pitch", "warp-stretch/Beats", "warp-stretch/Tones", "warp-stretch/Texture",
                        "warp-stretch/Complex", "warp-stretch/Pro"}) {
    CHECK(f.at(k).y == doctest::Approx(modeY));
    CHECK(f.at(k).y < static_cast<float>(box.y));
  }
  float x = -1e9f;
  for (const char* k : {"transpose", "unit/stp", "unit/\xC2\xA2", "unit/n/d", "detune", "reverse", "bpm", "tempo/:2", "loop"}) {
    const Vec2 p = f.at(k);
    CHECK_MESSAGE(p.y > static_cast<float>(box.bottom()), k);
    CHECK_MESSAGE(p.x > x, k);
    x = p.x;
  }

  // Detune: a drag up on the knob (one undo step), undone in one; the menu stays open and follows.
  size_t h = f.history();
  const Vec2 g = f.at("detune");
  f.d.drag(g, {g.x, g.y - 40.0f}, 5);
  CHECK(f.get(beat).detuneCents > 1.0f);
  CHECK(f.history() == h + 1);
  CHECK(f.arr->clipMenu() == &m);
  CHECK(f.ctx.store().undo());
  CHECK(f.get(beat).detuneCents == 0.0f);
  f.d.step();
  CHECK(m.valueOf(f.item("detune")) == 0.0);  // refreshed under it

  // Reverse: a click on the toggle.
  h = f.history();
  f.d.click(f.at("reverse"));
  CHECK(f.get(beat).reverse);
  CHECK(f.history() == h + 1);
  CHECK(m.litOf(f.item("reverse")));

  // Transpose: typed "5/4" (exact ratio), then the ¢ chip re-expresses it in cents, the same pitch.
  h = f.history();
  f.d.doubleClick(f.at("transpose"));
  REQUIRE(m.typingIn() == f.item("transpose"));
  f.d.type("5/4");  // the entry opens selected: typing replaces it
  f.d.press("Enter");
  REQUIRE(f.get(beat).transpose.unit == doc::TransposeUnit::Ratio);
  CHECK(f.get(beat).transpose.ratio == tuning::Rational(5, 4));
  CHECK(f.history() == h + 1);
  CHECK(m.valueTextOf(f.item("transpose")).find("5/4") != std::string::npos);
  f.d.click(f.at("unit/\xC2\xA2"));
  CHECK(f.get(beat).transpose.unit == doc::TransposeUnit::Cents);
  CHECK(f.get(beat).transpose.cents == doctest::Approx(386.3137).epsilon(1e-4));
  CHECK(f.history() == h + 2);
  // In steps: a knob drag lands on a degree of the tuning.
  f.d.click(f.at("unit/stp"));
  CHECK(f.get(beat).transpose.unit == doc::TransposeUnit::Steps);
  h = f.history();
  const Vec2 tk = f.at("transpose");
  f.d.drag(tk, {tk.x, tk.y - 30.0f}, 5);
  CHECK(f.get(beat).transpose.unit == doc::TransposeUnit::Steps);
  CHECK(f.get(beat).transpose.steps > 4);
  CHECK(f.history() == h + 1);

  // Warp: Beats (the BPM seeded as the panel does); its settings row appears directly above the modes: Preserve 1/8.
  h = f.history();
  f.d.move(f.at("warp-stretch/Tones"));  // hovering a mode opens nothing
  CHECK_FALSE(m.shown(f.item("tones-grain")));
  f.d.click(f.at("warp-stretch/Beats"));
  CHECK(f.get(beat).warp == doc::AudioWarp::Beats);
  CHECK(f.history() == h + 1);
  CHECK(m.enabledOf(f.item("bpm")));
  CHECK(m.shown(f.item("beats-env")));
  CHECK_FALSE(m.shown(f.item("flux")));
  CHECK(f.at("preserve/1/8").y < f.at("warp-stretch/Beats").y);
  CHECK(f.at("transient-loop/No Loop").y < f.at("warp-stretch/Beats").y);
  f.d.click(f.at("preserve/1/8"));
  CHECK(f.get(beat).warpParams.beatsGranulation == kPpq / 2);
  CHECK(f.history() == h + 2);
  // :2 halves the source tempo; Loop turns the loop on (both on the lower band, no sub-ring).
  const double bpm = f.get(beat).sourceBpm;
  f.d.click(f.at("tempo/:2"));
  CHECK(f.get(beat).sourceBpm == doctest::Approx(bpm / 2.0));
  f.d.click(f.at("loop"));
  CHECK(f.get(beat).loop.enabled);
  CHECK(f.history() == h + 4);
  // Texture: its row swaps in (Grain, Flux); a slider drag there is one step; Complex: no row.
  f.d.click(f.at("warp-stretch/Texture"));
  CHECK(f.get(beat).warp == doc::AudioWarp::Texture);
  CHECK_FALSE(m.shown(f.item("preserve/1/8")));
  REQUIRE(m.shown(f.item("flux")));
  CHECK(m.shown(f.item("texture-grain")));
  h = f.history();
  const int32_t fx = f.item("flux");
  const Vec2 s0 = m.sliderStart(fx), s1 = m.sliderEnd(fx);
  f.d.drag({s0.x + 1.0f, s0.y}, {s0.x + 0.5f * (s1.x - s0.x), s0.y}, 5);
  CHECK(f.get(beat).warpParams.flux > 10.0f);
  CHECK(f.history() == h + 1);
  f.d.click(f.at("warp-stretch/Complex"));
  for (const char* k : {"preserve/1/8", "beats-env", "tones-grain", "flux", "texture-grain", "formants"}) CHECK_FALSE(m.shown(f.item(k)));
  f.checkAround(beat);

  // The document changes under the menu (another view's edit): the values follow; undo brings Texture's row back.
  f.dispatch(doc::ops::setAudioClipDetune(*f.ctx.doc(), std::span<const doc::ClipRef>(&beat, 1), -12.0f));
  CHECK(m.valueOf(f.item("detune")) == doctest::Approx(-12.0));
  CHECK(f.ctx.store().undo());
  CHECK(f.ctx.store().undo());
  CHECK(f.get(beat).warp == doc::AudioWarp::Texture);
  CHECK(m.shown(f.item("flux")));

  // The clip deleted under it: the menu closes (no dangling pointer); undo brings the clip back, the menu stays shut.
  REQUIRE(f.arr->deleteSelection());
  f.d.step();
  CHECK(f.arr->clipMenu() == nullptr);
  CHECK(f.find(beat) == nullptr);
  CHECK(f.ctx.store().undo());
  CHECK(f.find(beat) != nullptr);
  f.d.step();

  // Re-Pitch (Vox): transpose and detune disabled, as in the panel.
  f.d.click(f.title(vox, kBar / 4));
  f.d.click(f.button(vox));
  f.settle();
  RadialMenu& mv = f.menu();
  CHECK_FALSE(mv.enabledOf(f.item("transpose")));
  CHECK_FALSE(mv.enabledOf(f.item("detune")));
  CHECK_FALSE(mv.enabledOf(f.item("unit/stp")));
  CHECK(mv.enabledOf(f.item("bpm")));
  h = f.history();
  const Vec2 vt = f.at("transpose");
  f.d.drag(vt, {vt.x, vt.y - 30.0f}, 4);
  CHECK(f.history() == h);
  // A click outside closes it.
  f.d.click({5.0f, 5.0f});
  f.d.step();
  CHECK(f.arr->clipMenu() == nullptr);
}

TEST_CASE("clip menu (SX.5): clips near the window's top / bottom and narrow clips: on screen, never over the clip") {
  SUBCASE("the first track at the top of a short window") {
    Fixture f(1280, 520);
    const doc::ClipRef beat = f.clip("Drums", 0);
    f.d.click(f.title(beat, kBar / 4));
    REQUIRE(f.arr->openClipMenu(beat, false) != nullptr);
    f.d.tree().layoutNow();
    f.d.step();
    f.checkAround(beat);
  }
  SUBCASE("the last audio track, scrolled to the bottom of a short window") {
    Fixture f(1280, 420);
    const doc::ClipRef vox = f.clip("Vox", 0);
    f.arr->scrollToY(1 << 20);
    f.d.tree().layoutNow();
    f.d.step();
    f.d.click(f.title(vox, kBar / 4));
    REQUIRE(f.arr->openClipMenu(vox, false) != nullptr);
    f.d.tree().layoutNow();
    f.d.step();
    f.checkAround(vox);
  }
  SUBCASE("a narrow clip (zoomed out): the bands are wider than it, on screen") {
    Fixture f;
    const doc::ClipRef beat = f.clip("Drums", 0);
    f.d.click(f.title(beat, kBar / 4));
    f.arr->setTimeView(-0.05 * static_cast<double>(kBar), 24.0 / static_cast<double>(kBar));
    f.d.tree().layoutNow();
    f.d.step();
    f.d.tree().setFocus(f.arr);
    REQUIRE(f.d.press("Ctrl+Space") == KeyResult::Action);  // no button on a narrow clip: the shortcut
    f.d.step();
    f.checkAround(beat);
    const IRect box = f.arr->clipRect(beat);
    const IRect band = f.menu().bandRect(1);
    CHECK(band.w > box.w);
    CHECK(band.x >= 0);
  }
}

// ---- parity: every clip panel control has a menu counterpart making the identical clip -------------------------------

namespace {

const RadialItem* findItem(const std::vector<RadialItem>& items, std::string_view key) {
  for (const RadialItem& it : items) {
    if (it.key == key) return &it;
    if (const RadialItem* c = findItem(it.chips_, key)) return c;
    if (const RadialItem* c = findItem(it.children, key)) return c;
  }
  return nullptr;
}

void collectControls(Widget& w, const std::string& path, std::vector<std::pair<std::string, Widget*>>& out) {
  for (size_t i = 0; i < w.childCount(); ++i) {
    Widget& c = w.child(i);
    const std::string p = path.empty() ? c.name() : path + "/" + c.name();
    if (dynamic_cast<Control*>(&c) != nullptr) out.emplace_back(p, &c);
    else collectControls(c, p, out);
  }
}

// One control: the warp mode it needs, the panel's action, the menu item's key and value.
struct Case {
  doc::AudioWarp warp = doc::AudioWarp::Off;
  std::function<void(AudioClipView&, Widget&)> panel;  // default: a chip's press / a value control's gesture to `value`
  std::string key;
  double value = 0.0;
  std::string typed;  // the transpose: a typed entry on both
};

}  // namespace

TEST_CASE("clip menu (SX.2 / SX.5): parity — every control of the clip panel but Gain and the fades has a menu counterpart "
          "that makes the identical clip through the same op") {
  Fixture f(1280, 900);
  const doc::ClipRef beat = f.clip("Drums", 0);
  f.ctx.setDetailVisible(true);
  f.ctx.setDetailClip(beat);
  f.d.tree().layoutNow();
  AudioClipView* vp = findAudioClipView(*f.shell);
  REQUIRE(vp != nullptr);
  AudioClipView& v = *vp;
  REQUIRE(v.hasClip());
  const std::vector<RadialItem> items = clipMenuItems(f.ctx, beat);
  for (const char* k : {"gain", "fade-in", "fade-out"}) CHECK(findItem(items, k) == nullptr);

  using W = doc::AudioWarp;
  std::map<std::string, Case> cases;
  cases["clip/transpose"] = {W::Off, [](AudioClipView& view, Widget&) { (void)view.setTranspose(std::string_view("5/4")); }, "transpose", 0.0, "5/4"};
  cases["clip/detune"] = {W::Off, {}, "detune", 12.0};
  cases["clip/side/reverse"] = {W::Off, {}, "reverse", 1.0};
  // SX.5: not in the menu: the clip's own handles edit gain and the fades.
  const std::set<std::string> excluded = {"clip/gain", "clip/side/fade-in", "clip/side/fade-out"};
  const char* modes[] = {"off", "repitch", "beats", "tones", "texture", "complex", "pro"};
  for (int i = 0; i < 7; ++i)
    cases[std::string(i < 4 ? "warp/modes-1/" : "warp/modes-2/") + modes[i]] = {i == 0 ? W::Beats : W::Off, {}, "warp", static_cast<double>(i)};
  cases["warp/tempo/bpm"] = {W::Beats, {}, "bpm", 96.0};
  cases["warp/tempo/halve"] = {W::Beats, {}, "tempo", 0.0};
  cases["warp/tempo/double"] = {W::Beats, {}, "tempo", 1.0};
  cases["warp/tempo/loop"] = {W::Beats, {}, "loop", 1.0};
  const char* preserve[] = {"transients", "1-4", "1-8", "1-16", "1-32"};
  for (int i = 0; i < 5; ++i) cases[std::string("mode/beats/preserve/") + preserve[i]] = {W::Beats, {}, "preserve", static_cast<double>(i)};
  const char* loops[] = {"off", "forward", "back-fwd"};
  for (int i = 0; i < 3; ++i) cases[std::string("mode/beats/beats-loop/") + loops[i]] = {W::Beats, {}, "transient-loop", static_cast<double>(i)};
  cases["mode/beats-env"] = {W::Beats, {}, "beats-env", 40.0};
  cases["mode/grain"] = {W::Tones, {}, "tones-grain", 80.0};
  cases["mode/flux"] = {W::Texture, {}, "flux", 35.0};
  cases["mode/formants"] = {W::ComplexPro, {}, "formants", 60.0};
  cases["mode/env-order"] = {W::ComplexPro, {}, "env-order", 64.0};
  // The Texture grain is the panel's same knob (shown for Tones and Texture): the menu has one per mode.
  struct Extra {
    std::string panel;
    W warp;
    std::string key;
    double value;
  };
  const Extra extras[] = {{"mode/grain", W::Texture, "texture-grain", 120.0}};

  std::vector<std::pair<std::string, Widget*>> controls;
  Widget* props = v.box().find("props");
  REQUIRE(props != nullptr);
  collectControls(*props, "", controls);
  CHECK(controls.size() == cases.size() + excluded.size());

  auto setWarp = [&](W w) {
    const doc::AudioClip& c = f.get(beat);
    if (c.warp != w) f.dispatch(doc::ops::setAudioClipWarp(*f.ctx.doc(), std::span<const doc::ClipRef>(&beat, 1), w, 120.0));
  };
  auto runMenu = [&](const std::string& key, double value, const std::string& typed) {
    const RadialItem* it = findItem(items, key);
    REQUIRE_MESSAGE(it != nullptr, "no menu control " << key);
    REQUIRE(it->binding != nullptr);
    const RadialBinding& b = *it->binding;
    if (b.enabled) REQUIRE_MESSAGE(b.enabled(), key << " disabled");
    if (!typed.empty()) {
      REQUIRE(b.parse);
      const std::optional<double> pv = b.parse(typed);
      REQUIRE(pv.has_value());
      value = *pv;
    }
    if (b.begin) b.begin();
    b.set(value);
    if (b.end) b.end();
  };
  auto runPanel = [&](const Case& c, Widget& w) {
    if (c.panel) {
      c.panel(v, w);
      return;
    }
    if (auto* chip = dynamic_cast<audio_clip::Chip*>(&w)) {
      REQUIRE(chip->onPress);
      chip->onPress();
      return;
    }
    auto* vc = dynamic_cast<ValueControl*>(&w);
    REQUIRE_MESSAGE(vc != nullptr, "not a chip or value control");
    REQUIRE(vc->onChange);
    if (vc->onGestureBegin) vc->onGestureBegin();
    vc->onChange(c.value);
    if (vc->onGestureEnd) vc->onGestureEnd();
  };
  auto check = [&](const std::string& path, Widget& w, const Case& c) {
    CAPTURE(path);
    setWarp(c.warp);
    const doc::AudioClip start = f.get(beat);
    const size_t h = f.history();
    runPanel(c, w);
    const doc::AudioClip byPanel = f.get(beat);
    const size_t hp = f.history();
    while (f.history() > h) REQUIRE(f.ctx.store().undo());
    REQUIRE(f.get(beat) == start);
    runMenu(c.key, c.value, c.typed);
    CHECK_MESSAGE(f.get(beat) == byPanel, path << " vs menu " << c.key);
    CHECK(f.history() == hp);  // the same number of undo steps (one)
    while (f.history() > h) REQUIRE(f.ctx.store().undo());
  };
  for (const auto& [path, w] : controls) {
    if (excluded.count(path) != 0) continue;
    const auto it = cases.find(path);
    if (it == cases.end()) {
      FAIL_CHECK("the clip panel's control " << path << " has no clip menu counterpart in this test");
      continue;
    }
    check(path, *w, it->second);
  }
  for (const Extra& e : extras) {
    Widget* w = props->find(e.panel);
    REQUIRE(w != nullptr);
    check(e.panel + " (" + e.key + ")", *w, Case{e.warp, {}, e.key, e.value});
  }
}

// ---- goldens ---------------------------------------------------------------------------------------------------------

namespace {

void checkGolden(const char* scene) {
  uint32_t w = 0, h = 0;
  viewSceneDefaultSize(scene, w, h);
  GpuOptions o;
  o.validation = true;
  auto img = renderViewScene(scene, views::test::fixtureDir("audio"), w, h, {}, o);
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  const std::string path = std::string(PQ_TEST_DATA_DIR) + "/views/golden/" + scene + ".png";
  auto ref = readPng(path);
  if (!ref) (void)writePng(std::string(scene) + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  ImageDiff d;
  CHECK(pq::ui::test::matchesGolden(scene, *img, *ref, true, &d));
  MESSAGE(std::string(scene) << ": " << pq::ui::test::describe(d));
}

}  // namespace

TEST_CASE("render: clip-menu-off (the clip menu on an Off clip) matches the golden") { checkGolden("clip-menu-off"); }

TEST_CASE("render: clip-menu-beats (a Beats clip, its settings row above the modes) matches the golden") { checkGolden("clip-menu-beats"); }
