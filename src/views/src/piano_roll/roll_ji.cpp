// Pitch paths and declaring JI in the piano roll (T7.4 / T7.13; docs/plans/M7.md "Rework: notes-first JI").
//
// The note is the only primitive (T7.13). What the roll keeps here until the group UI (T7.14):
//   - pitch paths: every note with a pitch curve is drawn along its pitch path instead of on a row
//     (roll_paint.cpp paintCurveNotes: the same line as every other note, T7.12). pathCents is the pitch of any note
//     at any tick (hit-testing and drawing along curves: noteAt uses it; the pen records against it), pathBox /
//     pathPoint give a path note's screen extent.
//   - "Is Harmonic…" on a note (declareHarmonic → ops::makeJiGroup): the note becomes the frozen anchor of a JI
//     group; the active group's harmonic series is drawn as guide lines (RollState::harmonics), the plain draw puts
//     members on it (DrawPitch::Ratio), and members carry their harmonic numbers (RollState::harmonicOf).
//   - parseProportion ("8:9:10:12:15") and the inline text field (openJiField) the proportion tool and the vibrato
//     entry use.
// Every edit is one doc::ops builder dispatched once: one undo step. Nothing here runs per frame.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>

#include "pq/document/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/ui/overlay.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

float scaleOf(const RollState& s) noexcept {
  return (s.noteArea && s.noteArea->tree()) ? s.noteArea->tree()->scale() : 1.0f;
}

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message), {}}; }

std::string_view trimmed(std::string_view t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
  return t;
}

}  // namespace

// --- pitch paths ------------------------------------------------------------------------------------------------

float RollState::pathHalfThickness() const noexcept {
  const float sc = scaleOf(*this);
  const float lane = static_cast<float>(std::max(1, rows_.laneHeight()));
  return 0.5f * std::clamp(0.55f * lane, 3.0f * sc, 9.0f * sc);
}

float RollState::yOfCents(double cents) const noexcept {
  if (rows_.empty()) return static_cast<float>(area().y);
  return static_cast<float>(static_cast<double>(area().y - scrollY_) + rows_.yOfCents(cents));
}

bool RollState::onPath(const doc::Note& n, const tuning::PitchId&) const noexcept {
  if (!clip_) return false;
  if (!clip_->curves && curvePreview_.empty()) return false;
  return !curvePoints(CurveTarget{n.id}).empty();
}

std::optional<double> RollState::pathCents(const doc::Note& n, Ticks start, const tuning::PitchId& pitch, double offset) const {
  std::optional<double> base = pathBaseCents(start, pitch, offset);
  if (!base) return std::nullopt;
  // T7.6: the note's own curve (a gesture's preview first).
  if (clip_->curves || !curvePreview_.empty())
    if (const std::span<const doc::Breakpoint> pts = curvePoints(CurveTarget{n.id}); !pts.empty())
      *base += pq::evalAutomation(pts, offset);
  return base;
}

std::optional<double> RollState::pathBaseCents(Ticks, const tuning::PitchId& pitch, double) const {
  if (!clip_) return std::nullopt;
  return centsOf(pitch);
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

// --- declaring JI -----------------------------------------------------------------------------------------------

DrawPitch RollState::drawPitchFor(ui::Mods m) const noexcept {
  if (m & mod::kAlt) return DrawPitch::Overlay;
  // With an active JI group drawing snaps to its harmonics; Shift then draws on the rows.
  const bool group = activeJiGroup() != nullptr;
  return (((m & mod::kShift) != 0) != group) ? DrawPitch::Ratio : DrawPitch::Row;
}

Result<std::vector<int64_t>> RollState::parseProportion(std::string_view text) {
  std::vector<int64_t> out;
  size_t pos = 0;
  if (trimmed(text).empty()) return invalid("type a proportion such as 4:5:6");
  while (pos <= text.size()) {
    const size_t colon = text.find(':', pos);
    const std::string_view part = trimmed(text.substr(pos, colon == std::string_view::npos ? std::string_view::npos : colon - pos));
    pos = colon == std::string_view::npos ? text.size() + 1 : colon + 1;
    int64_t term = 0;
    const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), term);
    if (part.empty() || ec != std::errc{} || end != part.data() + part.size())
      return invalid("\"" + std::string(part) + "\" is not a whole number: type a proportion such as 4:5:6");
    if (term < 1 || term > doc::kMaxFundamentalDenominator)
      return invalid("terms go from 1 to " + std::to_string(doc::kMaxFundamentalDenominator));
    out.push_back(term);
  }
  if (out.size() > doc::ops::kMaxProportionTerms) return invalid("at most " + std::to_string(doc::ops::kMaxProportionTerms) + " terms");
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i] <= out[i - 1]) return invalid("terms must ascend, such as 4:5:6");
  return out;
}

bool RollState::declareHarmonic(doc::NoteId note, uint32_t n, std::string* error) {
  auto fail = [&](std::string m) {
    ctx.setStatus("Is Harmonic: " + m);
    if (error) *error = std::move(m);
    return false;
  };
  if (!clip_) return fail("no clip");
  const doc::Note* nt = findNote(note);
  if (!nt) return fail("no such note");
  if (n < 1 || n > doc::kMaxHarmonicIndex) return fail("type a harmonic number from 1 to 4096");
  if (!pathCents(*nt, nt->start, nt->pitch, 0.0)) return fail("the note's pitch cannot be resolved");
  std::vector<uint32_t> keep = sel_;
  if (keep.empty()) keep.push_back(note.value);  // the declared note's group is the active one
  const doc::ops::EditResult e = doc::ops::makeJiGroup(*ctx.doc(), ctx.store().ids(), ref_, doc::keyOf(*nt), n);
  if (!e) return fail(e.error().message);
  if (!e->empty() && !apply(e, "Is Harmonic")) return fail("the document refused the harmonic");
  select(std::move(keep));
  if (const doc::JiGroup* g = activeJiGroup(); !g || g->anchor != note) selectOnly(note);  // its group is the active one
  setShowHarmonics(true);
  char buf[160];
  std::snprintf(buf, sizeof buf, "The note is harmonic %u: it stays where it is, drawing snaps to its harmonic series (Shift: the rows)", n);
  ctx.setStatus(buf);
  return true;
}

bool RollState::removeActiveJiGroup() {
  const doc::JiGroup* g = activeJiGroup();
  if (!g) return false;
  const std::vector<uint32_t> keep = sel_;
  if (!apply(doc::ops::removeJiGroup(*ctx.doc(), ref_, g->id), "Remove JI Group")) return false;
  select(keep);
  return true;
}

// --- inline fields ----------------------------------------------------------------------------------------------

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
    // The keys go back to the notes.
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

ui::TextEdit* RollState::openHarmonicEntry(doc::NoteId note) {
  const doc::Note* n = findNote(note);
  if (!n) return nullptr;
  ui::IRect anchor = onPath(*n, n->pitch) ? pathBox(*n, n->start, n->length, n->pitch) : screenRectOf(n->start, n->length, n->pitch);
  return openJiField("harmonic-entry", "This note is harmonic number:",
                     "Type N and press Enter: this note becomes harmonic N of a series and stays exactly where it is. The "
                     "series is drawn as guide lines; notes you draw snap to it as exact ratios to this note (Shift draws on "
                     "the rows). A note placed between the lines renumbers the series (5 and a 3/2 above it read 10 and "
                     "15). Remove JI Group is in the same menu. Esc closes.",
                     "", anchor, [note](RollState& s, const std::string& text, std::string& why) {
                       const std::string_view t = trimmed(text);
                       uint32_t k = 0;
                       const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), k);
                       if (t.empty() || ec != std::errc{} || end != t.data() + t.size()) {
                         why = "type a whole number such as 5";
                         return false;
                       }
                       return s.declareHarmonic(note, k, &why);
                     });
}

}  // namespace pq::views::roll
