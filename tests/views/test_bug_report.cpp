// T5.x-bugreport: the F12 bug report's pure parts (pq/views/bug_report/): report.md compose / parse, the writer's
// folder layout and size budget, the recent-input ring's formatting, the pointer crop's clamping, closing a ticket (on
// disk and by the panel's x).
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "pq/ui/input.h"
#include "pq/ui/overlay.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/widget.h"
#include "pq/views/bug_report/bug_panel.h"
#include "pq/views/bug_report/bug_report.h"
#include "pq/views/bug_report/input_log.h"

using namespace pq;
using namespace pq::views::bugreport;
namespace fs = std::filesystem;

namespace {

std::string readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A UI-like 1920x1080 picture: flat panels, lines, some text-like noise.
ui::Image uiLike(uint32_t w = 1920, uint32_t h = 1080) {
  ui::Image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(size_t(w) * h * 4);
  uint32_t seed = 1;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* p = img.pixel(x, y);
      uint8_t v = (x / 96 + y / 54) % 2 ? 40 : 52;
      if (x % 96 == 0 || y % 54 == 0) v = 90;
      if (y % 54 > 10 && y % 54 < 20 && x % 96 > 8 && x % 96 < 70) {  // "text"
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<uint8_t>(seed >> 24);
      }
      p[0] = v;
      p[1] = static_cast<uint8_t>(v + 10);
      p[2] = static_cast<uint8_t>(v + 20);
      p[3] = 255;
    }
  return img;
}

}  // namespace

TEST_CASE("bug report: report.md parse / compose round trip and appended shots") {
  const std::string md =
      "# Bug report 2026-10-31 12:00:00\n\ntags: ui, arrangement\n\nThe clip title is cut.\nSecond line.\n\n## Screenshots\n\n"
      "### Shot 1 (1920x1080)\n\n![shot 1](shot_1.png)\n\n- pointer: (1, 2)\n\n## Context (at the first shot)\n\n- build: x\n\n"
      "## Progress\n\n- 2026-11-01: fixed in abc\n";
  const ReportMd m = parseReportMd(md);
  CHECK(m.tags == std::vector<std::string>{"ui", "arrangement"});
  CHECK(m.text == "The clip title is cut.\nSecond line.");
  CHECK(m.shotCount == 1);
  CHECK(m.tail.rfind("## Context", 0) == 0);
  CHECK(composeReportMd(m, m.text, m.tags, "") == md);
  // Appending: a new section after the old one, the description and tags rewritten, the rest verbatim.
  const std::string out = composeReportMd(m, "New text", std::vector<std::string>{"perf"}, shotSection(2, 800, 600, true, "- added: now"));
  const ReportMd m2 = parseReportMd(out);
  CHECK(m2.shotCount == 2);
  CHECK(m2.text == "New text");
  CHECK(m2.tags == std::vector<std::string>{"perf"});
  CHECK(m2.tail == m.tail);
  CHECK(out.find("![shot 2 at the pointer](shot_2_cursor.png)") != std::string::npos);
  CHECK(normalizeTag("  Piano Roll! ") == "piano-roll");
  CHECK(normalizeTag("###") == "");
}

TEST_CASE("bug report: the writer's folder, files and size budget") {
  const fs::path root = fs::temp_directory_path() / "pq_bug_report_test";
  std::error_code ec;
  fs::remove_all(root, ec);
  Draft d;
  d.text = "Something is off.";
  d.tags = {"UI", "ui", "mixer"};
  Shot s;
  s.image = uiLike();
  s.crop = cursorCrop(s.image, 1900, 10);
  s.context = "- pointer: (1900, 10)\n";
  d.shots.push_back(std::move(s));
  d.trailer = "## Context (at the first shot)\n\n- build: test\n";
  d.project = "{\"schemaVersion\": 8}";
  d.attachProject = true;
  auto r = writeReport(root, d, "2026-10-31_12-00-00", "2026-10-31 12:00:00");
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
  CHECK(r->dir == root / "2026-10-31_12-00-00");
  for (const char* f : {"report.md", "shot_1.png", "shot_1_cursor.png", "project.pqd"}) CHECK(fs::exists(r->dir / f));
  const std::string md = readAll(r->dir / "report.md");
  CHECK(md.rfind("# Bug report 2026-10-31 12:00:00\n\ntags: ui, mixer\n\nSomething is off.\n\n## Screenshots\n\n### Shot 1 (1920x1080)", 0) == 0);
  CHECK(md.find("## Context (at the first shot)") != std::string::npos);
  uint64_t total = 0;
  for (const auto& e : fs::directory_iterator(r->dir)) total += fs::file_size(e.path());
  MESSAGE("synthetic 1920x1080 report: " << total << " bytes");
  CHECK(total < 600u * 1024u);
  // The same stamp again gets its own folder.
  auto r2 = writeReport(root, d, "2026-10-31_12-00-00", "2026-10-31 12:00:00");
  REQUIRE(r2.ok());
  CHECK(r2->dir == root / "2026-10-31_12-00-00_2");
  // A bug without a picture is refused.
  Draft empty;
  empty.text = "x";
  CHECK_FALSE(writeReport(root, empty, "s", "h").ok());
  fs::remove_all(root, ec);
}

TEST_CASE("bug report: the pointer crop is clamped inside the window") {
  CHECK(cursorCropRect(1920, 1080, 960, 540) == ui::IRect{704, 396, 512, 288});
  CHECK(cursorCropRect(1920, 1080, 3, 2) == ui::IRect{0, 0, 512, 288});
  CHECK(cursorCropRect(1920, 1080, 1919, 1079) == ui::IRect{1408, 792, 512, 288});
  CHECK(cursorCropRect(300, 200, 150, 100) == ui::IRect{0, 0, 300, 200});
  const ui::Image img = uiLike(640, 360);
  CHECK(cursorCrop(img, -1, 5).width == 0);  // outside: no crop
  const ui::Image c = cursorCrop(img, 600, 20);
  CHECK(c.width == 512);
  CHECK(c.height == 288);
}

TEST_CASE("bug report: the input ring formats clicks, drags, wheel bursts, keys and typing") {
  ui::UiTree tree;
  auto& root = tree.emplaceRoot<ui::Widget>();
  auto& lanes = root.add<ui::Widget>("lanes");
  lanes.setHittable(true);
  tree.setViewport(800, 600, ui::UiScale{});
  tree.layoutNow();
  InputLog log;
  auto ptr = [&](ui::PointerType t, float x, float y, double at, ui::Vec2 wheel = {0, 0}) {
    ui::InputEvent e;
    e.kind = ui::InputKind::Pointer;
    e.pointer.type = t;
    e.pointer.button = t == ui::PointerType::Wheel ? ui::PointerButton::None : ui::PointerButton::Left;
    e.pointer.pos = {x, y};
    e.pointer.wheel = wheel;
    e.pointer.clickCount = 1;
    log.record(e, tree, at);
  };
  ptr(ui::PointerType::Down, 10, 10, 1.0);
  ptr(ui::PointerType::Up, 11, 10, 1.1);
  ptr(ui::PointerType::Move, 50, 50, 1.2);  // not recorded
  ptr(ui::PointerType::Down, 10, 10, 2.0);
  ptr(ui::PointerType::Up, 200, 100, 2.5);
  for (int i = 0; i < 5; ++i) ptr(ui::PointerType::Wheel, 30, 30, 3.0 + 0.05 * i, {0, -1});
  ui::InputEvent k;
  k.kind = ui::InputKind::Key;
  k.key.down = true;
  k.key.key = 'z';
  k.key.mods = ui::mod::kCtrl;
  log.record(k, tree, 4.0);
  const std::string out = log.format(5.0);
  MESSAGE(out);
  CHECK(log.total() == 10);  // the move is not recorded
  CHECK(out.find("LMB click (10, 10) on lanes") != std::string::npos);
  CHECK(out.find("LMB drag (10, 10) \xE2\x86\x92 (200, 100) on lanes") != std::string::npos);
  CHECK(out.find("wheel x5 (dy -5.0)") != std::string::npos);
  CHECK(out.find("key Ctrl+Z") != std::string::npos);
  CHECK(out.find("-4.000 s") != std::string::npos);  // relative to F12
  // Lines are capped from the oldest.
  const std::string last = log.format(5.0, 1);
  CHECK(last.find("key Ctrl+Z") != std::string::npos);
  CHECK(last.find("click") == std::string::npos);
}

namespace {

void writeTicket(const fs::path& dir, std::string_view text) {
  fs::create_directories(dir);
  std::ofstream(dir / "report.md", std::ios::binary) << "# Bug report x\n\n" << text << "\n\n## Screenshots\n\n";
}

}  // namespace

TEST_CASE("bug report: closing a ticket moves its folder to closed/ and takes it off the list") {
  const fs::path root = fs::temp_directory_path() / "pq_bug_close_test";
  std::error_code ec;
  fs::remove_all(root, ec);
  writeTicket(root / "2026-11-01_10-00-00", "first");
  writeTicket(root / "2026-11-02_10-00-00", "second");
  writeTicket(root / "suggestions" / "2026-11-03_10-00-00", "idea");
  REQUIRE(listTickets(root, false).size() == 2);
  auto to = closeTicket(root / "2026-11-01_10-00-00");
  REQUIRE_MESSAGE(to.ok(), (to.ok() ? "" : to.error().message));
  CHECK(*to == root / "closed" / "2026-11-01_10-00-00");
  CHECK(fs::exists(*to / "report.md"));
  CHECK_FALSE(fs::exists(root / "2026-11-01_10-00-00"));
  const auto open = listTickets(root, false);
  REQUIRE(open.size() == 1);
  CHECK(open[0].id == "2026-11-02_10-00-00");
  // A taken name gets a suffix; a suggestion closes into suggestions/closed/.
  writeTicket(root / "2026-11-01_10-00-00", "again");
  CHECK(*closeTicket(root / "2026-11-01_10-00-00") == root / "closed" / "2026-11-01_10-00-00_2");
  CHECK(*closeTicket(root / "suggestions" / "2026-11-03_10-00-00") == root / "suggestions" / "closed" / "2026-11-03_10-00-00");
  CHECK(listTickets(root, true).empty());
  CHECK_FALSE(closeTicket(root / "nope").ok());
  fs::remove_all(root, ec);
}

TEST_CASE("bug report panel: the hovered ticket's x closes it (real dispatch)") {
  const fs::path root = fs::temp_directory_path() / "pq_bug_close_panel_test";
  std::error_code ec;
  fs::remove_all(root, ec);
  writeTicket(root / "2026-11-01_10-00-00", "older");
  writeTicket(root / "2026-11-02_10-00-00", "newer");
  ui::TestDriver d(1280, 800);
  ui::Widget& r = d.setRoot<ui::Widget>();
  auto& layer = r.add<ui::OverlayLayer>("overlay");
  d.tree().layoutNow();
  Draft draft;
  draft.text = "my new report";
  BugReportPanel* p = openBugReportPanel(layer, draft, root, {});
  REQUIRE(p != nullptr);
  d.tree().layoutNow();
  d.step();
  REQUIRE(p->tickets().size() == 2);
  const ui::IRect list = d.rect("overlay/bug-report/tickets");
  REQUIRE_FALSE(list.empty());
  // Row 0 is "new report"; row 1 the newest ticket. Pick it, then click its x (top right of the row).
  const float rh = 58.0f;
  const ui::Vec2 row1{static_cast<float>(list.x) + 150.0f, static_cast<float>(list.y) + rh + 30.0f};
  d.click(row1);
  CHECK(p->selectedTicket() == 0);
  CHECK(draft.target == root / "2026-11-02_10-00-00");
  const ui::Vec2 x{static_cast<float>(list.right()) - 14.0f, static_cast<float>(list.y) + rh + 14.0f};
  d.move(x);
  d.step();
  d.click(x);
  CHECK_FALSE(fs::exists(root / "2026-11-02_10-00-00"));
  CHECK(fs::exists(root / "closed" / "2026-11-02_10-00-00" / "report.md"));
  REQUIRE(p->tickets().size() == 1);
  CHECK(p->tickets()[0].id == "2026-11-01_10-00-00");
  // The closed ticket was picked: the panel is back on the new report, its text kept.
  CHECK(p->selectedTicket() == -1);
  CHECK(draft.target.empty());
  CHECK(draft.text == "my new report");
  CHECK(p->message().find("Closed 2026-11-02_10-00-00") == 0);
  // A click on the row body (not the x) only picks.
  d.click(row1);
  CHECK(p->selectedTicket() == 0);
  CHECK(fs::exists(root / "2026-11-01_10-00-00"));
  layer.closeAll();
  fs::remove_all(root, ec);
}
