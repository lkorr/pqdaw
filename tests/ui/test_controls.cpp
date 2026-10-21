// T3.2: theme file (load, round trip, named errors, contrast, hot reload), default keymap file, and scripted
// TestDriver tests of the base controls: drag box (coarse / fine / type-in / reset), fader, knob, text edit
// (caret, selection, word moves, clipboard, Enter / Esc), dropdown, context-menu keyboard navigation, popover
// dismiss, tooltip, scrollbar, meter damage, info text; the `widgets` golden at scale 1 and 1.5.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/ui/actions.h"
#include "pq/ui/controls.h"
#include "pq/ui/image.h"
#include "pq/ui/keymap_file.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/overlay.h"
#include "pq/ui/scenes.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/widget.h"

#include "golden_check.h"

using namespace pq::ui;

namespace {

std::string readFile(const std::string& path) {
  std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string stripCr(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
  return s;
}

// A root (Stack) holding `content` and the overlay layer on top, like an app root.
struct Fixture {
  TestDriver d;
  Widget* root = nullptr;
  Widget* content = nullptr;
  OverlayLayer* overlay = nullptr;
  explicit Fixture(uint32_t w = 400, uint32_t h = 300, float scale = 1.0f) : d(w, h, scale) {
    root = &d.setRoot<Widget>();
    content = &root->add<Widget>("content");
    content->setLayout(LayoutKind::Column).setPadding(Insets::all(10)).setGap(10);
    overlay = &root->add<OverlayLayer>("overlay");
    d.tree().layoutNow();
  }
  template <class W, class... A>
  W& add(float w, float h, A&&... args) {
    W& x = content->add<W>(std::forward<A>(args)...);
    x.setSize(Length::fixed(w), Length::fixed(h));
    d.tree().layoutNow();
    return x;
  }
};

Vec2 centre(const IRect& r) { return {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)}; }

struct GestureLog {
  int begins = 0, ends = 0, changes = 0;
  double last = 0;
  void attach(ValueControl& c) {
    c.onGestureBegin = [this] { ++begins; };
    c.onGestureEnd = [this] { ++ends; };
    c.onChange = [this](double v) {
      ++changes;
      last = v;
    };
  }
};

ValueSpec spec0to100(double def = 50) {
  ValueSpec s;
  s.min = 0;
  s.max = 100;
  s.def = def;
  s.dragPixels = 200;
  s.fineFactor = 0.1;
  s.decimals = 1;
  return s;
}

}  // namespace

// --- theme ----------------------------------------------------------------------------------------------

TEST_CASE("theme: the default file loads with every token and the full clip palette") {
  auto t = loadTheme(defaultThemePath());
  REQUIRE_MESSAGE(t.ok(), (t.ok() ? "" : t.error().message));
  CHECK(t->clipPalette.size() == kClipPaletteSize);
  CHECK(t->fonts.small.px == 12);
  CHECK(t->fonts.body.px == 13);
  CHECK(t->fonts.title.px == 14);
  CHECK(alphaOf(t->surface.panel) == 255);
  CHECK(themeTokenNames().size() == 1 + 57 + 16 + 20 + 5 + 14 + 1);  // + arrangement.* (T3.8), mosChart.* (T3.13)
}

TEST_CASE("theme: the file round-trips (struct and bytes)") {
  const std::string text = stripCr(readFile(defaultThemePath()));
  auto a = parseTheme(text);
  REQUIRE(a.ok());
  const std::string out = serializeTheme(*a);
  CHECK(out == text);  // the committed file is in canonical form
  auto b = parseTheme(out);
  REQUIRE(b.ok());
  CHECK(*a == *b);
  // A modified theme survives too (alpha colours, fractional metrics, tabular flags).
  Theme m = *a;
  m.surface.panel = rgba(1, 2, 3, 4);
  m.spacing.md = 7.25f;
  m.fonts.body.tabular = true;
  m.fonts.title.face = ThemeFace::Regular;
  auto c = parseTheme(serializeTheme(m));
  REQUIRE(c.ok());
  CHECK(*c == m);
}

TEST_CASE("theme: a missing, unknown or malformed token fails with its name") {
  const std::string text = stripCr(readFile(defaultThemePath()));
  // Every token in turn: removing its line (or entry) must fail naming it.
  auto removeLine = [&](const std::string& needle) {
    const size_t at = text.find(needle);
    REQUIRE(at != std::string::npos);
    const size_t b = text.rfind('\n', at) + 1, e = text.find('\n', at);
    std::string s = text.substr(0, b) + text.substr(e + 1);
    // Fix a dangling comma before a closing brace.
    for (const char* close : {",\n  }", ",\n  ]"}) {
      size_t p;
      while ((p = s.find(close)) != std::string::npos) s.erase(p, 1);
    }
    return s;
  };
  int checked = 0;
  for (std::string_view tok : themeTokenNames()) {
    const std::string name(tok);
    if (name == "name" || name == "clipPalette") continue;
    const std::string leaf = name.substr(name.find('.') + 1);
    auto r = parseTheme(removeLine("\"" + leaf + "\":"));
    REQUIRE_FALSE(r.ok());
    CHECK_MESSAGE(r.error().message.find("missing token \"" + name + "\"") != std::string::npos, r.error().message);
    ++checked;
  }
  CHECK(checked == 57 + 16 + 20 + 5 + 14);  // + the 16 arrangement.* colours (T3.8), 20 mosChart.* (T3.13)
  {
    auto r = parseTheme(removeLine("\"name\":"));
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("missing token \"name\"") != std::string::npos);
  }
  {
    std::string s = text;
    s.replace(s.find("\"surface.panel\""), 15, "\"surface.panelx\"");
    auto r = parseTheme(s);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("colors.surface.panel") != std::string::npos);
  }
  {
    std::string s = text;
    const size_t at = s.find("\"text.disabled\": \"#") + 18;
    s.replace(at, 7, "#80808Z");
    auto r = parseTheme(s);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("\"colors.text.disabled\"") != std::string::npos);
  }
  {
    std::string s = text;
    s.insert(s.find("\"colors\""), "\"extra\": 1,\n  ");
    auto r = parseTheme(s);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("unknown token \"extra\"") != std::string::npos);
  }
  {
    std::string s = text;
    s.insert(s.find("\"surface.panel\""), "\"surface.panel\": \"#000000\",\n    ");
    auto r = parseTheme(s);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("duplicate key \"surface.panel\"") != std::string::npos);
  }
  {
    auto r = parseTheme("{ not json");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.rfind("theme: invalid JSON", 0) == 0);
  }
  {
    auto r = loadTheme("does/not/exist.json");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("does/not/exist.json") != std::string::npos);
  }
}

// M0 review M10: contrast targets computed from the theme file. Panel steps ≥ 1.15:1, small text ≥ 4.5:1,
// disabled text ≥ 3:1.
TEST_CASE("theme: contrast ratios meet the M10 targets") {
  auto th = loadTheme(defaultThemePath());
  REQUIRE(th.ok());
  const Theme& t = *th;
  struct Pair {
    const char* what;
    Rgba fg, bg;
    double min;
  };
  const Pair pairs[] = {
      {"surface: panel vs background", t.surface.panel, t.surface.background, 1.15},
      {"surface: raised vs panel", t.surface.raised, t.surface.panel, 1.15},
      {"surface: panel vs sunken", t.surface.panel, t.surface.sunken, 1.15},
      {"track: lane vs background", t.track.lane, t.surface.background, 1.15},
      {"text.primary on panel", t.text.primary, t.surface.panel, 4.5},
      {"text.primary on raised", t.text.primary, t.surface.raised, 4.5},
      {"text.primary on control.fill", t.text.primary, t.control.fill, 4.5},
      {"text.primary on control.fillHover", t.text.primary, t.control.fillHover, 4.5},
      {"text.primary on control.fillPressed", t.text.primary, t.control.fillPressed, 4.5},
      {"text.primary on field.fill", t.text.primary, t.field.fill, 4.5},
      {"text.primary on menu.fill", t.text.primary, t.menu.fill, 4.5},
      {"text.primary on track.header", t.text.primary, t.track.header, 4.5},
      {"text.secondary on panel", t.text.secondary, t.surface.panel, 4.5},
      {"text.secondary on raised", t.text.secondary, t.surface.raised, 4.5},
      {"text.secondary on menu.fill", t.text.secondary, t.menu.fill, 4.5},
      {"text.secondary on field.fill", t.text.secondary, t.field.fill, 4.5},
      {"text.disabled on panel", t.text.disabled, t.surface.panel, 3.0},
      {"text.disabled on control.fillDisabled", t.text.disabled, t.control.fillDisabled, 3.0},
      {"text.disabled on menu.fill", t.text.disabled, t.menu.fill, 3.0},
      {"text.onAccent on accent.primary", t.text.onAccent, t.accent.primary, 4.5},
      {"text.onAccent on accent.activator", t.text.onAccent, t.accent.activator, 4.5},
      {"text.onAccent on accent.solo", t.text.onAccent, t.accent.solo, 4.5},
      {"text.onAccent on accent.arm", t.text.onAccent, t.accent.arm, 4.5},
      {"selection.text on selection.fill", t.selection.text, t.selection.fill, 4.5},
      {"text.primary on text.selection", t.text.primary, t.text.selection, 4.5},
      {"tooltip.text on tooltip.fill", t.tooltip.text, t.tooltip.fill, 4.5},
      {"accent.focus vs control.fill (focus ring, non-text 3:1)", t.accent.focus, t.control.fill, 3.0},
      {"accent.focus vs field.fill", t.accent.focus, t.field.fill, 3.0},
  };
  std::string table;
  for (const Pair& p : pairs) {
    const double r = contrastRatio(p.fg, p.bg);
    char line[160];
    std::snprintf(line, sizeof line, "  %-58s %6.2f:1 (min %.2f)\n", p.what, r, p.min);
    table += line;
    CHECK_MESSAGE(r >= p.min, p.what << ": " << r);
  }
  // A clip name must be readable on every palette colour, in the colour clipTextFor() picks for it.
  double worst = 99;
  uint32_t worstAt = 0, light = 0;
  for (uint32_t i = 0; i < kClipPaletteSize; ++i) {
    const Rgba ink = clipTextFor(t, t.clipPalette[i]);
    light += ink == t.clip.textLight;
    const double r = contrastRatio(ink, t.clipPalette[i]);
    if (r < worst) {
      worst = r;
      worstAt = i;
    }
  }
  char line[200];
  std::snprintf(line, sizeof line, "  clip name on its palette colour (worst: #%u)                %6.2f:1 (min 4.50; %u of %u take light text)\n",
                worstAt, worst, light, kClipPaletteSize);
  table += line;
  MESSAGE("contrast ratios:\n" << table);
  CHECK(worst >= 4.5);
  CHECK(light == kClipPaletteColumns);  // the dark row
}

TEST_CASE("theme: contrast helpers match WCAG reference values") {
  CHECK(contrastRatio(rgba(0, 0, 0), rgba(255, 255, 255)) == doctest::Approx(21.0));
  CHECK(contrastRatio(rgba(255, 255, 255), rgba(255, 255, 255)) == doctest::Approx(1.0));
  CHECK(contrastRatio(hex(0x777777), hex(0xFFFFFF)) == doctest::Approx(4.48).epsilon(0.01));
  CHECK(compositeOver(rgba(255, 255, 255, 128), rgba(0, 0, 0)) == rgba(128, 128, 128));
}

TEST_CASE("theme: the file watcher reloads a changed file, keeps the theme on a bad one, and idles otherwise") {
  namespace fs = std::filesystem;
  const fs::path p = fs::temp_directory_path() / "pq_t32_theme_watch.json";
  const std::string good = stripCr(readFile(defaultThemePath()));
  auto write = [&](const std::string& s, int secondsAhead) {
    {
      std::ofstream f(p, std::ios::binary | std::ios::trunc);
      f << s;
    }
    fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(secondsAhead));
  };
  write(good, 0);
  const std::u8string u8 = p.u8string();
  ThemeFileWatcher w(std::string(u8.begin(), u8.end()), 0.5);
  Theme t;
  std::string err;
  CHECK(w.poll(0.0, t, err) == ThemeFileWatcher::Poll::Unchanged);
  CHECK(w.poll(0.2, t, err) == ThemeFileWatcher::Poll::NotDue);  // at most one query per interval
  std::string changed = good;
  changed.replace(changed.find("\"surface.panel\": \"#") + 18, 7, "#303030");
  write(changed, 2);
  CHECK(w.poll(0.6, t, err) == ThemeFileWatcher::Poll::Reloaded);
  CHECK(t.surface.panel == hex(0x303030));
  CHECK(w.poll(1.2, t, err) == ThemeFileWatcher::Poll::Unchanged);
  write("{ broken", 4);
  CHECK(w.poll(1.8, t, err) == ThemeFileWatcher::Poll::Failed);
  CHECK(err.find("invalid JSON") != std::string::npos);
  CHECK(t.surface.panel == hex(0x303030));  // untouched
  CHECK(w.poll(2.4, t, err) == ThemeFileWatcher::Poll::Unchanged);  // reported once per change
  fs::remove(p);
}

// --- keymap ---------------------------------------------------------------------------------------------

TEST_CASE("keymap: the default file parses, chords are unique per scope, actions resolve or are deferred") {
  auto k = loadKeymapFile(defaultKeymapPath());
  REQUIRE_MESSAGE(k.ok(), (k.ok() ? "" : k.error().message));
  CHECK(k->entries.size() >= 50);
  std::set<std::pair<std::string, std::string>> seen;
  for (const KeymapEntry& e : k->entries) {
    auto c = parseChord(e.keys);  // every chord parses (also checked by the reader)
    REQUIRE_MESSAGE(c.ok(), e.keys);
    CHECK(*c == e.chord);
    CHECK_MESSAGE(seen.insert({e.scope, formatChord(e.chord)}).second, "duplicate " << e.keys << " in " << e.scope);
    CHECK((e.status == "implemented" || e.deferred()));
  }
  // SPEC §6.1's examples are all there.
  auto has = [&](std::string_view scope, std::string_view keys, std::string_view action) {
    return std::any_of(k->entries.begin(), k->entries.end(),
                       [&](const KeymapEntry& e) { return e.scope == scope && e.keys == keys && e.action == action; });
  };
  CHECK(has("global", "Space", "transport.playStop"));
  CHECK(has("global", "Shift+Space", "transport.continue"));
  CHECK(has("global", "F9", "transport.record"));
  CHECK(has("global", "Ctrl+T", "track.newMidi"));
  CHECK(has("global", "Ctrl+Shift+T", "track.newAudio"));
  CHECK(has("arrangement", "Ctrl+G", "track.group"));
  CHECK(has("arrangement", "Ctrl+R", "arrangement.rename"));
  CHECK(has("arrangement", "0", "arrangement.toggleActive"));
  CHECK(has("arrangement", "Ctrl+E", "arrangement.split"));
  CHECK(has("arrangement", "Ctrl+J", "arrangement.consolidate"));
  CHECK(has("arrangement", "Ctrl+D", "arrangement.duplicate"));
  CHECK(has("arrangement", "Ctrl+L", "arrangement.loopSelection"));
  for (const char* g : {"Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+4", "Ctrl+5"})
    CHECK(std::any_of(k->entries.begin(), k->entries.end(), [&](const KeymapEntry& e) { return e.keys == g; }));
  CHECK(has("global", "B", "edit.toggleDrawMode"));
  CHECK(has("arrangement", "A", "arrangement.toggleAutomationMode"));
  // T3.8: Z / X act on the focused editor, so they live in its scope (the piano roll binds its own).
  CHECK(has("arrangement", "Z", "arrangement.zoomToSelection"));
  CHECK(has("arrangement", "X", "arrangement.zoomBack"));
  CHECK(has("global", "Ctrl+Alt+B", "view.toggleBrowser"));
  CHECK(has("global", "Shift+Tab", "view.toggleClipDevice"));
  CHECK(has("arrangement", "Ctrl+Shift+M", "arrangement.insertMidiClip"));
  CHECK(has("global", "M", "keyboard.toggleComputerMidi"));
  CHECK(has("pianoroll", "Up", "pianoroll.stepUp"));
  CHECK(has("pianoroll", "Shift+Up", "pianoroll.equaveUp"));

  // Applying it: since T3.7 the global shell actions are implemented, so an empty registry fails (naming one) and
  // a registry with exactly the implemented ids binds exactly the implemented entries (the rest stay deferred).
  ActionRegistry none;
  Keymap km;
  CHECK_FALSE(applyKeymapFile(*k, none, km).ok());
  ActionRegistry some;
  size_t implemented = 0;
  for (const KeymapEntry& e : k->entries) {
    if (e.deferred()) continue;
    ++implemented;
    if (some.find(e.action) == kNoAction) REQUIRE(some.add(e.action, [] {}).ok());
  }
  CHECK(implemented >= 11);
  Keymap km2;
  REQUIRE(applyKeymapFile(*k, some, km2).ok());
  CHECK(km2.bindings().size() == implemented);
}

TEST_CASE("keymap: implemented entries must resolve; bad files fail with a precise error") {
  const std::string ok = R"({"name": "t", "bindings": [
    {"scope": "global", "keys": "Ctrl+D", "action": "edit.duplicate", "status": "implemented"},
    {"scope": "arrangement", "keys": "Ctrl+D", "action": "arrangement.duplicate", "status": "deferred:M3", "task": "T3.9"}]})";
  auto k = parseKeymapFile(ok);
  REQUIRE(k.ok());
  ActionRegistry reg;
  int ran = 0;
  REQUIRE(reg.add("edit.duplicate", [&] { ++ran; }).ok());
  Keymap km;
  REQUIRE(applyKeymapFile(*k, reg, km).ok());
  REQUIRE(km.bindings().size() == 1);  // the deferred one is not bound
  CHECK(km.lookup(kGlobalScope, *parseChord("Ctrl+D")) == "edit.duplicate");
  ActionRegistry empty;
  Keymap km2;
  auto bad = applyKeymapFile(*k, empty, km2);
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().message.find("\"edit.duplicate\"") != std::string::npos);

  auto fails = [](const std::string& s, const char* needle) {
    auto r = parseKeymapFile(s);
    REQUIRE_FALSE(r.ok());
    CHECK_MESSAGE(r.error().message.find(needle) != std::string::npos, r.error().message);
  };
  fails(R"({"name": "t", "bindings": [{"scope": "g", "keys": "Ctrl+Hyper+D", "action": "a", "status": "implemented"}]})",
        "bindings[0]: chord \"Ctrl+Hyper+D\"");
  fails(R"({"name": "t", "bindings": [{"scope": "g", "keys": "Ctrl+D", "action": "a", "status": "implemented"},
         {"scope": "g", "keys": "ctrl+d", "action": "b", "status": "implemented"}]})",
        "bindings[1]: chord \"Ctrl+D\" is bound twice in scope \"g\"");
  fails(R"({"name": "t", "bindings": [{"scope": "g", "keys": "D", "action": "a", "status": "later"}]})", "status \"later\"");
  fails(R"({"name": "t", "bindings": [{"scope": "g", "keys": "D", "action": "a", "status": "deferred:3"}]})", "status \"deferred:3\"");
  fails(R"({"name": "t", "bindings": [{"scope": "g", "keys": "Shift", "action": "a", "status": "implemented"}]})", "bindings[0]");
  fails(R"({"name": "t", "bindings": [{"scope": "g", "keys": "D", "action": "a", "status": "implemented", "x": 1}]})", "unknown key \"x\"");
  fails(R"({"name": "t", "bindings": [{"scope": "g", "keys": "D", "status": "implemented"}]})", "\"action\" must be a non-empty string");
}

TEST_CASE("keymap: docs/HOTKEYS.md lists every binding") {
  auto k = loadKeymapFile(defaultKeymapPath());
  REQUIRE(k.ok());
  const std::string doc = readFile(std::string(PQ_TEST_DATA_DIR) + "/../docs/HOTKEYS.md");
  REQUIRE(!doc.empty());
  for (const KeymapEntry& e : k->entries) {
    // Each row: | `action` | `keys` | scope | ... — matched by action, keys and scope on one line.
    bool found = false;
    size_t pos = 0;
    const std::string a = "`" + e.action + "`", keys = "`" + e.keys + "`";
    while ((pos = doc.find(a, pos)) != std::string::npos) {
      const size_t b = doc.rfind('\n', pos) + 1, en = doc.find('\n', pos);
      const std::string row = doc.substr(b, en - b);
      if (row.find(keys) != std::string::npos && row.find("| " + e.scope + " |") != std::string::npos) found = true;
      pos = en;
    }
    CHECK_MESSAGE(found, e.action << " " << e.keys << " (" << e.scope << ") missing from docs/HOTKEYS.md");
  }
}

// --- drag box -------------------------------------------------------------------------------------------

TEST_CASE("drag box: coarse drag, Shift fine, one gesture per drag") {
  Fixture f;
  auto& box = f.add<DragBox>(64, 18, "box", spec0to100());
  GestureLog g;
  g.attach(box);
  const Vec2 c = centre(box.rect());
  f.d.drag(c, {c.x, c.y - 20}, 5);  // 20 px up × (100 / 200 px) = +10
  CHECK(box.value() == doctest::Approx(60.0));
  CHECK(box.valueText() == "60.0");
  CHECK(g.begins == 1);
  CHECK(g.ends == 1);
  CHECK(g.changes == 5);
  f.d.drag(c, {c.x, c.y - 20}, 4, mod::kShift);  // fine: × 0.1
  CHECK(box.value() == doctest::Approx(61.0));
  f.d.drag(c, {c.x, c.y + 400}, 4);  // past the minimum: clamps...
  CHECK(box.value() == 0.0);
  // ...and the accumulator was clamped too: turning back responds immediately (Live).
  f.d.drag(c, {c.x, c.y - 2}, 1);
  CHECK(box.value() == doctest::Approx(1.0));
  CHECK(g.begins == 4);
  CHECK(g.ends == 4);
  // Shift pressed mid-drag changes the rate without a jump.
  f.d.move(c);
  f.d.down(c);
  f.d.move({c.x, c.y - 10});
  f.d.move({c.x, c.y - 20}, mod::kShift);
  f.d.up({c.x, c.y - 20});
  CHECK(box.value() == doctest::Approx(1.0 + 5.0 + 0.5));
  // A click without movement is not a gesture.
  const int before = g.begins;
  f.d.click(c);
  CHECK(g.begins == before);
}

TEST_CASE("drag box: quantised steps and keys") {
  Fixture f;
  ValueSpec s = spec0to100(10);
  s.step = 1;
  auto& box = f.add<DragBox>(64, 18, "box", s);
  GestureLog g;
  g.attach(box);
  const Vec2 c = centre(box.rect());
  f.d.drag(c, {c.x, c.y - 3}, 3);  // +1.5 → snaps to 12 (accumulates unquantised)
  CHECK(box.value() == 12.0);
  f.d.tree().setFocus(&box);
  f.d.press("Up");
  CHECK(box.value() == 13.0);
  f.d.press("Shift+Down");  // fine, but never below one step
  CHECK(box.value() == 12.0);
  const int ends = g.ends;
  f.d.press("Delete");  // Live: Delete returns to the default
  CHECK(box.value() == 10.0);
  CHECK(g.ends == ends + 1);
}

TEST_CASE("drag box: double-click type-in commits with Enter, cancels with Esc, rejects junk") {
  Fixture f;
  ValueSpec s = spec0to100(50);
  s.unit = "Hz";
  auto& box = f.add<DragBox>(64, 18, "box", s);
  GestureLog g;
  g.attach(box);
  const Vec2 c = centre(box.rect());
  f.d.doubleClick(c);
  REQUIRE(box.editing());
  CHECK(box.editBuffer().text() == "50.0");  // the number, no unit
  CHECK(box.editBuffer().selBegin() == 0);
  CHECK(box.editBuffer().selEnd() == 4);
  CHECK(g.begins == 0);  // the first click of the double-click changed nothing
  CHECK(f.d.press("B") == KeyResult::TextSwallowed);  // printable keys stay in the field
  f.d.type("42.5 hz");
  f.d.press("Enter");
  CHECK_FALSE(box.editing());
  CHECK(box.value() == doctest::Approx(42.5));
  CHECK(box.valueText() == "42.5 Hz");
  CHECK(g.begins == 1);
  CHECK(g.ends == 1);

  f.d.doubleClick(c);
  f.d.type("7");
  f.d.press("Escape");
  CHECK_FALSE(box.editing());
  CHECK(box.value() == doctest::Approx(42.5));

  f.d.doubleClick(c);
  f.d.type("abc");
  f.d.press("Enter");
  CHECK(box.value() == doctest::Approx(42.5));  // unparsable: reverted
  f.d.doubleClick(c);
  f.d.type("250");
  f.d.press("Enter");
  CHECK(box.value() == 100.0);  // clamped

  // Focus loss commits (clicking elsewhere).
  f.d.doubleClick(c);
  f.d.type("12");
  f.d.click({390, 290});
  CHECK_FALSE(box.editing());
  CHECK(box.value() == doctest::Approx(12.0));
  // Enter starts a type-in from the keyboard.
  f.d.tree().setFocus(&box, true);
  f.d.press("Enter");
  CHECK(box.editing());
  f.d.press("Escape");
}

TEST_CASE("drag box: a drag frame allocates nothing") {
  Fixture f;
  auto& box = f.add<DragBox>(64, 18, "box", spec0to100());
  box.onChange = [](double) {};
  const Vec2 c = centre(box.rect());
  f.d.move(c);
  f.d.down(c);
  f.d.move({c.x, c.y - 1});
  float y = c.y - 1;
  const uint64_t n = pq::alloc_trap::countRtViolations([&] {
    for (int i = 0; i < 50; ++i) {
      y -= 1;
      PointerEvent e;
      e.type = PointerType::Move;
      e.pos = {c.x, y};
      f.d.tree().dispatchPointer(e);
    }
  });
  CHECK(n == 0);
  f.d.up({c.x, y});
  CHECK(box.value() == doctest::Approx(75.5));
}

// --- fader / knob ---------------------------------------------------------------------------------------

TEST_CASE("fader: track press jumps, thumb drag is relative, Shift fine, Delete resets") {
  Fixture f;
  ValueSpec s;
  s.min = 0;
  s.max = 1;
  s.def = 0.75;
  auto& fd = f.add<Fader>(22, 108, "fader", s, Fader::Orientation::Vertical);
  GestureLog g;
  g.attach(fd);
  const IRect r = fd.rect();
  CHECK(fd.value() == 0.75);
  // Travel = 108 − 8 px thumb = 100 px. Press at the top of the track: jumps to the top (1.0).
  const Vec2 top{static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + 2)};
  f.d.move(top);
  f.d.down(top);
  CHECK(fd.value() == doctest::Approx(1.0));
  f.d.move({top.x, top.y + 50});  // relative: 50 px of 100 = −0.5
  f.d.up({top.x, top.y + 50});
  CHECK(fd.value() == doctest::Approx(0.5));
  CHECK(g.begins == 1);
  CHECK(g.ends == 1);
  // Drag the thumb itself: no jump.
  const Vec2 t = centre(fd.thumbRect());
  f.d.drag(t, {t.x, t.y - 20}, 4);
  CHECK(fd.value() == doctest::Approx(0.7));
  f.d.drag(centre(fd.thumbRect()), {t.x, t.y - 120}, 4, mod::kShift);  // fine
  CHECK(fd.value() == doctest::Approx(0.8));
  f.d.tree().setFocus(&fd);
  f.d.press("Delete");
  CHECK(fd.value() == 0.75);
  // Horizontal: right = more.
  auto& h = f.add<Fader>(108, 14, "h", s, Fader::Orientation::Horizontal);
  h.setValue(0.0);
  const Vec2 ht = centre(h.thumbRect());
  f.d.drag(ht, {ht.x + 25, ht.y}, 5);
  CHECK(h.value() == doctest::Approx(0.25));
}

TEST_CASE("knob: vertical drag over dragPixels sweeps the range; modulation ring hook repaints") {
  Fixture f;
  ValueSpec s;
  s.min = -50;
  s.max = 50;
  s.def = 0;
  s.dragPixels = 100;
  auto& k = f.add<Knob>(40, 40, "knob", s);
  GestureLog g;
  g.attach(k);
  const Vec2 c = centre(k.rect());
  f.d.drag(c, {c.x, c.y - 30}, 3);  // 30 px of 100 → +30
  CHECK(k.value() == doctest::Approx(30.0));
  f.d.drag(c, {c.x + 300, c.y}, 3);  // horizontal movement does nothing
  CHECK(k.value() == doctest::Approx(30.0));
  f.d.drag(c, {c.x, c.y + 50}, 5, mod::kShift);
  CHECK(k.value() == doctest::Approx(25.0));
  CHECK(g.begins == 2);  // the horizontal drag changed nothing: no gesture
  f.d.tree().setFocus(&k);
  f.d.press("Backspace");
  CHECK(k.value() == 0.0);
  f.d.step();
  f.d.tree().damage().clear();
  k.setModulation({true, 0.2f, 0.6f, 0.4f});
  CHECK(f.d.tree().needsPaint());
  f.d.step();
  k.setModulation({true, 0.2f, 0.6f, 0.4f});  // unchanged: no damage
  CHECK_FALSE(f.d.tree().needsPaint());
}

// --- buttons / toggles ----------------------------------------------------------------------------------

TEST_CASE("button and toggle: click semantics, keyboard, disabled, focus ring only after keyboard focus") {
  Fixture f;
  auto& b = f.add<Button>(72, 18, "b", "Freeze");
  int clicks = 0;
  b.onClick = [&] { ++clicks; };
  const Vec2 c = centre(b.rect());
  f.d.click(c);
  CHECK(clicks == 1);
  f.d.move(c);
  f.d.down(c);
  CHECK(b.visualState().pressed);
  f.d.move({c.x + 200, c.y});  // dragged off: not pressed, release does nothing
  CHECK_FALSE(b.visualState().pressed);
  f.d.up({c.x + 200, c.y});
  CHECK(clicks == 1);
  CHECK_FALSE(b.visualState().focus);  // pointer focus: no ring
  f.d.tree().setFocus(&b, true);
  CHECK(b.visualState().focus);
  f.d.press("Space");
  CHECK(clicks == 2);
  b.setEnabled(false);
  f.d.click(c);
  CHECK(clicks == 2);
  CHECK(b.visualState().disabled);

  auto& t = f.add<Toggle>(18, 18, "solo", "S", ToggleStyle::Solo);
  bool last = false;
  int toggles = 0;
  t.onToggle = [&](bool on) {
    last = on;
    ++toggles;
  };
  const Vec2 tc = centre(t.rect());
  f.d.move(tc);
  f.d.down(tc);  // Live switches on press
  CHECK(t.on());
  CHECK(last);
  f.d.up(tc);
  f.d.click(tc);
  CHECK_FALSE(t.on());
  CHECK(toggles == 2);
  t.setOn(true);  // programmatic: no callback
  CHECK(toggles == 2);
}

// --- text edit ------------------------------------------------------------------------------------------

TEST_CASE("text edit: caret moves, selection, words, clipboard, Enter commits, Esc reverts") {
  Fixture f;
  auto& te = f.add<TextEdit>(160, 18, "name", "Bass Track 2");
  std::vector<std::string> commits;
  int cancels = 0, changes = 0;
  te.onCommit = [&](const std::string& s) { commits.push_back(s); };
  te.onCancel = [&] { ++cancels; };
  te.onChange = [&](const std::string&) { ++changes; };
  f.d.tree().setFocus(&te, true);  // Tab-like focus selects all
  const TextBuffer& b = te.buffer();
  CHECK(b.selBegin() == 0);
  CHECK(b.selEnd() == 12);
  f.d.press("End");
  CHECK(b.caret() == 12);
  CHECK_FALSE(b.hasSelection());
  f.d.press("Ctrl+Left");
  CHECK(b.caret() == 11);  // before "2"
  f.d.press("Ctrl+Left");
  CHECK(b.caret() == 5);  // before "Track"
  f.d.press("Ctrl+Shift+Right");
  CHECK(b.selected() == "Track ");
  f.d.press("Shift+Left");
  CHECK(b.selected() == "Track");
  f.d.press("Ctrl+C");
  CHECK(f.d.tree().clipboard().text() == "Track");
  f.d.type("Lead");  // typing replaces the selection
  CHECK(te.text() == "Bass Lead 2");
  f.d.press("Home");
  f.d.press("Ctrl+V");
  CHECK(te.text() == "TrackBass Lead 2");
  f.d.press("Ctrl+Backspace");  // nothing before the caret... caret is after "Track": deletes the word
  CHECK(te.text() == "Bass Lead 2");
  f.d.press("Ctrl+Delete");
  CHECK(te.text() == "Lead 2");
  f.d.press("Delete");
  CHECK(te.text() == "ead 2");
  f.d.press("Ctrl+A");
  f.d.press("Ctrl+X");
  CHECK(te.text().empty());
  CHECK(f.d.tree().clipboard().text() == "ead 2");
  f.d.type("Pad");
  // Printable keys are claimed by the field: a "B" chord never reaches a keymap.
  CHECK(f.d.press("B") == KeyResult::TextSwallowed);
  f.d.press("Enter");
  REQUIRE(commits.size() == 1);
  CHECK(commits[0] == "Pad");
  f.d.type("dle");
  CHECK(te.text() == "Paddle");
  f.d.press("Escape");  // reverts to the last commit
  CHECK(te.text() == "Pad");
  CHECK(cancels == 1);
  CHECK(changes > 5);
  // UTF-8: the caret steps over whole code points.
  te.setText("a\xC2\xA2" "b");  // a¢b
  f.d.press("End");
  f.d.press("Left");
  CHECK(te.buffer().caret() == 3);
  f.d.press("Left");
  CHECK(te.buffer().caret() == 1);
  f.d.press("Delete");
  CHECK(te.text() == "ab");
  // Leaving the field commits.
  f.d.type("c");
  f.d.tree().setFocus(nullptr);
  REQUIRE(commits.size() == 2);
  CHECK(commits[1] == "acb");
}

TEST_CASE("text edit: clicks place the caret through TextRun hit testing; double-click selects a word") {
  Fixture f;
  REQUIRE(f.d.enableGpu().ok());
  auto& te = f.add<TextEdit>(200, 20, "name", "Bass Track");
  f.d.step();
  const PaintResources& res = f.d.resources();
  REQUIRE(res.uiFonts);
  const TextRun run = res.text->shape(res.uiFonts->body, "Bass Track");
  const int32_t pad = f.d.tree().uiScale().px(res.theme->spacing.sm);
  const float y = centre(te.rect()).y;
  const float xAfterBass = static_cast<float>(te.rect().x + pad + run.caretX(4));
  f.d.click({xAfterBass + 0.4f, y});
  CHECK(te.focused());
  CHECK(te.buffer().caret() == 4);
  f.d.drag({xAfterBass, y}, {static_cast<float>(te.rect().x + pad + run.caretX(10)) + 3, y}, 3);
  CHECK(te.buffer().selected() == " Track");
  f.d.doubleClick({static_cast<float>(te.rect().x + pad + run.caretX(1)), y});
  CHECK(te.buffer().selected() == "Bass");
  // Shift+click extends.
  f.d.click({static_cast<float>(te.rect().x + pad + run.caretX(7)), y}, mod::kShift);
  CHECK(te.buffer().selected() == "Bass Tr");
}

TEST_CASE("text edit: the caret blinks on a timer only while focused") {
  Fixture f;
  auto& te = f.add<TextEdit>(120, 18, "t", "x");
  f.d.advance(1.0);
  const uint64_t idle = f.d.framesDrawn();
  f.d.advance(2.0);
  CHECK(f.d.framesDrawn() == idle);  // unfocused: nothing
  f.d.tree().setFocus(&te);
  f.d.advance(2.0);
  const uint64_t blinking = f.d.framesDrawn() - idle;
  CHECK(blinking >= 3);
  CHECK(blinking <= 5);  // ≈ 2 / 0.53 s, never per frame
  f.d.tree().setFocus(nullptr);
  f.d.advance(0.1);
  const uint64_t after = f.d.framesDrawn();
  f.d.advance(2.0);
  CHECK(f.d.framesDrawn() == after);
}

// --- menus ----------------------------------------------------------------------------------------------

TEST_CASE("context menu: keyboard navigation through separators, disabled items and a nested submenu") {
  Fixture f;
  auto& host = f.add<Widget>(300, 200, "host");
  ActionRegistry reg;
  int ranD = 0, ranS2 = 0, ranA = 0;
  REQUIRE(reg.add("test.d", [&] { ++ranD; }).ok());
  bool eEnabled = false;
  REQUIRE(reg.add("test.e", [] {}, [&] { return eEnabled; }).ok());
  Keymap km;
  f.d.tree().setKeymap(&reg, &km);
  auto items = [&] {
    std::vector<MenuItem> m = {MenuItem::item("A", [&] { ++ranA; }), MenuItem::sep(), MenuItem::item("B"), MenuItem::item("C"),
                               MenuItem::sub("Sub", {MenuItem::item("S1"), MenuItem::item("S2", [&] { ++ranS2; }),
                                                     MenuItem::sub("Deeper", {MenuItem::item("X")})}),
                               MenuItem::item("D", {}, "Ctrl+D"), MenuItem::item("E")};
    m[2].enabled = false;
    m[5].action = "test.d";
    m[6].action = "test.e";  // disabled action: greyed, skipped
    return m;
  };
  Menu* m = openContextMenu(host, {50, 50}, items());
  REQUIRE(m);
  f.d.tree().layoutNow();
  CHECK(f.overlay->openCount() == 1);
  CHECK(f.d.tree().focus() == m);
  CHECK(m->rect().x == 50);
  CHECK(m->rect().y == 50);
  CHECK(m->highlighted() == -1);
  f.d.press("Down");
  CHECK(m->highlighted() == 0);
  f.d.press("Down");
  CHECK(m->highlighted() == 3);  // skipped the separator and disabled B
  f.d.press("Down");
  CHECK(m->highlighted() == 4);
  f.d.press("Right");  // opens Sub, focus moves in, first item highlighted
  Menu* sub = m->submenu();
  REQUIRE(sub);
  CHECK(f.overlay->openCount() == 2);
  CHECK(f.d.tree().focus() == sub);
  CHECK(sub->highlighted() == 0);
  CHECK(sub->rect().x == m->rect().right());  // to the right of the parent
  f.d.press("Down");
  f.d.press("Down");
  f.d.press("Right");  // a third level
  REQUIRE(sub->submenu());
  CHECK(f.overlay->openCount() == 3);
  f.d.press("Left");  // closes one level
  CHECK(f.overlay->openCount() == 2);
  CHECK(f.d.tree().focus() == sub);
  f.d.press("Up");
  CHECK(sub->highlighted() == 1);
  f.d.press("Escape");  // Esc in a submenu closes only it
  CHECK(f.overlay->openCount() == 1);
  CHECK(f.d.tree().focus() == m);
  CHECK(m->highlighted() == 4);
  f.d.press("Down");
  CHECK(m->highlighted() == 5);
  f.d.press("Down");  // E is disabled by its action's predicate: wraps to A
  CHECK(m->highlighted() == 0);
  f.d.press("End");
  CHECK(m->highlighted() == 5);
  f.d.press("Home");
  CHECK(m->highlighted() == 0);
  CHECK(f.d.press("Ctrl+D") == KeyResult::Widget);  // modal: chords do not reach keymaps underneath
  f.d.press("End");
  f.d.press("Enter");  // D: runs the action and closes the menu
  CHECK(ranD == 1);
  CHECK(f.overlay->openCount() == 0);

  // Enter on a submenu item opens it; Enter on a leaf inside closes the whole chain.
  m = openContextMenu(host, {50, 50}, items());
  f.d.tree().layoutNow();
  f.d.press("Down");
  f.d.press("Down");
  f.d.press("Down");
  f.d.press("Enter");
  REQUIRE(m->submenu());
  f.d.press("Down");
  f.d.press("Space");
  CHECK(ranS2 == 1);
  CHECK(f.overlay->openCount() == 0);
  f.d.step();  // the closed menus are destroyed at the next frame

  // Esc on the root menu closes it; the disabled action, once enabled, becomes selectable.
  eEnabled = true;
  m = openContextMenu(host, {50, 50}, items());
  f.d.tree().layoutNow();
  f.d.press("End");
  CHECK(m->highlighted() == 6);
  f.d.press("Escape");
  CHECK(f.overlay->openCount() == 0);
  CHECK(ranA == 0);
}

TEST_CASE("context menu: mouse hover opens a submenu after a rest; a click chooses; outside press dismisses") {
  Fixture f;
  auto& host = f.add<Widget>(300, 220, "host");
  int chosen = 0;
  Menu* m = openContextMenu(host, {20, 20}, {MenuItem::item("One", [&] { ++chosen; }), MenuItem::sub("More", {MenuItem::item("Two", [&] { chosen += 10; })})});
  REQUIRE(m);
  f.d.tree().layoutNow();
  f.d.move(centre(m->itemRect(1)));
  CHECK(m->highlighted() == 1);
  CHECK_FALSE(m->submenu());
  f.d.advance(0.3);  // hover rest
  REQUIRE(m->submenu());
  Menu* sub = m->submenu();
  f.d.move(centre(sub->itemRect(0)));
  f.d.click(centre(sub->itemRect(0)));
  CHECK(chosen == 10);
  CHECK(f.overlay->openCount() == 0);

  m = openContextMenu(host, {20, 20}, {MenuItem::item("One", [&] { ++chosen; })});
  f.d.tree().layoutNow();
  f.d.click({380, 280});  // outside: closes, nothing chosen
  CHECK(f.overlay->openCount() == 0);
  CHECK(chosen == 10);
  // Near the right/bottom edges the menu flips to stay on screen.
  m = openContextMenu(host, {395, 295}, {MenuItem::item("One"), MenuItem::item("Two")});
  f.d.tree().layoutNow();
  CHECK(m->rect().right() <= 400);
  CHECK(m->rect().bottom() <= 300);
  f.d.press("Escape");
}

TEST_CASE("dropdown: click opens a menu below it, keyboard chooses; Up/Down change it directly") {
  Fixture f;
  auto& dd = f.add<Dropdown>(120, 18, "grid", std::vector<std::string>{"Adaptive", "1/16", "1/8", "Off"}, 0);
  std::vector<int32_t> changes;
  dd.onChange = [&](int32_t i) { changes.push_back(i); };
  f.d.click(centre(dd.rect()));
  REQUIRE(dd.menuOpen());
  auto* menu = dynamic_cast<Menu*>(f.overlay->top());
  REQUIRE(menu);
  f.d.tree().layoutNow();
  CHECK(menu->rect().y == dd.rect().bottom());
  CHECK(menu->rect().w >= dd.rect().w);
  CHECK(menu->highlighted() == 0);
  CHECK(menu->items()[0].checked);
  f.d.press("Down");
  f.d.press("Down");
  f.d.press("Enter");
  CHECK_FALSE(dd.menuOpen());
  CHECK(dd.index() == 2);
  REQUIRE(changes.size() == 1);
  CHECK(f.d.tree().focus() == &dd);  // focus returns to the dropdown
  f.d.press("Up");
  CHECK(dd.index() == 1);
  f.d.press("Up");
  f.d.press("Up");
  CHECK(dd.index() == 0);
  CHECK(changes.size() == 3);
  // Destroying the dropdown while its menu is open closes the menu.
  f.d.press("Enter");
  REQUIRE(dd.menuOpen());
  f.content->removeChild(dd);
  CHECK(f.overlay->openCount() == 0);
}

// --- popover / tooltip ----------------------------------------------------------------------------------

TEST_CASE("popover: anchored below, presses inside keep it, a press outside or Esc dismisses it") {
  Fixture f;
  auto& anchor = f.add<Button>(80, 18, "anchor", "Tuning");
  auto& under = f.add<Button>(80, 18, "under", "Under");
  int underClicks = 0;
  under.onClick = [&] { ++underClicks; };
  auto& field = f.add<TextEdit>(80, 18, "field", "x");
  int closed = 0;
  auto open = [&] {
    auto p = std::make_unique<Popover>("pop");
    p->setSize(Length::fixed(150), Length::fixed(80));
    p->add<Toggle>("snap", "Snap").setSize(Length::fixed(40), Length::fixed(18));
    OverlayOptions o;
    o.anchor = anchor.rect();
    o.onClosed = [&] { ++closed; };
    return &f.overlay->open(std::move(p), std::move(o));
  };
  f.d.tree().setFocus(&field);
  Widget* pop = open();
  f.d.tree().layoutNow();
  CHECK(pop->rect().x == anchor.rect().x);
  CHECK(pop->rect().y == anchor.rect().bottom());
  CHECK(f.d.tree().focus() == pop);
  // Inside (empty area and a child control): stays open.
  f.d.click({static_cast<float>(pop->rect().right() - 5), static_cast<float>(pop->rect().bottom() - 5)});
  auto* snap = dynamic_cast<Toggle*>(pop->find("snap"));
  REQUIRE(snap);
  f.d.click(centre(snap->rect()));
  CHECK(snap->on());
  CHECK(f.overlay->isOpen(*pop));
  // Outside, over another button: dismissed, and the press does not reach the button.
  f.d.click({390, 290});
  CHECK(closed == 1);
  CHECK(f.overlay->openCount() == 0);
  CHECK(f.d.tree().focus() == &field);  // focus restored
  CHECK(underClicks == 0);

  pop = open();
  f.d.tree().layoutNow();
  f.d.press("Escape");
  CHECK(closed == 2);
  CHECK(f.overlay->openCount() == 0);
  // Esc with focus on a control inside the popover also dismisses (bubbles to the popover).
  pop = open();
  f.d.tree().layoutNow();
  f.d.tree().setFocus(pop->find("snap"));
  f.d.press("Escape");
  CHECK(closed == 3);
  f.d.step();
}

TEST_CASE("tooltip: shown after the hover delay, hidden on move away or press") {
  Fixture f;
  auto& b = f.add<Button>(80, 18, "b", "Solo");
  b.setTooltip("Solo this track");
  auto& other = f.add<Button>(80, 18, "o", "Other");
  f.d.move(centre(b.rect()));
  f.d.advance(0.3);
  CHECK_FALSE(f.overlay->tooltipVisible());
  f.d.advance(0.4);
  CHECK(f.overlay->tooltipVisible());
  f.d.move(centre(other.rect()));
  CHECK_FALSE(f.overlay->tooltipVisible());
  f.d.move(centre(b.rect()));
  f.d.advance(0.7);
  CHECK(f.overlay->tooltipVisible());
  f.d.down(centre(b.rect()));
  CHECK_FALSE(f.overlay->tooltipVisible());
  f.d.up(centre(b.rect()));
  f.d.advance(2.0);
  CHECK_FALSE(f.overlay->tooltipVisible());  // no re-show without a new hover
}

TEST_CASE("info text: a control's info, inherited by children that have none") {
  Fixture f;
  auto& k = f.add<Knob>(40, 40, "k", ValueSpec{});
  k.setInfo("Track Pan", "Pans the track.");
  CHECK(infoFor(&k).title == "Track Pan");
  auto& group = f.add<Widget>(100, 30, "group");
  auto& inner = group.add<Label>("inner", "x");
  CHECK(infoFor(&inner).empty());
  auto& nested = k.add<Label>("nested", "y");  // a child without info inherits the knob's
  CHECK(infoFor(&nested).title == "Track Pan");
  CHECK(infoFor(nullptr).empty());
  f.d.move(centre(k.rect()));
  CHECK(infoFor(f.d.tree().hovered()).body == "Pans the track.");
}

// --- scrollbar / meter ----------------------------------------------------------------------------------

namespace {
class Tall : public Widget {
public:
  Tall() : Widget("content") { setSize(Length::fixed(50), Length::fixed(400)); }
};
}  // namespace

TEST_CASE("scrollbar: thumb follows the view, drags scroll it, track presses page") {
  Fixture f;
  auto& row = f.add<Widget>(70, 100, "row");
  row.setLayout(LayoutKind::Row);
  auto& view = row.add<ScrollView>("view", false, true);
  view.add<Tall>();
  auto& sb = row.add<Scrollbar>("bar", view);
  sb.setWidth(Length::fixed(10));
  f.d.tree().layoutNow();
  IRect t = sb.thumbRect();
  CHECK(t.h == 25);  // 100 / 400 of the 100 px track
  CHECK(t.y == sb.rect().y);
  view.scrollTo(0, 150);  // half way
  f.d.tree().layoutNow();
  t = sb.thumbRect();
  CHECK(t.y == sb.rect().y + 37);  // 75 px travel × 150 / 300
  f.d.drag(centre(t), {centre(t).x, centre(t).y + 20}, 4);
  CHECK(view.scrollY() == 228);  // thumb at 37 + 20 px of 75 px travel × 300 px of scroll
  f.d.click({centre(t).x, static_cast<float>(sb.rect().y + 2)});  // above the thumb: one page (view − 10 %) up
  CHECK(view.scrollY() == 228 - 90);
  // Wheel scrolling the view repaints the scrollbar.
  f.d.step();
  f.d.wheel(centre(view.rect()), -1);
  CHECK(f.d.tree().needsPaint());
}

TEST_CASE("meter: repaints only when a drawn pixel changes") {
  Fixture f;
  auto& m = f.add<Meter>(11, 103, "m");
  MeterLevels l;
  l.rmsDb[0] = -20, l.rmsDb[1] = -22, l.peakDb[0] = -12, l.peakDb[1] = -13;
  m.setLevels(l);
  f.d.step();
  CHECK_FALSE(f.d.tree().needsPaint());
  l.rmsDb[0] = -20.01f;  // well under a pixel (100 px for 60 dB)
  m.setLevels(l);
  CHECK_FALSE(f.d.tree().needsPaint());
  l.rmsDb[0] = -19.0f;
  m.setLevels(l);
  CHECK(f.d.tree().needsPaint());
  f.d.step();
  l.clip[1] = true;
  m.setLevels(l);
  CHECK(f.d.tree().needsPaint());
  int resets = 0;
  m.onResetClip = [&] { ++resets; };
  f.d.click({static_cast<float>(m.rect().x + 2), static_cast<float>(m.rect().y + 1)});
  CHECK(resets == 1);
}

// --- gallery --------------------------------------------------------------------------------------------

TEST_CASE("widgets gallery: idle draws nothing; every control kind is present") {
  TestDriver d(1040, 720);
  buildWidgetsGallery(d.tree());
  d.advance(0.5);
  const uint64_t frames = d.framesDrawn();
  d.advance(2.0);
  CHECK(d.framesDrawn() == frames);
  for (const char* p : {"body/left/states/label/c0/ctl", "body/left/states/button/c1/ctl", "body/left/states/toggles-on/c2/group/solo",
                        "body/left/states/dragbox/c3/ctl", "body/left/states/fader/c4/ctl", "body/left/states/knob/c0/ctl",
                        "body/left/states/textedit/c3/ctl", "body/left/states/dropdown/c0/ctl", "body/left/states/scrollbar/c0/group/bar",
                        "body/right/mixer/strip/meter-hot", "body/right/mixer/strip/volume", "body/left/bottom/colours/palette"})
    CHECK_MESSAGE(d.find(p) != nullptr, p);
  auto* overlay = dynamic_cast<OverlayLayer*>(d.find("overlay"));
  REQUIRE(overlay);
  CHECK(overlay->openCount() == 3);  // context menu, its submenu, the popover
  CHECK(overlay->tooltipVisible());
}

namespace {
void checkWidgetsGolden(float scale, const char* scaleName) {
  uint32_t w = 0, h = 0;
  sceneDefaultSize("widgets", w, h);
  const std::string file = scaleName ? std::string("widgets@") + scaleName + "x" : std::string("widgets");
  GpuOptions o;
  o.validation = true;
  auto img = renderSceneImage("widgets", w, h, o, UiScale(scale));
  REQUIRE_MESSAGE(img.ok(), (img.ok() ? "" : img.error().message));
  auto ref = readPng(std::string(PQ_TEST_DATA_DIR) + "/ui/golden/" + file + ".png");
  if (!ref) (void)writePng(file + ".actual.png", *img);
  REQUIRE_MESSAGE(ref.ok(), (ref.ok() ? "" : ref.error().message));
  // T0.7-fix5: the shared golden rule (tests/ui/golden_check.h).
  ImageDiff d;
  const bool pass = pq::ui::test::matchesGolden(file, *img, *ref, true, &d);
  REQUIRE(d.sameSize);
  MESSAGE(file << ": " << pq::ui::test::describe(d));
  CHECK(pass);
}
}  // namespace

TEST_CASE("render: widgets matches the golden image") { checkWidgetsGolden(1.0f, nullptr); }
TEST_CASE("render: widgets matches the golden image at UI scale 1.5") { checkWidgetsGolden(1.5f, "1.5"); }
