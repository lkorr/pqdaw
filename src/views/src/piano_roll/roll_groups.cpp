// T7.14 piano roll: JI groups as the user reads them (docs/plans/M7.md "Rework: notes-first JI" R3–R6; the user:
// "the normal piano roll notes we're laying out should look more like these ji objects").
//
// A group is ordinary notes that share a fundamental: an anchor note (frozen) and the notes tied to it as exact
// ratios. The roll shows it as the inspiration's box, but the box is only a FRAME around notes:
//   frame      a translucent dashed box around the group's notes (doc::groupSpan × their pitch range, padded); its
//              header above it is the proportion ("4:5:6:7"; with warps the states in order: "12-EDO → 4:5:6");
//              every note carries its harmonic number at its left end (roll_paint.cpp; the anchor's with a diamond).
//              The frame takes no clicks: inside it notes and empty space behave as everywhere. Its HEADER is
//              clickable (selects the group's notes; a double-click on a tuned group's proportion opens the chooser,
//              T7.16a: a single click leaves the arrow keys to the selected chord), and so are the grips on its
//              bottom border.
//   Tune to JI Ctrl+Shift+J / the menu on selected notes, split into chords by overlapping time (T7.16a M6): one
//              chord → ops::tuneToJi with the best reading; several, or a group Back to Tuning →
//              ops::tuneChordsToJi; notes that all are (in) groups in JI → ops::setJiGroupsTuned(false): Back to
//              Tuning, a STATE the group keeps (M1: the reading and the warps come back with the next press; the frame
//              stays, headed "12-EDO (4:5:6)"). The chooser lists ops::tuneToJiCandidates (proportion, total error).
//   warps      "play a tempered chord, then warp it into JI": drag from the frame's bottom-left corner grip inward:
//              before the marker the members sound (and are drawn) as written, then glide into JI; from the
//              bottom-right corner: back to the tuning. A marker is a dotted line through the frame with a diamond on
//              its bottom border (drag: move; click: select, Delete removes; right-click: Straight / S-Curve /
//              Remove) and a round glide grip (drag: the glide's length); under the frame the states are named
//              ("12-EDO" | "4:5:6"). Stored in group ticks (ops::setJiWarps).
//   walk       Alt+Up / Alt+Down: ops::walkJiGroup ± 1 on the active group.
//   proportion J, drag a box, type "8:9:10:12:15", Enter: ops::proportionNotes (also "Insert Proportion…" in the
//              empty-space menu).
//   ratio      "Add Note at Ratio…" on a note of a group: "3/2", "7:4" or a harmonic number: ops::addGroupNote.
// Every edit is one doc::ops builder dispatched once (one undo step); a warp drag previews the warps it will store
// (the members are re-resolved with them: RollState::WarpPreview).
//
// Cost: groupInfos() is rebuilt once per clip snapshot (O(groups · notes)); a paint visits the groups in view and
// their notes (frame bounds), nothing per note of the clip.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>

#include "../common/paint.h"
#include "pq/document/commands.h"
#include "pq/document/note_bend.h"
#include "pq/tuning/ji.h"
#include "pq/ui/overlay.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

constexpr const char* kArrow = " \xE2\x86\x92 ";  // " → "

float scaleOf(const RollState& s) noexcept { return (s.noteArea && s.noteArea->tree()) ? s.noteArea->tree()->scale() : 1.0f; }

std::string_view trimmed(std::string_view t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
  return t;
}

// "4:5:6:7"; more than `maxTerms` terms: the first ones, "…" and the last.
std::string proportionString(std::span<const int64_t> terms, size_t maxTerms = 8) {
  std::string s;
  for (size_t i = 0; i < terms.size(); ++i) {
    if (terms.size() > maxTerms && i == maxTerms - 2) {
      s += ":\xE2\x80\xA6";
      i = terms.size() - 2;
      continue;
    }
    if (!s.empty()) s += ':';
    s += std::to_string(terms[i]);
  }
  return s;
}

// The width of a small-font string (device px); an estimate without a text system.
int32_t textWidth(const RollState& s, std::string_view text) {
  if (s.noteArea && s.noteArea->tree())
    if (const PaintResources* res = s.noteArea->tree()->resources(); res && res->text && res->uiFonts) return res->text->measure(res->uiFonts->small, text);
  return static_cast<int32_t>(std::lround(6.5f * scaleOf(s) * static_cast<float>(text.size())));
}

// The header of a group with these warps: its states in order (at most three named).
std::string headerOf(const std::string& proportion, const std::string& tuning, std::span<const doc::JiWarp> warps) {
  if (warps.empty()) return proportion;
  bool written = warps.front().toJi;  // the state before the first warp
  std::string s = written ? tuning : proportion;
  size_t shown = 1;
  for (const doc::JiWarp& w : warps) {
    written = !w.toJi;
    if (shown == 3) {
      s += " \xE2\x80\xA6";
      break;
    }
    s += kArrow;
    s += written ? tuning : proportion;
    ++shown;
  }
  return s;
}

std::string beats(Ticks t) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(t) / static_cast<double>(kPpq));
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

void dashedH(DrawList& dl, float x0, float x1, float y, float clipX0, float clipX1, Rgba col, float dash, float gap, float thick) {
  const float period = dash + gap;
  float x = x0 + std::max(0.0f, std::floor((clipX0 - x0) / period)) * period;
  for (; x < x1 && x < clipX1; x += period) dl.addRectFilled({x, y, std::min(x + dash, x1), y + thick}, col);
}
void dashedV(DrawList& dl, float x, float y0, float y1, float clipY0, float clipY1, Rgba col, float dash, float gap, float thick) {
  const float period = dash + gap;
  float y = y0 + std::max(0.0f, std::floor((clipY0 - y0) / period)) * period;
  for (; y < y1 && y < clipY1; y += period) dl.addRectFilled({x, y, x + thick, std::min(y + dash, y1)}, col);
}

// The chooser: a menu that lets the Tune to JI chord through (Ctrl+Shift+J with it open switches back).
class JiChooser final : public Menu {
public:
  JiChooser(std::vector<MenuItem> items, std::string name, std::shared_ptr<RollState*> state) : Menu(std::move(items), std::move(name)), state_(std::move(state)) {}
  bool onKey(const KeyEvent& e) override {
    if (e.down && (e.key == 'j' || e.key == 'J') && (e.mods & mod::kCtrl) && (e.mods & mod::kShift)) {
      const std::shared_ptr<RollState*> alive = state_;
      if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*this);  // destroys this
      if (*alive) (void)(*alive)->toggleTuneToJi(false);
      return true;
    }
    return Menu::onKey(e);
  }

private:
  std::shared_ptr<RollState*> state_;
};

}  // namespace

// --- what a group is, per clip snapshot ---------------------------------------------------------------------------

std::string RollState::tuningLabel() const {
  if (const tuning::Edo* e = tuning_.edo()) return std::to_string(e->n()) + "-EDO";
  return tuningName_.empty() ? std::string("tuning") : tuningName_;
}

const std::vector<GroupInfo>& RollState::groupInfos() const {
  if (!clip_ || !clip_->groups) {
    groupInfos_.clear();
    groupInfosFor_ = clip_.get();
    return groupInfos_;
  }
  if (groupInfosFor_ == clip_.get() && groupInfosGen_ == tuningGen_ && groupInfosTuning_ == tuningName_) return groupInfos_;
  groupInfosFor_ = clip_.get();
  groupInfosGen_ = tuningGen_;
  groupInfosTuning_ = tuningName_;
  groupInfos_.clear();
  const std::string tuning = tuningLabel();
  for (const doc::JiGroup& g : doc::jiGroupsOf(*clip_)) {  // sorted by id
    const doc::Note* anchor = clip_->notes.findById(g.anchor);
    const std::optional<doc::GroupHarmonics> gh = anchor ? doc::groupHarmonics(*clip_, g) : std::nullopt;
    const std::optional<doc::GroupSpan> span = gh ? doc::groupSpan(*clip_, g) : std::nullopt;
    if (!span) continue;
    GroupInfo info;
    info.group = &g;
    info.anchor = anchor;
    info.anchorNumber = gh->anchor;
    info.proportion = gh->proportion;
    info.start = span->start;
    info.end = span->end;
    info.origin = span->origin;
    info.inJi = g.tuned;
    for (const doc::Note& n : clip_->notes) {
      const bool member = doc::noteAnchorOf(n.pitch) == g.anchor;
      if (!member && n.id != g.anchor) continue;
      info.notes.push_back(&n);
      if (member && clip_->written && doc::findNoteWritten(*clip_, n.id) != nullptr) info.tuned = true;
    }
    info.proportionText = proportionString(info.proportion);
    // T7.16a: Back to Tuning keeps the group: its header names the tuning it plays in and the reading it keeps.
    info.header = g.tuned ? headerOf(info.proportionText, tuning, g.warps) : tuning + " (" + info.proportionText + ")";
    groupInfos_.push_back(std::move(info));
  }
  return groupInfos_;
}

const GroupInfo* RollState::groupInfo(doc::JiGroupId id) const {
  const std::vector<GroupInfo>& all = groupInfos();
  const auto it = std::lower_bound(all.begin(), all.end(), id, [](const GroupInfo& g, doc::JiGroupId x) { return g.group->id < x; });
  return it != all.end() && it->group->id == id ? &*it : nullptr;
}

std::span<const doc::JiWarp> RollState::shownWarps(const GroupInfo& g) const {
  if (warpPreview_.valid() && warpPreview_.group == g.group->id) return warpPreview_.warps;
  return g.group->warps;
}

double RollState::warpStateAt(const GroupInfo& g, Ticks contentTick) const {
  if (!g.inJi) return 1.0;  // Back to Tuning: written throughout
  return doc::jiWarpWritten(shownWarps(g), static_cast<double>(contentTick - g.origin));
}

GroupFrame RollState::groupFrame(const GroupInfo& g) const {
  GroupFrame f;
  if (!clip_ || rows_.empty() || g.notes.empty()) return f;
  double lo = 1e300, hi = -1e300;
  auto take = [&](const std::optional<double>& c) {
    if (!c) return;
    lo = std::min(lo, *c);
    hi = std::max(hi, *c);
  };
  const bool warped = !shownWarps(g).empty() && g.inJi;
  const float sc = scaleOf(*this);
  const float lane = static_cast<float>(std::max(1, rows_.laneHeight()));
  float bottom = -1e30f;  // device y of the frame's bottom border
  for (const doc::Note* n : g.notes) {
    const std::optional<double> c = startCents(*n);
    take(c);
    if (c) {
      float below = 0.5f * lane + 5.0f * sc;
      // T7.16a (M7 review H4): under a vibrating note the frame's bottom (where the warp and corner grips sit) goes
      // below the envelope's corner grips, so the two never share a spot.
      if (const doc::NoteVibrato v = vibratoOf(n->id); v.set()) {
        const float depth = std::fabs(yOfCents(*c + static_cast<double>(v.depthCents)) - yOfCents(*c));
        below = std::max(below, std::max(depth, kVibratoGripRise * sc) + (kVibratoGrip + 2.0f) * sc);
      }
      bottom = std::max(bottom, yOfCents(*c) + below);
    }
    if (!warped || !g.tuned || n == g.anchor) continue;
    // A member a warp moves: both the pitch it was written at and its JI pitch are inside the frame.
    take(centsOf(n->pitch));
    if (const doc::NoteWritten* w = doc::findNoteWritten(*clip_, n->id)) {
      take(centsOf(w->written));
      if (const std::optional<double> wc = centsOf(w->written)) bottom = std::max(bottom, yOfCents(*wc) + 0.5f * lane + 5.0f * sc);
    }
  }
  if (lo > hi) return f;
  const float x0 = std::round(xOfTick(static_cast<double>(g.start))) - std::round(7.0f * sc);
  const float x1 = std::round(xOfTick(static_cast<double>(g.end))) + std::round(7.0f * sc);
  const float y0 = std::round(yOfCents(hi) - 0.5f * lane - 15.0f * sc);  // room for the numbers above the top note
  const float y1 = std::round(std::max(bottom, yOfCents(lo) + 0.5f * lane + 5.0f * sc));
  f.box = {x0, y0, std::max(x1, x0 + 8.0f), y1};
  const float hh = std::round(15.0f * sc);
  const bool preview = warpPreview_.valid() && warpPreview_.group == g.group->id;
  const std::string previewHeader = preview ? headerOf(g.proportionText, tuningLabel(), warpPreview_.warps) : std::string();
  const std::string& header = preview ? previewHeader : g.header;
  const float w = static_cast<float>(textWidth(*this, header));
  f.header = {x0, y0 - hh, x0 + w + 4.0f * sc, y0 - 1.0f};
  // The proportion inside the header (its first occurrence).
  const size_t at = header.find(g.proportionText);
  if (at != std::string::npos && !g.proportionText.empty()) {
    const float before = at == 0 ? 0.0f : static_cast<float>(textWidth(*this, std::string_view(header).substr(0, at)));
    f.proportion = {x0 + before, f.header.y0, x0 + before + static_cast<float>(textWidth(*this, g.proportionText)) + 4.0f * sc, f.header.y1};
  }
  return f;
}

float RollState::warpX(const GroupInfo& g, const doc::JiWarp& w) const {
  return std::round(xOfTick(static_cast<double>(g.origin + w.at)));
}

float RollState::warpGlideX(const GroupInfo& g, const doc::JiWarp& w) const {
  const float x = warpX(g, w);
  return std::max(std::round(xOfTick(static_cast<double>(g.origin + w.at + w.glide))), x + std::round(kWarpGripGap * scaleOf(*this)));
}

bool RollState::gripsShown(const GroupInfo& g) const {
  if (!g.tuned || !g.inJi) return false;  // (Back to Tuning: no warps apply until it is tuned again)
  const doc::JiGroupId id = g.group->id;
  if (hoverGroup_ == id || drag.group == id) return true;
  const doc::JiGroup* active = activeJiGroup();
  return active && active->id == id;
}

doc::JiGroupId RollState::groupFrameAt(ui::Vec2 p) const {
  if (!clip_ || !clip_->groups || rows_.empty()) return {};
  const float half = 0.5f * kWarpGrip * scaleOf(*this);
  doc::JiGroupId hit;
  for (const GroupInfo& g : groupInfos()) {
    const GroupFrame f = groupFrame(g);
    if (f.box.x1 > f.box.x0 && p.x >= f.box.x0 - half && p.x <= f.box.x1 + half && p.y >= f.header.y0 && p.y <= f.box.y1 + half) hit = g.group->id;
  }
  return hit;
}

void RollState::setHoverGroup(doc::JiGroupId id) {
  if (id == hoverGroup_) return;
  hoverGroup_ = id;
  if (noteArea) noteArea->markDirty();
}

GroupHit RollState::groupPartAt(ui::Vec2 p) const {
  GroupHit hit;
  if (!clip_ || !clip_->groups || rows_.empty()) return hit;
  const float sc = scaleOf(*this);
  const float half = 0.5f * kWarpGrip * sc;
  const double t0 = tickAtX(p.x - 4.0f * half), t1 = tickAtX(p.x + 4.0f * half);
  for (const GroupInfo& g : groupInfos()) {
    const GroupFrame f = groupFrame(g);
    if (f.box.x1 <= f.box.x0) continue;
    const doc::JiGroupId id = g.group->id;
    const bool nearX = static_cast<double>(g.end) + 0.0 >= t0 - 1.0 && static_cast<double>(g.start) <= t1 + 1.0;
    if (nearX || (p.x >= f.box.x0 - 2.0f * half && p.x <= f.box.x1 + 2.0f * half)) {
      if (std::fabs(p.y - f.box.y1) <= half) {
        const std::span<const doc::JiWarp> warps = g.inJi ? shownWarps(g) : std::span<const doc::JiWarp>();
        for (size_t i = 0; i < warps.size(); ++i)
          if (std::fabs(p.x - warpX(g, warps[i])) <= half) return {GroupPart::Marker, id, static_cast<int32_t>(i)};
        for (size_t i = 0; i < warps.size(); ++i)
          if (std::fabs(p.x - warpGlideX(g, warps[i])) <= half) return {GroupPart::GlideGrip, id, static_cast<int32_t>(i)};
        if (gripsShown(g)) {
          if (std::fabs(p.x - (f.box.x0 + half)) <= half) return {GroupPart::LeftGrip, id, -1};
          if (std::fabs(p.x - (f.box.x1 - half)) <= half) return {GroupPart::RightGrip, id, -1};
        }
      }
    }
    if (p.y >= f.header.y0 && p.y < f.header.y1 && p.x >= f.header.x0 && p.x < f.header.x1) {
      const bool onProportion = g.tuned && p.x >= f.proportion.x0 && p.x < f.proportion.x1;
      hit = {onProportion ? GroupPart::Proportion : GroupPart::Header, id, -1};  // a later (newer) group's header wins
    }
  }
  return hit;
}

void RollState::selectGroup(doc::JiGroupId id) {
  const GroupInfo* g = groupInfo(id);
  if (!g) return;
  std::vector<uint32_t> ids;
  ids.reserve(g->notes.size());
  for (const doc::Note* n : g->notes) ids.push_back(n->id.value);
  select(std::move(ids));
}

void RollState::forgetActiveGroup() noexcept {
  lastActiveGroup_ = {};
  activeGroupValid_ = false;
  if (noteArea) noteArea->markDirty();
}

// --- Tune to JI -------------------------------------------------------------------------------------------------

bool RollState::toggleTuneToJi(bool chooser) {
  if (!clip_) return false;
  const std::vector<doc::NoteKey> keys = selectionKeys();
  if (keys.empty()) {
    ctx.setStatus("Tune to JI: select the notes of a chord first");
    return false;
  }
  // T7.16a (M7 review H1 / M6): the selection as chords — notes that overlap in time (through each other) are one
  // chord — and what each chord is: all of one group (in JI or Back to Tuning), or notes to tune.
  std::vector<const doc::Note*> notes;
  notes.reserve(keys.size());
  for (const doc::Note& n : clip_->notes)  // in start order
    if (selected(n.id)) notes.push_back(&n);
  std::vector<std::vector<doc::NoteKey>> chords;
  Ticks chordEnd = 0;
  for (const doc::Note* n : notes) {
    if (chords.empty() || n->start >= chordEnd) chords.emplace_back();
    chords.back().push_back(doc::keyOf(*n));
    chordEnd = chords.back().size() == 1 ? n->start + n->length : std::max(chordEnd, n->start + n->length);
  }
  auto groupOfChord = [&](const std::vector<doc::NoteKey>& ch) -> const doc::JiGroup* {
    const doc::JiGroup* one = nullptr;
    for (const doc::NoteKey& k : ch) {
      const doc::Note* n = clip_->notes.locate(k);
      const doc::JiGroup* g = n ? doc::jiGroupOfNote(*clip_, *n) : nullptr;
      if (!g || (one && g != one)) return nullptr;
      one = g;
    }
    return one;
  };
  // A melody (no two notes overlap) of notes in no group is read as one chord, as it always was.
  bool melody = chords.size() > 1;
  for (const std::vector<doc::NoteKey>& ch : chords) melody = melody && ch.size() == 1 && groupOfChord(ch) == nullptr;
  if (melody) {
    chords.assign(1, {});
    for (const doc::Note* n : notes) chords[0].push_back(doc::keyOf(*n));
  }
  // Every chord is (in) a group in JI: Back to Tuning, for all of them; the groups keep their reading and warps (M1).
  std::vector<doc::JiGroupId> back;
  bool allInJi = true;
  for (const std::vector<doc::NoteKey>& ch : chords) {
    const doc::JiGroup* g = groupOfChord(ch);
    if (!g || !g->tuned) {
      allInJi = false;
      break;
    }
    if (std::find(back.begin(), back.end(), g->id) == back.end()) back.push_back(g->id);
  }
  const std::vector<uint32_t> keep = sel_;
  if (allInJi && !back.empty()) {
    const GroupInfo* info = groupInfo(back.front());
    const std::string was = back.size() == 1 && info ? info->proportionText : std::to_string(back.size()) + " chords";
    if (!apply(doc::ops::setJiGroupsTuned(*ctx.doc(), ref_, back, false), "Back to Tuning")) return false;
    select(keep);
    ctx.setStatus("Back to " + tuningLabel() + ": the notes play as written; " + was + " is kept with its warps (Ctrl+Shift+J: JI again)");
    return true;
  }
  const bool one = chords.size() == 1;
  const doc::JiGroup* oneGroup = one ? groupOfChord(chords[0]) : nullptr;
  if (one && keys.size() < 2 && !(oneGroup && !oneGroup->tuned)) {
    ctx.setStatus("Tune to JI: select two or more notes (one note: right-click it > Is Harmonic\xE2\x80\xA6)");
    return false;
  }
  if (!one || oneGroup) {
    // Several chords, or a group Back to Tuning: one step for all (each chord its best reading; a group its own).
    const doc::ops::EditResult e = doc::ops::tuneChordsToJi(*ctx.doc(), ctx.store().ids(), ref_, chords);
    if (!e) {
      ctx.setStatus("Tune to JI: " + e.error().message);
      return false;
    }
    if (e->empty()) {
      ctx.setStatus("Tune to JI: the chords already are in JI");
      return false;
    }
    if (!apply(e, "Tune to JI")) return false;
    select(keep);
    setShowHarmonics(true);
    std::string list;
    size_t n = 0;
    for (const doc::JiGroupId id : e->jiGroups)
      if (const GroupInfo* info = groupInfo(id); info && list.find(info->proportionText) == std::string::npos) {
        list += (n++ ? ", " : "") + info->proportionText;
        if (n == 4) break;
      }
    ctx.setStatus(one ? "Tuned to " + list + " again (its reading and warps) \xC2\xB7 Ctrl+Shift+J: back"
                      : "Tuned " + std::to_string(chords.size()) + " chords to JI: " + list + " \xC2\xB7 Ctrl+Shift+J: back");
    return true;
  }
  const doc::ops::EditResult e = doc::ops::tuneToJi(*ctx.doc(), ctx.store().ids(), ref_, keys);
  if (!e) {
    ctx.setStatus("Tune to JI: " + e.error().message);
    return false;
  }
  const doc::JiGroupId id = e->jiGroups.empty() ? doc::JiGroupId{} : e->jiGroups.front();
  if (e->empty()) {
    ctx.setStatus("Tune to JI: the notes already are this proportion");
    return false;
  }
  if (!apply(e, "Tune to JI")) return false;
  select(keep);
  setShowHarmonics(true);
  const GroupInfo* info = groupInfo(id);
  const size_t readings = info ? groupCandidates(id).size() : 0;
  ctx.setStatus("Tuned to " + (info ? info->proportionText : std::string("JI")) + " (just throughout). Drag the frame's bottom-left corner inward to start in " +
                tuningLabel() + " and warp into JI \xC2\xB7 Ctrl+Shift+J: back" +
                (readings > 1 ? " \xC2\xB7 double-click the header: " + std::to_string(readings - 1) + " other reading" + (readings > 2 ? "s" : "") : std::string()));
  if (chooser && readings > 1) (void)openJiChooser(id);
  return true;
}

std::vector<doc::ops::JiCandidate> RollState::groupCandidates(doc::JiGroupId id) const {
  const GroupInfo* g = groupInfo(id);
  if (!g) return {};
  std::vector<doc::NoteKey> keys;
  keys.reserve(g->notes.size());
  for (const doc::Note* n : g->notes) keys.push_back(doc::keyOf(*n));
  Result<std::vector<doc::ops::JiCandidate>> c = doc::ops::tuneToJiCandidates(*ctx.doc(), ref_, keys);
  return c ? std::move(*c) : std::vector<doc::ops::JiCandidate>{};
}

bool RollState::retuneGroup(doc::JiGroupId id, const std::vector<int64_t>& terms) {
  const GroupInfo* g = groupInfo(id);
  if (!g) return false;
  std::vector<doc::NoteKey> keys;
  for (const doc::Note* n : g->notes) keys.push_back(doc::keyOf(*n));
  doc::ops::TuneToJiOptions o;
  o.proportion = terms;
  const std::vector<uint32_t> keep = sel_;
  const doc::ops::EditResult e = doc::ops::tuneToJi(*ctx.doc(), ctx.store().ids(), ref_, keys, o);
  if (!e) {
    ctx.setStatus("Tune to JI: " + e.error().message);
    return false;
  }
  if (e->empty()) return false;  // the reading it already has
  if (!apply(e, "Tune to JI")) return false;
  if (keep.empty()) selectGroup(id);
  else select(keep);
  ctx.setStatus("Tuned to " + proportionString(terms));
  return true;
}

ui::Menu* RollState::openJiChooser(doc::JiGroupId id) {
  const GroupInfo* g = groupInfo(id);
  if (!g || !noteArea) return nullptr;
  ui::OverlayLayer* layer = ui::OverlayLayer::of(*noteArea);
  if (!layer) return nullptr;
  const std::vector<doc::ops::JiCandidate> cands = groupCandidates(id);
  if (cands.empty()) {
    ctx.setStatus("This group has no other reading: its notes were placed as exact ratios");
    return nullptr;
  }
  // The group's proportion in lowest terms (what the candidates are listed in).
  std::vector<int64_t> now = g->proportion;
  int64_t d = 0;
  for (const int64_t t : now) d = std::gcd(d, t);
  if (d > 1)
    for (int64_t& t : now) t /= d;
  std::vector<ui::MenuItem> items;
  std::shared_ptr<RollState*> alive = self_;
  int32_t current = -1;
  for (const doc::ops::JiCandidate& c : cands) {
    char err[32];
    std::snprintf(err, sizeof err, "moves %.0f \xC2\xA2", c.error);  // L4: the notes' total move, labelled
    ui::MenuItem m = ui::MenuItem::item(proportionString(c.terms, 16), [alive, id, terms = c.terms] {
      if (*alive) (void)(*alive)->retuneGroup(id, terms);
    }, err);
    m.checked = c.terms == now;
    if (m.checked) current = static_cast<int32_t>(items.size());
    items.push_back(std::move(m));
  }
  const GroupFrame f = groupFrame(*g);
  ui::OverlayOptions o;
  o.anchor = intersect(IRect{static_cast<int32_t>(f.header.x0), static_cast<int32_t>(f.header.y0), std::max(1, static_cast<int32_t>(f.header.x1 - f.header.x0)),
                             std::max(1, static_cast<int32_t>(f.header.y1 - f.header.y0))},
                       area());
  if (o.anchor.empty()) o.anchor = {area().x + 8, area().y + 8, 1, 1};
  o.placement = ui::Placement::Below;
  auto menu = std::make_unique<JiChooser>(std::move(items), layer->uniqueName("ji-chooser"), alive);
  ui::Menu& opened = static_cast<ui::Menu&>(layer->open(std::move(menu), std::move(o)));
  opened.setInfo("Readings of this chord", "The chord as written can be heard as several just proportions. Each line is one: the "
                                           "proportion, and how many cents the notes move in total. Click one, or use Up / Down "
                                           "and Enter; Esc keeps the ticked one.");
  opened.setHighlighted(current >= 0 ? current : 0);
  return &opened;
}

bool RollState::walkActiveGroup(int32_t steps) {
  const doc::JiGroup* g = activeJiGroup();
  if (!clip_ || !g) {
    ctx.setStatus("Walk: select a note of a JI group first");
    return false;
  }
  const doc::JiGroupId id = g->id;
  const std::vector<uint32_t> keep = sel_;
  if (!apply(doc::ops::walkJiGroup(*ctx.doc(), ref_, id, steps), "Walk")) return false;
  select(keep);
  if (const GroupInfo* info = groupInfo(id))
    ctx.setStatus(std::string(steps > 0 ? "Walked up to " : "Walked down to ") + info->proportionText + " (the frozen note stays where it is)");
  return true;
}

// --- warps ------------------------------------------------------------------------------------------------------

void RollState::setWarpPreview(doc::JiGroupId group, std::vector<doc::JiWarp> warps) {
  if (!clip_ || !snap_) return;
  if (warpPreview_.valid() && warpPreview_.group == group && warpPreview_.warps == warps) return;
  doc::JiGroupList list(doc::jiGroupsOf(*clip_).begin(), doc::jiGroupsOf(*clip_).end());
  bool found = false;
  for (doc::JiGroup& g : list)
    if (g.id == group) {
      g.warps = warps;
      found = true;
    }
  if (!found) return;
  doc::NoteClip c = doc::withNotes(*clip_, clip_->notes);
  c.groups = std::make_shared<const doc::JiGroupList>(std::move(list));
  warpPreview_.group = group;
  warpPreview_.warps = std::move(warps);
  warpPreview_.clip = std::make_shared<const doc::NoteClip>(std::move(c));
  warpPreview_.pitches = {};
  if (const std::optional<doc::TrackPath> path = doc::findTrack(*snap_, ref_.track))
    warpPreview_.pitches = doc::resolveClipPitches(*snap_, *path, *warpPreview_.clip);
  warpPreview_.index = doc::NotePitchIndex(warpPreview_.pitches);
  touchPreview();
  markNotesDirty();
}

bool RollState::applyWarps(doc::JiGroupId group, std::vector<doc::JiWarp> warps, std::string_view what) {
  const std::vector<uint32_t> keep = sel_;
  const doc::ops::EditResult e = doc::ops::setJiWarps(*ctx.doc(), ref_, group, std::move(warps));
  if (!e) {
    ctx.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (!apply(e, what)) return false;
  select(keep);
  return true;
}

void RollState::selectWarp(WarpSel w) {
  if (w.group == warpSel_.group && w.index == warpSel_.index) return;
  warpSel_ = w;
  if (noteArea) noteArea->markDirty();
}

bool RollState::removeWarp(doc::JiGroupId id, int32_t index) {
  const GroupInfo* g = groupInfo(id);
  if (!g || index < 0 || static_cast<size_t>(index) >= g->group->warps.size()) return false;
  std::vector<doc::JiWarp> warps = g->group->warps;
  warps.erase(warps.begin() + index);
  // What followed keeps its direction: the neighbour that would now repeat one goes with it.
  if (static_cast<size_t>(index) < warps.size() && index > 0 && warps[static_cast<size_t>(index)].toJi == warps[static_cast<size_t>(index) - 1].toJi)
    warps.erase(warps.begin() + index);
  selectWarp({});
  return applyWarps(id, std::move(warps), "Remove Warp");
}

bool RollState::removeSelectedWarp() {
  if (!warpSel_.valid()) return false;
  const WarpSel w = warpSel_;
  return removeWarp(w.group, w.index);
}

bool RollState::setWarpShape(doc::JiGroupId id, int32_t index, doc::GlideShape::Kind kind) {
  const GroupInfo* g = groupInfo(id);
  if (!g || index < 0 || static_cast<size_t>(index) >= g->group->warps.size()) return false;
  std::vector<doc::JiWarp> warps = g->group->warps;
  warps[static_cast<size_t>(index)].shape.kind = kind;
  return applyWarps(id, std::move(warps), "Warp Shape");
}

bool RollState::insertWarpAt(doc::JiGroupId id, Ticks contentTick) {
  const GroupInfo* g = groupInfo(id);
  if (!g) return false;
  if (!g->tuned) {
    ctx.setStatus("Warp: this group has no tempered pitches to go back to (its notes were placed as exact ratios)");
    return false;
  }
  if (!g->inJi) {
    ctx.setStatus("Warp: the group is back in the tuning; Ctrl+Shift+J tunes it again (its warps come back with it)");
    return false;
  }
  // Group ticks count from the anchor's start (M7).
  const Ticks t = std::clamp<Ticks>(contentTick - g->origin, g->start - g->origin, std::max<Ticks>(g->end - g->origin, g->start - g->origin));
  std::vector<doc::JiWarp> warps = g->group->warps;
  const double state = doc::jiWarpWritten(warps, static_cast<double>(t));
  if (state != 0.0 && state != 1.0) {
    ctx.setStatus("Warp: the group is gliding here; drag its marker instead");
    return false;
  }
  size_t at = 0;
  while (at < warps.size() && warps[at].at <= t) ++at;
  if (at > 0 && warps[at - 1].at >= t) {
    ctx.setStatus("Warp: there is a marker here already");
    return false;
  }
  doc::JiWarp w;
  w.at = t;
  w.toJi = state == 1.0;
  // The next transition moves here (it has this direction); else a new one.
  if (at < warps.size()) warps.erase(warps.begin() + static_cast<std::ptrdiff_t>(at));
  const Ticks room = (at < warps.size() ? warps[at].at : std::max<Ticks>(g->end - g->origin, t)) - t;
  Ticks glide = kPpq / 2;
  if (const Grid gr = grid(); gr.enabled()) {
    const TickRange cell = gr.cellAt(contentTick);
    glide = std::max<Ticks>(1, cell.end - cell.start);
  }
  w.glide = std::clamp<Ticks>(glide, 0, std::max<Ticks>(room, 0));
  warps.insert(warps.begin() + static_cast<std::ptrdiff_t>(at), w);
  if (!applyWarps(id, std::move(warps), w.toJi ? "Warp into JI" : "Back to Tuning")) return false;
  selectWarp({id, static_cast<int32_t>(at)});
  ctx.setStatus(w.toJi ? "From here the chord glides into " + g->proportionText + ": drag the marker to move it, its round grip for the glide's length"
                       : "From here the chord glides back to " + tuningLabel() + ": drag the marker to move it, its round grip for the glide's length");
  return true;
}

void RollState::beginWarpDrag(const GroupHit& h, ui::Vec2 p) {
  drag = {};
  const GroupInfo* g = groupInfo(h.group);
  if (!g) return;
  DragState& d = drag;
  d.press = d.last = p;
  d.group = h.group;
  d.groupStart = g->origin;
  d.groupFirst = g->start;
  d.groupEnd = g->end;
  d.warps = g->group->warps;
  d.warpIndex = h.warp;
  switch (h.part) {
    case GroupPart::LeftGrip:
    case GroupPart::RightGrip:
      d.kind = DragKind::WarpNew;
      d.fromLeft = h.part == GroupPart::LeftGrip;
      d.warpIndex = -1;
      break;
    case GroupPart::Marker: d.kind = DragKind::WarpMove; break;
    case GroupPart::GlideGrip: d.kind = DragKind::WarpGlide; break;
    default: drag = {}; return;
  }
  if (d.kind != DragKind::WarpNew && (h.warp < 0 || static_cast<size_t>(h.warp) >= d.warps.size())) drag = {};
}

void RollState::warpDragTo(ui::Vec2 p, ui::Mods mods) {
  DragState& d = drag;
  const GroupInfo* g = groupInfo(d.group);
  if (!g) return;
  const std::vector<doc::JiWarp>& base = g->group->warps;
  // Group ticks from the anchor's start (M7): the group's notes span [first, span].
  const Ticks first = std::min<Ticks>(d.groupFirst - d.groupStart, 0);
  const Ticks span = std::max<Ticks>(d.groupEnd - d.groupStart, first + 1);
  const Grid gr = grid();
  auto snapped = [&](float x) {
    Ticks t = static_cast<Ticks>(std::llround(tickAtX(x)));
    if (gr.snapping() && !(mods & mod::kShift)) t = gr.snap(t);
    return t - d.groupStart;
  };
  Ticks cell = kPpq / 2;
  if (gr.enabled()) {
    const TickRange c = gr.cellAt(std::max<Ticks>(static_cast<Ticks>(std::llround(tickAtX(p.x))), 0));
    cell = std::max<Ticks>(1, c.end - c.start);
  }
  std::vector<doc::JiWarp> warps = base;
  int32_t index = d.warpIndex;
  if (d.kind == DragKind::WarpNew) {
    doc::JiWarp w;
    if (d.fromLeft) {
      // A new first warp: what follows keeps its states, so it leads INTO the state the group started in.
      const Ticks hi = base.empty() ? span : base.front().at - 1;
      if (hi < first) return;
      w.at = std::clamp<Ticks>(snapped(p.x), first, hi);
      w.toJi = base.empty() || !base.front().toJi;
      w.glide = std::clamp<Ticks>(cell, 0, (base.empty() ? span : base.front().at) - w.at);
      warps.insert(warps.begin(), w);
      index = 0;
    } else {
      // A new last warp: it leaves the state the group ended in.
      const Ticks lo = base.empty() ? first : base.back().at + std::max<Ticks>(base.back().glide, 1);
      if (lo > span) return;
      w.at = std::clamp<Ticks>(snapped(p.x), lo, span);
      w.toJi = !base.empty() && !base.back().toJi;
      w.glide = std::clamp<Ticks>(cell, 0, span - w.at);
      warps.push_back(w);
      index = static_cast<int32_t>(warps.size()) - 1;
    }
  } else if (index >= 0 && static_cast<size_t>(index) < warps.size()) {
    doc::JiWarp& w = warps[static_cast<size_t>(index)];
    const Ticks dt = snapped(p.x) - snapped(d.press.x);
    const bool last = static_cast<size_t>(index) + 1 == warps.size();
    const Ticks next = last ? std::max(span, w.at + w.glide) : warps[static_cast<size_t>(index) + 1].at;
    if (d.kind == DragKind::WarpMove) {
      const Ticks lo = index > 0 ? warps[static_cast<size_t>(index) - 1].at + std::max<Ticks>(warps[static_cast<size_t>(index) - 1].glide, 1) : first;
      const Ticks hi = last ? std::max<Ticks>(span, lo) : next - std::max<Ticks>(w.glide, 1);
      w.at = std::clamp<Ticks>(w.at + dt, lo, std::max(lo, hi));
    } else {
      w.glide = std::clamp<Ticks>(w.glide + dt, 0, std::max<Ticks>(next - w.at, 0));
    }
  } else {
    return;
  }
  d.warps = warps;
  d.warpIndex = index;
  const doc::JiWarp& w = d.warps[static_cast<size_t>(index)];
  ctx.setStatus(std::string(w.toJi ? "Warp into " + g->proportionText : "Warp back to " + tuningLabel()) + " " + beats(w.at) + " beats into the chord \xC2\xB7 glide " +
                beats(w.glide) + " beats");
  setWarpPreview(d.group, std::move(warps));
}

bool RollState::commitWarpDrag(DragState& d) {
  warpPreview_ = {};
  touchPreview();
  if (!d.group.valid()) return false;
  const doc::JiGroupId id = d.group;
  const int32_t index = d.warpIndex;
  const bool fresh = d.kind == DragKind::WarpNew;
  if (!applyWarps(id, std::move(d.warps), fresh ? "Add Warp" : d.kind == DragKind::WarpMove ? "Move Warp" : "Warp Glide")) {
    markNotesDirty();
    return false;
  }
  selectWarp({id, index});
  if (fresh)
    ctx.setStatus("Warp added: drag the marker to move it, the round grip for the glide's length; right-click it for its shape, Delete removes it");
  return true;
}

// --- proportions, ratios ------------------------------------------------------------------------------------------

void RollState::armProportionTool(bool on) {
  if (on == proportionArmed_) return;
  proportionArmed_ = on;
  if (on) ctx.setStatus("Proportion: drag a box where the chord goes (its bottom row is the lowest note), then type it, such as 8:9:10:12:15. Esc cancels");
  if (noteArea) noteArea->markDirty();
}

bool RollState::insertProportion(Ticks start, Ticks length, int64_t bottomDegree, const std::vector<int64_t>& terms, std::string* error) {
  auto fail = [&](std::string m) {
    ctx.setStatus("Proportion: " + m);
    if (error) *error = std::move(m);
    return false;
  };
  if (!clip_) return fail("no clip");
  const doc::ops::EditResult e =
      doc::ops::proportionNotes(*ctx.doc(), ctx.store().ids(), ref_, std::max<Ticks>(start, 0), std::max<Ticks>(length, 1), degreePitch(bottomDegree), terms);
  if (!e) return fail(e.error().message);
  if (!apply(e, "Add Proportion")) return fail("the document refused the notes");
  setShowHarmonics(true);
  ctx.setStatus(proportionString(terms) + ": " + std::to_string(terms.size()) + " notes in one group; the lowest is frozen on its row, the others are exact ratios to it");
  return true;
}

ui::TextEdit* RollState::openProportionEntry(Ticks start, Ticks length, int64_t bottomDegree, ui::IRect anchor) {
  return openJiField("proportion-entry", "Proportion, lowest note first:",
                     "Type a chord as whole numbers, such as 4:5:6:7 or 8:9:10:12:15, and press Enter: one note per number over "
                     "the box, the lowest on the box's bottom row, the others exact ratios to it. Esc closes.",
                     "", anchor, [start, length, bottomDegree](RollState& s, const std::string& text, std::string& why) {
                       const Result<std::vector<int64_t>> terms = RollState::parseProportion(text);
                       if (!terms) {
                         why = terms.error().message;
                         return false;
                       }
                       return s.insertProportion(start, length, bottomDegree, *terms, &why);
                     });
}

bool RollState::addNoteAtRatio(doc::NoteId note, std::string_view text, std::string* error) {
  auto fail = [&](std::string m) {
    ctx.setStatus("Add Note at Ratio: " + m);
    if (error) *error = std::move(m);
    return false;
  };
  const doc::Note* n = clip_ ? findNote(note) : nullptr;
  if (!n) return fail("no such note");
  const doc::JiGroup* g = doc::jiGroupOfNote(*clip_, *n);
  if (!g) return fail("this note is in no JI group: right-click it > Is Harmonic\xE2\x80\xA6 first");
  const std::string_view t = trimmed(text);
  const size_t sep = t.find_first_of("/:");
  auto whole = [](std::string_view part, int64_t& out) {
    part = trimmed(part);
    const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), out);
    return !part.empty() && ec == std::errc{} && end == part.data() + part.size() && out >= 1;
  };
  doc::ops::GroupNotePitch pitch;
  std::string said;
  if (sep == std::string_view::npos) {
    int64_t k = 0;
    if (!whole(t, k) || k > doc::kMaxFundamentalDenominator) return fail("type a ratio such as 3/2 or 7:4, or a harmonic number such as 5");
    pitch.harmonic = k;
    said = "harmonic " + std::to_string(k);
  } else {
    int64_t num = 0, den = 0;
    if (!whole(t.substr(0, sep), num) || !whole(t.substr(sep + 1), den)) return fail("type a ratio such as 3/2 or 7:4, or a harmonic number such as 5");
    const std::optional<tuning::Rational> q = tuning::Rational::tryMake(num, den);
    // The ratio is to THIS note: a member's own ratio to the anchor comes first.
    const std::optional<tuning::Rational> toAnchor =
        !q ? std::nullopt : n->id == g->anchor ? q : tuning::mul(n->pitch.ratio(), *q);
    if (!toAnchor || !toAnchor->isPositive()) return fail("that ratio is out of range");
    pitch.ratio = *toAnchor;
    said = std::to_string(q->num()) + "/" + std::to_string(q->den());
  }
  const doc::JiGroupId id = g->id;
  const doc::ops::EditResult e = doc::ops::addGroupNote(*ctx.doc(), ctx.store().ids(), ref_, id, pitch, n->start, n->length, n->velocity);
  if (!e) return fail(e.error().message);
  if (!apply(e, "Add Note at Ratio")) return fail("the document refused the note");
  if (const GroupInfo* info = groupInfo(id)) ctx.setStatus("Added " + said + ": the group now reads " + info->proportionText);
  return true;
}

ui::TextEdit* RollState::openRatioEntry(doc::NoteId note) {
  const doc::Note* n = findNote(note);
  if (!n) return nullptr;
  const ui::IRect anchor = onPath(*n, n->pitch) ? pathBox(*n, n->start, n->length, n->pitch) : screenRectOf(n->start, n->length, n->pitch);
  return openJiField("ratio-entry", "Add a note at this ratio to the note:",
                     "Type a ratio to this note, such as 3/2 or 7:4 (below it: 2/3), or a harmonic number of its group, such "
                     "as 5, and press Enter. The new note starts and ends with this one. When it falls between the harmonics the "
                     "group is renumbered: 3/2 above harmonic 5 reads 10 and 15. Esc closes.",
                     "", anchor, [note](RollState& s, const std::string& text, std::string& why) { return s.addNoteAtRatio(note, text, &why); });
}

std::optional<int32_t> RollState::harmonicDragSteps(const doc::Note& pressed, float y) const {
  if (!clip_ || !clip_->groups) return std::nullopt;
  const doc::NoteId anchorId = doc::noteAnchorOf(pressed.pitch);
  if (!anchorId.valid()) return std::nullopt;
  if (selected(anchorId)) return std::nullopt;  // T7.16a (H2): the whole chord moves, by rows
  if (const doc::JiGroup* g = doc::findJiGroupByAnchor(*clip_, anchorId); g && !g->tuned) return std::nullopt;  // tempered
  const doc::JiGroup* g = doc::findJiGroupByAnchor(*clip_, anchorId);
  const GroupInfo* info = g ? groupInfo(g->id) : nullptr;
  const std::optional<uint64_t> own = harmonicOf(pressed);
  if (!info || !own || *own == 0) return std::nullopt;
  const std::optional<double> anchor = startCents(*info->anchor);
  if (!anchor) return std::nullopt;
  // (Where the anchor is at the member's start: the guides it is drawn against follow a moving anchor.)
  const double fundamental = *anchor + harmonicsShift(Harmonics{info->anchor->id}, static_cast<double>(pressed.start)) -
                             1200.0 * std::log2(static_cast<double>(info->anchorNumber));
  const double k = std::max(1.0, std::round(std::exp2((centsAtScreenY(y) - fundamental) / 1200.0)));
  const double steps = std::clamp(k - static_cast<double>(*own), -1.0e6, 1.0e6);
  return static_cast<int32_t>(steps);
}

// --- paint ------------------------------------------------------------------------------------------------------------

// Under the notes: every group in view as a frame with its header.
void paintGroupFrames(PaintContext& ctx, RollState& s, const IRect& vis) {
  if (!ctx.res.theme || !s.hasClip() || !s.clip()->groups) return;
  const Theme& th = *ctx.res.theme;
  const float sc = ctx.scale.factor;
  const float px = std::max(1.0f, std::round(sc));
  const float vx0 = static_cast<float>(vis.x), vx1 = static_cast<float>(vis.right());
  const float vy0 = static_cast<float>(vis.y), vy1 = static_cast<float>(vis.bottom());
  const doc::JiGroup* active = s.activeJiGroup();
  const ThemeFonts* fonts = ctx.res.uiFonts;
  for (const GroupInfo& g : s.groupInfos()) {
    const GroupFrame f = s.groupFrame(g);
    const Rect& b = f.box;
    if (b.x1 <= b.x0 || b.x1 < vx0 - 1.0f || b.x0 > vx1 + 1.0f || b.y1 < vy0 - 20.0f * sc || f.header.y0 > vy1) continue;
    const bool isActive = active && active->id == g.group->id;
    ctx.dl.addRectFilled({std::max(b.x0, vx0), std::max(b.y0, vy0), std::min(b.x1, vx1), std::min(b.y1, vy1)}, th.roll.jiFill);
    const Rgba border = isActive ? th.roll.jiSelected : th.roll.jiBorder;
    const float dash = std::round(5.0f * sc), gap = std::round(3.0f * sc);
    dashedH(ctx.dl, b.x0, b.x1, b.y0, vx0, vx1, border, dash, gap, px);
    dashedH(ctx.dl, b.x0, b.x1, b.y1 - px, vx0, vx1, border, dash, gap, px);
    dashedV(ctx.dl, b.x0, b.y0, b.y1, vy0, vy1, border, dash, gap, px);
    dashedV(ctx.dl, b.x1 - px, b.y0, b.y1, vy0, vy1, border, dash, gap, px);
    if (fonts && f.header.y1 > vy0) {
      const IRect box{static_cast<int32_t>(f.header.x0), static_cast<int32_t>(f.header.y0), static_cast<int32_t>(std::max(b.x1 - b.x0, f.header.x1 - f.header.x0)) + 400,
                      static_cast<int32_t>(f.header.y1 - f.header.y0)};
      const bool preview = s.drag.group == g.group->id && s.hasCurvePreview();
      const std::string header = preview ? headerOf(g.proportionText, s.tuningLabel(), s.shownWarps(g)) : g.header;
      paint::text(ctx, fonts->small, box, header, isActive ? th.roll.jiSelected : th.roll.jiText, TextAlign::Left, 0, 0);
    }
  }
}

// Over the notes: warp markers with their grips and labels, the corner grips, the proportion tool's box.
void paintGroupOverlays(PaintContext& ctx, RollState& s, const IRect& vis) {
  if (!ctx.res.theme || !s.hasClip()) return;
  const Theme& th = *ctx.res.theme;
  const float sc = ctx.scale.factor;
  const float px = std::max(1.0f, std::round(sc));
  const DragState& d = s.drag;
  if (d.kind == DragKind::ProportionBox && d.moved) {
    const Rect band = d.rubber.toRect();
    ctx.dl.addRectFilled(band, th.roll.jiFill);
    ctx.dl.addRectStroke(band, th.roll.jiSelected, px);
  }
  if (!s.clip()->groups) return;
  const float vx0 = static_cast<float>(vis.x), vx1 = static_cast<float>(vis.right());
  const float vy0 = static_cast<float>(vis.y), vy1 = static_cast<float>(vis.bottom());
  const float half = 0.5f * RollState::kWarpGrip * sc;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  const std::string tuning = s.tuningLabel();
  const RollState::WarpSel& sel = s.selectedWarp();
  for (const GroupInfo& g : s.groupInfos()) {
    // (Back to Tuning: the warps are kept but do not apply, so they are not drawn; the header names the state.)
    const std::span<const doc::JiWarp> warps = g.inJi ? s.shownWarps(g) : std::span<const doc::JiWarp>();
    const bool grips = s.gripsShown(g);
    if (warps.empty() && !grips) continue;
    const GroupFrame f = s.groupFrame(g);
    const Rect& b = f.box;
    if (b.x1 <= b.x0 || b.x1 < vx0 - 2.0f * half || b.x0 > vx1 + 2.0f * half || b.y0 > vy1 || b.y1 + 18.0f * sc < vy0) continue;
    const int32_t labelH = static_cast<int32_t>(std::round(14.0f * sc));
    const int32_t labelY = static_cast<int32_t>(b.y1 + half);
    auto label = [&](float x, float xEnd, bool written) {
      const int32_t w = static_cast<int32_t>(xEnd - x) - static_cast<int32_t>(4.0f * sc);
      if (!fonts || w < static_cast<int32_t>(18.0f * sc)) return;
      paint::text(ctx, fonts->small, {static_cast<int32_t>(x) + static_cast<int32_t>(3.0f * sc), labelY, w, labelH}, written ? std::string_view(tuning) : std::string_view(g.proportionText),
                  th.roll.jiText, TextAlign::Left, 0, 0);
    };
    if (!warps.empty()) {
      // The state before the first marker, then after each.
      bool written = warps.front().toJi;
      label(b.x0, s.warpX(g, warps.front()), written);
      for (size_t i = 0; i < warps.size(); ++i) {
        const doc::JiWarp& w = warps[i];
        const float x = s.warpX(g, w);
        const float xe = std::round(s.xOfTick(static_cast<double>(g.origin + w.at + w.glide)));
        const float gx = s.warpGlideX(g, w);
        const bool selected = (sel.group == g.group->id && sel.index == static_cast<int32_t>(i)) || (d.group == g.group->id && d.warpIndex == static_cast<int32_t>(i));
        const Rgba ink = selected ? th.roll.jiSelected : th.roll.jiLine;
        // The dotted line through the frame where the glide starts, a fainter one where it ends.
        dashedV(ctx.dl, x, b.y0, b.y1, vy0, vy1, ink, 2.0f * px, 2.0f * px, px);
        if (xe > x + 2.0f) dashedV(ctx.dl, xe, b.y0, b.y1, vy0, vy1, th.roll.jiStop, px, 3.0f * px, px);
        // The glide along the bottom border, its round grip, the marker's diamond.
        ctx.dl.addRectFilled({x, b.y1 - px, std::max(gx, xe), b.y1 + px}, ink);
        ctx.dl.addCircleFilled({gx, b.y1}, 0.42f * RollState::kWarpGrip * sc, ink);
        ctx.dl.addCircleFilled({gx, b.y1}, 0.42f * RollState::kWarpGrip * sc - 1.5f * px, th.roll.rowNatural);
        const Vec2 diamond[4] = {{x - half, b.y1}, {x, b.y1 - half}, {x + half, b.y1}, {x, b.y1 + half}};
        ctx.dl.addConvexPolyFilled(diamond, ink);
        written = !w.toJi;
        label(std::max(gx + half, xe), i + 1 < warps.size() ? s.warpX(g, warps[i + 1]) : std::max(b.x1, gx + 200.0f * sc), written);
      }
    }
    if (grips) {
      // Corner grips: small tabs pointing inward (drag one in to add a warp).
      const bool hotL = d.kind == DragKind::WarpNew && d.group == g.group->id && d.fromLeft;
      const bool hotR = d.kind == DragKind::WarpNew && d.group == g.group->id && !d.fromLeft;
      const float lx = b.x0, rx = b.x1;
      if (warps.empty() || s.warpX(g, warps.front()) > lx + 2.0f * half)
        ctx.dl.addTriangleFilled({lx, b.y1 - half}, {lx + 2.0f * half, b.y1}, {lx, b.y1 + half}, hotL ? th.roll.jiSelected : th.roll.jiLine);
      if (warps.empty() || s.warpGlideX(g, warps.back()) < rx - 2.0f * half)
        ctx.dl.addTriangleFilled({rx, b.y1 - half}, {rx, b.y1 + half}, {rx - 2.0f * half, b.y1}, hotR ? th.roll.jiSelected : th.roll.jiLine);
    }
  }
}

}  // namespace pq::views::roll
