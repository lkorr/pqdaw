// Macro mode (T8.1): see pq/views/macro/macro_mode.h.

#include "pq/views/macro/macro_mode.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "../piano_roll/roll_widgets.h"
#include "macro_doc.h"
#include "pq/tuning/monzo.h"
#include "pq/ui/overlay.h"
#include "pq/views/view_context.h"

namespace pq::views {

using tuning::Rational;

namespace {

constexpr Ticks kNoGridStep = kPpq / 4;  // the cursor's time step and note length while the grid is off
constexpr double kSamePitchCents = 0.05;  // "the note at the cursor": this near its pitch

const char* const kDot = " \xC2\xB7 ";  // " · "

void appendRatio(std::string& out, const Rational& r) {
  char buf[48];
  const int n = std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(r.num()), static_cast<long long>(r.den()));
  out.append(buf, static_cast<size_t>(std::max(0, n)));
}

// "−11.7" with a real minus sign.
void appendSigned(std::string& out, double v, int decimals, bool plus) {
  char buf[48];
  const int n = std::snprintf(buf, sizeof buf, "%.*f", decimals, std::fabs(v));
  const bool zero = std::string_view(buf, static_cast<size_t>(std::max(0, n))).find_first_not_of("0.") == std::string_view::npos;
  if (!zero && v < 0) out += "\xE2\x88\x92";
  else if (plus && !zero) out += '+';
  out.append(buf, static_cast<size_t>(std::max(0, n)));
}

// gcd of two positive rationals: the largest F both are whole multiples of. nullopt: too fine to compute.
std::optional<Rational> gcdRational(const Rational& x, const Rational& y) {
  const int64_t l = doc::lcmCapped(x.den(), y.den(), int64_t{1} << 40);
  if (l < 0) return std::nullopt;
  int64_t a = 0, b = 0;
  if (tuning::detail::mulOverflows(x.num(), l / x.den(), a) || tuning::detail::mulOverflows(y.num(), l / y.den(), b)) return std::nullopt;
  return Rational::tryMake(std::gcd(a, b), l);
}

int digitOf(const ui::KeyEvent& e) noexcept {
  if (e.key >= '0' && e.key <= '9') return static_cast<int>(e.key - '0');
  const auto sc = static_cast<uint16_t>(e.scancode);
  if (sc >= static_cast<uint16_t>(ui::Scancode::Kp1) && sc <= static_cast<uint16_t>(ui::Scancode::Kp9))
    return static_cast<int>(sc - static_cast<uint16_t>(ui::Scancode::Kp1)) + 1;
  if (e.scancode == ui::Scancode::Kp0) return 0;
  return -1;
}

bool sameClip(const doc::ClipRef& a, const doc::ClipRef& b) noexcept { return a.track == b.track && a.clip == b.clip; }

}  // namespace

struct MacroMode::Anchor {
  const doc::Note* note = nullptr;  // nullptr: a free position
  double cents = 0.0;               // its pitch from the reference
  int64_t degree = 0;               // its row (a note between rows: the nearest)
  std::optional<macro::JiMembership> ji;
  bool ok = false;
};

MacroMode::MacroMode(ViewContext& ctx) : ctx_(ctx), preset_(builtinMacroPreset()) {}
MacroMode::~MacroMode() = default;

// --- switch -----------------------------------------------------------------------------------------------

bool MacroMode::active() const noexcept {
  return on_ && roll_ && roll_->hasClip() && ctx_.liveEditedClip().clip.valid();
}

void MacroMode::setOn(bool on) {
  if (on == on_) return;
  if (on) ctx_.computerKeyboard().setActive(false);  // one key listener at a time
  on_ = on;
  lockPending_ = false;
  lockDigits_.clear();
  ring_.clear();
  ringPos_ = 0;
  if (on_) {
    // The chosen preset is read from its file the first time the mode is used (until then: the built-in table).
    std::string presetError;
    if (!presetLoaded_) {
      presetLoaded_ = true;
      if (Result<MacroPreset> p = loadMacroPreset(ctx_.viewPrefs().macroPreset); p) {
        preset_ = std::move(*p);
        ++presetSerial_;
      } else {
        presetError = p.error().message;
      }
    }
    resetState();
    if (!presetError.empty()) {
      ctx_.setStatus("Macro mode on with the built-in keys: preset " + presetError);
    } else if (active()) {
      const std::string place = preset_.keyLabel(MacroOp::Place), layer = preset_.keyLabel(MacroOp::Layer);
      ctx_.setStatus("Macro mode on (" + std::string(layer_ == Layer::Ji ? "JI" : "tempered") + "): interval keys compose, " +
                     (place.empty() ? std::string("no key") : place) + " places a note" +
                     (layer.empty() ? std::string() : ", " + layer + " switches the layer") + "; ? in the note editor lists every key");
    } else {
      ctx_.setStatus("Macro mode on: open a note clip in the clip view to use it");
    }
  } else {
    ctx_.setStatus("Macro mode off");
  }
  changed();
}

void MacroMode::setLayer(Layer l) {
  if (l == layer_) return;
  layer_ = l;
  s_.rowOffset = 0;
  ctx_.setStatus(l == Layer::Ji ? "Macro layer: JI (notes are exact ratios to the anchor)"
                                : "Macro layer: tempered (notes go to the tuning's nearest step; from a JI note they stay just)");
  changed();
}

void MacroMode::resetState() {
  s_ = {};
  seenHover_ = seenSel_ = 0;
  if (!roll_ || !roll_->hasClip()) return;
  ringClip_ = roll_->clipRef();
  // A free cursor at the start of what plays, on the row in the middle of the view; then the pointer / selection rule.
  s_.tick = std::max<Ticks>(0, roll_->regionStart());
  if (!roll_->rows().empty()) {
    const ui::IRect a = roll_->area();
    s_.anchorDegree = roll_->rows().row(roll_->rows().indexAtY(roll_->contentY(static_cast<float>(a.y + a.h / 2)))).degree;
  }
  (void)applySync(s_, seenHover_, seenSel_);
}

void MacroMode::changed() {
  if (onChanged) onChanged();
}

// --- the roll ---------------------------------------------------------------------------------------------

void MacroMode::attachRoll(roll::RollState* roll) {
  roll_ = roll;
  if (on_) {
    resetState();
    changed();
  }
}

void MacroMode::detachRoll(roll::RollState* roll) {
  if (roll_ != roll) return;
  roll_ = nullptr;
  ring_.clear();
  ringPos_ = 0;
  if (on_) changed();
}

void MacroMode::rollChanged() {
  if (!on_ || inStep_) return;
  if (!roll_ || !roll_->hasClip()) {
    changed();  // the lamp reads "MACRO" without a clip
    return;
  }
  if (!sameClip(roll_->clipRef(), ringClip_)) {  // another clip: a fresh cursor, no macro history
    ring_.clear();
    ringPos_ = 0;
    lockPending_ = false;
    lockDigits_.clear();
    resetState();
    changed();
    return;
  }
  if (s_.anchor.valid() && !roll_->findNote(s_.anchor)) s_.anchor = {};  // deleted (Ctrl+Z, the roll): a free cursor on its row
  (void)applySync(s_, seenHover_, seenSel_);
  changed();
}

uint32_t MacroMode::hoveredNote() const {
  if (!roll_ || !roll_->noteArea) return 0;
  const ui::UiTree* t = roll_->noteArea->tree();
  if (!t || t->hovered() != roll_->noteArea) return 0;
  const doc::Note* n = roll_->noteAt(t->pointerPos());
  return n ? n->id.value : 0;
}

uint32_t MacroMode::selectedNote() const {
  return roll_ && roll_->selection().size() == 1 ? roll_->selection().front() : 0;
}

void MacroMode::adopt(State& s, const doc::Note& n) const {
  s.anchor = n.id;
  s.tick = n.start;
  const std::optional<double> c = roll_->centsOf(n.pitch);
  s.anchorDegree = n.pitch.kind() == tuning::PitchKind::Degree
                       ? roll::absoluteDegree(roll_->tuning(), n.pitch.degree(), n.pitch.equave())
                       : roll_->nearestDegree(c.value_or(0.0));
}

bool MacroMode::applySync(State& s, uint32_t& seenHover, uint32_t& seenSel) const {
  if (!active()) return false;
  const uint32_t h = hoveredNote(), sel = selectedNote();
  bool ch = false;
  if (s.factors.empty() && s.rowOffset == 0 && !lockPending_) {
    // The pointer arriving on a note wins; else a note becoming the single selection.
    const uint32_t pick = (h != seenHover && h != 0) ? h : (sel != seenSel && sel != 0) ? sel : 0;
    const doc::Note* n = pick ? roll_->findNote(doc::NoteId{pick}) : nullptr;
    if (n && roll_->centsOf(n->pitch)) {
      const State before = s;
      adopt(s, *n);
      ch = !(s == before);
    }
  }
  seenHover = h;
  seenSel = sel;
  return ch;
}

bool MacroMode::sync() {
  if (inStep_ || !applySync(s_, seenHover_, seenSel_)) return false;
  changed();  // no scrolling here: the view must not move under a pointer that is choosing a note
  return true;
}

bool MacroMode::syncPending() const {
  if (inStep_ || !active()) return false;
  State s = s_;
  uint32_t h = seenHover_, sel = seenSel_;
  return applySync(s, h, sel);
}

void MacroMode::onPointerDown(ui::Vec2 pos) {
  if (!active() || !roll_->noteArea || roll_->rows().empty()) return;
  const ui::UiTree* t = roll_->noteArea->tree();
  if (!t || t->hovered() != roll_->noteArea) return;  // a popup over the roll, another widget
  const ui::IRect a = roll_->area();
  if (pos.x < static_cast<float>(a.x) || pos.x >= static_cast<float>(a.right()) || pos.y < static_cast<float>(a.y) ||
      pos.y >= static_cast<float>(a.bottom()))
    return;
  if (roll_->noteAt(pos)) return;  // a note: the pointer / selection rule makes it the anchor
  // Empty space: a free cursor in that grid cell, on that row.
  const Ticks raw = static_cast<Ticks>(std::floor(roll_->tickAtX(pos.x)));
  const Grid g = roll_->grid();
  lockPending_ = false;
  lockDigits_.clear();
  step([&] {
    s_.tick = std::max<Ticks>(0, g.enabled() ? g.lineAtOrBefore(raw) : raw);
    s_.anchor = {};
    s_.anchorDegree = roll_->rows().row(roll_->rows().indexAtY(roll_->contentY(pos.y))).degree;
    s_.factors.clear();
    s_.comp = Rational{1};
    s_.rowOffset = 0;
  });
}

// --- derived ----------------------------------------------------------------------------------------------

MacroMode::Anchor MacroMode::resolve(const State& s) const {
  Anchor a;
  if (!roll_ || !roll_->hasClip()) return a;
  a.note = s.anchor.valid() ? roll_->findNote(s.anchor) : nullptr;
  if (a.note) {
    if (const std::optional<double> c = roll_->centsOf(a.note->pitch)) {
      a.cents = *c;
      a.degree = a.note->pitch.kind() == tuning::PitchKind::Degree
                     ? roll::absoluteDegree(roll_->tuning(), a.note->pitch.degree(), a.note->pitch.equave())
                     : roll_->nearestDegree(*c);
      a.ji = macro::jiMembership(*roll_->clip(), *a.note);
      a.ok = true;
      return a;
    }
    a.note = nullptr;
  }
  a.degree = s.anchorDegree;
  a.cents = roll::degreeCents(roll_->tuning(), a.degree);
  a.ok = true;
  return a;
}

MacroMode::Target MacroMode::targetOf(const State& s) const {
  Target t;
  const Anchor a = resolve(s);
  if (!a.ok) return t;
  const tuning::Tuning& tun = roll_->tuning();
  const double compCents = tuning::cents(s.comp);
  if (a.note && (a.ji || layer_ == Layer::Ji)) {
    // Just: a Ratio in the anchor's group (a tempered anchor in the JI layer: anchored to that note).
    const macro::JiRoot root = a.ji ? a.ji->root : macro::JiRoot{false, a.note->id};
    const std::optional<Rational> r = tuning::mul(a.ji ? a.ji->ratio : Rational{1}, s.comp);
    if (!r) return t;
    t.pitch = root.reference ? tuning::PitchId::fromRatio(*r) : tuning::PitchId::fromRatio(*r, tuning::AnchorKind::Note, root.note.value);
    t.cents = a.cents + compCents;
    t.just = true;
    if (a.ji && a.ji->fundamental)
      if (const std::optional<Rational> h = tuning::div(*r, *a.ji->fundamental); h && h->isInteger() && h->num() > 0)
        t.harmonic = static_cast<uint64_t>(h->num());
    t.ok = true;
    return t;
  }
  if (!a.note && layer_ == Layer::Ji && !s.factors.empty()) {
    // The JI layer from a free position: exact from the row's pitch. On a row that is the reference in some equave
    // the note is a Ratio over the reference; elsewhere there is no note to hang a ratio on: a Cents note.
    t.just = true;
    t.cents = a.cents + compCents;
    t.ok = true;
    t.pitch = tuning::PitchId::fromCents(t.cents);
    const tuning::Edo* edo = tun.edo();
    const std::optional<Rational> eq = edo ? edo->equaveRatio() : std::nullopt;
    const int64_t n = edo ? static_cast<int64_t>(edo->n()) : 0;
    if (eq && n > 0 && a.degree % n == 0 && std::llabs(a.degree / n) <= 8) {
      std::optional<Rational> r = s.comp;
      for (int64_t k = a.degree / n; r && k != 0; k += k > 0 ? -1 : 1) r = k > 0 ? tuning::mul(*r, *eq) : tuning::div(*r, *eq);
      if (r) t.pitch = tuning::PitchId::fromRatio(*r);
    }
    return t;
  }
  // Tempered: the tuning's step for the composed interval, measured from the anchor's row.
  int64_t steps = 0;
  if (!s.factors.empty()) {
    const tuning::Edo* edo = tun.edo();
    std::optional<int64_t> mapped;
    if (edo)
      if (const Result<tuning::Monzo> m = tuning::toMonzo(s.comp, 31); m.ok()) mapped = tuning::mapToSteps(preset_.valFor(*edo), *m);
    steps = mapped ? *mapped : roll_->nearestDegree(roll::degreeCents(tun, a.degree) + compCents) - a.degree;
  }
  const int64_t degree = a.degree + steps + s.rowOffset;
  t.pitch = roll_->degreePitch(degree);
  t.cents = roll::degreeCents(tun, degree);
  t.steps = steps + s.rowOffset;
  t.errorCents = compCents - (roll::degreeCents(tun, a.degree + steps) - roll::degreeCents(tun, a.degree));
  t.ok = true;
  return t;
}

MacroMode::Target MacroMode::target() const { return targetOf(s_); }

std::optional<double> MacroMode::anchorCents() const {
  const Anchor a = resolve(s_);
  return a.ok ? std::optional<double>(a.cents) : std::nullopt;
}

int64_t MacroMode::anchorRow() const { return resolve(s_).degree; }

Ticks MacroMode::noteLength() const {
  const Anchor a = resolve(s_);
  if (a.note) return std::max<Ticks>(1, a.note->length);
  if (!roll_) return kNoGridStep;
  const Grid g = roll_->grid();
  if (!g.enabled()) return kNoGridStep;
  const TickRange c = g.cellAt(s_.tick);
  return std::max<Ticks>(1, c.end - c.start);
}

bool MacroMode::locked() const {
  const Anchor a = resolve(s_);
  return a.ji && a.ji->declaredIndex > 0;
}

std::string MacroMode::compositionText() const {
  std::string out;
  if (s_.factors.empty()) return "1/1";
  for (size_t i = 0; i < s_.factors.size(); ++i) {
    if (i) out += kDot;
    appendRatio(out, s_.factors[i]);
  }
  if (s_.factors.size() > 1) {
    out += " = ";
    appendRatio(out, s_.comp);
  }
  return out;
}

std::string MacroMode::detailText() const {
  if (lockPending_) return "Is Harmonic " + lockDigits_ + "_";
  const Target t = targetOf(s_);
  if (!t.ok) return "cannot place a note here";
  if (t.pitch.kind() == tuning::PitchKind::Ratio) return t.harmonic ? "just" + std::string(kDot) + "harmonic " + std::to_string(t.harmonic) : "just";
  if (t.pitch.kind() == tuning::PitchKind::Cents) return "exact";
  if (s_.factors.empty() && s_.rowOffset == 0) return {};
  std::string out = "\xE2\x86\x92 ";  // →
  appendSigned(out, static_cast<double>(t.steps), 0, false);
  out += std::llabs(t.steps) == 1 ? " step" : " steps";
  if (!s_.factors.empty()) {
    out += ", ";
    appendSigned(out, t.errorCents, 1, true);
    out += " \xC2\xA2";  // ¢
  }
  return out;
}

std::string MacroMode::lampText() const {
  if (!active()) return "MACRO";
  if (layer_ == Layer::Ji) return "MACRO" + std::string(kDot) + "JI";
  // An EDO by its size (whatever the project calls the tuning); a MOS / scale by its name.
  std::string name;
  if (const tuning::Edo* edo = roll_->tuning().edo()) name = std::to_string(edo->n()) + "-EDO";
  else name = roll_->tuningName();
  if (name.empty()) name = roll_->tuning().mos() ? "MOS" : "Scale";
  return "MACRO" + std::string(kDot) + name;
}

// --- presets, fewest-keypress labels ------------------------------------------------------------------------

void MacroMode::setPreset(MacroPreset p) {
  preset_ = std::move(p);
  ++presetSerial_;
  lockPending_ = false;
  lockDigits_.clear();
  changed();
}

Status MacroMode::usePreset(std::string_view id) {
  Result<MacroPreset> p = loadMacroPreset(id);
  if (!p) {
    ctx_.setStatus("Macro: " + p.error().message + " (keeping \"" + preset_.name + "\")");
    return p.error();
  }
  presetLoaded_ = true;
  setPreset(std::move(*p));
  ctx_.setStatus("Macro preset: " + preset_.name);
  return Ok{};
}

const tuning::FewestKeypressMap* MacroMode::keypressMap() {
  const tuning::Edo* edo = roll_ && roll_->hasClip() ? roll_->tuning().edo() : nullptr;
  if (!edo || edo->equaveRatio() != std::optional<Rational>(Rational{2}) || edo->n() > 4096) return nullptr;
  for (const KeypressTable& t : keypress_)
    if (t.edo == edo->n() && t.presetSerial == presetSerial_) return t.map ? &*t.map : nullptr;
  // Built once per (EDO, preset): the trainer's breadth-first search, then every row's label as text.
  const auto t0 = std::chrono::steady_clock::now();
  KeypressTable table;
  table.edo = edo->n();
  table.presetSerial = presetSerial_;
  const std::vector<tuning::KeyRatio> keys = tuning::jsObjectKeyOrder(preset_.intervalKeys());
  Result<tuning::FewestKeypressMap> map = tuning::FewestKeypressMap::build(keys, edo->n(), preset_.primeLimit);
  if (map) {
    table.map = std::move(*map);
    std::vector<std::string> labels(keys.size());
    bool oneChar = true;
    for (size_t i = 0; i < keys.size(); ++i) {
      for (const MacroBinding& b : preset_.bindings)
        if (b.op == MacroOp::Interval && b.key.name == keys[i].key) labels[i] = b.key.label;
      oneChar = oneChar && labels[i].size() == 1;
    }
    const int32_t range = table.map->maxRange();
    table.labels.resize(static_cast<size_t>(2 * range + 1));
    for (int32_t s = -range; s <= range; ++s) {
      if (s == 0 || !table.map->find(s)) continue;
      std::string& l = table.labels[static_cast<size_t>(s + range)];
      if (!oneChar) {
        l = table.map->ratioLabel(s);
        continue;
      }
      for (uint32_t k : table.map->keySequence(s)) l += labels[k];
    }
  }
  ++keypressBuilds_;
  keypressBuildMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (keypress_.size() >= kKeypressCache) keypress_.erase(keypress_.begin());
  keypress_.push_back(std::move(table));
  return keypress_.back().map ? &*keypress_.back().map : nullptr;
}

std::string_view MacroMode::keypressLabel(int64_t steps) {
  const tuning::FewestKeypressMap* map = keypressMap();
  if (!map || steps < -map->maxRange() || steps > map->maxRange()) return {};
  for (const KeypressTable& t : keypress_)
    if (t.map && &*t.map == map) return t.labels[static_cast<size_t>(steps + map->maxRange())];
  return {};
}

// --- keys ---------------------------------------------------------------------------------------------------

bool MacroMode::blocked() const {
  if (!roll_ || !roll_->noteArea) return true;
  const ui::UiTree* t = roll_->noteArea->tree();
  if (!t) return true;
  if (const ui::Widget* f = t->focus(); f && f->enabled() && f->acceptsTextInput()) return true;  // typing
  if (const ui::OverlayLayer* ov = ui::OverlayLayer::of(*roll_->noteArea); ov && ov->top()) return true;  // a menu, the help panel
  return false;
}

bool MacroMode::onTreeKey(const ui::KeyEvent& e) {
  if (!e.down || !on_) return false;
  if ((e.mods & (ui::mod::kCtrl | ui::mod::kAlt | ui::mod::kSuper | ui::mod::kShift)) != 0) return false;
  if (!active() || blocked()) return false;
  (void)sync();
  if (lockPending_) {
    if (const int d = digitOf(e); d >= 0) {
      if (lockDigits_.size() < 4 && !(lockDigits_.empty() && d == 0)) lockDigits_ += static_cast<char>('0' + d);
      changed();
      return true;
    }
    const MacroBinding* b = preset_.find(e);
    const bool cancel = e.key == ui::key::kEscape || (b && b->op == MacroOp::Lock) || (b && b->op == MacroOp::Back && lockDigits_.empty());
    if (cancel) {
      lockPending_ = false;
      ctx_.setStatus("Is Harmonic: cancelled");
      changed();
      return true;
    }
    if (b && b->op == MacroOp::Back) {
      lockDigits_.pop_back();
      changed();
      return true;
    }
    commitLock();  // Enter, or the next other key (which then acts as usual)
    if (e.key == ui::key::kReturn || e.scancode == ui::Scancode::KpEnter) return true;
  }
  const MacroBinding* b = preset_.find(e);
  if (!b) return false;
  run(*b, e.repeat);
  return true;
}

template <class F>
void MacroMode::step(F&& f) {
  State before = s_;
  const doc::Snapshot docBefore = ctx_.doc();
  inStep_ = true;
  f();
  inStep_ = false;
  if (s_.anchor.valid() && roll_ && !roll_->findNote(s_.anchor)) s_.anchor = {};
  // What this step itself put under the pointer or selected is no new pointer / selection anchor.
  seenHover_ = hoveredNote();
  seenSel_ = selectedNote();
  const bool edit = ctx_.doc() != docBefore;
  if (s_ == before && !edit) return;
  ring_.resize(ringPos_);
  if (ring_.size() >= kUndoRing) ring_.erase(ring_.begin());
  ring_.push_back({std::move(before), s_, edit, docBefore, ctx_.doc()});
  ringPos_ = ring_.size();
  changed();
}

void MacroMode::run(const MacroBinding& b, bool repeat) {
  switch (b.op) {
    case MacroOp::Interval:
      if (!repeat) pressInterval(b.ratio);
      break;
    case MacroOp::Place:
      if (!repeat) place(false);
      break;
    case MacroOp::PlaceChain:
      if (!repeat) place(true);
      break;
    case MacroOp::Back:
      if (!repeat) back();
      break;
    case MacroOp::Left: moveTime(-1); break;
    case MacroOp::Right: moveTime(1); break;
    case MacroOp::Up: movePitch(1); break;
    case MacroOp::Down: movePitch(-1); break;
    case MacroOp::Undo: undo(); break;
    case MacroOp::Redo: redo(); break;
    case MacroOp::Layer:
      if (!repeat) setLayer(layer_ == Layer::Ji ? Layer::Tempered : Layer::Ji);
      break;
    case MacroOp::Lock:
      if (!repeat) pressLock();
      break;
  }
}

void MacroMode::afterMove(bool audition) {
  reveal();
  if (!audition) return;
  const Target t = targetOf(s_);
  if (t.ok) roll_->audition(t.pitch, 100.0f, s_.tick, noteLength());
}

void MacroMode::reveal() {
  if (!roll_ || !roll_->hasClip() || roll_->rows().empty()) return;
  const Target t = targetOf(s_);
  const ui::IRect a = roll_->area();
  if (a.empty() || !t.ok) return;
  const float mx = std::min(48.0f, static_cast<float>(a.w) * 0.2f), my = std::min(40.0f, static_cast<float>(a.h) * 0.2f);
  const float x0 = roll_->xOfTick(static_cast<double>(s_.tick));
  const float x1 = roll_->xOfTick(static_cast<double>(s_.tick + noteLength()));
  if (x0 < static_cast<float>(a.x) + mx) roll_->scrollByPx(x0 - (static_cast<float>(a.x) + mx), 0.0f);
  else if (x1 > static_cast<float>(a.right()) - mx && x1 - x0 < static_cast<float>(a.w) - 2.0f * mx)
    roll_->scrollByPx(x1 - (static_cast<float>(a.right()) - mx), 0.0f);
  const float y = roll_->yOfCents(t.cents);
  if (y < static_cast<float>(a.y) + my) roll_->scrollByPx(0.0f, y - (static_cast<float>(a.y) + static_cast<float>(a.h) / 3.0f));
  else if (y > static_cast<float>(a.bottom()) - my) roll_->scrollByPx(0.0f, y - (static_cast<float>(a.bottom()) - static_cast<float>(a.h) / 3.0f));
}

void MacroMode::pressInterval(const Rational& r) {
  const std::optional<Rational> c = tuning::mul(s_.comp, r);
  if (!c) {
    ctx_.setStatus("Macro: the composition got too large");
    return;
  }
  step([&] {
    s_.factors.push_back(r);
    s_.comp = *c;
  });
  afterMove(true);
}

void MacroMode::moveTime(int dir) {
  const Grid g = roll_->grid();
  Ticks t = s_.tick;
  if (g.enabled()) t = dir > 0 ? g.lineAfter(t) : g.lineAtOrBefore(t - 1);
  else t += dir * kNoGridStep;
  t = std::max<Ticks>(0, t);
  step([&] { s_.tick = t; });
  afterMove(false);
}

void MacroMode::movePitch(int dir) {
  const Anchor a = resolve(s_);
  if (!a.ok) return;
  const bool just = a.note ? (a.ji.has_value() || layer_ == Layer::Ji) : (layer_ == Layer::Ji && !s_.factors.empty());
  if (!just) {
    step([&] {
      if (!a.note && s_.factors.empty() && s_.rowOffset == 0) s_.anchorDegree += dir;  // the free cursor itself
      else s_.rowOffset += dir;
    });
    afterMove(true);
    return;
  }
  // The next harmonic: of the group's fundamental when it has one (the lock, a derived one), else of the cursor's own
  // denominator (the roll's Up / Down on a Ratio note).
  const std::optional<Rational> rr = tuning::mul(a.ji ? a.ji->ratio : Rational{1}, s_.comp);
  if (!rr) return;
  std::optional<Rational> f;
  if (a.ji && a.ji->fundamental) f = gcdRational(*rr, *a.ji->fundamental);
  if (!f) f = Rational::tryMake(1, rr->den());
  const std::optional<Rational> m = f ? tuning::div(*rr, *f) : std::nullopt;
  if (!m || !m->isInteger()) return;
  if (m->num() + dir < 1) {
    ctx_.setStatus("Macro: there is no harmonic below the fundamental");
    return;
  }
  const std::optional<Rational> next = tuning::mul(Rational{m->num() + dir}, *f);
  const std::optional<Rational> factor = next ? tuning::div(*next, *rr) : std::nullopt;
  const std::optional<Rational> comp = factor ? tuning::mul(s_.comp, *factor) : std::nullopt;
  if (!comp) return;
  step([&] {
    s_.factors.push_back(*factor);
    s_.comp = *comp;
  });
  afterMove(true);
}

bool MacroMode::dispatch(const doc::ops::EditResult& e, std::string_view what) {
  if (!e) {
    ctx_.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx_.store().dispatch(e->command); !s) {
    ctx_.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  (void)roll_->refresh();  // the clip view's listener did this when it is subscribed; cheap when nothing moved
  return true;
}

void MacroMode::place(bool chain) {
  const Anchor a = resolve(s_);
  const Target t = targetOf(s_);
  if (!t.ok) {
    ctx_.setStatus("Macro: cannot place a note here");
    return;
  }
  const Ticks length = noteLength();
  const float velocity = a.note ? a.note->velocity : 100.0f;
  const bool rootPlaced = !a.note && s_.factors.empty();
  step([&] {
    doc::ops::EditResult e = Error{};
    if (t.pitch.kind() == tuning::PitchKind::Ratio) {
      const macro::JiRoot root{t.pitch.anchorKind() == tuning::AnchorKind::Reference, doc::NoteId{t.pitch.anchorId()}};
      e = macro::placeJustNote(*ctx_.doc(), ctx_.store().ids(), roll_->clipRef(), *roll_->clip(), root, t.pitch.ratio(), s_.tick, length, velocity);
    } else {
      doc::Note n;
      n.start = s_.tick;
      n.length = length;
      n.velocity = velocity;
      n.pitch = t.pitch;
      e = doc::ops::addNotes(*ctx_.doc(), ctx_.store().ids(), roll_->clipRef(), std::vector<doc::Note>{n});
    }
    if (!dispatch(e, "Macro: Add Note") || e->notes.empty()) return;
    s_.factors.clear();
    s_.comp = Rational{1};
    s_.rowOffset = 0;
    if (chain || rootPlaced) {
      s_.anchor = e->notes.front().id;
      s_.anchorDegree = roll_->nearestDegree(t.cents);
    }
  });
  roll_->audition(t.pitch, velocity, s_.tick, length);
  reveal();
}

void MacroMode::back() {
  if (!s_.factors.empty() || s_.rowOffset != 0) {
    step([&] {
      s_.factors.clear();
      s_.comp = Rational{1};
      s_.rowOffset = 0;
    });
    afterMove(true);
    return;
  }
  const Target t = targetOf(s_);
  const doc::Note* hit = nullptr;
  if (t.ok)
    roll_->forNotesIn(s_.tick, s_.tick + 1, [&](const doc::Note& n) {
      const std::optional<double> c = roll_->centsOf(n.pitch);
      if (c && std::fabs(*c - t.cents) < kSamePitchCents && (!hit || n.id == s_.anchor)) hit = &n;
    });
  if (!hit) {
    ctx_.setStatus("Macro: no note at the cursor");
    return;
  }
  const doc::NoteKey key = doc::keyOf(*hit);
  step([&] { (void)dispatch(doc::ops::deleteNotes(*ctx_.doc(), roll_->clipRef(), std::span<const doc::NoteKey>(&key, 1)), "Macro: Delete Note"); });
  reveal();
}

void MacroMode::undo() {
  if (ringPos_ == 0) {
    ctx_.setStatus("Macro: nothing to undo");
    return;
  }
  const Step st = ring_[ringPos_ - 1];
  inStep_ = true;
  if (st.edit) {
    // The step's edit goes with it only while the document is still as the step left it (a Ctrl+Z, an edit in the
    // roll since: the document is not touched, the cursor still goes back).
    if (ctx_.doc() == st.docAfter && ctx_.store().history().canUndo() && !ctx_.store().inGesture()) (void)ctx_.undo();
    else ctx_.setStatus("Macro undo: the document changed since that step; only the cursor went back");
  }
  inStep_ = false;
  --ringPos_;
  s_ = st.before;
  if (s_.anchor.valid() && !roll_->findNote(s_.anchor)) s_.anchor = {};
  seenHover_ = hoveredNote();
  seenSel_ = selectedNote();
  changed();
  afterMove(false);
}

void MacroMode::redo() {
  if (ringPos_ >= ring_.size()) {
    ctx_.setStatus("Macro: nothing to redo");
    return;
  }
  const Step st = ring_[ringPos_];
  inStep_ = true;
  if (st.edit) {
    if (ctx_.doc() == st.docBefore && ctx_.store().history().canRedo() && !ctx_.store().inGesture()) (void)ctx_.redo();
    else ctx_.setStatus("Macro redo: the document changed since that step; only the cursor moved");
  }
  inStep_ = false;
  ++ringPos_;
  s_ = st.after;
  if (s_.anchor.valid() && !roll_->findNote(s_.anchor)) s_.anchor = {};
  seenHover_ = hoveredNote();
  seenSel_ = selectedNote();
  changed();
  afterMove(false);
}

void MacroMode::pressLock() {
  if (locked()) {  // `b` on a note of a declared group: the declaration goes (the members keep their pitch)
    const Anchor a = resolve(s_);
    step([&] { (void)dispatch(macro::undeclareHarmonic(*ctx_.doc(), roll_->clipRef(), *roll_->clip(), *a.note), "Macro: Unlock"); });
    ctx_.setStatus("Is Harmonic: removed");
    return;
  }
  if (!resolve(s_).note) {
    ctx_.setStatus("Is Harmonic needs an anchor note: point at a note, select one, or place one first");
    return;
  }
  lockPending_ = true;
  lockDigits_.clear();
  ctx_.setStatus("Is Harmonic: type the number, then Enter (no number: " + std::to_string(lastIndex_) + ")");
  changed();
}

void MacroMode::commitLock() {
  lockPending_ = false;
  uint32_t index = lastIndex_;
  if (!lockDigits_.empty()) index = static_cast<uint32_t>(std::stoul(lockDigits_));
  lockDigits_.clear();
  const Anchor a = resolve(s_);
  if (!a.note || index < 1 || index > doc::kMaxHarmonicIndex) {
    ctx_.setStatus("Is Harmonic: needs an anchor note and a number from 1 to " + std::to_string(doc::kMaxHarmonicIndex));
    changed();
    return;
  }
  const doc::NoteKey key = doc::keyOf(*a.note);
  bool done = false;
  step([&] { done = dispatch(macro::declareHarmonic(*ctx_.doc(), ctx_.store().ids(), roll_->clipRef(), key, index), "Is Harmonic"); });
  if (done) {
    lastIndex_ = index;
    ctx_.setStatus("Is Harmonic " + std::to_string(index) + ": the anchor is harmonic " + std::to_string(index) +
                   "; Up / Down step the harmonics, notes placed from it are just");
  }
  changed();
}

}  // namespace pq::views
