// FD.2 (docs/plans/FD.md "FD.2"; pq/document/modules.h "lane cables"): the N CHIP ("lanecount") of the lane modules'
// faces (Lanes, Merge, Unpack, Pack) — four cells 2 | 4 | 8 | 16, the module's lane count lit.
//
//   binding  slot 0: a Setting binding on a lane module's kLaneCountSetting. The count is read from the document (the
//            setting is no patch param: the target does not resolve it), once per document and target revision, so a
//            paint looks nothing up.
//   click    a cell runs doc::ops::setLaneCount through PanelTarget::run — one undo step, the module's inside rebuilt
//            for that N (the scene rebuilds inside it: this element is gone by then, so the click touches only the
//            target and the group id). The lit cell does nothing.
//   value    selection() is N, so other elements can show by it (Visibility).
// UI thread only.

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "elements_internal.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr std::string_view kSlots[] = {"lanes"};
constexpr size_t kCells = std::size(doc::kLaneCounts);
constexpr float kChipPx = 7.5f;

class LaneCount final : public Element {
public:
  using Element::Element;

  uint32_t staticLayerCount() const noexcept override { return 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t) const noexcept override { return ui::lerp(th.face.body, th.face.track, 0.6f); }
  void paintStatic(ui::FacePainter& p, uint32_t) const override {
    for (size_t k = 0; k < kCells; ++k) p.roundedRect(cell(k), 3.0f, ui::rgba(255, 255, 255));
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const uint32_t n = env.target != nullptr ? lanesOf(*env.target) : 0u;
    for (size_t k = 0; k < kCells; ++k) {
      const Rect c = cell(k);
      const bool on = doc::kLaneCounts[k] == n;
      if (on) p.roundedRect(c, 3.0f, th.face.value);
      else if (env.hovered && static_cast<int32_t>(k) == hoverCell_) p.roundedRect(c, 3.0f, ui::mulAlpha(th.face.hover, 0.35f));
      p.text({(c.x0 + c.x1) * 0.5f, (c.y0 + c.y1) * 0.5f}, std::to_string(doc::kLaneCounts[k]), kChipPx, on ? th.face.body : th.face.ink);
    }
  }
  bool ghosted(const PanelTarget& /*t*/) const override { return false; }
  void paintGhost(ui::FacePainter&, const ElementEnv&) override {}
  uint64_t liveKey(const PanelTarget& t) const override {
    return (static_cast<uint64_t>(lanesOf(t)) << 8) | static_cast<uint64_t>(static_cast<uint8_t>(hoverCell_ + 1));
  }

  bool interactive() const noexcept override { return true; }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    if (e.kind == FacePointer::Kind::Move) {
      const int32_t h = cellAt(e.local);
      const bool changed = h != hoverCell_;
      hoverCell_ = h;
      return changed;
    }
    if (e.kind != FacePointer::Kind::Down || e.button != ui::PointerButton::Left) return false;
    const int32_t k = cellAt(e.local);
    const doc::Binding* b = binding();
    if (k < 0 || b == nullptr) return k >= 0;
    const uint32_t n = doc::kLaneCounts[static_cast<size_t>(k)];
    if (n == lanesOf(t)) return true;
    const doc::DeviceId group = b->device;
    (void)t.run([group, n](const doc::Project& p, doc::IdSource& ids) { return doc::ops::setLaneCount(p, ids, group, n); });
    return true;
  }
  ui::Cursor cursor() const noexcept override { return ui::Cursor::Pointer; }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    title = "Lanes: " + std::to_string(lanesOf(t));
    body = "How many lanes the cable has (2, 4, 8 or 16, in total). Click to change; the modules on a lane cable run once per lane.";
  }
  std::optional<double> selection(const PanelTarget& t) const override {
    const uint32_t n = lanesOf(t);
    return n != 0 ? std::optional<double>(static_cast<double>(n)) : std::nullopt;
  }

private:
  Rect cell(size_t k) const noexcept {
    const float gap = 2.0f, w = (desc_.w - gap * static_cast<float>(kCells - 1)) / static_cast<float>(kCells);
    const float x0 = static_cast<float>(k) * (w + gap);
    return {x0, 0.5f, x0 + w, desc_.h - 0.5f};
  }
  int32_t cellAt(Vec2 l) const noexcept {
    for (size_t k = 0; k < kCells; ++k) {
      const Rect c = cell(k);
      if (l.x >= c.x0 && l.x <= c.x1 && l.y >= c.y0 && l.y <= c.y1) return static_cast<int32_t>(k);
    }
    return -1;
  }
  // The lane count the bound setting holds (cached per document and target revision).
  uint32_t lanesOf(const PanelTarget& t) const {
    const doc::Project* p = t.project();
    if (p == nullptr) return 0;
    const uint64_t rev = t.revision();
    if (p == of_ && rev == rev_) return lanes_;
    of_ = p;
    rev_ = rev;
    lanes_ = 0;
    if (const doc::Binding* b = binding())
      if (const doc::Device* d = doc::findDevice(*p, b->device).device)
        if (const doc::DeviceValue* v = d->setting(b->name); v != nullptr && v->value >= 1.0 && v->value <= doc::kMaxLaneCount)
          lanes_ = static_cast<uint32_t>(v->value);
    return lanes_;
  }
  mutable const doc::Project* of_ = nullptr;
  mutable uint64_t rev_ = 0;
  mutable uint32_t lanes_ = 0;
  int32_t hoverCell_ = -1;
};

std::unique_ptr<Element> makeLaneCount(const doc::PanelElement& d) { return std::make_unique<LaneCount>(d); }

}  // namespace

void registerLaneElement(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                     float, float, ElementFactory)) {
  reg("lanecount", "Lane Count", {}, kSlots, 100, 14, &makeLaneCount);
}

}  // namespace pq::views::panel
