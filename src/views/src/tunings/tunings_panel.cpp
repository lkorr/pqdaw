// The Tunings panel (T3.19). See pq/views/tunings/tunings_panel.h.

#include "pq/views/tunings/tunings_panel.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "../common/colour_grid.h"
#include "../common/paint.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/mos_wheel/mos_wheel.h"

namespace pq::views {

using namespace pq::ui;
namespace tu = pq::tuning;

namespace {

constexpr float kHeadH = 20.0f;     // name row
constexpr float kRowH = 18.0f;      // summary and parameter rows
constexpr float kUsageH = 16.0f;
constexpr float kCardPad = 6.0f;
constexpr float kCardGap = 2.0f;
constexpr float kPanelHeader = 26.0f;

std::string quoted(std::string_view s) { return "\xE2\x80\x9C" + std::string(s) + "\xE2\x80\x9D"; }

// "2/1", or the cents ("1901.96 ¢") when the equave is not that ratio.
std::string equaveText(const doc::TuningDef* def, double cents) {
  if (def)
    if (const tu::Edo* e = def->edo(); e && e->equaveRatio() && std::fabs(e->equaveCents() - cents) < 1e-9) {
      const tu::Rational r = *e->equaveRatio();
      return std::to_string(r.num()) + "/" + std::to_string(r.den());
    }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f \xC2\xA2", cents);
  return buf;
}

// "p/q" (or "p:q") → the ratio; nullopt for anything else.
std::optional<tu::Rational> parseRatio(std::string_view t) {
  while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
  while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
  size_t slash = t.find('/');
  if (slash == std::string_view::npos) slash = t.find(':');
  if (slash == std::string_view::npos) return std::nullopt;
  int64_t n = 0, d = 0;
  const std::string_view a = t.substr(0, slash), b = t.substr(slash + 1);
  const auto ra = std::from_chars(a.data(), a.data() + a.size(), n);
  const auto rb = std::from_chars(b.data(), b.data() + b.size(), d);
  if (ra.ec != std::errc{} || ra.ptr != a.data() + a.size() || rb.ec != std::errc{} || rb.ptr != b.data() + b.size()) return std::nullopt;
  if (n <= 0 || d <= 0) return std::nullopt;
  return tu::Rational::tryMake(n, d);
}

std::optional<double> parseNumber(std::string_view t) {
  while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
  double v = 0.0;
  const auto r = std::from_chars(t.data(), t.data() + t.size(), v);
  if (r.ec != std::errc{} || r.ptr == t.data()) return std::nullopt;
  return v;
}

const doc::TuningSlot* slotOf(const doc::Project& p, doc::TuningId id) { return doc::findTuning(p, id); }

// The colour swatch of a card: the tuning's colour; a click opens the palette.
class Swatch : public Control {
public:
  Swatch(TuningsPanel& panel, doc::TuningId id) : Control("colour"), panel_(panel), id_(id) {
    setSize(Length::fixed(14), Length::fixed(14));
    setFocusable(false);
    setCursor(Cursor::Pointer);  // T4.16a: a click opens the palette
    setInfo("Tuning Colour", "The colour of this main tuning's chips on the tracks and clips that follow it. Click to choose.");
  }
  void setColour(uint32_t rgb) {
    if (rgb == rgb_) return;
    rgb_ = rgb;
    markDirty();
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    const IRect r = rect();
    const float rad = static_cast<float>(r.w) / 2.0f;
    const Vec2 c{static_cast<float>(r.x) + rad, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f};
    if (hovered()) {  // T4.16a: hover feedback (clickable): a light ring around the colour
      ctx.dl.addCircleFilled(c, rad, th->text.primary);
      ctx.dl.addCircleFilled(c, rad - 1.0f - ctx.scale.exact(1.0f), hex(rgb_));
      return;
    }
    ctx.dl.addCircleFilled(c, rad - 1.0f, hex(rgb_));
  }
  bool onPointer(const PointerEvent& e) override {
    if (e.type == PointerType::Down && e.button == PointerButton::Left) {
      openPalette();
      return true;
    }
    return e.type == PointerType::Up;
  }
  void openPalette() {
    OverlayLayer* layer = OverlayLayer::of(*this);
    if (!layer) return;
    auto pop = std::make_unique<Popover>(layer->uniqueName("tuning-colours"));
    Popover* popPtr = pop.get();
    ViewContext* c = &panel_.context();
    const doc::TuningId id = id_;
    pop->add<paint::ColourGrid>(rgb_, [c, id, layer, popPtr](uint32_t rgb) {
      if (Status s = c->store().dispatch(doc::SetTuningColour{id, doc::Colour{rgb}}); !s) c->setStatus("Tuning: " + s.error().message);
      layer->close(*popPtr);
    }, "Tuning Colour", "Click a colour for this main tuning.");
    pop->setSize(Length::fixed(paint::colourGridWidth() + 16.0f), Length::fixed(paint::colourGridHeight() + 16.0f));
    OverlayOptions o;
    o.anchor = rect();
    o.placement = Placement::Below;
    layer->open(std::move(pop), std::move(o));
  }

private:
  TuningsPanel& panel_;
  doc::TuningId id_;
  uint32_t rgb_ = 0;
};

}  // namespace

std::string nextTuningName(const doc::Project& p) {
  for (char c = 'A'; c <= 'Z'; ++c) {
    const std::string n = std::string("Tuning ") + c;
    bool used = false;
    for (const doc::TuningSlotPtr& s : *p.tunings) used = used || s->name == n || s->name.rfind(n + ":", 0) == 0;
    if (!used && c > 'A') return n;
    if (!used && c == 'A' && p.tunings->empty()) return n;
  }
  return "Tuning " + std::to_string(p.tunings->size() + 1);
}

// --- parameter row ---------------------------------------------------------------------------------------------------

TuningParamRow::TuningParamRow(std::string name, TuningsPanel& panel, doc::TuningId tuning, std::optional<doc::ParamKind> param,
                               std::string label, ValueSpec spec)
    : Widget(std::move(name)), panel_(panel), tuning_(tuning), param_(param) {
  setLayout(LayoutKind::Row);
  setHeight(Length::fixed(kRowH));
  setPadding({10, 1, 0, 1});
  auto& l = add<Label>("label", std::move(label), FontRoleId::Small, TextTone::Secondary);
  l.setWidth(Length::flex(1));
  box_ = &add<DragBox>("value", std::move(spec));
  box_->setWidth(Length::fixed(92));
}

void TuningParamRow::setAutomated(bool on) {
  if (on == automated_) return;
  automated_ = on;
  markDirty();
}

void TuningParamRow::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th || !automated_) return;
  // Live's automated-parameter marker: a small red dot before the name.
  const IRect r = rect();
  const float rad = ctx.scale.exact(2.5f);
  ctx.dl.addCircleFilled({static_cast<float>(r.x) + ctx.scale.exact(4.0f), static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}, rad,
                         th->accent.arm);
}

bool TuningParamRow::onPointer(const PointerEvent& e) {
  if (e.button != PointerButton::Right || !param_) return false;
  if (e.type != PointerType::Down) return true;
  TuningsPanel* panel = &panel_;
  const doc::ParamRef ref = doc::ParamRef::tuning(*param_, tuning_);
  std::vector<MenuItem> items;
  MenuItem show = MenuItem::item("Show Automation Lane", [panel, ref] { (void)panel->showAutomationLane(ref); });
  items.push_back(std::move(show));
  (void)openContextMenu(*this, e.pos, std::move(items));
  return true;
}

// --- card ------------------------------------------------------------------------------------------------------------

TuningCard::TuningCard(std::string name, TuningsPanel& panel, const doc::TuningSlot& slot)
    : Widget(std::move(name)), panel_(panel), id_(slot.id), kind_(slot.def.system.index()) {
  setLayout(LayoutKind::Column);
  setPadding({static_cast<int16_t>(kCardPad), static_cast<int16_t>(kCardPad), static_cast<int16_t>(kCardPad), static_cast<int16_t>(kCardPad)});
  setGap(static_cast<int16_t>(kCardGap));
  ViewContext& ctx = panel.context();
  ViewContext* c = &ctx;
  const doc::TuningId id = id_;

  auto& head = add<Widget>("head");
  head.setLayout(LayoutKind::Row);
  head.setHeight(Length::fixed(kHeadH));
  head.setGap(4);
  head.setAlign(Align::Start, Align::Center);
  swatch_ = &head.add<Swatch>(panel, id);
  name_ = &head.add<TextEdit>("name", slot.name);
  name_->setSize(Length::flex(1), Length::fixed(18));
  name_->setInfo("Tuning Name", "The main tuning's name, shown on the chips of the tracks and clips that follow it. Type a "
                                "name and press Enter (one undo step); Esc restores it.");
  name_->onCommit = [c, id, this](const std::string& text) {
    const doc::TuningSlot* s = slotOf(*c->doc(), id);
    if (!s) return;
    if (text.empty()) {
      name_->setText(s->name);
      return;
    }
    if (text == s->name) return;
    if (Status st = c->store().dispatch(doc::RenameTuning{id, text}); !st) c->setStatus("Tuning: " + st.error().message);
  };
  remove_ = &head.add<Button>("remove", "\xC3\x97");
  remove_->setSize(Length::fixed(18), Length::fixed(18));
  remove_->setFocusable(false);
  remove_->setInfo("Delete Tuning", "Deletes this main tuning. Tracks and clips that follow it move to the first other main "
                                    "tuning; its automation lanes go with it (one undo step).");
  TuningsPanel* p = &panel;
  remove_->onClick = [p, id] { (void)p->removeTuning(id); };

  auto& kindRow = add<Widget>("kind");
  kindRow.setLayout(LayoutKind::Row);
  kindRow.setHeight(Length::fixed(kRowH));
  kindRow.setAlign(Align::Start, Align::Center);
  summary_ = &kindRow.add<Label>("summary", tuningSummary(slot.def), FontRoleId::Small, TextTone::Primary);
  summary_->setWidth(Length::flex(1));
  wheel_ = &kindRow.add<Button>("wheel", "Wheel\xE2\x80\xA6");
  wheel_->setSize(Length::fixed(56), Length::fixed(16));
  wheel_->setFocusable(false);
  wheel_->setInfo("MOS Wheel", "Opens the MOS wheel for this main tuning: a MOS, an EDO, a Scala file or a temperament "
                               "(one undo step when it closes).");
  wheel_->onClick = [c, id, this] { (void)openMosWheel(*wheel_, *c, TuningTarget::forMain(id), wheel_->rect()); };

  // Parameter rows. Continuous ones: SetTuningParam in a store gesture (Param: no recompile while dragging).
  // At the end of a gesture the panel re-reads the document: a refused or clamped value snaps back, and the equave's
  // text follows the definition (an exact ratio typed in).
  auto bindParam = [c, id, p](TuningParamRow& row, doc::ParamKind k, const char* label) {
    DragBox& b = row.box();
    b.onGestureBegin = [c, label] { (void)c->store().beginGesture(label); };
    b.onChange = [c, id, k](double v) {
      if (Status s = c->store().dispatch(doc::SetTuningParam{id, k, v}); !s) c->setStatus("Tuning: " + s.error().message);
    };
    b.onGestureEnd = [c, p] {
      if (c->store().inGesture()) (void)c->store().commitGesture();
      p->refreshLater();
    };
    b.onGestureCancel = [c, p] {
      if (c->store().inGesture()) (void)c->store().cancelGesture();
      p->refreshLater();
    };
  };
  auto range = [](doc::ParamKind k, int decimals, const char* unit, double dragPixels) {
    const doc::ParamRange r = doc::paramRange(k);
    ValueSpec s;
    s.min = r.min;
    s.max = r.max;
    s.decimals = decimals;
    s.unit = unit;
    s.dragPixels = dragPixels;
    return s;
  };
  auto addRow = [this, &panel, id](const char* name, std::optional<doc::ParamKind> k, const char* label, ValueSpec spec) -> TuningParamRow& {
    auto& r = add<TuningParamRow>(name, panel, id, k, label, std::move(spec));
    rows_.push_back(&r);
    return r;
  };
  int nParams = 0;
  if (slot.def.edo()) {
    ValueSpec ss;
    ss.min = 1;
    ss.max = 999;
    ss.step = 1;
    ss.keyStep = 1;
    ss.decimals = 0;
    ss.dragPixels = 600;
    auto& steps = addRow("steps", std::nullopt, "Steps", ss);
    steps.box().setInfo("EDO Steps", "Equal divisions of the equave. Drag or type; the size is applied when you let go (one "
                                     "undo step).");
    // The size is discrete (a structural change): applied once at the end of the drag or type-in, not per pixel.
    DragBox* sb = &steps.box();
    sb->onGestureEnd = [c, id, sb] {
      const doc::TuningSlot* s = slotOf(*c->doc(), id);
      if (!s || !s->def.edo()) return;
      const tu::Edo& old = *s->def.edo();
      const auto n = static_cast<uint32_t>(std::lround(sb->value()));
      if (n == old.n()) return;
      Result<tu::Edo> e = old.equaveRatio() ? tu::Edo::make(n, *old.equaveRatio()) : tu::Edo::makeCents(n, old.equaveCents());
      if (!e) {
        c->setStatus("Tuning: " + e.error().message);
        sb->setValue(old.n());
        return;
      }
      doc::TuningDef d = s->def;
      d.system = *e;
      if (Status st = c->store().dispatch(doc::SetTuningDef{id, std::move(d)}); !st) c->setStatus("Tuning: " + st.error().message);
    };
    sb->onGestureCancel = [c, id, sb] {
      if (const doc::TuningSlot* s = slotOf(*c->doc(), id); s && s->def.edo()) sb->setValue(s->def.edo()->n());
    };
    auto& eq = addRow("equave", doc::ParamKind::TuningEquave, "Equave", range(doc::ParamKind::TuningEquave, 2, "\xC2\xA2", 2400));
    DragBox* eb = &eq.box();
    eb->setInfo("Equave", "The interval the EDO divides: drag in cents (automatable, no recompile), or type a ratio such as "
                          "3/1 (Bohlen\xE2\x80\x93Pierce) or cents such as 1901.96.");
    eb->formatter = [c, id](double v) {
      const doc::TuningSlot* s = slotOf(*c->doc(), id);
      return equaveText(s ? &s->def : nullptr, v);
    };
    // A typed ratio is kept exact: the gesture ends with SetTuningDef(Edo::make(n, ratio)) instead of the cents value.
    auto pending = std::make_shared<std::optional<tu::Rational>>();
    eb->parser = [pending](std::string_view t) -> std::optional<double> {
      pending->reset();
      if (const auto r = parseRatio(t); r && r->toDouble() > 1.0) {
        *pending = *r;
        return 1200.0 * std::log2(r->toDouble());
      }
      return parseNumber(t);
    };
    bindParam(eq, doc::ParamKind::TuningEquave, "Change Equave");
    eb->onChange = [c, id, pending](double v) {
      const doc::TuningSlot* s = slotOf(*c->doc(), id);
      if (!s || !s->def.edo()) return;
      Status st = Ok{};
      if (*pending && std::fabs(1200.0 * std::log2((*pending)->toDouble()) - v) < 1e-9) {
        Result<tu::Edo> e = tu::Edo::make(s->def.edo()->n(), **pending);
        if (!e) st = std::move(e).error();
        else {
          doc::TuningDef d = s->def;
          d.system = *e;
          st = c->store().dispatch(doc::SetTuningDef{id, std::move(d)});
        }
      } else {
        st = c->store().dispatch(doc::SetTuningParam{id, doc::ParamKind::TuningEquave, v});
      }
      pending->reset();
      if (!st) c->setStatus("Tuning: " + st.error().message);
    };
    eb->setSpec(eb->spec());  // format with the formatter
    nParams = 3;
  } else if (const tu::MosParams* m = slot.def.mos()) {
    ValueSpec gs = range(doc::ParamKind::MosGenerator, 2, "\xC2\xA2", 2400);
    gs.min = 0.0;
    gs.max = m->period;
    gs.keyStep = 0.1;
    auto& g = addRow("generator", doc::ParamKind::MosGenerator, "Generator", gs);
    g.box().setInfo("Generator", "The MOS generator in cents: dragging retunes every Degree note that follows this tuning "
                                 "(a parameter change: no recompile; automatable). Region changes: the MOS wheel.");
    bindParam(g, doc::ParamKind::MosGenerator, "Change Generator");
    ValueSpec ps = range(doc::ParamKind::MosPeriod, 2, "\xC2\xA2", 4800);
    auto& per = addRow("period", doc::ParamKind::MosPeriod, "Period", ps);
    per.box().setInfo("Period", "The MOS period in cents (1200 for an octave-repeating MOS).");
    bindParam(per, doc::ParamKind::MosPeriod, "Change Period");
    ValueSpec st = range(doc::ParamKind::MosStretch, 2, "\xC2\xA2", 2400);
    st.keyStep = 0.1;
    auto& stretch = addRow("stretch", doc::ParamKind::MosStretch, "Stretch", st);
    stretch.box().setInfo("Octave Stretch", "Cents added to the 1200-cent octave the MOS repeats at.");
    bindParam(stretch, doc::ParamKind::MosStretch, "Change Stretch");
    nParams = 4;
  } else {
    nParams = 1;
  }
  // Reference: Hz, dragged on a log scale (one octave per 120 px; Up / Down: a 12-EDO semitone).
  ValueSpec rs;
  rs.min = std::log2(doc::paramRange(doc::ParamKind::TuningReference).min);
  rs.max = std::log2(doc::paramRange(doc::ParamKind::TuningReference).max);
  rs.decimals = 6;
  rs.dragPixels = 120.0 * (rs.max - rs.min);
  rs.keyStep = 1.0 / 12.0;
  rs.def = std::log2(tu::kDefaultReferenceHz);
  auto& ref = addRow("reference", doc::ParamKind::TuningReference, "Reference", rs);
  DragBox* rb = &ref.box();
  rb->setInfo("Reference Pitch", "The Hz of degree 0 (default C3 = 261.626 Hz): every note of this tuning moves with it. "
                                 "Drag (one octave per 120 px), type Hz, or Delete for the default.");
  rb->formatter = [](double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f Hz", std::exp2(v));
    return std::string(buf);
  };
  rb->parser = [](std::string_view t) -> std::optional<double> {
    const std::optional<double> hz = parseNumber(t);
    if (!hz || !(*hz > 0.0)) return std::nullopt;
    return std::log2(*hz);
  };
  bindParam(ref, doc::ParamKind::TuningReference, "Change Reference");
  rb->onChange = [c, id](double v) {
    if (Status s = c->store().dispatch(doc::SetTuningParam{id, doc::ParamKind::TuningReference, std::exp2(v)}); !s)
      c->setStatus("Tuning: " + s.error().message);
  };
  rb->setSpec(rb->spec());

  usage_ = &add<Label>("usage", "", FontRoleId::Small, TextTone::Secondary);
  usage_->setHeight(Length::fixed(kUsageH));
  height_ = 2 * kCardPad + kHeadH + kRowH + static_cast<float>(nParams) * kRowH + kUsageH + static_cast<float>(nParams + 2) * kCardGap;
  setHeight(Length::fixed(height_));
}

TuningParamRow* TuningCard::row(std::optional<doc::ParamKind> param) const noexcept {
  for (TuningParamRow* r : rows_)
    if (r->param() == param) return r;
  return nullptr;
}

void TuningCard::refresh(const doc::Project& p, const doc::TuningSlot& slot, size_t index, bool full) {
  if (!name_->focused() && name_->text() != slot.name) name_->setText(slot.name);
  static_cast<Swatch*>(swatch_)->setColour(slot.colour.rgb);
  if (colour_ != slot.colour.rgb) {
    colour_ = slot.colour.rgb;
    markDirty();
  }
  summary_->setText(tuningSummary(slot.def));
  remove_->setEnabled(p.tunings->size() > 1);
  for (TuningParamRow* r : rows_) {
    DragBox& b = r->box();
    if (!b.inGesture() && !b.editing()) {
      if (!r->param()) {
        if (const tu::Edo* e = slot.def.edo()) b.setValue(e->n());
      } else {
        const doc::ParamKind k = *r->param();
        if (k == doc::ParamKind::MosGenerator)
          if (const tu::MosParams* m = slot.def.mos(); m && b.spec().max != m->period) {
            ValueSpec s = b.spec();
            s.max = m->period;
            s.def = m->generator;
            b.setSpec(s);
          }
        double v = 0.0;
        switch (k) {
          case doc::ParamKind::TuningReference: v = std::log2(slot.def.referenceHz); break;
          case doc::ParamKind::TuningEquave: v = slot.def.edo() ? slot.def.edo()->equaveCents() : 0.0; break;
          case doc::ParamKind::MosGenerator: v = slot.def.mos() ? slot.def.mos()->generator : 0.0; break;
          case doc::ParamKind::MosPeriod: v = slot.def.mos() ? slot.def.mos()->period : 0.0; break;
          case doc::ParamKind::MosStretch: v = slot.def.mos() ? slot.def.mos()->octaveStretch : 0.0; break;
          default: break;
        }
        // Re-format the equave at an unchanged value: its text depends on the definition (an exact ratio "3/1" or cents).
        if (k == doc::ParamKind::TuningEquave && b.value() == v && b.valueText() != equaveText(&slot.def, v)) b.setSpec(b.spec());
        b.setValue(v);
      }
    }
    if (full) r->setAutomated(r->param() && findAutomationLane(p, doc::ParamRef::tuning(*r->param(), id_)).lane != nullptr);
  }
  if (!full) return;  // a parameter drag: nobody's assignment or lanes changed
  // Who uses it.
  uint32_t tracks = 0, clips = 0;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (t.tuning.mode == doc::TuningMode::Main && t.tuning.main == id_) ++tracks;
    for (const doc::ClipSlot& cl : t.clips)
      if (const doc::TuningAssignment& a = cl.audio ? cl.audio->tuning : cl.clip->tuning;
          a.mode == doc::TuningMode::Main && a.main == id_)
        ++clips;
  });
  std::string u = index == 0 ? "Default" : "";
  auto part = [&](uint32_t n, const char* one, const char* many) {
    if (!n) return;
    if (!u.empty()) u += u == "Default" ? " \xC2\xB7 " : ", ";
    u += std::to_string(n) + " " + (n == 1 ? one : many);
  };
  part(tracks, "track", "tracks");
  part(clips, "clip", "clips");
  if (u.empty()) u = "Not assigned";
  usage_->setText(std::move(u));
}

void TuningCard::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->surface.raised);
  // The tuning's colour as a strip on the left edge (as the clip box's title bar carries the clip colour).
  ctx.dl.addRectFilled(IRect{r.x, r.y, ctx.scale.px(2.0f), r.h}.toRect(), hex(colour_));
}

// --- panel -----------------------------------------------------------------------------------------------------------

TuningsPanel::TuningsPanel(Shell& shell) : Widget("tunings-panel"), shell_(shell), ctx_(shell.context()) {
  setLayout(LayoutKind::Column);
  auto& header = add<Widget>("header");
  header.setLayout(LayoutKind::Row);
  header.setHeight(Length::fixed(kPanelHeader));
  header.setPadding({8, 4, 6, 4});
  header.setAlign(Align::Start, Align::Center);
  auto& title = header.add<Label>("title", "Tunings", FontRoleId::Title, TextTone::Primary);
  title.setWidth(Length::flex(1));
  auto& add = header.add<Button>("new", "New");
  add.setSize(Length::fixed(44), Length::fixed(18));
  add.setFocusable(false);
  add.setInfo("New Main Tuning", "Adds a main tuning (12-EDO) that tracks and clips can follow; each main tuning is its own "
                                 "automatable signal (Create \xE2\x86\x92 New Main Tuning).");
  add.onClick = [this] { (void)addTuning(); };
  auto& scroll = this->add<ScrollView>("list", false, true);
  scroll.setHeight(Length::flex(1));
  scroll.setWheelStep(40.0f);
  list_ = &scroll.add<Widget>("cards");
  list_->setLayout(LayoutKind::Column);
  list_->setPadding({6, 0, 6, 6});
  list_->setGap(6);

  // A tuning or who uses it can change only on these events (the tuning chip's filter); hidden, the panel only marks
  // itself stale and catches up when it is shown.
  docSub_ = ctx_.subscribe([this](const doc::StoreEvent& e) {
    if (!TuningChip::tuningMayChange(e)) return;
    // A parameter drag (Param only) changes values, not who uses a tuning or which lanes exist.
    const bool full = staleFull_ || e.kind == doc::StoreEventKind::Reset || !e.changes.paramOnly();
    stale_ = true;
    staleFull_ = full;
    if (ctx_.tuningsVisible()) refresh(full);
  });
  ctx_.setViewAction("tuning.newMain", [this] { (void)addTuning(); });
  viewSub_ = ctx_.observe([this](uint32_t bits) {
    if ((bits & view_change::kLayout) && ctx_.tuningsVisible() && stale_) refresh();
  });
  refresh();
}

TuningsPanel::~TuningsPanel() { ctx_.clearViewAction("tuning.newMain"); }

TuningCard* TuningsPanel::card(doc::TuningId id) const noexcept {
  for (TuningCard* c : cards_)
    if (c->id() == id) return c;
  return nullptr;
}

void TuningsPanel::rebuild(const doc::Project& p) {
  list_->clearChildren();
  cards_.clear();
  built_.clear();
  float h = 6.0f;
  for (size_t i = 0; i < p.tunings->size(); ++i) {
    const doc::TuningSlot& s = *(*p.tunings)[i];
    auto& c = list_->add<TuningCard>("tuning[" + std::to_string(i) + "]", *this, s);
    cards_.push_back(&c);
    built_.emplace_back(s.id.value, s.def.system.index());
    h += c.logicalHeight() + 6.0f;
  }
  list_->setHeight(Length::fixed(h));
}

void TuningsPanel::refresh(bool full) {
  stale_ = false;
  staleFull_ = false;
  const doc::Project& p = *ctx_.doc();
  bool same = built_.size() == p.tunings->size();
  for (size_t i = 0; same && i < built_.size(); ++i)
    same = built_[i].first == (*p.tunings)[i]->id.value && built_[i].second == (*p.tunings)[i]->def.system.index();
  if (!same) {
    rebuild(p);
    full = true;
  }
  for (size_t i = 0; i < cards_.size(); ++i) cards_[i]->refresh(p, *(*p.tunings)[i], i, full);
}

void TuningsPanel::reveal(doc::TuningId id) {
  TuningCard* c = card(id);
  auto* scroll = list_ ? dynamic_cast<ScrollView*>(list_->parent()) : nullptr;
  if (!c || !scroll || !tree()) return;
  tree()->layoutNow();
  const int32_t top = c->rect().y - list_->rect().y, h = c->rect().h, view = scroll->rect().h;
  const int32_t y = scroll->scrollY();
  if (top < y) scroll->scrollTo(0, top);
  else if (top + h > y + view) scroll->scrollTo(0, top + h - view);
  tree()->layoutNow();
}

void TuningsPanel::refreshLater() {
  refreshPending_ = true;
  requestFrame();
}

void TuningsPanel::onFrame(double) {
  if (!refreshPending_) return;
  refreshPending_ = false;
  if (ctx_.tuningsVisible()) refresh(true);
  else stale_ = staleFull_ = true;
}

doc::TuningId TuningsPanel::addTuning() {
  const doc::Project& p = *ctx_.doc();
  doc::Colour colour{0x808080};
  if (const PaintResources* res = tree() ? tree()->resources() : nullptr; res && res->theme && !res->theme->clipPalette.empty()) {
    const size_t n = std::min<size_t>(res->theme->clipPalette.size(), kClipPaletteColumns - 1);
    colour.rgb = paint::rgbOf(res->theme->clipPalette[(p.tunings->size() * 3 + 2) % n]);
  }
  doc::TuningDef def;  // 12-EDO at C3
  const auto e = doc::ops::addMainTuning(p, ctx_.store().ids(), nextTuningName(p), std::move(def), colour);
  if (!e) {
    ctx_.setStatus("New Tuning: " + e.error().message);
    return {};
  }
  if (Status s = ctx_.store().dispatch(e->command); !s) {
    ctx_.setStatus("New Tuning: " + s.error().message);
    return {};
  }
  const doc::TuningId id = e->tunings.front();
  ctx_.setTuningsVisible(true);
  refresh();
  if (TuningCard* c = card(id); c && tree()) {
    reveal(id);
    tree()->setFocus(&c->nameField());
    c->nameField().buffer().selectAll();
    c->nameField().markDirty();
  }
  return id;
}

bool TuningsPanel::removeTuning(doc::TuningId id) {
  const doc::Project& p = *ctx_.doc();
  const doc::TuningSlot* s = doc::findTuning(p, id);
  if (!s) return false;
  const std::string name = s->name;
  const auto e = doc::ops::removeMainTuning(p, id);
  if (!e) {
    ctx_.setStatus("Delete Tuning: " + e.error().message);
    return false;
  }
  const doc::TuningSlot* to = doc::findTuning(p, e->tunings.front());
  const std::string toName = to ? to->name : std::string();
  const size_t moved = e->tracks.size();
  if (Status st = ctx_.store().dispatch(e->command); !st) {
    ctx_.setStatus("Delete Tuning: " + st.error().message);
    return false;
  }
  ctx_.setStatus("Deleted " + quoted(name) + (moved ? "; its tracks and clips now follow " + quoted(toName) : std::string()));
  return true;
}

bool TuningsPanel::showAutomationLane(const doc::ParamRef& r) {
  auto* view = dynamic_cast<ArrangementView*>(shell_.slot(ShellSlot::TrackArea).find("arrangement"));
  if (!view) return false;
  const doc::Project& p = *ctx_.doc();
  // Where it is automated, else the selected track, else the first MIDI track that follows the tuning, else the first one.
  doc::TrackId t;
  if (const LaneLookup l = findAutomationLane(p, r); l.lane) t = l.owner;
  if (!t.valid() && ctx_.selectedTrack().valid() && doc::findTrackPtr(p, ctx_.selectedTrack())) t = ctx_.selectedTrack();
  doc::TrackId firstMidi;
  doc::forEachTrack(p, [&](const doc::Track& tr, const doc::Track*) {
    if (tr.kind != doc::TrackKind::Midi) return;
    if (!firstMidi.valid()) firstMidi = tr.id;
    if (!t.valid() && tr.tuning.mode == doc::TuningMode::Main && tr.tuning.main.value == r.owner) t = tr.id;
  });
  if (!t.valid()) t = firstMidi;
  if (!t.valid()) {
    ctx_.setStatus("Automation: add a MIDI track to hold the lane");
    return false;
  }
  ArrangementAutomation& au = view->automation();
  au.showLanes(t, true);
  std::vector<doc::ParamRef> shown = au.shownLanes(t);
  if (std::find(shown.begin(), shown.end(), r) == shown.end()) {
    if (au.addLane(t)) shown = au.shownLanes(t);
    au.setLaneParam(t, shown.empty() ? 0 : shown.size() - 1, r);
  }
  return true;
}

void TuningsPanel::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.panel);
}

InfoText TuningsPanel::info() const {
  return {"Tunings", "The project's main tunings. Tracks and clips follow one (right-click a track's tuning chip to "
                     "assign it); each is its own signal, so automating one re-voices only what follows it. Right-click a "
                     "parameter to show its automation lane."};
}

TuningsPanel& installTuningsPanel(Shell& shell) { return shell.slot(ShellSlot::Tunings).add<TuningsPanel>(shell); }

}  // namespace pq::views
