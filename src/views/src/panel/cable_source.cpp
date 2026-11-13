// DY.2 (docs/plans/DY.md "DY.2"; SPEC §8 "sidechain"): the CABLE SOURCE element ("cablesource") — a menu that picks
// which other track's signal a cable brings into an input of the panel's circuit (a compressor's sidechain). General:
// it knows nothing of devices — only that its target is an input (a cable from another track into a signal input is a
// latency-compensated sidechain, pq/document/lower.h "Sidechains").
//
//   slots    0 "input"  an Input binding (doc::inputBinding: a block, "" its main input): where the cable lands
//            1 "on"     optional: a Control binding (a value block's `value`) set to 1 when a source is picked, 0 at None
//   menu     "None", then per other track (doc::ops::cableSources: not the master, not the input's own track, tree
//            order) "<track>: Pre FX" (its own signal; a MIDI track's instrument output) and "<track>: Post FX" (the block
//            feeding its mixer, when there is one past Pre). The current one is checked. A pick is ONE undo step through
//            PanelTarget::run (doc::ops::setCableSource: the old cross-track wires into the input removed, the new one
//            connected, "on" set).
//   shows    the current source READ FROM THE WIRES (a cable drawn in the project patch view shows too): its menu label,
//            else "<track>: <block>" (a cable from another block), "+N" when several cables reach the input; "None".
//            Recomputed only when the document changes (the project's identity), so a repaint formats nothing.
// UI thread only.

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "elements_internal.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/overlay.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

constexpr std::string_view kSlots[] = {"input", "on"};
constexpr float kLabelPx = 7.5f, kValuePx = 8.0f;

std::string sourceLabel(const doc::Project& p, const doc::ops::CableSource& s) {
  const doc::Track* t = doc::findTrackPtr(p, s.track);
  return (t != nullptr ? t->name : std::string("?")) + (s.post ? ": Post FX" : ": Pre FX");
}

Rgba swatchOf(const doc::Project& p, doc::TrackId id) {
  const doc::Track* t = doc::findTrackPtr(p, id);
  const uint32_t c = t != nullptr ? t->colour.rgb : 0x808080u;
  return ui::rgba((c >> 16) & 0xFFu, (c >> 8) & 0xFFu, c & 0xFFu);
}

class CableSourceElement final : public Element {
public:
  using Element::Element;
  static constexpr StyleSpec kStyle[] = {
      {"colour", StyleSpec::Type::Token, 0, "value"},
      {"label", StyleSpec::Type::Text, 0, ""},
  };
  bool interactive() const noexcept override { return true; }
  uint32_t staticLayerCount() const noexcept override { return 2; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? ui::lerp(th.face.body, th.face.track, 0.5f) : th.face.frame;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rect r = box();
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) {
      p.roundedRect(r, 3.0f, w);
      return;
    }
    outlineRect(p, r, 3.0f, 1.0f, w);
    // A jack: a ring with a cable leaving it (what the element picks), left of the text.
    const float cy = (r.y0 + r.y1) * 0.5f, cx = r.x0 + 7.0f;
    const float rr = std::min(3.0f, (r.y1 - r.y0) * 0.22f);
    for (int k = 0; k < 12; ++k) {
      const float a0 = static_cast<float>(k) * 0.5236f, a1 = a0 + 0.5236f;
      p.line({cx + rr * std::cos(a0), cy + rr * std::sin(a0)}, {cx + rr * std::cos(a1), cy + rr * std::sin(a1)}, 1.0f, w);
    }
    const float ax = r.x1 - 6.0f;  // the chevron
    p.line({ax - 2.5f, cy - 1.2f}, {ax, cy + 1.3f}, 1.1f, w);
    p.line({ax, cy + 1.3f}, {ax + 2.5f, cy - 1.2f}, 1.1f, w);
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    PanelTarget& t = *env.target;
    const Rect r = box();
    if (env.hovered) outlineRect(p, r, 3.0f, 1.25f, th.face.hover);
    const Shown& s = shown(t);
    const float cy = (r.y0 + r.y1) * 0.5f;
    if (s.any) p.roundedRect({r.x0 + 5.5f, cy - 1.5f, r.x0 + 8.5f, cy + 1.5f}, 1.5f, s.swatch);  // plugged: the jack lit
    p.textAligned({r.x0 + 13.0f, cy}, s.text, kValuePx, s.any ? colour(th, "colour", "value", th.face.value) : th.face.ink, 0.0f);
    const std::string& label = labelText(t);
    if (!label.empty() && r.y0 > 6.0f) p.textAligned({r.x0, 4.5f}, label, kLabelPx, th.face.ink, 0.0f);
  }
  uint64_t liveKey(const PanelTarget& t) const override {
    const Shown& s = shown(t);
    uint64_t h = std::hash<std::string>{}(s.text) ^ (static_cast<uint64_t>(s.swatch) << 1) ^ (s.any ? 0x5Cull : 0ull);
    if (const doc::Binding* b = binding()) h ^= t.info(*b).generation * 0x9E3779B97F4A7C15ull;
    return h == kAnyChange ? h - 1 : h;
  }
  bool onPointer(const FacePointer& e, PanelTarget& t) override {
    if (e.kind != FacePointer::Kind::Down || e.button != ui::PointerButton::Left) return false;
    ui::Widget* host = t.menuHost();
    const doc::Binding* in = binding(0);
    const doc::Project* p = t.project();
    if (host == nullptr || in == nullptr || in->kind != doc::BindingKind::Input || p == nullptr) return true;
    const doc::DeviceLoc loc = doc::findDevice(*p, in->device);
    if (loc.track == nullptr) return true;
    const doc::DeviceLibrary* lib = t.deviceLibrary();
    const std::vector<doc::WireId> now = doc::ops::crossTrackWiresInto(*p, lib, in->device, in->name);
    const doc::Wire* cur = now.empty() ? nullptr : doc::findWire(*p, now.front());
    PanelTarget* target = &t;
    const doc::Binding input = *in;
    const doc::Binding* onB = binding(1);
    const doc::Binding on = onB != nullptr && onB->kind == doc::BindingKind::Control ? *onB : doc::Binding{};
    auto pick = [target, input, on, lib](std::optional<doc::Wire> src) {
      return [target, input, on, lib, src] {
        (void)target->run([&](const doc::Project& q, doc::IdSource& ids) {
          return doc::ops::setCableSource(q, ids, lib, input.device, input.name, src, on.device, on.name);
        });
      };
    };
    std::vector<ui::MenuItem> menu;
    ui::MenuItem none = ui::MenuItem::item("None", pick(std::nullopt));
    none.checked = now.empty();
    menu.push_back(std::move(none));
    const std::vector<doc::ops::CableSource> sources = doc::ops::cableSources(*p, loc.track->id);
    if (!sources.empty()) menu.push_back(ui::MenuItem::sep());
    for (const doc::ops::CableSource& s : sources) {
      ui::MenuItem m = ui::MenuItem::item(sourceLabel(*p, s), pick(s.ends));
      m.checked = now.size() == 1 && cur != nullptr && doc::ops::sameSourceEnd(*cur, s.ends);
      m.swatch = swatchOf(*p, s.track);
      menu.push_back(std::move(m));
    }
    (void)ui::openContextMenu(*host, e.device, std::move(menu));
    return true;
  }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    title = labelText(t);
    if (title.empty()) title = "Source";
    body = shown(t).text + ". Click to pick another track's signal (Pre FX: its own; Post FX: after its devices).";
  }

private:
  struct Shown {
    std::string text = "None";
    Rgba swatch = 0;
    bool any = false;
  };
  // What the wires say now, recomputed when the document changed (a new project object).
  const Shown& shown(const PanelTarget& t) const {
    const doc::Project* p = t.project();
    const uint64_t rev = t.revision();
    if (p == seen_ && rev == seenRev_) return shown_;
    seen_ = p;
    seenRev_ = rev;
    shown_ = Shown{};
    const doc::Binding* in = binding(0);
    if (p == nullptr || in == nullptr || in->kind != doc::BindingKind::Input) return shown_;
    const doc::DeviceLoc loc = doc::findDevice(*p, in->device);
    if (loc.track == nullptr) return shown_;
    const std::vector<doc::WireId> now = doc::ops::crossTrackWiresInto(*p, t.deviceLibrary(), in->device, in->name);
    const doc::Wire* w = now.empty() ? nullptr : doc::findWire(*p, now.front());
    if (w == nullptr) return shown_;
    const doc::Track* from = doc::ops::sourceTrackOf(*p, *w);
    if (from == nullptr) return shown_;
    shown_.any = true;
    shown_.swatch = swatchOf(*p, from->id);
    for (const doc::ops::CableSource& s : doc::ops::cableSources(*p, loc.track->id))
      if (s.track == from->id && doc::ops::sameSourceEnd(*w, s.ends)) shown_.text = sourceLabel(*p, s);
    if (shown_.text == "None") {  // a cable from another block of that track (drawn in the circuit)
      const doc::Device* d = w->fromNoteSource() ? nullptr : doc::findDevice(*p, w->from).device;
      shown_.text = from->name + (d != nullptr ? ": " + (d->name.empty() ? std::string("Block") : d->name) : std::string());
    }
    if (now.size() > 1) shown_.text += " +" + std::to_string(now.size() - 1);
    return shown_;
  }
  Rect box() const noexcept {
    const float top = txt("label", "") != "-" && desc_.h >= 26.0f ? std::min(10.0f, desc_.h * 0.4f) : 0.0f;
    return {0.5f, top + 0.5f, desc_.w - 0.5f, desc_.h - 0.5f};
  }
  mutable const doc::Project* seen_ = nullptr;
  mutable uint64_t seenRev_ = ~0ull;
  mutable Shown shown_;
};

}  // namespace

void registerCableSource(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                     float, float, ElementFactory)) {
  reg("cablesource", "Cable Source", CableSourceElement::kStyle, kSlots, 120, 30,
      [](const doc::PanelElement& d) -> std::unique_ptr<Element> { return std::make_unique<CableSourceElement>(d); });
}

}  // namespace pq::views::panel
