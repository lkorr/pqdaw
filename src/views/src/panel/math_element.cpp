// MX.3 (docs/plans/MX.md "MX.3"; pq/document/modules.h "the Math module"): the OP CIRCLE ("mathop") of the Math module's
// face — the op's symbol drawn large in a circle, its category small under it.
//
//   binding  slot 0: a Setting binding on a Math group's kMathOpSetting (the op's code; doc::mathOpOfCode). The op is
//            read from the document (the setting is no patch param: the target does not resolve it), once per document
//            (its identity), so a paint looks nothing up.
//   click    opens the radial menu (pq/ui/radial_menu.h) on the circle: the five categories of doc::mathOpInfos, each
//            op a leaf (its symbol, its name, search words); the current op opened to. A pick runs doc::ops::setMathOp
//            through PanelTarget::run — one undo step, the op subgraph swapped. The pick handler touches only the
//            target and the group id (the scene rebuilds inside it: this element is gone by then).
//   value    selection() is the op's code, so other elements show by it (Visibility): the face's Tolerance box only
//            while == or ≠, its output scope as a 0 / 1 gate while the op gives a gate.
// UI thread only.

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "elements_internal.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/model.h"
#include "pq/ui/radial_menu.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr std::string_view kSlots[] = {"op"};

// Search words of an op (the radial menu's type-to-filter matches them besides its key, symbol and name).
std::string_view keywordsOf(std::string_view key) noexcept {
  struct K {
    std::string_view key, words;
  };
  static constexpr K kWords[] = {
      {"lt", "less smaller below"},          {"gt", "greater larger above"},       {"le", "<= less equal"},
      {"ge", ">= greater equal"},            {"eq", "= equal same tolerance"},     {"ne", "!= <> not equal different"},
      {"and", "&& both"},                    {"or", "|| either"},                  {"xor", "exclusive"},
      {"nand", "not and"},                   {"nor", "not or"},                    {"xnor", "equivalent"},
      {"not", "! invert negate"},            {"add", "+ plus sum"},                {"sub", "- minus subtract difference"},
      {"mul", "* times multiply product"},   {"div", "/ divide quotient"},         {"mod", "% modulo remainder"},
      {"pow", "^ power exponent"},           {"avg", "average mean middle"},       {"absdiff", "absolute difference distance"},
      {"min", "minimum smaller lower"},      {"max", "maximum larger upper"},      {"clip", "clamp limit saturate"},
      {"wrap", "modulo wrap around"},        {"fold", "mirror reflect wavefold"},  {"atan2", "angle arctangent phase polar"},
      {"hypot", "magnitude length distance radius"},
  };
  for (const K& k : kWords)
    if (k.key == key) return k.words;
  return {};
}

size_t glyphCount(std::string_view s) noexcept {
  size_t n = 0;
  for (const char c : s) n += (static_cast<unsigned char>(c) & 0xC0u) != 0x80u ? 1 : 0;
  return n;
}

class MathOp final : public Element {
public:
  using Element::Element;

  // ---- static layers: the disc, its rim ----
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? ui::lerp(th.face.body, th.face.track, 0.5f) : th.face.frame;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba white = ui::rgba(255, 255, 255);
    if (layer == 0) p.disc(centre(), radius(), white);
    else p.ring(centre(), radius(), 1.4f, white);
  }

  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    const Vec2 c = centre();
    const float r = radius();
    if (env.hovered || *open_) p.ring(c, r - 0.2f, 1.8f, *open_ ? th.circuit.selected : th.face.hover);
    const doc::MathOpInfo* op = env.target != nullptr ? opOf(*env.target) : nullptr;
    if (op == nullptr) {
      p.text(c, "?", r * 0.8f, th.face.ink);
      return;
    }
    const size_t n = glyphCount(op->symbol);
    const float px = r * (n <= 1 ? 0.8f : n == 2 ? 0.6f : n == 3 ? 0.56f : 0.42f);
    p.text({c.x, c.y - r * 0.08f}, op->symbol, px, th.face.value);
    p.text({c.x, c.y + r * 0.56f}, doc::mathCategoryName(op->category), std::max(5.0f, r * 0.17f), th.face.ink);
  }
  bool ghosted(const PanelTarget& /*t*/) const override { return false; }
  void paintGhost(ui::FacePainter&, const ElementEnv&) override {}

  // ---- interaction ----
  bool interactive() const noexcept override { return true; }
  bool hit(Vec2 l) const noexcept override {
    const Vec2 c = centre();
    return std::hypot(l.x - c.x, l.y - c.y) <= radius() + 1.0f;
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    if (e.kind != FacePointer::Kind::Down || e.button != ui::PointerButton::Left) return false;
    const doc::Binding* b = binding();
    ui::Widget* host = t.menuHost();
    if (b == nullptr || host == nullptr) return true;
    std::vector<ui::RadialItem> cats;
    for (const doc::MathOpInfo& m : doc::mathOpInfos()) {
      const std::string_view cat = doc::mathCategoryName(m.category);
      if (cats.empty() || cats.back().label != cat) cats.push_back(ui::RadialItem::category(std::string(cat), {}));
      cats.back().children.push_back(ui::RadialItem::leaf(std::string(m.key), std::string(m.name), std::string(m.symbol),
                                                          std::string(keywordsOf(m.key))));
    }
    // The circle in the host's device px: the menu's rings centre on it.
    const Vec2 c = centre();
    const Vec2 dc{e.device.x + (c.x - e.local.x) * e.pxPerUnit, e.device.y + (c.y - e.local.y) * e.pxPerUnit};
    const float rd = radius() * e.pxPerUnit;
    const ui::IRect anchor{static_cast<int32_t>(std::lround(dc.x - rd)), static_cast<int32_t>(std::lround(dc.y - rd)),
                           static_cast<int32_t>(std::lround(2.0f * rd)), static_cast<int32_t>(std::lround(2.0f * rd))};
    ui::RadialMenuOptions o;
    if (const doc::MathOpInfo* op = opOf(t)) o.current = std::string(op->key);
    PanelTarget* target = &t;
    const doc::DeviceId group = b->device;
    o.onPick = [target, group](std::string_view key) {
      (void)target->run([group, k = std::string(key)](const doc::Project& p, doc::IdSource& ids) {
        return doc::ops::setMathOp(p, ids, group, k);
      });
    };
    // (The menu can outlive the element — a pick rebuilds the panel: its onClosed holds the flag, not the element.)
    o.onClosed = [open = open_] { *open = false; };
    *open_ = ui::openRadialMenu(*host, anchor, std::move(cats), std::move(o)) != nullptr;
    return true;
  }
  ui::Cursor cursor() const noexcept override { return ui::Cursor::Pointer; }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    const doc::MathOpInfo* op = opOf(t);
    title = op != nullptr ? std::string(op->name) : std::string("Operation");
    body = "Click to choose the operation (type to search).";
  }
  std::optional<double> selection(const PanelTarget& t) const override {
    const doc::MathOpInfo* op = opOf(t);
    return op != nullptr ? std::optional<double>(op->code) : std::nullopt;
  }

private:
  Vec2 centre() const noexcept { return {desc_.w * 0.5f, desc_.h * 0.5f}; }
  float radius() const noexcept { return std::max(4.0f, std::min(desc_.w, desc_.h) * 0.5f - 1.0f); }
  // The op the bound setting names (cached per document and target revision: looked up once per edit).
  const doc::MathOpInfo* opOf(const PanelTarget& t) const {
    const doc::Project* p = t.project();
    if (p == nullptr) return nullptr;
    const uint64_t rev = t.revision();
    if (p == of_ && rev == rev_) return op_;
    of_ = p;
    rev_ = rev;
    op_ = nullptr;
    if (const doc::Binding* b = binding())
      if (const doc::Device* d = doc::findDevice(*p, b->device).device)
        if (const doc::DeviceValue* v = d->setting(b->name)) op_ = doc::mathOpOfCode(v->value);
    return op_;
  }
  mutable const doc::Project* of_ = nullptr;
  mutable const doc::MathOpInfo* op_ = nullptr;
  mutable uint64_t rev_ = 0;
  std::shared_ptr<bool> open_ = std::make_shared<bool>(false);
};

std::unique_ptr<Element> makeMathOp(const doc::PanelElement& d) { return std::make_unique<MathOp>(d); }

}  // namespace

void registerMathElement(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                     float, float, ElementFactory)) {
  reg("mathop", "Math Op", {}, kSlots, 80, 80, &makeMathOp);
}

}  // namespace pq::views::panel
