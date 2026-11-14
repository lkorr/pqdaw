// The inner roll and morph editing of JI objects (T7.5; docs/plans/M7.md "T7.5", amendments A1, A2, A4; Q275, Q279).
//
// What the user does, and where it lands:
//   - Enter an object (double-click it; Esc or a double-click outside leaves): its voices are the rows inside the box,
//     the rest of the roll is dimmed (roll_paint.cpp), drawing puts Harmonic notes on the voice under the pointer
//     (DrawPitch::Voice), Up / Down step a selected inner note to the next voice (ops::stepPitch), and only the object
//     and its notes are hit.
//   - Stops: typing a proportion (or the menu) adds a stop at the pointer; T adds a tuning stop; a stop's label drags
//     it in time, the grip on the box's bottom border sets its glide; Delete removes the selected stop; a double-click
//     on a label retypes it. A voice's transition has a shape (straight / S / bezier with two handles) and its own
//     glide / delay (the object's right-click menu on that voice).
//   - A harmonic number (voice label) can be selected and walked with Alt+Up / Down (ops::walkJiTerm).
//   - "Tune to JI" (selected notes -> ops::jiFromNotes), "Back to Tuning" (ops::addJiTuningStop), "Remove JI Tuning"
//     (ops::deleteJiObject: the plain notes come back).
//   - "Is Harmonic…" on a note locks the clip's virtual fundamental at note / N (A4).
//   - Ctrl+C / Ctrl+X / Ctrl+V of a selected object with its inner notes (ops::pasteJiObject).
// Every edit is one doc::ops builder dispatched once: one undo step. Nothing here runs per frame; the pointer-driven
// queries (jiVoiceAt, jiTransitionAt) evaluate one object at one tick (O(voices · stops)).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <variant>

#include "pq/document/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/document/ji_object.h"
#include "pq/ui/overlay.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

std::string_view trimmed(std::string_view t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
  return t;
}

std::string proportionText(const doc::JiStop& stop, bool undertone) {
  std::string out;
  for (size_t i = 0; i < stop.terms.size(); ++i) {
    if (i) out += ':';
    if (undertone) out += "1/";
    out += std::to_string(stop.terms[i]);
  }
  return out;
}

// "0.25", "1", "1.5": ticks as beats.
std::string beatsText(Ticks t) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(t) / static_cast<double>(kPpq));
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s.empty() ? "0" : s;
}

// One grid step at `t` (a sixteenth when the grid is off): the default glide of a new stop.
Ticks gridStepAt(const Grid& g, Ticks t) {
  if (!g.enabled()) return kPpq / 4;
  const TickRange c = g.cellAt(t);
  return std::max<Ticks>(1, c.end - c.start);
}

}  // namespace

// --- the entered object, the selected stop / voice label --------------------------------------------------------------

const doc::JiObject* RollState::jiObject(doc::JiObjectId id) const noexcept {
  return (clip_ && id.valid()) ? doc::findJiObject(*clip_, id) : nullptr;
}

const JiView* RollState::jiViewOf(doc::JiObjectId id) const noexcept {
  for (const JiView& v : jiViews_)
    if (v.id == id) return &v;
  return nullptr;
}

void RollState::enterJiObject(doc::JiObjectId id) {
  if (!jiObject(id) || id == jiEntered_) return;
  setJiTool(false);
  select({});
  jiEntered_ = id;
  jiSel_ = id;
  jiStopSel_ = jiTermSel_ = -1;
  ctx.setStatus("Inside the JI object: notes go on its voices. Type a proportion for a new stop, T = back to the tuning, Esc leaves");
  markNotesDirty();
}

void RollState::leaveJiObject() {
  if (!jiEntered_.valid()) return;
  const doc::JiObjectId was = jiEntered_;
  jiEntered_ = {};
  select({});
  jiSel_ = jiObject(was) ? was : doc::JiObjectId{};  // it stays selected, as before entering
  jiStopSel_ = jiTermSel_ = -1;
  ctx.setStatus("Left the JI object");
  markNotesDirty();
}

void RollState::selectJiStop(doc::JiObjectId id, int32_t stop) {
  if (jiEntered_.valid() && id != jiEntered_) return;
  select({});
  if (!jiEntered_.valid()) jiSel_ = id;
  jiStopSel_ = stop;
  jiTermSel_ = -1;
  markNotesDirty();
}

void RollState::selectJiTerm(doc::JiObjectId id, int32_t voice) {
  if (jiEntered_.valid() && id != jiEntered_) return;
  select({});
  if (!jiEntered_.valid()) jiSel_ = id;
  jiTermSel_ = voice;
  jiStopSel_ = -1;
  if (voice >= 0) ctx.setStatus("Harmonic selected: Alt+Up / Alt+Down walk it (Shift: the top voice stays instead of the bottom one)");
  markNotesDirty();
}

DrawPitch RollState::drawPitchFor(ui::Mods m) const noexcept {
  if (jiEntered_.valid()) return DrawPitch::Voice;
  if (m & mod::kAlt) return DrawPitch::Overlay;
  // A4: with a locked fundamental drawing snaps to its harmonics; Shift then draws on the rows.
  const bool locked = clip_ && clip_->fundamental.lock.has_value();
  return (((m & mod::kShift) != 0) != locked) ? DrawPitch::Ratio : DrawPitch::Row;
}

// --- pointer queries --------------------------------------------------------------------------------------------------

int32_t RollState::jiVoiceAt(doc::JiObjectId id, ui::Vec2 p) const {
  const doc::JiObject* o = jiObject(id);
  if (!o || !tuningValid_) return -1;
  const double t = std::clamp(tickAtX(p.x) - static_cast<double>(o->start), 0.0, static_cast<double>(o->length));
  int32_t best = -1;
  float bestD = 1e30f;
  for (uint32_t v = 0; v < o->voices(); ++v) {
    const std::optional<double> c = doc::jiVoiceCents(*o, tuning_, v, t);
    if (!c) continue;
    const float d = std::fabs(yOfCents(*c) - p.y);
    if (d < bestD) {  // of voices in unison the lowest index
      bestD = d;
      best = static_cast<int32_t>(v);
    }
  }
  return best;
}

int32_t RollState::jiTransitionAt(doc::JiObjectId id, uint32_t voice, ui::Vec2 p) const {
  const doc::JiObject* o = jiObject(id);
  if (!o || voice >= o->voices()) return -1;
  const double t = std::clamp(tickAtX(p.x) - static_cast<double>(o->start), 0.0, static_cast<double>(o->length));
  const doc::JiPosition pos = doc::jiPosition(*o, static_cast<int32_t>(voice), t);
  if (!pos.holding()) return pos.next;
  if (pos.stop + 1 < o->stops.size() && o->stops[pos.stop + 1].at < o->length) return static_cast<int32_t>(pos.stop + 1);
  return pos.stop >= 1 ? static_cast<int32_t>(pos.stop) : -1;  // after the last stop: the transition that led here
}

std::optional<Ticks> RollState::jiStopTickAt(doc::JiObjectId id, float x) const {
  const doc::JiObject* o = jiObject(id);
  if (!o || o->length < 2) return std::nullopt;
  const Ticks raw = static_cast<Ticks>(std::llround(tickAtX(x))) - o->start;
  if (raw <= 0 || raw >= o->length) return std::nullopt;
  const Grid g = grid();
  const Ticks snapped = (g.snapping() ? g.snap(raw + o->start) : raw + o->start) - o->start;
  return (snapped > 0 && snapped < o->length) ? snapped : raw;
}

float RollState::jiPointerX() const {
  if (hoverPartPos_) return hoverPartPos_->x;
  if (!clip_) return 0.0f;
  const Ticks play = ctx.transport().playhead().tick;
  return xOfTick(static_cast<double>(play - clip_->start + clip_->contentStart));
}

std::optional<ui::Vec2> RollState::pathPoint(const doc::Note& n, double frac) const {
  if (!onPath(n, n.pitch)) return std::nullopt;
  const double off = std::clamp(frac, 0.0, 1.0) * static_cast<double>(n.length);
  const std::optional<double> c = pathCents(n, n.start, n.pitch, off);
  if (!c) return std::nullopt;
  return ui::Vec2{xOfTick(static_cast<double>(n.start) + off), yOfCents(*c)};
}

ui::IRect RollState::pathBox(const doc::Note& n, Ticks start, Ticks length, const tuning::PitchId& pitch) const {
  const float x0 = std::round(xOfTick(static_cast<double>(start)));
  const float x1 = std::max(x0 + 3.0f, std::round(xOfTick(static_cast<double>(start + length))));
  const int samples = std::clamp(static_cast<int>((x1 - x0) / 8.0f) + 2, 2, 256);
  float lo = 1e30f, hi = -1e30f;
  for (int k = 0; k < samples; ++k) {
    const double off = static_cast<double>(length) * k / (samples - 1);
    const std::optional<double> c = pathCents(n, start, pitch, off);
    if (!c) return {};
    const float y = yOfCents(*c);
    lo = std::min(lo, y);
    hi = std::max(hi, y);
  }
  const float h = pathHalfThickness() + 2.0f;
  return {static_cast<int32_t>(x0) - 1, static_cast<int32_t>(std::floor(lo - h)), static_cast<int32_t>(x1 - x0) + 2,
          static_cast<int32_t>(std::ceil(hi + h)) - static_cast<int32_t>(std::floor(lo - h))};
}

// --- stops ------------------------------------------------------------------------------------------------------------

bool RollState::addJiStopAt(doc::JiObjectId id, Ticks at, std::string_view proportion, std::string* error) {
  auto fail = [&](std::string m) {
    ctx.setStatus("JI Stop: " + m);
    if (error) *error = std::move(m);
    return false;
  };
  const doc::JiObject* o = jiObject(id);
  if (!o) return fail("no JI object");
  Result<JiProportion> pr = parseProportion(proportion);
  if (!pr) return fail(pr.error().message);
  if (pr->undertone != o->undertone)
    return fail(o->undertone ? "this is an undertone object: type 1/4:1/5:1/6" : "this is an overtone object: type 4:5:6");
  if (at <= 0 || at >= o->length) return fail("a stop goes inside the object");
  Ticks prev = 0;
  for (const doc::JiStop& s : o->stops)
    if (s.at < at) prev = s.at;
  const Ticks glide = std::min(gridStepAt(grid(), o->start + at), at - prev);
  const doc::ops::EditResult e = doc::ops::addJiStop(*ctx.doc(), ctx.store().ids(), ref_, id, at, pr->terms, glide);
  if (!e) return fail(e.error().message);
  if (!applyJi(e, "Add JI Stop", id)) return fail("nothing changed");
  for (size_t i = 0; const doc::JiObject* now = jiObject(id); ++i) {  // select the new stop
    if (i >= now->stops.size()) break;
    if (now->stops[i].at == at) jiStopSel_ = static_cast<int32_t>(i);
  }
  return true;
}

bool RollState::addJiTuningStopAt(doc::JiObjectId id, Ticks at) {
  const doc::JiObject* o = jiObject(id);
  if (!o) return false;
  if (at <= 0 || at >= o->length) {
    ctx.setStatus("Back to Tuning: point inside the object");
    return false;
  }
  Ticks prev = 0;
  for (const doc::JiStop& s : o->stops)
    if (s.at < at) prev = s.at;
  const Ticks glide = std::min(gridStepAt(grid(), o->start + at), at - prev);
  if (!applyJi(doc::ops::addJiTuningStop(*ctx.doc(), ref_, id, at, glide), "Back to Tuning", id)) return false;
  for (size_t i = 0; const doc::JiObject* now = jiObject(id); ++i) {
    if (i >= now->stops.size()) break;
    if (now->stops[i].at == at) jiStopSel_ = static_cast<int32_t>(i);
  }
  return true;
}

bool RollState::retypeJiStop(doc::JiObjectId id, uint32_t stop, std::string_view proportion, std::string* error) {
  auto fail = [&](std::string m) {
    ctx.setStatus("JI Stop: " + m);
    if (error) *error = std::move(m);
    return false;
  };
  const doc::JiObject* o = jiObject(id);
  if (!o || stop >= o->stops.size()) return fail("no such stop");
  Result<JiProportion> pr = parseProportion(proportion);
  if (!pr) return fail(pr.error().message);
  if (pr->undertone != o->undertone)
    return fail(o->undertone ? "this is an undertone object: type 1/4:1/5:1/6" : "this is an overtone object: type 4:5:6 (Undertone is in its menu)");
  std::vector<doc::JiStop> stops = o->stops;
  doc::JiStop& s = stops[stop];
  if (!s.isTuning() && s.terms == pr->terms) return true;  // unchanged
  // Stop 0 keeps its lowest voice where it is (the chord stays in its box, as the JI tool placed it); a later stop
  // stays on its scale (the same fundamental).
  if (stop == 0 && !s.isTuning() && !s.terms.empty()) {
    const std::optional<tuning::Rational> scale =
        o->undertone ? tuning::mul(s.scale, tuning::Rational(pr->terms.back(), s.terms.back()))
                     : tuning::mul(s.scale, tuning::Rational(s.terms.front(), pr->terms.front()));
    if (!scale) return fail("the proportion is out of range");
    s.scale = *scale;
  }
  s.terms = pr->terms;
  s.pitches.clear();
  s.voiceTerm.clear();
  const doc::ops::EditResult e = doc::ops::setJiStops(*ctx.doc(), ctx.store().ids(), ref_, id, std::move(stops));
  if (!e) return fail(e.error().message);
  const int32_t keepStop = static_cast<int32_t>(stop);
  if (!applyJi(e, "Retype JI Stop", id)) return fail("nothing changed");
  jiStopSel_ = keepStop;
  return true;
}

bool RollState::deleteJiStop(doc::JiObjectId id, uint32_t stop) {
  const doc::JiObject* o = jiObject(id);
  if (!o || stop >= o->stops.size()) return false;
  if (o->stops.size() < 2) {
    ctx.setStatus("Delete Stop: an object keeps one stop (delete the object instead)");
    return false;
  }
  std::vector<doc::JiStop> stops = o->stops;
  stops.erase(stops.begin() + static_cast<std::ptrdiff_t>(stop));
  stops.front().at = 0;  // the first stop is the object's start
  if (!applyJi(doc::ops::setJiStops(*ctx.doc(), ctx.store().ids(), ref_, id, std::move(stops)), "Delete JI Stop", id)) return false;
  jiStopSel_ = -1;
  return true;
}

bool RollState::setJiVoiceShape(doc::JiObjectId id, uint32_t stop, uint32_t voice, doc::JiShape::Kind kind) {
  const doc::JiObject* o = jiObject(id);
  if (!o || stop >= o->stops.size() || voice >= o->voices()) return false;
  std::vector<doc::JiStop> stops = o->stops;
  doc::JiStop& s = stops[stop];
  if (s.voiceTiming.size() != o->voices()) s.voiceTiming.assign(o->voices(), std::nullopt);
  doc::JiTiming t = s.voiceTiming[voice].value_or(s.timing);
  t.shape.kind = kind;
  s.voiceTiming[voice] = t;
  return applyJi(doc::ops::setJiStops(*ctx.doc(), ctx.store().ids(), ref_, id, std::move(stops)), "Transition Shape", id);
}

bool RollState::setJiVoiceTiming(doc::JiObjectId id, uint32_t stop, uint32_t voice, std::optional<Ticks> glide, std::optional<Ticks> delay) {
  const doc::JiObject* o = jiObject(id);
  if (!o || stop >= o->stops.size() || voice >= o->voices()) return false;
  std::vector<doc::JiStop> stops = o->stops;
  doc::JiStop& s = stops[stop];
  if (s.voiceTiming.size() != o->voices()) s.voiceTiming.assign(o->voices(), std::nullopt);
  doc::JiTiming t = s.voiceTiming[voice].value_or(s.timing);
  if (glide) t.glide = std::max<Ticks>(0, *glide);
  if (delay) t.delay = std::max<Ticks>(0, *delay);
  s.voiceTiming[voice] = t;
  return applyJi(doc::ops::setJiStops(*ctx.doc(), ctx.store().ids(), ref_, id, std::move(stops)), "Transition Timing", id);
}

bool RollState::setJiTimingForAll(doc::JiObjectId id, uint32_t stop, uint32_t voice) {
  const doc::JiObject* o = jiObject(id);
  if (!o || stop >= o->stops.size() || voice >= o->voices()) return false;
  std::vector<doc::JiStop> stops = o->stops;
  doc::JiStop& s = stops[stop];
  if (s.voiceTiming.size() == o->voices() && s.voiceTiming[voice]) s.timing = *s.voiceTiming[voice];
  s.voiceTiming.clear();
  return applyJi(doc::ops::setJiStops(*ctx.doc(), ctx.store().ids(), ref_, id, std::move(stops)), "Transition for All Voices", id);
}

bool RollState::walkJiSelection(int32_t steps, bool pinHighest) {
  const doc::JiObjectId id = jiActive();
  const doc::JiObject* o = jiObject(id);
  const JiView* v = jiViewOf(id);
  if (!o || !v || jiTermSel_ < 0 || v->labelStop < 0 || jiTermSel_ >= static_cast<int32_t>(v->voiceEntry.size())) return false;
  const uint32_t stop = static_cast<uint32_t>(v->labelStop);
  const uint32_t term = v->voiceEntry[static_cast<size_t>(jiTermSel_)];
  // The pinned voice: the lowest in pitch (an undertone object's lowest voice is its LAST entry, 1 / the largest term).
  const uint32_t pin = (pinHighest != o->undertone) ? static_cast<uint32_t>(o->stops[stop].terms.size()) - 1 : 0;
  const int32_t keep = jiTermSel_;
  if (!applyJi(doc::ops::walkJiTerm(*ctx.doc(), ref_, id, stop, term, steps, pin), "Walk JI Term", id)) return false;
  jiTermSel_ = keep;
  return true;
}

bool RollState::toggleJiUndertone(doc::JiObjectId id) {
  const doc::JiObject* o = jiObject(id);
  if (!o) return false;
  doc::JiObject m = *o;
  m.undertone = !o->undertone;
  // Every proportion keeps its lowest voice where it is: overtone lowest = scale · min, undertone lowest = scale / max.
  for (doc::JiStop& s : m.stops) {
    if (s.isTuning() || s.terms.empty()) continue;
    const tuning::Rational span(s.terms.front() * s.terms.back());
    const std::optional<tuning::Rational> scale = m.undertone ? tuning::mul(s.scale, span) : tuning::div(s.scale, span);
    if (!scale) {
      ctx.setStatus("Undertone: the proportion is out of range");
      return false;
    }
    s.scale = *scale;
  }
  return applyJi(doc::ops::setJiObject(*ctx.doc(), ctx.store().ids(), ref_, std::move(m)), "Undertone", id);
}

// --- Tune to JI, back to the tuning -----------------------------------------------------------------------------------

bool RollState::tuneSelectionToJi() {
  if (!clip_) return false;
  const std::vector<doc::NoteKey> keys = selectionKeys();
  if (keys.empty()) {
    ctx.setStatus("Tune to JI: select the notes of a chord first");
    return false;
  }
  doc::ops::JiFromNotesOptions opt;
  opt.glide = gridStepAt(grid(), keys.front().start);
  const doc::ops::EditResult e = doc::ops::jiFromNotes(*ctx.doc(), ctx.store().ids(), ref_, keys, opt);
  if (!e) {
    ctx.setStatus("Tune to JI: " + e.error().message);
    return false;
  }
  const doc::JiObjectId id = e->jiObjects.empty() ? doc::JiObjectId{} : e->jiObjects.front();
  if (!applyJi(e, "Tune to JI", id)) return false;
  if (const JiView* v = jiViewOf(id); v && v->stops.size() > 1) ctx.setStatus("Tuned to JI: " + v->stops[0].label + " \xE2\x86\x92 " + v->stops[1].label);
  return true;
}

bool RollState::jiTuningStopAtPointer() {
  const doc::JiObjectId id = jiActive();
  if (!jiObject(id)) return false;
  const std::optional<Ticks> at = jiStopTickAt(id, jiPointerX());
  if (!at) {
    ctx.setStatus("Back to Tuning: point inside the object (or put the playhead there)");
    return false;
  }
  return addJiTuningStopAt(id, *at);
}

// --- the fundamental with the mouse (A4) -------------------------------------------------------------------------------

bool RollState::lockFundamentalAt(doc::NoteId note, uint32_t n, bool subharmonic, std::string* error) {
  auto fail = [&](std::string m) {
    ctx.setStatus("Fundamental: " + m);
    if (error) *error = std::move(m);
    return false;
  };
  if (!clip_) return fail("no clip");
  const doc::Note* nt = findNote(note);
  if (!nt) return fail("no such note");
  if (n < 1 || n > 4096) return fail("type a harmonic number from 1 to 4096");
  const bool exact = nt->pitch.kind() == tuning::PitchKind::Ratio && nt->pitch.anchorKind() == tuning::AnchorKind::Reference;
  std::optional<tuning::Rational> r;
  double off = 0.0;  // how far the note is from the ratio the lock uses (0: exact)
  if (exact) {
    r = nt->pitch.ratio();
  } else {
    const std::optional<double> c = pathCents(*nt, nt->start, nt->pitch, 0.0);
    if (!c) return fail("the note's pitch cannot be resolved");
    r = doc::ops::simplestRatioNear(*c, doc::ops::ratioSnapTolerance(tuning_, *c));
    if (!r) return fail("no simple ratio is near this note");
    off = *c - tuning::cents(*r);
  }
  const tuning::Rational k(static_cast<int64_t>(n));
  const std::optional<tuning::Rational> F = subharmonic ? tuning::mul(*r, k) : tuning::div(*r, k);
  if (!F || !F->isPositive()) return fail("the fundamental is out of range");
  const std::vector<uint32_t> keep = sel_;
  bool ok = false;
  if (exact && !subharmonic) {
    ok = clip_->fundamental.lock == F || apply(doc::ops::lockFundamental(*ctx.doc(), ref_, doc::keyOf(*nt), n), "Lock Fundamental");
  } else if (clip_->fundamental.lock == F) {
    ok = true;
  } else {
    doc::ClipFundamental cf = clip_->fundamental;
    cf.lock = F;
    doc::ops::Edit e;
    e.command = doc::Batch{"Lock Fundamental", {doc::SetClipFundamental{ref_, std::move(cf)}}};
    e.noteClip = ref_;
    ok = apply(doc::ops::EditResult(std::move(e)), "Lock Fundamental");
  }
  if (!ok) return fail("the document refused the fundamental");
  select(keep);
  setShowHarmonics(true);
  char buf[160];
  if (std::fabs(off) >= 0.05)
    std::snprintf(buf, sizeof buf, "Fundamental locked at %lld/%lld: the note is %sharmonic %u (%+.1f \xC2\xA2 from it); drawing snaps to the harmonics",
                  static_cast<long long>(F->num()), static_cast<long long>(F->den()), subharmonic ? "sub" : "", n, off);
  else
    std::snprintf(buf, sizeof buf, "Fundamental locked at %lld/%lld: the note is %sharmonic %u; drawing snaps to the harmonics",
                  static_cast<long long>(F->num()), static_cast<long long>(F->den()), subharmonic ? "sub" : "", n);
  ctx.setStatus(buf);
  return true;
}

// --- inline fields ----------------------------------------------------------------------------------------------------

ui::TextEdit* RollState::openJiField(std::string_view name, std::string_view title, std::string_view info, std::string_view initial,
                                     ui::IRect anchor, std::function<bool(RollState&, const std::string&, std::string&)> commit) {
  if (!clip_ || !noteArea) return nullptr;
  ui::OverlayLayer* layer = ui::OverlayLayer::of(*noteArea);
  if (!layer) return nullptr;
  anchor = intersect(anchor, area());
  if (anchor.empty()) anchor = {area().x + 8, area().y + 8, 1, 1};
  auto pop = std::make_unique<ui::Popover>(layer->uniqueName(name));
  ui::Popover* popPtr = pop.get();
  pop->setSize(ui::Length::fixed(280), ui::Length::fixed(78));
  pop->setInfo(std::string(title), std::string(info));
  pop->add<ui::Label>("title", std::string(title), ui::FontRoleId::Small, ui::TextTone::Secondary).setHeight(ui::Length::fixed(16));
  auto& edit = pop->add<ui::TextEdit>("value");
  edit.setHeight(ui::Length::fixed(20));
  if (!initial.empty()) edit.setText(std::string(initial));
  auto& err = pop->add<ui::Label>("error", "", ui::FontRoleId::Small, ui::TextTone::Primary);
  err.setHeight(ui::Length::fixed(16));
  std::shared_ptr<RollState*> alive = self_;
  ui::TextEdit* editPtr = &edit;
  ui::Label* errPtr = &err;
  edit.onEnter = [alive, layer, popPtr, editPtr, errPtr, commit = std::move(commit)] {
    RollState* s = *alive;
    if (!s) return;
    std::string why;
    const std::string text = editPtr->text();
    if (!commit(*s, text, why)) {
      errPtr->setText(why);  // the field stays open; nothing changed
      return;
    }
    if (layer->isOpen(*popPtr)) layer->close(*popPtr);
    // The keys go back to the notes (T, Delete, the next stop's digits).
    if (RollState* st = *alive; st && st->noteArea && st->noteArea->tree()) st->noteArea->tree()->setFocus(st->noteArea);
  };
  edit.onCancel = [alive, layer, popPtr] {
    if (layer->isOpen(*popPtr)) layer->close(*popPtr);
    if (RollState* st = *alive; st && st->noteArea && st->noteArea->tree()) st->noteArea->tree()->setFocus(st->noteArea);
  };
  ui::OverlayOptions o;
  o.anchor = anchor;
  o.placement = ui::Placement::Below;
  layer->open(std::move(pop), std::move(o));
  if (ui::UiTree* t = layer->tree()) {
    t->layoutNow();
    t->setFocus(&edit);
    if (!initial.empty()) edit.buffer().selectAll();
  }
  return &edit;
}

ui::TextEdit* RollState::openJiStopEntry(doc::JiObjectId id, Ticks at) {
  const JiView* v = jiViewOf(id);
  const doc::JiObject* o = jiObject(id);
  if (!v || !o) return nullptr;
  const ui::Rect b = jiBoxRect(*v);
  const float x = xOfTick(static_cast<double>(o->start + at));
  const ui::IRect anchor{static_cast<int32_t>(x), static_cast<int32_t>(b.y0), 2, static_cast<int32_t>(b.y1 - b.y0)};
  return openJiField("ji-stop", o->undertone ? "New stop: 1/4:1/5:1/6" : "New stop: a proportion such as 4:5:6",
                     "Type the proportion the chord morphs to at this point and press Enter. It stands on the same "
                     "fundamental as the stop before it. Esc closes.",
                     "", anchor, [id, at](RollState& s, const std::string& text, std::string& why) { return s.addJiStopAt(id, at, text, &why); });
}

ui::TextEdit* RollState::openJiStopRetype(doc::JiObjectId id, uint32_t stop) {
  const JiView* v = jiViewOf(id);
  const doc::JiObject* o = jiObject(id);
  if (!v || !o || stop >= o->stops.size()) return nullptr;
  jiLayout(*v, jiLayoutScratch_);
  ui::IRect anchor{};
  for (const JiLayout::Label& l : jiLayoutScratch_.stopLabels)
    if (l.index == static_cast<int32_t>(stop))
      anchor = {static_cast<int32_t>(l.box.x0), static_cast<int32_t>(l.box.y0), static_cast<int32_t>(l.box.x1 - l.box.x0),
                static_cast<int32_t>(l.box.y1 - l.box.y0)};
  const doc::JiStop& s = o->stops[stop];
  return openJiField("ji-stop", s.isTuning() ? "Stop: a proportion instead of the tuning" : "Stop: its proportion",
                     "Retype the stop's proportion and press Enter. Esc closes.",
                     s.isTuning() ? std::string() : proportionText(s, o->undertone), anchor,
                     [id, stop](RollState& st, const std::string& text, std::string& why) { return st.retypeJiStop(id, stop, text, &why); });
}

ui::TextEdit* RollState::openHarmonicEntry(doc::NoteId note, bool subharmonic) {
  const doc::Note* n = findNote(note);
  if (!n) return nullptr;
  ui::IRect anchor = onPath(*n, n->pitch) ? pathBox(*n, n->start, n->length, n->pitch) : screenRectOf(n->start, n->length, n->pitch);
  return openJiField("harmonic-entry", subharmonic ? "This note is subharmonic number:" : "This note is harmonic number:",
                     subharmonic ? "Type N and press Enter: the virtual fundamental is locked N times above this note (the note "
                                   "is its N-th subharmonic). Esc closes."
                                 : "Type N and press Enter: the virtual fundamental is locked at this note's pitch / N. Its "
                                   "harmonics are drawn as guide lines and drawing snaps to them (Shift draws on the rows). "
                                   "Unlock Fundamental is in the same menu. Esc closes.",
                     "", anchor, [note, subharmonic](RollState& s, const std::string& text, std::string& why) {
                       const std::string_view t = trimmed(text);
                       uint32_t k = 0;
                       const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), k);
                       if (t.empty() || ec != std::errc{} || end != t.data() + t.size()) {
                         why = "type a whole number such as 5";
                         return false;
                       }
                       return s.lockFundamentalAt(note, k, subharmonic, &why);
                     });
}

ui::TextEdit* RollState::openJiTimingEntry(doc::JiObjectId id, uint32_t stop, uint32_t voice, bool delay) {
  const JiView* v = jiViewOf(id);
  const doc::JiObject* o = jiObject(id);
  if (!v || !o || stop >= o->stops.size() || voice >= o->voices()) return nullptr;
  const doc::JiTransition tr = doc::jiTransition(*o, voice, stop);
  const doc::JiStop& s = o->stops[stop];
  const doc::JiTiming t = (s.voiceTiming.size() == o->voices() && s.voiceTiming[voice]) ? *s.voiceTiming[voice] : s.timing;
  const ui::Rect b = jiBoxRect(*v);
  const ui::IRect anchor{static_cast<int32_t>(xOfTick(static_cast<double>(o->start + tr.arrive))), static_cast<int32_t>(b.y0), 2,
                         static_cast<int32_t>(b.y1 - b.y0)};
  return openJiField("ji-timing", delay ? "This voice arrives later by (beats):" : "This voice glides for (beats):",
                     delay ? "How many beats after the stop this voice arrives (0 = on the stop). Enter applies, Esc closes."
                           : "How many beats this voice glides before it arrives (0 = a jump). Enter applies, Esc closes.",
                     beatsText(delay ? t.delay : t.glide), anchor,
                     [id, stop, voice, delay](RollState& st, const std::string& text, std::string& why) {
                       const std::string_view tx = trimmed(text);
                       double beats = 0.0;
                       const auto [end, ec] = std::from_chars(tx.data(), tx.data() + tx.size(), beats);
                       if (tx.empty() || ec != std::errc{} || end != tx.data() + tx.size() || !std::isfinite(beats) || beats < 0.0 || beats > 4096.0) {
                         why = "type a number of beats such as 0.5";
                         return false;
                       }
                       const Ticks ticks = static_cast<Ticks>(std::llround(beats * static_cast<double>(kPpq)));
                       const doc::JiObject* ob = st.jiObject(id);
                       if (!ob || stop >= ob->stops.size()) return false;
                       const doc::JiStop& sp = ob->stops[stop];
                       const doc::JiTiming cur = (sp.voiceTiming.size() == ob->voices() && sp.voiceTiming[voice]) ? *sp.voiceTiming[voice] : sp.timing;
                       if ((delay ? cur.delay : cur.glide) == ticks) return true;  // unchanged
                       if (!st.setJiVoiceTiming(id, stop, voice, delay ? std::nullopt : std::optional<Ticks>(ticks),
                                                delay ? std::optional<Ticks>(ticks) : std::nullopt)) {
                         why = "the object refused that timing";
                         return false;
                       }
                       return true;
                     });
}

// --- copy / paste of an object ----------------------------------------------------------------------------------------

bool RollState::copyJiSelection() {
  const doc::JiObject* o = jiObject(jiSel_);
  if (!o) return false;
  JiClipboard cb;
  cb.object = *o;
  for (const doc::Note& n : clip_->notes) {
    if (doc::jiObjectOf(n.pitch) != o->id) continue;
    cb.notes.push_back(n);
    if (const doc::NoteCurves* nc = doc::findNoteCurves(*clip_, n.id)) cb.curves.push_back(*nc);
  }
  jiClipboard_ = std::move(cb);
  clipboard_.clear();
  clipboardCurves_.clear();
  insertTick_ = o->end();  // paste right after it unless the user clicks elsewhere
  ctx.setStatus(jiClipboard_->notes.empty() ? "JI object copied" : "JI object copied with its notes");
  return true;
}

bool RollState::pasteJiObject() {
  if (!clip_ || !jiClipboard_) return false;
  const Ticks at = insertTick_.value_or(0);
  const doc::ops::EditResult e =
      doc::ops::pasteJiObject(*ctx.doc(), ctx.store().ids(), ref_, jiClipboard_->object, jiClipboard_->notes, jiClipboard_->curves, at);
  if (!applyJi(e, "Paste JI Object", e && !e->jiObjects.empty() ? e->jiObjects.front() : doc::JiObjectId{})) return false;
  insertTick_ = at + jiClipboard_->object.length;
  return true;
}

}  // namespace pq::views::roll
