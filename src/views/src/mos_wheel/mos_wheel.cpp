// MOS wheel popover and tuning chip (T3.14). See pq/views/mos_wheel/mos_wheel.h.

#include "pq/views/mos_wheel/mos_wheel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>

#include "../common/paint.h"
#include "pq/tuning/mos_data.h"
#include "pq/tuning/scala.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"

namespace pq::views {

using namespace pq::ui;
namespace tu = pq::tuning;

// --- helpers -------------------------------------------------------------------------------------------------------

namespace {

const doc::NoteClip* findClip(const doc::Project& p, const doc::ClipRef& ref, std::optional<doc::TrackPath>& path) {
  path = doc::findTrack(p, ref.track);
  if (!path) return nullptr;
  const doc::Track& t = doc::trackAt(p, *path);
  const int32_t i = doc::findClipIndex(t, ref.clip);
  return i >= 0 ? t.clips[static_cast<size_t>(i)].clip.get() : nullptr;
}

std::string quoted(std::string_view s) { return "\xE2\x80\x9C" + std::string(s) + "\xE2\x80\x9D"; }

}  // namespace

const doc::TuningDef* targetTuning(const doc::Project& p, const TuningTarget& t) {
  switch (t.kind) {
    case TuningTarget::Kind::Clip: {
      std::optional<doc::TrackPath> path;
      const doc::NoteClip* c = findClip(p, t.clip, path);
      return c ? doc::effectiveTuning(p, *path, c) : nullptr;
    }
    case TuningTarget::Kind::Track: {
      const std::optional<doc::TrackPath> path = doc::findTrack(p, t.track);
      return path ? doc::effectiveTuning(p, *path, nullptr) : nullptr;
    }
    case TuningTarget::Kind::Main: {
      const doc::TuningSlot* slot = doc::findTuning(p, t.main);
      return slot ? &slot->def : nullptr;
    }
  }
  return nullptr;
}

doc::Command tuningCommand(const TuningTarget& t, doc::TuningDef def) {
  switch (t.kind) {
    case TuningTarget::Kind::Clip: return doc::SetClipTuning{t.clip, doc::TuningAssignment::privateTuning(std::move(def))};
    case TuningTarget::Kind::Track: return doc::SetTrackTuning{t.track, doc::TuningAssignment::privateTuning(std::move(def))};
    case TuningTarget::Kind::Main: break;
  }
  return doc::SetTuningDef{t.main, std::move(def)};
}

std::string tuningTargetLabel(const doc::Project& p, const TuningTarget& t) {
  switch (t.kind) {
    case TuningTarget::Kind::Clip: {
      std::optional<doc::TrackPath> path;
      const doc::NoteClip* c = findClip(p, t.clip, path);
      return "Clip " + quoted(c && !c->name.empty() ? std::string_view(c->name) : std::string_view("MIDI Clip"));
    }
    case TuningTarget::Kind::Track: {
      const doc::Track* tr = doc::findTrackPtr(p, t.track);
      return "Track " + quoted(tr ? std::string_view(tr->name) : std::string_view("?"));
    }
    case TuningTarget::Kind::Main: {
      const doc::TuningSlot* slot = doc::findTuning(p, t.main);
      return "Main tuning " + quoted(slot ? std::string_view(slot->name) : std::string_view("?"));
    }
  }
  return {};
}

std::string tuningSummary(const doc::TuningDef& def) {
  char buf[96];
  if (const tu::Edo* e = def.edo()) {
    const auto r = e->equaveRatio();
    if (r && *r == tu::Rational{2}) {
      std::snprintf(buf, sizeof buf, "%u-EDO", e->n());
    } else {
      std::snprintf(buf, sizeof buf, "%u-ED %.2f\xC2\xA2", e->n(), e->equaveCents());
    }
    return buf;
  }
  if (const tu::MosParams* m = def.mos()) {
    // The family from the step counts (largest / smallest step classes of the unrotated scale).
    const tu::RegionHint hint = m->region.value_or(tu::RegionHint{});
    const tu::MosScale sc = tu::buildMosScale(m->generator, m->n, m->period, m->region ? &hint : nullptr);
    int32_t nL = 0, nS = 0;
    for (double st : sc.steps) (std::fabs(st - sc.L) < 0.005 ? nL : nS) += 1;
    if (sc.L == sc.s) nS = 0, nL = m->n;
    const int32_t reps = std::max(1, m->periodsPerEquave);
    std::string s = tu::mosDisplayLabel({nL * reps, nS * reps});
    std::snprintf(buf, sizeof buf, " \xC2\xB7 %.2f\xC2\xA2", m->generator);
    s += buf;
    if (m->rotation != 0) s += " \xC2\xB7 mode " + std::to_string(m->rotation);
    if (tu::hasModmos(m->modmos)) s += " \xC2\xB7 MODMOS";
    return s;
  }
  if (const doc::ScaleDef* sd = def.scale()) {
    std::string d = sd->scale.descriptionUtf8();
    if (d.empty()) d = std::to_string(sd->scale.pitches.size()) + "-note scale";
    return "Scala: " + d;
  }
  return "Tuning";
}

TuningTarget defaultTuningTarget(const ViewContext& ctx) {
  const doc::ClipRef& c = ctx.detailClip();
  if (c.clip.valid()) {
    std::optional<doc::TrackPath> path;
    if (findClip(*ctx.doc(), c, path)) return TuningTarget::forClip(c);
  }
  const doc::Project& p = *ctx.doc();
  return TuningTarget::forMain(p.tunings->empty() ? doc::TuningId{} : p.tunings->front()->id);
}

// --- popover -------------------------------------------------------------------------------------------------------

MosWheelPopover::MosWheelPopover(ViewContext& ctx, const TuningTarget& target)
    : Popover("mos-wheel"), ctx_(ctx), target_(target) {
  setSize(Length::fixed(kWidth), Length::fixed(kHeight));
  if (const doc::TuningDef* d = targetTuning(*ctx_.doc(), target_)) original_ = *d;
  draft_ = original_;

  MosChartConfig cfg;
  if (const tu::MosParams* m = original_.mos()) {
    cfg.period = m->period;
    cfg.periodsPerEquave = std::max(1, m->periodsPerEquave);
    cfg.octaveStretch = m->octaveStretch;
  }

  // Header: title, EDO entry, Scala, temperament presets.
  auto& header = add<Widget>("header");
  header.setLayout(LayoutKind::Row);
  header.setHeight(Length::fixed(22));
  header.setGap(6);
  title_ = &header.add<Label>("title", "Tuning \xC2\xB7 " + tuningTargetLabel(*ctx_.doc(), target_), FontRoleId::Title, TextTone::Primary);
  auto& edoLabel = header.add<Label>("edo-label", "EDO", FontRoleId::Small, TextTone::Secondary, TextAlign::Right);
  edoLabel.setWidth(Length::fixed(30));
  edo_ = &header.add<TextEdit>("edo");
  edo_->setWidth(Length::fixed(48));
  edo_->setPlaceholder("n");
  edo_->setInfo("EDO", "Type a number of equal divisions of the octave and press Enter.");
  edo_->onCommit = [this](const std::string& t) {
    int32_t n = 0;
    if (std::sscanf(t.c_str(), "%d", &n) == 1 && n >= 1 && n <= 9999) chooseEdo(n);
  };
  auto& scala = header.add<Button>("scala", "Scala\xE2\x80\xA6");
  scala.setWidth(Length::fixed(64));
  scala.setInfo("Scala File", "Imports a Scala scale (.scl) as this tuning.");
  scala.onClick = [this] { requestScalaFile(); };
  std::vector<std::string> items{"Temperament\xE2\x80\xA6"};
  const auto temps = tu::temperaments();
  for (size_t i = 0; i < temps.size(); ++i) {
    if (temps[i].disabled) continue;
    items.emplace_back(temps[i].name);
    presetIndex_.push_back(static_cast<int32_t>(i));
  }
  presets_ = &header.add<Dropdown>("presets", std::move(items), 0);
  presets_->setWidth(Length::fixed(170));
  presets_->setInfo("Temperament Presets", "Selects a temperament's MOS with the generator in the middle of its range.");
  presets_->onChange = [this](int32_t i) {
    if (i > 0 && static_cast<size_t>(i - 1) < presetIndex_.size()) chooseTemperament(presetIndex_[static_cast<size_t>(i - 1)]);
  };

  // Body: the chart and the circle.
  auto& body = add<Widget>("body");
  body.setLayout(LayoutKind::Row);
  body.setGap(6);
  chart_ = &body.add<MosChart>("chart", cfg);
  circle_ = &body.add<MosCircle>("circle", cfg);
  circle_->setWidth(Length::fixed(kCircleWidth));
  chart_->setOnChange([this](const std::optional<MosSelection>& s, MosGesture g) { onChart(s, g); });
  circle_->setOnChange([this](const std::optional<MosSelection>& s, std::span<const double> mm, MosGesture g) { onCircle(s, mm, g); });
  circle_->onAudition = [this](double cents) {
    const double hz = draft_.referenceHz * std::exp2(cents / 1200.0);
    ctx_.transport().auditionHz(static_cast<float>(hz), 100.0f, 400.0f);
  };

  // Footer: what would be committed, Cancel / Apply.
  auto& footer = add<Widget>("footer");
  footer.setLayout(LayoutKind::Row);
  footer.setHeight(Length::fixed(22));
  footer.setGap(6);
  summary_ = &footer.add<Label>("summary", tuningSummary(draft_), FontRoleId::Small, TextTone::Secondary);
  auto& cancelBtn = footer.add<Button>("cancel", "Cancel");
  cancelBtn.setWidth(Length::fixed(64));
  cancelBtn.onClick = [this] { cancel(); };
  auto& applyBtn = footer.add<Button>("apply", "Apply");
  applyBtn.setWidth(Length::fixed(64));
  applyBtn.onClick = [this] { apply(); };

  showDraft();
}

MosWheelPopover::~MosWheelPopover() {
  *alive_ = false;
  if (!done_) {
    done_ = true;
    ctx_.clearTuningPreview();
  }
}

// The chart, circle and fields showing the draft (no draft change, no preview).
void MosWheelPopover::showDraft() {
  if (const tu::MosParams* m = draft_.mos()) {
    // The region of n notes per period containing the generator (the stored region hint first).
    const tu::MosRegionSet& rs = chart_->regionSet();
    const tu::MosRegion* best = nullptr;
    for (const tu::MosRegion& r : rs.regions) {
      if (r.perPeriodSize != m->n || m->generator < r.lowCents || m->generator > r.highCents) continue;
      if (m->region && std::fabs(r.lowCents - m->region->lowCents) < 0.01 && std::fabs(r.highCents - m->region->highCents) < 0.01) {
        best = &r;
        break;
      }
      if (!best) best = &r;
    }
    if (best && chart_->select(best->perPeriodKey, m->generator, m->rotation)) {
      // select() clamps into the region and reduces; the circle takes the chart's selection with the exact value.
      std::optional<MosSelection> sel = chart_->selection();
      if (sel) sel->generatorCents = std::clamp(m->generator, sel->regionLow, sel->regionHigh);
      circle_->setSelection(sel);
      circle_->setModmos(m->modmos);
      focusChart();
    }
    edo_->setText({});
  } else {
    chart_->clearSelection();
    circle_->setSelection(std::nullopt);
    circle_->setModmos({});
    edo_->setText(draft_.edo() ? std::to_string(draft_.edo()->n()) : std::string());
  }
  summary_->setText(tuningSummary(draft_));
}

void MosWheelPopover::focusChart() {
  const std::optional<MosSelection>& sel = chart_->selection();
  if (!sel) return;
  const double w = sel->regionHigh - sel->regionLow;
  chart_->setView(sel->regionLow - 1.5 * w, sel->regionHigh + 1.5 * w);
}

void MosWheelPopover::setDraft(doc::TuningDef def) {
  draft_ = std::move(def);
  if (changed()) ctx_.setTuningPreview(target_, draft_);
  else ctx_.clearTuningPreview();
  summary_->setText(tuningSummary(draft_));
}

void MosWheelPopover::draftFromCircle() {
  const std::optional<tu::MosParams> p = circle_->params();
  if (!p) {
    setDraft(original_);
    return;
  }
  doc::TuningDef d;
  d.system = *p;
  d.referenceHz = original_.referenceHz;
  setDraft(std::move(d));
}

void MosWheelPopover::onChart(const std::optional<MosSelection>& sel, MosGesture) {
  circle_->setSelection(sel);
  if (sel) draftFromCircle();
  else setDraft(original_);  // nothing selected: the target keeps its tuning
  if (sel) edo_->setText({});
}

void MosWheelPopover::onCircle(const std::optional<MosSelection>& sel, std::span<const double>, MosGesture) {
  if (sel) chart_->select(sel->perPeriodKey, sel->generatorCents, sel->modeRotation);
  draftFromCircle();
}

bool MosWheelPopover::chooseEdo(int32_t n) {
  if (n < 1) return false;
  Result<tu::Edo> e = Error{ErrorCode::InvalidArgument, "", {}};
  const tu::Edo* old = original_.edo();
  if (old && old->equaveRatio()) e = tu::Edo::make(static_cast<uint32_t>(n), *old->equaveRatio());
  else if (old) e = tu::Edo::makeCents(static_cast<uint32_t>(n), old->equaveCents());
  else e = tu::Edo::make(static_cast<uint32_t>(n));
  if (!e) return false;
  doc::TuningDef d;
  d.system = *e;
  d.referenceHz = original_.referenceHz;
  chart_->clearSelection();
  circle_->setSelection(std::nullopt);
  edo_->setText(std::to_string(n));
  setDraft(std::move(d));
  return true;
}

Status MosWheelPopover::chooseScalaFile(std::string_view pathUtf8) {
  const std::filesystem::path path(std::u8string(reinterpret_cast<const char8_t*>(pathUtf8.data()), pathUtf8.size()));
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ctx_.setStatus("Scala: cannot open " + std::string(pathUtf8));
    return Error{ErrorCode::NotFound, "cannot open " + std::string(pathUtf8), {}};
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::u8string leaf = path.filename().u8string();
  Result<tu::ScalaScale> scl = tu::parseScl(text, std::string_view(reinterpret_cast<const char*>(leaf.data()), leaf.size()));
  if (!scl) {
    ctx_.setStatus("Scala: " + scl.error().message);
    return std::move(scl).error();
  }
  Result<doc::TuningDef> def = doc::makeScaleDef(std::move(*scl), std::nullopt, original_.referenceHz);
  if (!def) {
    ctx_.setStatus("Scala: " + def.error().message);
    return std::move(def).error();
  }
  chart_->clearSelection();
  circle_->setSelection(std::nullopt);
  edo_->setText({});
  setDraft(std::move(*def));
  return Ok{};
}

void MosWheelPopover::requestScalaFile() {
  FileDialogs* d = ctx_.fileDialogs();
  if (!d) {
    ctx_.setStatus("Scala: no file dialog available");
    return;
  }
  // The dialog may answer after the popover closed: the token says whether `this` still exists.
  std::shared_ptr<bool> alive = alive_;
  d->chooseScalaFile([this, alive](std::string path) {
    if (*alive && !path.empty()) (void)chooseScalaFile(path);
  });
}

bool MosWheelPopover::chooseMos(tu::MosKey perPeriodKey, double generatorCents, int32_t modeRotation, std::vector<double> modmos) {
  if (!chart_->select(perPeriodKey, generatorCents, modeRotation)) return false;
  circle_->setSelection(chart_->selection());
  circle_->setModmos(std::move(modmos));
  focusChart();
  edo_->setText({});
  draftFromCircle();
  return true;
}

bool MosWheelPopover::chooseTemperament(int32_t index) {
  const auto temps = tu::temperaments();
  if (index < 0 || static_cast<size_t>(index) >= temps.size()) return false;
  const tu::Temperament& t = temps[static_cast<size_t>(index)];
  const MosChartConfig before = chart_->config();
  MosChartConfig cfg = before;
  const double ratio = 1200.0 / t.period;
  cfg.period = t.period;
  cfg.periodsPerEquave = std::fabs(ratio - std::round(ratio)) < 1e-6 ? std::max(1, static_cast<int32_t>(std::lround(ratio))) : 1;
  chart_->setConfig(cfg);
  circle_->setConfig(cfg);
  const double mid = (t.genLow + t.genHigh) / 2.0;
  const int32_t reps = cfg.periodsPerEquave;
  for (const tu::MosKey& k : t.mos) {
    // Tagged with whole-scale counts; a multi-period chart takes counts per period.
    for (const tu::MosKey pk : {tu::MosKey{k.nL / reps, k.nS / reps}, k}) {
      if (!(pk.nL * reps == k.nL && pk.nS * reps == k.nS) && !(pk == k)) continue;
      if (pk.size() < 2) continue;
      if (!chart_->select(pk, mid, 0)) continue;
      const std::optional<MosSelection>& sel = chart_->selection();
      if (!sel || mid < sel->regionLow - 1e-9 || mid > sel->regionHigh + 1e-9) continue;
      circle_->setSelection(sel);
      circle_->setModmos({});
      edo_->setText({});
      focusChart();
      draftFromCircle();
      return true;
    }
  }
  chart_->setConfig(before);  // back to what the draft is
  circle_->setConfig(before);
  showDraft();
  ctx_.setStatus("Temperament " + std::string(t.name) + ": no MOS of it within the chart's sizes");
  return false;
}

void MosWheelPopover::commit() {
  if (!changed()) return;
  if (!targetTuning(*ctx_.doc(), target_)) {
    ctx_.setStatus("Tuning: the clip or track no longer exists");
    return;
  }
  if (Status st = ctx_.store().dispatch(tuningCommand(target_, draft_)); !st) ctx_.setStatus("Tuning: " + st.error().message);
}

void MosWheelPopover::closed() {
  if (done_) return;
  done_ = true;
  if (!cancelled_) commit();
  ctx_.clearTuningPreview();
}

void MosWheelPopover::apply() {
  if (done_) return;
  if (OverlayLayer* layer = OverlayLayer::of(*this); layer && layer->isOpen(*this)) layer->close(*this);  // → closed()
  else closed();
}

void MosWheelPopover::cancel() {
  if (done_) return;
  cancelled_ = true;
  apply();
}

bool MosWheelPopover::onKey(const KeyEvent& e) {
  if (!e.down) return false;
  if (e.key == key::kEscape) {
    cancel();
    return true;
  }
  if (e.key == key::kReturn) {
    apply();
    return true;
  }
  return false;
}

InfoText MosWheelPopover::info() const {
  return {"MOS Wheel", "Choose a tuning for " + tuningTargetLabel(*ctx_.doc(), target_) +
                           ": a MOS in the chart and the circle, an EDO, a Scala file or a temperament. The piano roll "
                           "previews it; Apply, Enter or a click outside commits it (one undo step), Esc cancels."};
}

MosWheelPopover* openMosWheel(Widget& from, ViewContext& ctx, const TuningTarget& target, IRect anchor) {
  OverlayLayer* layer = OverlayLayer::of(from);
  if (!layer) return nullptr;
  if (MosWheelPopover* open = openMosWheelPopover(from)) open->apply();  // one wheel at a time
  auto pop = std::make_unique<MosWheelPopover>(ctx, target);
  MosWheelPopover* raw = pop.get();
  OverlayOptions o;
  if (anchor.empty()) {
    UiTree* tree = layer->tree();
    const UiScale s(tree ? tree->scale() : 1.0f);
    const int32_t W = tree ? static_cast<int32_t>(tree->width()) : 0, H = tree ? static_cast<int32_t>(tree->height()) : 0;
    o.anchor = {std::max(0, (W - s.px(MosWheelPopover::kWidth)) / 2), std::max(0, (H - s.px(MosWheelPopover::kHeight)) / 2), 0, 0};
    o.placement = Placement::AtPoint;
  } else {
    o.anchor = anchor;
    o.placement = Placement::Below;
  }
  o.onClosed = [raw] { raw->closed(); };
  layer->open(std::move(pop), std::move(o));
  return raw;
}

MosWheelPopover* openMosWheelPopover(const Widget& w) {
  OverlayLayer* layer = OverlayLayer::of(w);
  return layer ? dynamic_cast<MosWheelPopover*>(layer->top()) : nullptr;
}

namespace {

// Holds the tuning.openMosWheel handler for the shell's lifetime (a hidden child of the clip detail slot).
class MosWheelHost : public Widget {
public:
  explicit MosWheelHost(Shell& shell) : Widget("mos-wheel-host"), ctx_(shell.context()) {
    setVisible(false);
    ui::OverlayLayer* layer = &shell.overlay();
    ctx_.setViewAction("tuning.openMosWheel", [this, layer] {
      if (MosWheelPopover* open = openMosWheelPopover(*layer)) {
        open->apply();  // the hotkey toggles the wheel
        return;
      }
      (void)openMosWheel(*layer, ctx_, defaultTuningTarget(ctx_));
    });
  }
  ~MosWheelHost() override { ctx_.clearViewAction("tuning.openMosWheel"); }

private:
  ViewContext& ctx_;
};

}  // namespace

void installMosWheel(Shell& shell) { shell.slot(ShellSlot::ClipDetail).add<MosWheelHost>(shell); }

// --- tuning chip ---------------------------------------------------------------------------------------------------

TuningChip::TuningChip(std::string name, ViewContext& ctx) : Control(std::move(name)), ctx_(ctx) {
  setFocusable(false);
  setInfo("Tuning", "The clip's tuning. Click to open the MOS wheel (Ctrl+Shift+W): MOS, EDO, Scala or a temperament.");
  docSub_ = ctx_.subscribe([this](const doc::StoreEvent&) {
    stale_ = true;
    markDirty();
  });
  viewSub_ = ctx_.observe([this](uint32_t bits) {
    if (bits & view_change::kTuningPreview) {
      stale_ = true;
      markDirty();
    }
  });
}

void TuningChip::setTarget(std::optional<TuningTarget> target) {
  const bool same = target.has_value() == target_.has_value() &&
                    (!target || (target->kind == target_->kind && target->clip.clip == target_->clip.clip &&
                                 target->clip.track == target_->clip.track && target->track == target_->track && target->main == target_->main));
  if (same) return;
  target_ = target;
  stale_ = true;
  markDirty();
}

void TuningChip::refresh() const {
  if (!stale_) return;
  stale_ = false;
  text_.clear();
  hasSwatch_ = false;
  if (!target_) return;
  const doc::Project& p = *ctx_.doc();
  const doc::TuningDef* def = nullptr;
  switch (target_->kind) {
    case TuningTarget::Kind::Clip: {
      std::optional<doc::TrackPath> path;
      if (const doc::NoteClip* c = findClip(p, target_->clip, path)) def = ctx_.previewedTuning(p, *path, c, target_->clip.clip);
      break;
    }
    case TuningTarget::Kind::Track:
      if (const auto path = doc::findTrack(p, target_->track)) def = ctx_.previewedTuning(p, *path, nullptr, {});
      break;
    case TuningTarget::Kind::Main: {
      const auto& pv = ctx_.tuningPreview();
      if (pv && pv->target.kind == TuningTarget::Kind::Main && pv->target.main == target_->main) def = &pv->def;
      else if (const doc::TuningSlot* s = doc::findTuning(p, target_->main)) def = &s->def;
      break;
    }
  }
  if (!def) return;
  for (const doc::TuningSlotPtr& slot : *p.tunings)
    if (slot->def == *def) {
      text_ = slot->name;
      swatch_ = rgba((slot->colour.rgb >> 16) & 0xFFu, (slot->colour.rgb >> 8) & 0xFFu, slot->colour.rgb & 0xFFu);
      hasSwatch_ = true;
      return;
    }
  text_ = tuningSummary(*def);
}

const std::string& TuningChip::text() const {
  refresh();
  return text_;
}

void TuningChip::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th || !target_) return;
  refresh();
  const IRect r = rect();
  const VisualState v = visualState();
  Rgba fill = th->field.fill;
  if (v.pressed) fill = paint::pressOf(fill);
  else if (v.hover) fill = paint::hoverOf(fill);
  paint::box(ctx.dl, r, ctx.scale.exact(th->radius.control), fill, th->field.border);
  int32_t x = r.x + ctx.scale.px(6.0f);
  if (hasSwatch_) {
    const float cr = ctx.scale.exact(3.5f);
    ctx.dl.addCircleFilled({static_cast<float>(x) + cr, static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f}, cr, swatch_);
    x += ctx.scale.px(11.0f);
  }
  // A small disclosure triangle on the right.
  const float tx = static_cast<float>(r.right()) - ctx.scale.exact(9.0f), ty = static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f;
  const float h = ctx.scale.exact(3.0f);
  ctx.dl.addTriangleFilled({tx - h, ty - h * 0.6f}, {tx + h, ty - h * 0.6f}, {tx, ty + h * 0.8f}, th->text.secondary);
  if (const ThemeFonts* f = ctx.res.uiFonts) {
    const IRect tr{x, r.y, std::max(0, static_cast<int32_t>(tx - h) - ctx.scale.px(4.0f) - x), r.h};
    ctx.dl.pushClip(r.toRect());
    paint::text(ctx, f->small, tr, text_, th->text.primary, TextAlign::Left);
    ctx.dl.popClip();
  }
}

bool TuningChip::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Left && target_) {
    setPressed(true);
    return true;
  }
  if (e.type == PointerType::Up && e.button == PointerButton::Left && pressedNow()) {
    setPressed(false);
    if (rect().contains(e.pos)) open();
    return true;
  }
  return false;
}

bool TuningChip::onKey(const KeyEvent& e) {
  if (e.down && (e.key == key::kReturn || e.key == ' ') && target_) {
    open();
    return true;
  }
  return false;
}

void TuningChip::open() {
  if (target_) (void)openMosWheel(*this, ctx_, *target_, rect());
}

}  // namespace pq::views
