// GU.6 (docs/plans/GU.md "Panels and the panel window" Nesting, "Phases" 6): an EMBEDDED CHILD PANEL — the element a child
// group left whole on its parent's panel is (kind doc::kEmbedKind, slot 0 a Panel binding naming the child). It holds
// the child's panel as a GroupPanelFace of its own, live: the child's elements, bound to the same controls through the
// parent's target (every binding inside the child is inside the parent), drawn fitted into the element's rect
// (doc::embedFit), its pointer events, keys, text, popups, voices and static tiles forwarded at that fit. The child's
// panel nests again (an embed inside it is the same element): any depth. Break apart (ops::breakApartPanelElements)
// replaces it with copies of the child's elements at the same places.
//
// CPU: the child panel is rebuilt only when its document panel changes (a persistent document: pointer compare per
// paint); its static layers come from the shared element tile cache at the embed's scale (prepare), as the parent's.

#include <memory>
#include <string>
#include <utility>

#include "elements_internal.h"
#include "pq/views/panel/group_panel_face.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Vec2;

namespace {

// The parent's target, shared by the child panel (not owned: the parent face outlives its elements).
class ForwardTarget final : public PanelTarget {
public:
  explicit ForwardTarget(PanelTarget& t) : t_(t) {}
  const BoundInfo& info(const doc::Binding& b) const override { return t_.info(b); }
  double position(const doc::Binding& b) const override { return t_.position(b); }
  std::string format(const doc::Binding& b, double position) const override { return t_.format(b, position); }
  std::optional<double> parse(const doc::Binding& b, std::string_view text) const override { return t_.parse(b, text); }
  void begin(const doc::Binding& b, const char* label) override { t_.begin(b, label); }
  void setPosition(const doc::Binding& b, double position) override { t_.setPosition(b, position); }
  void setCurve(const doc::Binding& b, const dsp::Curve& c) override { t_.setCurve(b, c); }
  void end() override { t_.end(); }
  void cancel() override { t_.cancel(); }
  void setOnce(const doc::Binding& b, double position, const char* label) override { t_.setOnce(b, position, label); }
  void reset(const doc::Binding& b) override { t_.reset(b); }
  uint64_t revision() const override { return t_.revision(); }
  void notice(std::string_view message) override { t_.notice(message); }
  // (Not forwarded: the parent's per-binding cache also holds the parent's own elements; a child rebuild only adds
  // entries for its new bindings.)
  void bindingsChanged() override {}
  ui::Widget* menuHost() const noexcept override { return t_.menuHost(); }
  bool driven(const doc::Binding& b) const override { return t_.driven(b); }
  bool connect(const doc::Binding& from, const doc::Binding& to) override { return t_.connect(from, to); }
  VoiceTapReader* voices(const doc::Binding& b) override { return t_.voices(b); }
  doc::PanelPtr childPanel(const doc::Binding& b) const override { return t_.childPanel(b); }
  // GU.8: an embedded engine module (the Wavetable Synth's tabs) shows its engine's face, not the missing-face box.
  std::shared_ptr<MetaFace> engineFace(const doc::Binding& b, std::string_view typeId, FaceHost* host) override {
    return t_.engineFace(b, typeId, host);
  }
  devices::SpectrumFeed* signalFeed(const doc::Binding& b) override { return t_.signalFeed(b); }

private:
  PanelTarget& t_;
};

constexpr std::string_view kPanelSlot[] = {"panel"};

class EmbedElement final : public Element {
public:
  using Element::Element;

  void bind(PanelTarget& t, FaceHost* host) override {
    if (face_ == nullptr || parent_ != &t) {
      parent_ = &t;
      const doc::Binding b = binding() != nullptr ? *binding() : doc::Binding{};
      PanelTarget* pt = &t;
      face_ = std::make_unique<GroupPanelFace>([pt, b]() -> doc::PanelPtr { return b.bound() ? pt->childPanel(b) : nullptr; },
                                               std::make_unique<ForwardTarget>(t));
      face_->setOwner(b.device);
    }
    face_->attach(host);
  }

  // ---- painting ----
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    if (face_ == nullptr || env.theme == nullptr) return;
    (void)face_->sync();
    if (!env.hovered && hover_ != Face::kNone) {
      hover_ = Face::kNone;
      (void)face_->setHoveredPart(Face::kNone);
    }
    ui::FacePainter q = childPainter(p);
    FaceEnv fe;
    fe.theme = env.theme;
    face_->paintLive(q, fe);
  }
  bool needsPrepare(float px) const noexcept override { return face_ != nullptr && face_->needsPrepare(px * fit().k); }
  bool prepare(float px) override { return face_ != nullptr && face_->prepare(px * fit().k); }

  // ---- interaction ----
  bool interactive() const noexcept override { return true; }
  bool hit(Vec2 local) const noexcept override { return face_ != nullptr && face_->elementAt(toChild(local)) >= 0; }
  bool onPointer(const FacePointer& e, PanelTarget& /*t*/) override {
    if (face_ == nullptr) return false;
    FacePointer x = e;
    x.local = toChild(e.local);
    x.pxPerUnit = e.pxPerUnit * fit().k;
    bool changed = false;
    if (e.kind == FacePointer::Kind::Move && !face_->dragging()) {
      const int32_t part = face_->hitTest(x.local, x.pxPerUnit);
      hover_ = part;
      changed = face_->setHoveredPart(part);
    }
    if (e.kind == FacePointer::Kind::Down) hover_ = face_->hitTest(x.local, x.pxPerUnit);
    return face_->onPointer(x) || changed;
  }
  bool dragging() const noexcept override { return face_ != nullptr && face_->dragging(); }
  void cancelDrag(PanelTarget& /*t*/) override {
    if (face_ != nullptr) face_->cancelDrag();
  }
  bool onKey(const ui::KeyEvent& e, PanelTarget& /*t*/) override { return face_ != nullptr && face_->onKey(e); }
  bool onText(const ui::TextEvent& e, PanelTarget& /*t*/) override { return face_ != nullptr && face_->onText(e); }
  bool wantsText() const noexcept override { return face_ != nullptr && face_->wantsText(); }
  void focusLost(PanelTarget& /*t*/) override {
    if (face_ != nullptr) face_->focusLost();
  }
  ui::Cursor cursor() const noexcept override { return face_ != nullptr ? face_->cursor(hover_) : ui::Cursor::Inherit; }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    if (face_ != nullptr && hover_ >= 0) {
      face_->describe(hover_, title, body);
      return;
    }
    const doc::Binding* b = binding();
    title = b != nullptr ? t.info(*b).label : std::string();
    if (title.empty()) title = "Embedded panel";
    body = "A child group's panel, embedded: its controls are the child's. In the panel window's edit mode, Break Apart "
           "(Ctrl+Shift+G) puts copies of its elements here instead, each movable on its own.";
  }
  const doc::Binding* controlAt(Vec2 local) const override {
    if (face_ == nullptr) return nullptr;
    const Vec2 c = toChild(local);
    const int32_t i = face_->elementAt(c);
    if (i < 0) return nullptr;
    const Element& e = face_->element(static_cast<size_t>(i));
    return e.controlAt({c.x - e.rect().x0, c.y - e.rect().y0});
  }
  std::optional<ControlAnchor> anchorFor(const doc::Binding& want, bool shownOnly) const override {
    if (face_ == nullptr) return std::nullopt;
    std::optional<ControlAnchor> a = face_->bindingAnchor(want, shownOnly);
    if (!a) return std::nullopt;
    const doc::EmbedFit f = fit();
    a->at = {f.x + a->at.x * f.k, f.y + a->at.y * f.k};
    a->radius *= f.k;
    a->clear *= f.k;
    return a;
  }

  // ---- what the child's panel brings (GU.5b's outline, popups and voices) ----
  std::optional<FaceShape> outline() const noexcept override {
    if (face_ == nullptr) return std::nullopt;
    FaceShape s = face_->shape();
    if (!s.round) return std::nullopt;
    const doc::EmbedFit f = fit();
    s.centre = {f.x + s.centre.x * f.k, f.y + s.centre.y * f.k};
    s.radius *= f.k;
    return s;
  }
  std::optional<Rect> popupRect() const noexcept override {
    if (face_ == nullptr) return std::nullopt;
    const std::optional<Rect> r = face_->popupRect();
    if (!r) return std::nullopt;
    const doc::EmbedFit f = fit();
    return Rect{f.x + r->x0 * f.k, f.y + r->y0 * f.k, f.x + r->x1 * f.k, f.y + r->y1 * f.k};
  }
  void paintPopup(ui::FacePainter& p, const ElementEnv& env) override {
    if (face_ == nullptr || env.theme == nullptr) return;
    ui::FacePainter q = childPainter(p);
    FaceEnv fe;
    fe.theme = env.theme;
    face_->paintPopup(q, fe);
  }
  void setPopupOverFace(bool on) override {
    if (face_ != nullptr) face_->setPopupOverFace(on);
  }
  bool pollVoices(PanelTarget& /*t*/) override { return face_ != nullptr && face_->pollVoices(); }
  bool animating() const noexcept override { return face_ != nullptr && face_->animating(); }
  VoiceTapReader* voiceReader(PanelTarget& /*t*/) const override { return face_ != nullptr ? face_->voices() : nullptr; }

private:
  doc::EmbedFit fit() const noexcept {
    const Vec2 ws = face_ != nullptr ? face_->worldSize() : Vec2{desc_.w, desc_.h};
    return doc::embedFit(desc_.w, desc_.h, ws.x, ws.y);
  }
  Vec2 toChild(Vec2 l) const noexcept {
    const doc::EmbedFit f = fit();
    return {(l.x - f.x) / f.k, (l.y - f.y) / f.k};
  }
  ui::FacePainter childPainter(ui::FacePainter& p) const {
    const doc::EmbedFit f = fit();
    const float sc = p.transform().scale;
    const Vec2 o = p.transform().origin;
    return p.at({{o.x + f.x * sc, o.y + f.y * sc}, sc * f.k});
  }

  std::unique_ptr<GroupPanelFace> face_;
  PanelTarget* parent_ = nullptr;
  int32_t hover_ = Face::kNone;
};

std::unique_ptr<Element> makeEmbed(const doc::PanelElement& d) { return std::make_unique<EmbedElement>(d); }

}  // namespace

void registerEmbedElement(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                      float, float, ElementFactory)) {
  reg(doc::kEmbedKind, "Embedded Panel", {}, kPanelSlot, 120, 60, &makeEmbed);
}

}  // namespace pq::views::panel
