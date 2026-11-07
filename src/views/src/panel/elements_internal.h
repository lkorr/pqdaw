#pragma once

// GU.3: what the element kinds share (private to pq_views: elements.cpp, curve_element.cpp).

#include <bit>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/views/panel/element.h"

namespace pq::views::panel {

// "a|b|c" → {"a", "b", "c"} ("" → none).
std::vector<std::string> splitItems(std::string_view s);

// An element bound to values: their positions, a relative drag measured in device px from the press, one gesture.
class ValueElement : public Element {
public:
  using Element::Element;
  static constexpr size_t kSlots = 2;
  bool dragging() const noexcept override { return drag_; }
  void cancelDrag(PanelTarget& t) override;
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override;

protected:
  double pos(const PanelTarget& t, size_t slot = 0) const;
  const BoundInfo& bound(const PanelTarget& t, size_t slot = 0) const;  // an unbound slot: an invalid one
  bool editable(const PanelTarget& t, size_t slot = 0) const;
  void startDrag(PanelTarget& t, const FacePointer& e, const char* label);
  void endDrag(PanelTarget& t);
  // A stepped binding's position rounded to its steps.
  double quantise(const PanelTarget& t, double p, size_t slot = 0) const;
  // The slot's position after a relative drag (dx, dy: the axis's weight; `axisPx` device px per whole range).
  double dragged(const FacePointer& e, size_t slot, float dx, float dy, float axisPx) const;
  virtual std::string hint() const { return {}; }

  bool drag_ = false;
  ui::Vec2 startDev_{0.0f, 0.0f};
  double start_[kSlots] = {0.0, 0.0};
};

// GU.5b: the rich kinds (rich_elements.cpp: dial, arc, orbit), registered by registerGeneric through `reg`.
void registerRichElements(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                      std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// GU.6: the embedded child panel (embed_element.cpp, kind doc::kEmbedKind).
void registerEmbedElement(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                      std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// The curve editor (curve_element.cpp).
std::span<const StyleSpec> curveElementStyle() noexcept;
std::unique_ptr<Element> makeCurveElement(const doc::PanelElement& desc);

}  // namespace pq::views::panel
