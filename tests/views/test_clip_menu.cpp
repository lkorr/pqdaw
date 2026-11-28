// SX.2 / SX.5 / SX.6: the clip popup (pq/views/audio_clip/clip_menu.h: a compact modeless popup in bands around the
// clip) on the audio fixture (tests/views/fixtures/audio). It is there whenever the selection is exactly one audio clip
// (real dispatch: window -> arrangement -> overlay -> menu), centred on the press, follows the clip (time scroll, its
// move), hides during a drag and comes back after, closes on any other selection or the clip deleted; presses beside it
// reach the arrangement and keys it does not use go there (Delete, the arrows), only a typed entry takes the keyboard;
// Esc dismisses it until the selection changes, Ctrl+Space toggles it. Its controls edit the clip, one undo step each
// (knob drags, typed transpose, unit chips, warp chips with the chosen mode's settings row, toggles, :2); disabled where
// the mode ignores a control; the document changing under it refreshes it; clips near the window's top / bottom and
// narrow clips keep it on screen and off the clip. Parity: every control of the clip panel (AudioClipView's cards) but
// Gain and the fades (the clip's own handles) has a menu counterpart that makes the identical clip. Goldens: the popup
// on an Off clip and on a Beats clip (its settings row).

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
  // Whether `p` is on the popup (its band panels): a press there is the popup's.
  bool onPopup(Vec2 p) {
    RadialMenu* m = arr->clipMenu();
    if (!m || !m->visible()) return false;
    for (int32_t b = 0; b < 2; ++b)
      if (m->bandRect(b).contains(p)) return true;
    return false;
  }
  // A point on clip `r`'s title strip in the lanes' view and off the popup (a press there is the arrangement's).
  Vec2 freeTitle(const doc::ClipRef& r) {
    const IRect lv = arr->lanesViewport();
    for (Ticks dt = kBar / 8; dt < get(r).length; dt += kBar / 8) {
      const Vec2 p = title(r, dt);
      if (p.x > static_cast<float>(lv.x) + 8.0f && p.x < static_cast<float>(lv.right()) - 8.0f && !onPopup(p)) return p;
    }
    FAIL("no free spot on the clip's title");
    return {};
  }
  // The centre x of the popup's band `b`.
  float bandCentre(int32_t b) {
    const IRect r = menu().bandRect(b);
    return 0.5f * static_cast<float>(r.x + r.right());
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

TEST_CASE("clip popup (SX.6, real dispatch): shown whenever one audio clip is selected; follows the clip, hides during a "
          "drag, closes on another selection; modeless; Esc / Ctrl+Space") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0), vox = f.clip("Vox", 0), keys = f.clip("Keys", 0);
  CHECK(f.arr->clipMenu() == nullptr);

  // A click on an audio clip selects it and the popup is there, centred on the press, never over the clip, on screen.
  const size_t h0 = f.history();
  const doc::Snapshot before = f.ctx.doc();
  const Vec2 p0 = f.title(beat, kBar / 4);
  f.d.click(p0);
  REQUIRE(f.arr->selection().clips.size() == 1);
  REQUIRE(f.arr->clipMenu() != nullptr);
  CHECK(f.arr->clipPopupShown());
  CHECK(f.ctx.doc() == before);
  CHECK(f.history() == h0);
  f.settle();
  RadialMenu& m = f.menu();
  CHECK(m.bands());
  f.checkAround(beat);
  for (int32_t k = 0; k < 2; ++k)  // centred on the press, unless clamped at the window's edge
    CHECK((std::abs(f.bandCentre(k) - p0.x) <= 1.0f || m.bandRect(k).x <= 5));
  CHECK(m.find("gain") < 0);  // the clip's handles edit gain and the fades
  CHECK(m.find("fade-in") < 0);
  CHECK(m.find("fade-out") < 0);
  CHECK_FALSE(m.focused());  // the keyboard stays with the arrangement
  // No button: the title's right end is a plain title / edge part.
  {
    const IRect box = f.arr->clipRect(beat);
    f.d.move({static_cast<float>(std::min(box.right(), f.arr->lanesViewport().right())) - 14.0f, static_cast<float>(box.y + 7)});
    CHECK(f.d.tree().cursor() != Cursor::Pointer);
    CHECK(f.arr->clipHover().zone != 6);
  }

  // A press beside it goes to the arrangement as if it were not there: on Vox's title it selects Vox, whose popup it is now.
  const Vec2 pv = f.freeTitle(vox);
  f.d.click(pv);
  REQUIRE(f.arr->selection().clips.size() == 1);
  CHECK(f.arr->selection().clips[0].clip == vox.clip);
  REQUIRE(f.arr->clipMenu() != nullptr);
  f.settle();
  f.checkAround(vox);
  for (int32_t k = 0; k < 2; ++k)
    CHECK((std::abs(f.bandCentre(k) - pv.x) <= 1.0f || f.menu().bandRect(k).x <= 5 ||
           f.menu().bandRect(k).right() >= f.d.root()->rect().right() - 5));

  // It follows the clip: scrolled in time, the bands move with it (still centred on the same place in the clip).
  {
    const float c0 = f.bandCentre(0), x0 = f.arr->xOfTick(static_cast<double>(kBar));
    const double origin = f.arr->originTick();
    f.arr->setTimeView(origin + 0.1 * static_cast<double>(kBar), kPxPerBar / static_cast<double>(kBar));
    f.d.step();
    const float dx = f.arr->xOfTick(static_cast<double>(kBar)) - x0;
    REQUIRE(dx < -20.0f);
    CHECK(f.bandCentre(0) == doctest::Approx(c0 + dx).epsilon(0.01));
    f.checkAround(vox);
    f.arr->setTimeView(origin, kPxPerBar / static_cast<double>(kBar));
    f.d.step();
  }

  // A drag hides it; on release it is back, around the moved clip.
  {
    const Ticks s0 = f.get(vox).start;
    const Vec2 a = pv, b{a.x - 0.5f * static_cast<float>(kPxPerBar), a.y};
    f.d.down(a);
    CHECK(f.arr->clipPopupShown());  // a press alone does not hide it
    f.d.move({a.x - 20.0f, a.y});
    f.d.move({a.x - 80.0f, a.y});
    CHECK(f.arr->clipMenu() != nullptr);
    CHECK_FALSE(f.arr->clipPopupShown());
    f.d.move(b);
    f.d.up(b);
    REQUIRE(f.get(vox).start < s0);
    REQUIRE(f.arr->clipMenu() != nullptr);
    CHECK(f.arr->clipPopupShown());
    f.d.step();
    f.checkAround(vox);
    REQUIRE(f.ctx.store().undo());
    f.d.step();
    f.checkAround(vox);  // undone: back where the clip is
  }

  // A press on the popup's panel is the popup's (the selection stays).
  {
    const IRect band = f.menu().bandRect(1);
    f.d.click({static_cast<float>(band.x) + 3.0f, static_cast<float>(band.y) + 3.0f});
    CHECK(f.arr->selection().clips.size() == 1);
    CHECK(f.arr->clipPopupShown());
  }

  // Keys it does not use reach the arrangement: Right nudges the clip (the popup follows), Delete deletes it (and the
  // popup closes); undo.
  {
    f.d.tree().setFocus(f.arr);
    const Ticks s0 = f.get(vox).start;
    CHECK(f.d.press("Right") == KeyResult::Action);
    CHECK(f.get(vox).start > s0);
    f.d.step();
    f.checkAround(vox);
    REQUIRE(f.ctx.store().undo());
    CHECK(f.d.press("Delete") == KeyResult::Action);
    CHECK(f.find(vox) == nullptr);
    f.d.step();
    CHECK(f.arr->clipMenu() == nullptr);
    REQUIRE(f.ctx.store().undo());
    f.d.step();
  }

  // Esc dismisses it (the selection stays); a second Esc deselects. Ctrl+Space toggles it.
  f.d.click(f.title(beat, kBar / 4));
  f.d.tree().setFocus(f.arr);
  REQUIRE(f.arr->clipPopupShown());
  CHECK(f.d.press("Escape") == KeyResult::Action);
  f.d.step();
  CHECK(f.arr->clipMenu() == nullptr);
  CHECK(f.arr->selection().clips.size() == 1);
  CHECK(f.d.press("Ctrl+Space") == KeyResult::Action);
  REQUIRE(f.arr->clipPopupShown());
  f.settle();
  f.checkAround(beat);
  CHECK(f.d.press("Ctrl+Space") == KeyResult::Action);
  f.d.step();
  CHECK(f.arr->clipMenu() == nullptr);
  CHECK(f.d.press("Ctrl+Space") == KeyResult::Action);
  CHECK(f.arr->clipPopupShown());
  CHECK(f.d.press("Escape") == KeyResult::Action);
  CHECK(f.d.press("Escape") == KeyResult::Action);
  CHECK(f.arr->selection().clips.empty());
  CHECK(f.arr->clipMenu() == nullptr);
  // A dismissal lasts until the selection changes: selected again, it is back.
  f.d.click(f.title(beat, kBar / 4));
  CHECK(f.arr->clipPopupShown());

  // A click on empty lane space: gone.
  {
    const IRect kb = f.arr->clipRect(keys);
    const Vec2 empty{f.arr->xOfTick(3.0 * static_cast<double>(kBar)), static_cast<float>(kb.y + kb.h / 2)};
    REQUIRE_FALSE(f.onPopup(empty));
    f.d.click(empty);
    CHECK(f.arr->selection().clips.empty());
    CHECK(f.arr->clipMenu() == nullptr);
  }

  // Several clips selected: none; one again (Ctrl-click takes the other off): back.
  f.d.click(f.title(beat, kBar / 4));
  REQUIRE(f.arr->clipMenu() != nullptr);
  const Vec2 pv2 = f.freeTitle(vox);
  f.d.click(pv2, mod::kCtrl);
  REQUIRE(f.arr->selection().clips.size() == 2);
  CHECK(f.arr->clipMenu() == nullptr);
  f.d.tree().setFocus(f.arr);
  CHECK(f.d.press("Ctrl+Space") != KeyResult::Action);
  CHECK(f.arr->clipMenu() == nullptr);
  f.d.click(pv2, mod::kCtrl);
  REQUIRE(f.arr->selection().clips.size() == 1);
  CHECK(f.arr->clipPopupShown());

  // A MIDI clip: none (and Ctrl+Space does nothing). (Vox's popup covers Keys' clip: Esc hides it first.)
  {
    CHECK(f.d.press("Escape") == KeyResult::Action);
    const IRect kb = f.arr->clipRect(keys);
    Vec2 pk{static_cast<float>(kb.x + 8), static_cast<float>(kb.y + 7)};
    while (f.onPopup(pk) && pk.x < static_cast<float>(kb.right() - 8)) pk.x += 16.0f;
    REQUIRE_FALSE(f.onPopup(pk));
    f.d.click(pk);
    REQUIRE(f.arr->selection().clips.size() == 1);
    CHECK(f.arr->clipMenu() == nullptr);
    CHECK(f.d.press("Ctrl+Space") != KeyResult::Action);
    CHECK(f.arr->clipMenu() == nullptr);
  }

  // The context menu has no "Clip Menu…" entry any more.
  {
    f.d.click(f.title(beat, kBar / 4));
    const IRect box = f.arr->clipRect(beat);
    f.d.click({f.arr->xOfTick(static_cast<double>(kBar) * 0.5), static_cast<float>(box.bottom() - 8)}, mod::kNone, PointerButton::Right);
    f.d.step();
    OverlayLayer* layer = OverlayLayer::of(*f.arr);
    REQUIRE(layer != nullptr);
    auto* cm = dynamic_cast<Menu*>(layer->top());
    REQUIRE(cm != nullptr);
    for (const MenuItem& it : cm->items()) CHECK(it.label.find("Clip Menu") == std::string::npos);
    f.d.press("Escape");
    f.d.step();
  }
}

TEST_CASE("clip popup (SX.6): a typed entry takes the keyboard and gives it back; a press elsewhere commits it") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0);
  f.d.click(f.title(beat, kBar / 4));
  f.settle();
  RadialMenu& m = f.menu();
  Widget* had = f.d.tree().focus();
  REQUIRE(had != nullptr);
  CHECK(had != &m);
  f.d.doubleClick(f.at("transpose"));
  REQUIRE(m.typingIn() == f.item("transpose"));
  CHECK(m.focused());
  f.d.type("5/4");
  CHECK(f.d.press("Enter") != KeyResult::Unhandled);
  CHECK(f.get(beat).transpose.ratio == tuning::Rational(5, 4));
  CHECK(m.typingIn() < 0);
  CHECK(f.d.tree().focus() == had);  // back to the arrangement: its keys work again
  // Opened again, then a press on the arrangement: the entry commits there, the press does what it does.
  f.d.doubleClick(f.at("transpose"));
  REQUIRE(m.typingIn() >= 0);
  f.d.type("3/2");
  const doc::ClipRef vox = f.clip("Vox", 0);
  const Vec2 pv = f.freeTitle(vox);
  f.d.click(pv);
  CHECK(f.get(beat).transpose.ratio == tuning::Rational(3, 2));
  CHECK(f.arr->selection().clips.size() == 1);
  CHECK(f.arr->selection().clips[0].clip == vox.clip);
}

TEST_CASE("clip menu (SX.2 / SX.5, real dispatch): its controls edit the clip, one undo step each; the chosen mode's "
          "settings row; disabled where the mode ignores them; refreshed under it; closed when the clip goes") {
  Fixture f;
  const doc::ClipRef beat = f.clip("Drums", 0), vox = f.clip("Vox", 0);
  f.d.click(f.title(beat, kBar / 4));
  f.settle();
  RadialMenu& m = f.menu();

  // Off: transpose and detune apply, the tempo controls do not; no settings row.
  CHECK(m.enabledOf(f.item("transpose")));
  CHECK(m.enabledOf(f.item("detune")));
  CHECK_FALSE(m.enabledOf(f.item("bpm")));
  CHECK_FALSE(m.enabledOf(f.item("loop")));
  CHECK_FALSE(m.enabledOf(f.item("tempo/:2")));
  for (const char* k : {"preserve/1/8", "beats-env", "tones-grain", "flux", "formants"}) CHECK_FALSE(m.shown(f.item(k)));
  // Above the clip: Warp and the six modes in one row (none lit: Off); below: Transpose, its units, Detune, BPM, :2 /
  // ×2, Loop. No Reverse (WB: R and the panel).
  const IRect box = f.arr->clipRect(beat);
  const float modeY = f.at("warped").y;
  CHECK_FALSE(m.litOf(f.item("warped")));
  CHECK(m.find("reverse") < 0);
  for (const char* k : {"warped", "warp/Re-Pitch", "warp-stretch/Beats", "warp-stretch/Tones", "warp-stretch/Texture",
                        "warp-stretch/Complex", "warp-stretch/Pro"}) {
    CHECK(f.at(k).y == doctest::Approx(modeY));
    CHECK(f.at(k).y < static_cast<float>(box.y));
  }
  float x = -1e9f;
  for (const char* k : {"transpose", "unit/stp", "unit/\xC2\xA2", "unit/n/d", "detune", "bpm", "tempo/:2", "loop"}) {
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

  // Warp (WB): the toggle warps the clip in the default mode from the shape it has (same span, same source frame at its
  // start, read at the speed it played: at the song's tempo the source BPM is that tempo), one step; again: Off.
  h = f.history();
  const doc::AudioClip unwarped = f.get(beat);
  f.d.click(f.at("warped"));
  REQUIRE(f.get(beat).warp == f.ctx.viewPrefs().defaultWarpMode);
  CHECK(m.litOf(f.item("warped")));
  CHECK(f.get(beat).start == unwarped.start);
  CHECK(f.get(beat).length == unwarped.length);
  CHECK(f.get(beat).markers.empty());
  CHECK(f.history() == h + 1);
  f.d.click(f.at("warped"));
  CHECK(f.get(beat).warp == doc::AudioWarp::Off);
  CHECK(f.get(beat).seconds == doctest::Approx(unwarped.seconds));
  CHECK(f.get(beat).contentStartFrame == doctest::Approx(unwarped.contentStartFrame));
  CHECK(f.history() == h + 2);

  // Warp: Beats (from Off, from its shape as the toggle); its settings row appears directly above the modes: Preserve 1/8.
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

  // The clip deleted under it: the menu closes (no dangling pointer); undo brings the clip back.
  REQUIRE(f.arr->deleteSelection());
  f.d.step();
  CHECK(f.arr->clipMenu() == nullptr);
  CHECK(f.find(beat) == nullptr);
  CHECK(f.ctx.store().undo());
  CHECK(f.find(beat) != nullptr);
  f.d.step();

  // Re-Pitch (Vox): transpose and detune disabled, as in the panel.
  f.d.click(f.freeTitle(vox));
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
  // A click outside the arrangement leaves it: the selection decides.
  f.d.click({5.0f, 5.0f});
  f.d.step();
  CHECK(f.arr->clipMenu() == &mv);
}

TEST_CASE("clip menu (SX.5): clips near the window's top / bottom and narrow clips: on screen, never over the clip") {
  SUBCASE("the first track at the top of a short window") {
    Fixture f(1280, 520);
    const doc::ClipRef beat = f.clip("Drums", 0);
    f.d.click(f.title(beat, kBar / 4));
    REQUIRE(f.arr->clipMenu() != nullptr);
    f.settle();
    f.checkAround(beat);
  }
  SUBCASE("an audio track scrolled to the bottom of a short window") {
    Fixture f(1280, 420);
    const doc::ClipRef vox = f.clip("Vox", 0);
    // RT.4: the rows under Vox are taller now (three header lines), so scrolling to the end would take it out of
    // view: scroll so its row ends at the bottom of the view.
    for (const ArrangementRow& row : f.arr->rows())
      if (row.id == vox.track) f.arr->scrollToY(row.y + row.h - f.arr->lanesViewport().h);
    f.d.tree().layoutNow();
    f.d.step();
    f.d.click(f.freeTitle(vox));
    REQUIRE(f.arr->clipMenu() != nullptr);
    f.settle();
    f.checkAround(vox);
  }
  SUBCASE("a narrow clip (zoomed out): the bands are wider than it, on screen") {
    Fixture f;
    const doc::ClipRef beat = f.clip("Drums", 0);
    f.d.click(f.title(beat, kBar / 4));
    f.arr->setTimeView(-0.05 * static_cast<double>(kBar), 24.0 / static_cast<double>(kBar));
    f.d.tree().layoutNow();
    f.d.step();
    REQUIRE(f.arr->clipPopupShown());  // it follows the zoom
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

TEST_CASE("clip menu (SX.2 / SX.5): parity — every control of the clip panel but Gain, the fades and Reverse has a menu counterpart "
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
  // SX.5: not in the menu: the clip's own handles edit gain and the fades; WB: Reverse (R) neither.
  const std::set<std::string> excluded = {"clip/gain", "clip/side/fade-in", "clip/side/fade-out", "clip/side/reverse"};
  const char* modes[] = {"warp", "repitch", "beats", "tones", "texture", "complex", "pro"};
  cases["warp/modes-1/warp"] = {W::Off, {}, "warped", 1.0};  // WB: the Warp toggle
  for (int i = 1; i < 7; ++i)
    cases[std::string(i < 4 ? "warp/modes-1/" : "warp/modes-2/") + modes[i]] = {W::Off, {}, "warp", static_cast<double>(i)};
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
