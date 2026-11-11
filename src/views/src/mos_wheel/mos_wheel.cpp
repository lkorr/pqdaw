// MOS wheel popover and tuning chip (T3.14). See pq/views/mos_wheel/mos_wheel.h.

#include "pq/views/mos_wheel/mos_wheel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>

#include "../common/paint.h"
#include "circle_notes.h"
#include "pq/tuning/mos_data.h"
#include "pq/tuning/scala.h"
#include "pq/ui/overlay.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/library_load.h"
#include "pq/document/edit_ops.h"
#include "pq/document/pitch_resolve.h"
#include "pq/views/tunings/tunings_panel.h"

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

doc::TuningId targetTuningId(const doc::Project& p, const TuningTarget& t) {
  switch (t.kind) {
    case TuningTarget::Kind::Clip: {
      std::optional<doc::TrackPath> path;
      const doc::NoteClip* c = findClip(p, t.clip, path);
      return c ? doc::effectiveTuningRef(p, *path, c).main : doc::TuningId{};
    }
    case TuningTarget::Kind::Track: {
      const std::optional<doc::TrackPath> path = doc::findTrack(p, t.track);
      return path ? doc::effectiveTuningRef(p, *path, nullptr).main : doc::TuningId{};
    }
    case TuningTarget::Kind::Main: return doc::findTuning(p, t.main) ? t.main : doc::TuningId{};
    case TuningTarget::Kind::Project: return p.defaultTuning;
  }
  return {};
}

const doc::TuningDef* targetTuning(const doc::Project& p, const TuningTarget& t) {
  const doc::TuningSlot* slot = doc::findTuning(p, targetTuningId(p, t));
  return slot ? &slot->def : nullptr;
}

doc::Command tuningCommand(const doc::Project& p, const TuningTarget& t, doc::TuningDef def) {
  return doc::SetTuningDef{targetTuningId(p, t), std::move(def)};
}

std::string tuningTargetLabel(const doc::Project& p, const TuningTarget& t) {
  const doc::TuningSlot* slot = doc::findTuning(p, targetTuningId(p, t));
  const std::string_view name = slot ? std::string_view(slot->name) : std::string_view("?");
  switch (t.kind) {
    case TuningTarget::Kind::Clip: {
      std::optional<doc::TrackPath> path;
      const doc::NoteClip* c = findClip(p, t.clip, path);
      return "Clip " + quoted(c && !c->name.empty() ? std::string_view(c->name) : std::string_view("MIDI Clip")) +
             " \xC2\xB7 " + std::string(name);
    }
    case TuningTarget::Kind::Track: {
      const doc::Track* tr = doc::findTrackPtr(p, t.track);
      return "Track " + quoted(tr ? std::string_view(tr->name) : std::string_view("?")) + " \xC2\xB7 " + std::string(name);
    }
    case TuningTarget::Kind::Main: return "Main tuning " + quoted(name);
    case TuningTarget::Kind::Project: return "Project default \xC2\xB7 " + std::string(name);
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
    // L18: the circle header's numbering (genchain / brightness order, "Mode 3"), not the step rotation.
    if (m->rotation != 0)
      if (const auto gmn = tu::computeGmn(m->generator, m->n, m->rotation, m->period, m->region ? &hint : nullptr))
        s += " \xC2\xB7 " + tu::gmnLabel(*gmn);
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
  return TuningTarget::forProject();
}

// --- choosing (TU.3) -----------------------------------------------------------------------------------------------

namespace {

std::optional<doc::Command> assignCommand(const TuningTarget& t, doc::TuningId id) {
  const doc::TuningAssignment a = id.valid() ? doc::TuningAssignment::mainTuning(id) : doc::TuningAssignment::inherit();
  switch (t.kind) {
    case TuningTarget::Kind::Clip: return doc::Command{doc::SetClipTuning{t.clip, a}};
    case TuningTarget::Kind::Track: return doc::Command{doc::SetTrackTuning{t.track, a}};
    case TuningTarget::Kind::Project:
      if (id.valid()) return doc::Command{doc::SetDefaultTuning{id}};
      break;
    case TuningTarget::Kind::Main: break;
  }
  return std::nullopt;
}

// The target's own assignment: the clip's / track's (invalid: Inherit), the project's default; nullopt for a main tuning
// or a target that is gone.
std::optional<doc::TuningId> assignedTuning(const doc::Project& p, const TuningTarget& t) {
  auto idOf = [](const doc::TuningAssignment& a) { return a.mode == doc::TuningMode::Main ? a.main : doc::TuningId{}; };
  switch (t.kind) {
    case TuningTarget::Kind::Clip: {
      std::optional<doc::TrackPath> path;
      if (const doc::NoteClip* c = findClip(p, t.clip, path)) return idOf(c->tuning);
      return std::nullopt;
    }
    case TuningTarget::Kind::Track:
      if (const doc::Track* tr = doc::findTrackPtr(p, t.track)) return idOf(tr->tuning);
      return std::nullopt;
    case TuningTarget::Kind::Project: return p.defaultTuning;
    case TuningTarget::Kind::Main: break;
  }
  return std::nullopt;
}

std::string titleOf(std::string_view what, const Error& e) { return std::string(what) + ": " + e.message; }

}  // namespace

Status assignTuning(ViewContext& ctx, const TuningTarget& target, doc::TuningId id) {
  const std::optional<doc::Command> c = assignCommand(target, id);
  if (!c) return Error{ErrorCode::InvalidArgument, "this target chooses no tuning", {}};
  Status s = ctx.store().dispatch(*c);
  if (!s) ctx.setStatus(titleOf("Tuning", s.error()));
  return s;
}

doc::TuningId newTuningFor(const Widget& from, ViewContext& ctx, const TuningTarget& target) {
  const doc::Project& p = *ctx.doc();
  const doc::TuningDef* cur = targetTuning(p, target);
  if (!cur) {
    ctx.setStatus("New Tuning: the clip or track no longer exists");
    return {};
  }
  auto e = doc::ops::addMainTuning(p, ctx.store().ids(), nextTuningName(p), *cur, nextTuningColour(from, p));
  if (!e) {
    ctx.setStatus(titleOf("New Tuning", e.error()));
    return {};
  }
  const doc::TuningId id = e->tunings.front();
  doc::Batch b{"New Tuning", {e->command}};
  if (std::optional<doc::Command> a = assignCommand(target, id)) b.commands.push_back(std::move(*a));
  if (Status s = ctx.store().dispatch(doc::Command{std::move(b)}); !s) {
    ctx.setStatus(titleOf("New Tuning", s.error()));
    return {};
  }
  return id;
}

std::vector<MenuItem> tuningMenuItems(Widget& from, ViewContext& ctx, const TuningTarget& target, bool projectDefault) {
  const doc::Project& p = *ctx.doc();
  ViewContext* c = &ctx;
  Widget* w = &from;
  auto swatchOf = [](const doc::TuningSlot& s) {
    return rgba((s.colour.rgb >> 16) & 0xFFu, (s.colour.rgb >> 8) & 0xFFu, s.colour.rgb & 0xFFu);
  };
  // One item per project tuning: the dot, its name, its summary on the right (where a shortcut goes).
  auto tuningItems = [&](const TuningTarget& t, std::optional<doc::TuningId> checked) {
    std::vector<MenuItem> out;
    for (const doc::TuningSlotPtr& slot : *p.tunings) {
      const doc::TuningId id = slot->id;
      MenuItem m = MenuItem::item(slot->name, [c, t, id] { (void)assignTuning(*c, t, id); });
      if (const std::string sum = tuningSummary(slot->def); sum != slot->name) m.shortcut = sum;
      m.swatch = swatchOf(*slot);
      m.checked = checked && *checked == id;
      out.push_back(std::move(m));
    }
    return out;
  };
  std::vector<MenuItem> items;
  const std::optional<doc::TuningId> assigned = assignedTuning(p, target);
  if (target.kind == TuningTarget::Kind::Main) {
    const doc::TuningId id = target.main;
    MenuItem d = MenuItem::item("Set as Project Default", [c, id] { (void)assignTuning(*c, TuningTarget::forProject(), id); });
    d.checked = p.defaultTuning == id;
    items.push_back(std::move(d));
  } else {
    if (target.kind == TuningTarget::Kind::Clip || target.kind == TuningTarget::Kind::Track) {
      // Inherit names what it resolves to: the track's (a clip) or the group's / project default's (a track).
      std::string inheritLabel = "Inherit";
      if (const std::optional<doc::TrackPath> path = doc::findTrack(p, target.track)) {
        doc::TuningId up;
        if (target.kind == TuningTarget::Kind::Clip) {
          up = doc::effectiveTuningRef(p, *path, nullptr).main;
        } else if (const doc::Track* parent = doc::parentOf(p, *path)) {
          up = doc::effectiveTuningRef(p, *doc::findTrack(p, parent->id), nullptr).main;
        } else {
          up = p.defaultTuning;
        }
        if (const doc::TuningSlot* s = doc::findTuning(p, up)) inheritLabel += " (" + s->name + ")";
      }
      MenuItem inh = MenuItem::item(std::move(inheritLabel), [c, target] { (void)assignTuning(*c, target, {}); });
      inh.checked = assigned && !assigned->valid();
      items.push_back(std::move(inh));
    }
    for (MenuItem& m : tuningItems(target, assigned)) items.push_back(std::move(m));
  }
  items.push_back(MenuItem::item("New Tuning", [c, w, target] { (void)newTuningFor(*w, *c, target); }));
  if (projectDefault && target.kind != TuningTarget::Kind::Project)
    items.push_back(MenuItem::sub("Project Default", tuningItems(TuningTarget::forProject(), p.defaultTuning)));
  items.push_back(MenuItem::sep());
  items.push_back(MenuItem::item("MOS Wheel\xE2\x80\xA6", [c, w, target] { (void)openMosWheel(*w, *c, target, w->rect()); }));
  items.push_back(MenuItem::item("Save to Library", [c, target] {
    auto r = saveTuningToLibrary(*c, target);
    c->setStatus(r ? "Tuning saved: " + *r : "Save to Library: " + r.error().message);
  }));
  items.push_back(MenuItem::item("Hot-Swap Tuning\xE2\x80\xA6", [c, target] {
    HotSwapTarget t;
    t.kind = HotSwapTarget::Kind::Tuning;
    t.tuning = target;
    t.label = tuningTargetLabel(*c->doc(), target);
    if (!c->library().requestHotSwap(t)) c->setStatus("Hot-Swap needs the browser");
  }));
  return items;
}

void openTuningMenu(Widget& from, ViewContext& ctx, const TuningTarget& target, Vec2 at, bool projectDefault) {
  (void)openContextMenu(from, at, tuningMenuItems(from, ctx, target, projectDefault));
}

// --- live edits (TU.1) ---------------------------------------------------------------------------------------------

namespace {

// For a main tuning: the generator `def` differs from `current` by, when nothing else changes but what
// SetTuningParam(MosGenerator) changes with it (the mode, carried by transferModeRotation) and a region hint that is
// not needed (the generator is not at an end of the hint's region, where only the hint decides the degenerate side).
std::optional<double> generatorOnly(const doc::TuningDef& current, const doc::TuningDef& def) {
  const tu::MosParams* a = current.mos();
  const tu::MosParams* b = def.mos();
  if (!a || !b || current.referenceHz != def.referenceHz || a->generator == b->generator) return std::nullopt;
  doc::TuningDef c = current;
  tu::MosParams& m = std::get<tu::MosParams>(c.system);
  if (m.n > 1 && m.genSequence.empty())
    m.rotation = tu::transferModeRotation(m.generator, m.n, m.rotation, b->generator, m.n, m.period, m.region ? &*m.region : nullptr);
  m.generator = b->generator;
  if (b->region) {
    constexpr double kEdge = 1e-6;  // buildMOSScale's degenerate window is 1e-6/q ¢: at an end of the hint's region
    if (std::fabs(b->generator - b->region->lowCents) < kEdge || std::fabs(b->generator - b->region->highCents) < kEdge)
      return std::nullopt;
  }
  m.region = b->region;
  if (c == def) return b->generator;
  m.region = a->region;
  if (c == def) return b->generator;
  return std::nullopt;
}

}  // namespace

doc::Command TuningEdit::command(const doc::TuningDef& current, const doc::TuningDef& def) const {
  const doc::TuningId id = targetTuningId(*ctx_.doc(), target_);  // TU.3: always the shared named tuning
  if (const std::optional<double> g = generatorOnly(current, def)) return doc::SetTuningParam{id, doc::ParamKind::MosGenerator, *g};
  return doc::SetTuningDef{id, def};
}

Status TuningEdit::apply(const doc::TuningDef& def, MosGesture g, std::string_view label) {
  doc::DocumentStore& st = ctx_.store();
  if (g == MosGesture::Cancel) {
    cancel();
    return Ok{};
  }
  const doc::TuningDef* cur = targetTuning(*ctx_.doc(), target_);
  if (!cur) {
    finish();
    ctx_.setStatus("Tuning: the clip or track no longer exists");
    return Error{ErrorCode::NotFound, "tuning target not found", {}};
  }
  if (g == MosGesture::Live && !gesture_ && !st.inGesture()) gesture_ = st.beginGesture(std::string(label)).ok();
  Status s = Ok{};
  if (!(*cur == def)) s = st.dispatch(command(*cur, def));
  if (!s) ctx_.setStatus("Tuning: " + s.error().message);
  if (g == MosGesture::Commit) finish();
  return s;
}

void TuningEdit::cancel() {
  if (!gesture_) return;
  gesture_ = false;
  if (ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
}

void TuningEdit::finish() {
  if (!gesture_) return;
  gesture_ = false;
  if (ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
}

// --- popover -------------------------------------------------------------------------------------------------------

MosWheelPopover::MosWheelPopover(ViewContext& ctx, const TuningTarget& target)
    : Popover("mos-wheel"), ctx_(ctx), target_(target), edit_(std::make_unique<TuningEdit>(ctx, target)) {
  setSize(Length::fixed(kWidth), Length::fixed(kHeight));
  setPadding({12, 8, 12, 10});
  setGap(8);
  if (const doc::TuningDef* d = targetTuning(*ctx_.doc(), target_)) original_ = *d;
  draft_ = original_;

  MosChartConfig cfg;
  if (const tu::MosParams* m = original_.mos()) {
    cfg.period = m->period;
    cfg.periodsPerEquave = std::max(1, m->periodsPerEquave);
    cfg.octaveStretch = m->octaveStretch;
  }

  // Header: title, EDO entry, Scala, the temperament search.
  auto& header = add<Widget>("header");
  header.setLayout(LayoutKind::Row);
  header.setHeight(Length::fixed(24));
  header.setGap(6);
  header.setAlign(Align::Start, Align::Center);
  title_ = &header.add<Label>("title", tuningTargetLabel(*ctx_.doc(), target_), FontRoleId::Title, TextTone::Primary);
  title_->setWidth(Length::flex(1));
  auto& edoLabel = header.add<Label>("edo-label", "EDO", FontRoleId::Small, TextTone::Secondary, TextAlign::Right);
  edoLabel.setWidth(Length::fixed(28));
  edo_ = &header.add<TextEdit>("edo");
  edo_->setSize(Length::fixed(44), Length::fixed(20));
  edo_->setPlaceholder("n");
  edo_->setInfo("EDO", "Type a number of equal divisions of the octave and press Enter: sounding notes retune at once.");
  edo_->onCommit = [this](const std::string& t) {
    int32_t n = 0;
    if (std::sscanf(t.c_str(), "%d", &n) == 1 && n >= 1 && n <= 9999) chooseEdo(n);
  };
  auto& scala = header.add<Button>("scala", "Scala\xE2\x80\xA6");
  scala.setSize(Length::fixed(60), Length::fixed(20));
  scala.setFocusable(false);
  scala.setInfo("Scala File", "Imports a Scala scale (.scl) as this tuning.");
  scala.onClick = [this] { requestScalaFile(); };
  search_ = &header.add<TextEdit>("temperament");
  search_->setSize(Length::fixed(150), Length::fixed(20));
  search_->setPlaceholder("Temperament\xE2\x80\xA6");
  search_->setInfo("Find Temperament", "Type part of a temperament's name (meantone, porcupine, magic\xE2\x80\xA6) and press "
                                       "Enter: its MOS at the middle of its generator range. Several matches open a short list.");
  search_->onCommit = [this](const std::string& t) {
    if (!t.empty() && !findTemperament(t)) ctx_.setStatus("Temperament: no name contains \xE2\x80\x9C" + t + "\xE2\x80\x9D");
  };

  // Body: the chart and the circle.
  auto& body = add<Widget>("body");
  body.setLayout(LayoutKind::Row);
  body.setGap(8);
  body.setHeight(Length::flex(1));
  chart_ = &body.add<MosChart>("chart", cfg);
  circle_ = &body.add<MosCircle>("circle", cfg);
  circle_->setWidth(Length::fixed(kCircleWidth));
  chart_->setOnChange([this](const std::optional<MosSelection>& s, MosGesture g) { onChart(s, g); });
  circle_->setOnChange([this](const std::optional<MosSelection>& s, std::span<const double> mm, MosGesture g) { onCircle(s, mm, g); });
  circle_->onAudition = [this](double cents) {
    const double hz = draft_.referenceHz * std::exp2(cents / 1200.0);
    ctx_.transport().auditionHz(static_cast<float>(hz), 100.0f, 400.0f);
    circle_->noteOn(CircleNotes::auditionId(0), 0, hz, 0.4);  // TU.2: the tonic lights as it sounds
  };

  // Footer: what the target plays now, Revert / Done.
  auto& footer = add<Widget>("footer");
  footer.setLayout(LayoutKind::Row);
  footer.setHeight(Length::fixed(22));
  footer.setGap(6);
  footer.setAlign(Align::Start, Align::Center);
  summary_ = &footer.add<Label>("summary", tuningSummary(draft_), FontRoleId::Small, TextTone::Secondary);
  summary_->setWidth(Length::flex(1));
  revert_ = &footer.add<Button>("revert", "Revert");
  revert_->setSize(Length::fixed(64), Length::fixed(20));
  revert_->setFocusable(false);
  revert_->setInfo("Revert", "Puts back the tuning this had when the wheel opened (one undo step).");
  revert_->onClick = [this] { revert(); };
  auto& doneBtn = footer.add<Button>("done", "Done");
  doneBtn.setSize(Length::fixed(64), Length::fixed(20));
  doneBtn.setFocusable(false);
  doneBtn.setInfo("Done", "Closes the wheel. Every change is already applied (Ctrl+Z undoes one drag or click at a time).");
  doneBtn.onClick = [this] { apply(); };

  showDraft();
  notes_ = std::make_unique<CircleNotes>(ctx_, target_, *circle_, *this);
}

void MosWheelPopover::onFrame(double now) {
  if (notes_) notes_->frame(now);
}

MosWheelPopover::~MosWheelPopover() {
  *alive_ = false;
  done_ = true;
  edit_->finish();
}

// The chart, circle and fields showing the draft (no document change).
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
  syncFooter();
}

void MosWheelPopover::syncFooter() {
  summary_->setText(tuningSummary(draft_));
  revert_->setEnabled(changed());
}

void MosWheelPopover::focusChart() {
  const std::optional<MosSelection>& sel = chart_->selection();
  if (!sel) return;
  const double w = sel->regionHigh - sel->regionLow;
  chart_->setView(sel->regionLow - 1.5 * w, sel->regionHigh + 1.5 * w);
}

bool MosWheelPopover::setDraft(doc::TuningDef def, MosGesture g) {
  if (g == MosGesture::Cancel) {  // the drag's gesture is undone: show the document again
    edit_->cancel();
    if (const doc::TuningDef* d = targetTuning(*ctx_.doc(), target_)) draft_ = *d;
    showDraft();
    return true;
  }
  const bool ok = edit_->apply(def, g).ok();
  if (ok) draft_ = std::move(def);
  syncFooter();
  return ok;
}

bool MosWheelPopover::draftFromCircle(MosGesture g) {
  const std::optional<tu::MosParams> p = circle_->params();
  if (!p) return false;
  doc::TuningDef d;
  d.system = *p;
  d.referenceHz = draft_.referenceHz;
  return setDraft(std::move(d), g);
}

void MosWheelPopover::onChart(const std::optional<MosSelection>& sel, MosGesture g) {
  if (g == MosGesture::Cancel) {
    (void)setDraft(draft_, g);
    return;
  }
  circle_->setSelection(sel);
  if (!sel) return;  // nothing selected: the target keeps its tuning
  edo_->setText({});
  (void)draftFromCircle(g);
}

void MosWheelPopover::onCircle(const std::optional<MosSelection>& sel, std::span<const double>, MosGesture g) {
  if (g == MosGesture::Cancel) {
    (void)setDraft(draft_, g);
    return;
  }
  if (sel) chart_->select(sel->perPeriodKey, sel->generatorCents, sel->modeRotation);
  (void)draftFromCircle(g);
}

bool MosWheelPopover::chooseEdo(int32_t n) {
  if (n < 1) return false;
  Result<tu::Edo> e = Error{ErrorCode::InvalidArgument, "", {}};
  const tu::Edo* old = draft_.edo();
  if (old && old->equaveRatio()) e = tu::Edo::make(static_cast<uint32_t>(n), *old->equaveRatio());
  else if (old) e = tu::Edo::makeCents(static_cast<uint32_t>(n), old->equaveCents());
  else e = tu::Edo::make(static_cast<uint32_t>(n));
  if (!e) return false;
  doc::TuningDef d;
  d.system = *e;
  d.referenceHz = draft_.referenceHz;
  if (!setDraft(std::move(d), MosGesture::Commit)) return false;
  chart_->clearSelection();
  circle_->setSelection(std::nullopt);
  edo_->setText(std::to_string(n));
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
  Result<doc::TuningDef> def = doc::makeScaleDef(std::move(*scl), std::nullopt, draft_.referenceHz);
  if (!def) {
    ctx_.setStatus("Scala: " + def.error().message);
    return std::move(def).error();
  }
  if (!setDraft(std::move(*def), MosGesture::Commit)) return Error{ErrorCode::InvalidArgument, "the tuning was refused", {}};
  chart_->clearSelection();
  circle_->setSelection(std::nullopt);
  edo_->setText({});
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
  return draftFromCircle(MosGesture::Commit);
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
      search_->setText(std::string(t.name));
      focusChart();
      return draftFromCircle(MosGesture::Commit);
    }
  }
  chart_->setConfig(before);  // back to what the draft is
  circle_->setConfig(before);
  showDraft();
  ctx_.setStatus("Temperament " + std::string(t.name) + ": no MOS of it within the chart's sizes");
  return false;
}

std::vector<int32_t> MosWheelPopover::matchTemperaments(std::string_view text, size_t limit) {
  auto lower = [](std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
  };
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  if (text.empty()) return {};
  const std::string q = lower(text);
  std::vector<int32_t> exact, prefix, inside;
  const auto temps = tu::temperaments();
  for (size_t i = 0; i < temps.size(); ++i) {
    if (temps[i].disabled) continue;
    const std::string n = lower(temps[i].name);
    const size_t at = n.find(q);
    if (at == std::string::npos) continue;
    (n == q ? exact : at == 0 ? prefix : inside).push_back(static_cast<int32_t>(i));
  }
  exact.insert(exact.end(), prefix.begin(), prefix.end());
  exact.insert(exact.end(), inside.begin(), inside.end());
  if (exact.size() > limit) exact.resize(limit);
  return exact;
}

bool MosWheelPopover::findTemperament(std::string_view text) {
  const std::vector<int32_t> found = matchTemperaments(text);
  if (found.empty()) return false;
  const auto temps = tu::temperaments();
  auto sameName = [](std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
             return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
  };
  std::string_view q = text;
  while (!q.empty() && q.front() == ' ') q.remove_prefix(1);
  while (!q.empty() && q.back() == ' ') q.remove_suffix(1);
  if (found.size() == 1 || sameName(temps[static_cast<size_t>(found.front())].name, q)) return chooseTemperament(found.front());
  std::vector<MenuItem> items;
  std::shared_ptr<bool> alive = alive_;
  for (const int32_t i : found)
    items.push_back(MenuItem::item(std::string(temps[static_cast<size_t>(i)].name), [this, alive, i] {
      if (*alive) (void)chooseTemperament(i);
    }));
  const IRect r = search_->rect();
  (void)openContextMenu(*search_, {static_cast<float>(r.x), static_cast<float>(r.bottom())}, std::move(items));
  return true;
}

void MosWheelPopover::revert() {
  if (!changed()) return;
  edit_->finish();
  if (!targetTuning(*ctx_.doc(), target_)) {
    ctx_.setStatus("Tuning: the clip or track no longer exists");
    return;
  }
  if (Status st = ctx_.store().dispatch(tuningCommand(*ctx_.doc(), target_, original_)); !st) {
    ctx_.setStatus("Tuning: " + st.error().message);
    return;
  }
  if (const tu::MosParams* m = original_.mos()) {
    MosChartConfig cfg = chart_->config();
    cfg.period = m->period;
    cfg.periodsPerEquave = std::max(1, m->periodsPerEquave);
    cfg.octaveStretch = m->octaveStretch;
    chart_->setConfig(cfg);
    circle_->setConfig(cfg);
  }
  draft_ = original_;
  showDraft();
}

void MosWheelPopover::closed() {
  if (done_) return;
  done_ = true;
  edit_->finish();
}

void MosWheelPopover::apply() {
  if (done_) return;
  if (OverlayLayer* layer = OverlayLayer::of(*this); layer && layer->isOpen(*this)) layer->close(*this);  // → closed()
  else closed();
}

void MosWheelPopover::cancel() { apply(); }

bool MosWheelPopover::onKey(const KeyEvent& e) {
  if (!e.down) return false;
  if (e.key == key::kEscape || e.key == key::kReturn) {
    apply();
    return true;
  }
  return false;
}

InfoText MosWheelPopover::info() const {
  return {"MOS Wheel", "Choose a tuning for " + tuningTargetLabel(*ctx_.doc(), target_) +
                           ": a MOS in the chart and the circle, an EDO, a Scala file or a temperament. Every change "
                           "applies at once, so sounding notes retune while you drag (one undo step per drag); Revert "
                           "puts back the tuning it had when the wheel opened."};
}

MosWheelPopover* openMosWheel(Widget& from, ViewContext& ctx, const TuningTarget& target, IRect anchor) {
  OverlayLayer* layer = OverlayLayer::of(from);
  if (!layer) return nullptr;
  if (MosWheelPopover* open = openMosWheelPopover(from)) open->apply();  // one wheel at a time
  auto pop = std::make_unique<MosWheelPopover>(ctx, target);
  MosWheelPopover* raw = pop.get();
  OverlayOptions o;
  if (const std::optional<Vec2> at = MosWheelPopover::rememberedPosition()) {  // where the user last moved it
    const float sc = layer->tree() ? layer->tree()->scale() : 1.0f;
    o.anchor = {static_cast<int32_t>(std::lround(at->x * sc)), static_cast<int32_t>(std::lround(at->y * sc)), 0, 0};
    o.placement = Placement::AtPoint;
  } else if (anchor.empty()) {
    UiTree* tree = layer->tree();
    const UiScale s(tree ? tree->scale() : 1.0f);
    const int32_t W = tree ? static_cast<int32_t>(tree->width()) : 0, H = tree ? static_cast<int32_t>(tree->height()) : 0;
    o.anchor = {std::max(0, (W - s.px(MosWheelPopover::kWidth)) / 2), std::max(0, (H - s.px(MosWheelPopover::kHeight)) / 2), 0, 0};
    o.placement = Placement::AtPoint;
  } else {
    o.anchor = anchor;
    o.placement = Placement::Below;
  }
  // L17: kept inside the panels (below the control bar, above the status bar) wherever it fits.
  if (UiTree* tree = layer->tree()) o.within = tree->find("main/body");
  o.onClosed = [raw] { raw->closed(); };
  layer->open(std::move(pop), std::move(o));
  return raw;
}

namespace {
std::optional<Vec2> gWheelPos;  // logical px; UI thread only (the session's last wheel position)
}  // namespace

std::optional<Vec2> MosWheelPopover::rememberedPosition() noexcept { return gWheelPos; }
void MosWheelPopover::forgetPosition() noexcept { gWheelPos.reset(); }

namespace {
constexpr float kTitleBand = 30.0f;  // logical px at the top that move the popover
}  // namespace

Cursor MosWheelPopover::cursorAt(Vec2 pos) const {
  const float sc = tree() ? tree()->scale() : 1.0f;
  if (moving_ || pos.y < static_cast<float>(rect().y) + kTitleBand * sc) return Cursor::Move;
  return Popover::cursorAt(pos);
}

bool MosWheelPopover::onPointer(const PointerEvent& e) {
  UiTree* tree = this->tree();
  const float sc = tree ? tree->scale() : 1.0f;
  const IRect r = rect();
  if (e.type == PointerType::Down && e.button == PointerButton::Left && e.pos.y < static_cast<float>(r.y) + kTitleBand * sc) {
    moving_ = true;
    moveGrab_ = {e.pos.x - static_cast<float>(r.x), e.pos.y - static_cast<float>(r.y)};
    return true;
  }
  if (moving_ && e.type == PointerType::Move) {
    OverlayLayer* layer = OverlayLayer::of(*this);
    if (!layer || !tree) return true;
    const int32_t W = static_cast<int32_t>(tree->width()), H = static_cast<int32_t>(tree->height());
    const int32_t x = std::clamp(static_cast<int32_t>(std::lround(e.pos.x - moveGrab_.x)), 0, std::max(0, W - r.w));
    const int32_t y = std::clamp(static_cast<int32_t>(std::lround(e.pos.y - moveGrab_.y)), 0, std::max(0, H - r.h));
    (void)layer->move(*this, {x, y, 0, 0}, Placement::AtPoint);
    gWheelPos = Vec2{static_cast<float>(x) / sc, static_cast<float>(y) / sc};
    return true;
  }
  if (moving_ && e.type == PointerType::Up) {
    moving_ = false;
    return true;
  }
  return Popover::onPointer(e);
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
  setInfo("Tuning", "The named tuning this follows. Click: the MOS wheel (Ctrl+Shift+W) edits it, and every track and clip "
                    "on it changes too. Right-click: choose another tuning, Inherit, or New Tuning (a copy of this one).");
  // T3.x-fix-paint (M3 review M12a): a chip repaints only when what it shows changed. Most document events cannot
  // change a resolved tuning (a mixer drag is a Param event per frame: before, every chip of every header repainted
  // on each one, so a volume drag cost O(tracks)); the rest re-resolve and compare.
  docSub_ = ctx_.subscribe([this](const doc::StoreEvent& e) {
    if (tuningMayChange(e)) recheck();
  });
}

bool TuningChip::tuningMayChange(const doc::StoreEvent& e) noexcept {
  if (e.kind == doc::StoreEventKind::Reset || e.changes.structural() || !e.changes.tunings.empty()) return true;
  return std::any_of(e.changes.params.begin(), e.changes.params.end(),
                     [](const doc::ParamRef& r) { return r.kind >= doc::ParamKind::TuningReference; });
}

void TuningChip::recheck() {
  const std::string oldText = text_;
  const Rgba oldSwatch = swatch_;
  const bool oldHas = hasSwatch_, wasStale = stale_;
  stale_ = true;
  refresh();
  if (wasStale || text_ != oldText || hasSwatch_ != oldHas || (hasSwatch_ && swatch_ != oldSwatch)) markDirty();
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
  const doc::TuningSlot* slot = doc::findTuning(p, targetTuningId(p, *target_));
  if (!slot) return;
  compact_ = compactTuningLabel(slot->def);
  text_ = slot->name;
  // TU.3: what an EDO / MOS tuning is, unless its name already says it ("31-EDO", "Tuning B: 22edo"): "Tuning B · 5L 2s"
  // (a Scala scale's name is its own description).
  if (!slot->def.scale() && text_.find(compact_) == std::string::npos) text_ += " \xC2\xB7 " + compact_;
  swatch_ = rgba((slot->colour.rgb >> 16) & 0xFFu, (slot->colour.rgb >> 8) & 0xFFu, slot->colour.rgb & 0xFFu);
  hasSwatch_ = true;
}

// "12", "31" (an EDO's size), "5L 2s" (a MOS's family, tuningSummary's first part), "Scala": what fits a narrow chip.
std::string compactTuningLabel(const doc::TuningDef& def) {
  if (const tu::Edo* e = def.edo()) return std::to_string(e->n());
  if (def.mos()) {
    std::string s = tuningSummary(def);
    if (const size_t dot = s.find(" \xC2\xB7"); dot != std::string::npos) s.resize(dot);
    return s;
  }
  return "Scala";
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
    // Too narrow for the name (a mixer strip: "12-E…", M3 review L17): its compact form ("12", "5L 2s"), the dot keeps
    // the colour (T3.x-fix-look).
    std::string_view shown = text_;
    if (ctx.res.text && ctx.res.text->measure(f->small, text_) > tr.w) shown = compact_;
    paint::text(ctx, f->small, tr, shown, th->text.primary, TextAlign::Left);
  }
}

bool TuningChip::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Right && target_) {
    openMenu(e.pos);
    return true;
  }
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

void TuningChip::openMenu(Vec2 at) {
  if (target_) openTuningMenu(*this, ctx_, *target_, at, projectDefault_);
}

}  // namespace pq::views
