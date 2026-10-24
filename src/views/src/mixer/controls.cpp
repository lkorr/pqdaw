// The mixer's Live-styled controls (T3.10): activator / solo / arm / Pre-Post toggles, number boxes with value bars,
// the output chooser and the tuning chip; plus the value mappings (volume taper, pan text).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "../common/paint.h"
#include "internal.h"

namespace pq::views {

using namespace pq::ui;

// --- value mapping --------------------------------------------------------------------------------------------------

double volumeToFader(double db) noexcept {
  if (!(db > doc::kMinVolumeDb)) return 0.0;
  return std::clamp(std::pow(10.0, (std::min<double>(db, doc::kMaxVolumeDb) - doc::kMaxVolumeDb) / 40.0), 0.0, 1.0);
}

double faderToVolume(double pos) noexcept {
  if (!(pos > 0.0)) return doc::kMinVolumeDb;
  const double db = doc::kMaxVolumeDb + 40.0 * std::log10(std::min(pos, 1.0));
  return std::clamp(db, static_cast<double>(doc::kMinVolumeDb), static_cast<double>(doc::kMaxVolumeDb));
}

std::string formatVolume(double db) {
  if (!(db > doc::kMinVolumeDb)) return "-inf";
  char buf[32];
  const double r = std::round(db * 10.0) / 10.0;
  std::snprintf(buf, sizeof buf, "%.1f", r == 0.0 ? 0.0 : r);
  return buf;
}

std::string formatPan(double pan) {
  const long v = std::lround(pan * 50.0);
  if (v == 0) return "C";
  return std::to_string(v < 0 ? -v : v) + (v < 0 ? "L" : "R");
}

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool iequalsTail(std::string_view s, std::string_view tail) {
  if (s.size() < tail.size()) return false;
  for (size_t i = 0; i < tail.size(); ++i) {
    char c = s[s.size() - tail.size() + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != tail[i]) return false;
  }
  return true;
}

std::optional<double> number(std::string_view s) {
  s = trim(s);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  if (s.empty()) return std::nullopt;
  double v = 0.0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) return std::nullopt;
  return v;
}

}  // namespace

std::optional<double> parseVolume(std::string_view text) {
  std::string_view s = trim(text);
  if (iequalsTail(s, "db")) s = trim(s.substr(0, s.size() - 2));
  if (iequalsTail(s, "-inf") || iequalsTail(s, "inf")) return static_cast<double>(doc::kMinVolumeDb);
  const auto v = number(s);
  if (!v) return std::nullopt;
  return std::clamp(*v, static_cast<double>(doc::kMinVolumeDb), static_cast<double>(doc::kMaxVolumeDb));
}

std::optional<double> parsePan(std::string_view text) {
  std::string_view s = trim(text);
  if (s.size() == 1 && (s[0] == 'c' || s[0] == 'C')) return 0.0;
  double sign = 1.0;
  if (!s.empty() && (s.back() == 'l' || s.back() == 'L')) {
    sign = -1.0;
    s.remove_suffix(1);
  } else if (!s.empty() && (s.back() == 'r' || s.back() == 'R')) {
    s.remove_suffix(1);
  }
  const auto v = number(s);
  if (!v) return std::nullopt;
  return std::clamp(sign * *v / 50.0, -1.0, 1.0);
}

// --- FixedMeterSource -------------------------------------------------------------------------------------------------

void FixedMeterSource::set(doc::TrackId id, const MeterLevels& l) {
  const auto it = std::lower_bound(levels_.begin(), levels_.end(), id, [](const auto& e, doc::TrackId v) { return e.first < v; });
  if (it != levels_.end() && it->first == id) it->second = l;
  else levels_.insert(it, {id, l});
}

void FixedMeterSource::read(std::span<const doc::TrackId> ids, std::span<MeterLevels> out) noexcept {
  ++reads_;
  for (size_t i = 0; i < ids.size() && i < out.size(); ++i) {
    const auto it = std::lower_bound(levels_.begin(), levels_.end(), ids[i], [](const auto& e, doc::TrackId v) { return e.first < v; });
    out[i] = (it != levels_.end() && it->first == ids[i]) ? it->second : MeterLevels{};
  }
}

namespace mixer {

namespace {

Rgba dim(Rgba c, float t) noexcept { return lerp(c, rgba(0, 0, 0, alphaOf(c)), t); }

// The boxes' common frame: 1 device px border, flat fill.
void frame(DrawList& dl, const IRect& r, Rgba fill, Rgba border) {
  if (r.empty()) return;
  dl.addRectFilled(r.toRect(), border);
  const IRect in = paint::inset(r, 1);
  if (!in.empty()) dl.addRectFilled(in.toRect(), fill);
}

}  // namespace

// --- LiveToggle -------------------------------------------------------------------------------------------------------

LiveToggle::LiveToggle(std::string name, Kind kind) : Toggle(std::move(name), {}, ToggleStyle::Accent), kind_(kind) {
  setFocusable(false);  // Live's header buttons do not take the keyboard (`0`, Space keep working)
  switch (kind) {
    case Kind::Activator:
      setInfo("Track Activator", "Turns the track on or off (mute). With several tracks selected, all of them. Hotkey: 0.");
      break;
    case Kind::Solo:
      setInfo("Solo", "Solos this track and unsolos the others. Ctrl-click solos several tracks. A soloed group solos its tracks.");
      label_ = "S";
      break;
    case Kind::Arm:
      setInfo("Arm Recording", "Arms the track for recording (M5). Ctrl-click arms several tracks.");
      break;
    case Kind::Post:
      setInfo("Send Tap (Pre/Post)", "Whether the sends to this return tap the tracks before (Pre) or after (Post) their faders.");
      break;
  }
}

void LiveToggle::setLabel(std::string_view s) {
  if (label_ == s) return;
  label_ = std::string(s);
  markDirty();
}

bool LiveToggle::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down) mods_ = e.mods;
  return Toggle::onPointer(e);
}

void LiveToggle::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const auto& m = th->mixer;
  const IRect r = rect();
  const VisualState v = visualState();
  Rgba fill = m.buttonOff, ink = m.buttonText;
  if (on()) {
    switch (kind_) {
      case Kind::Activator: fill = m.activatorOn; break;
      case Kind::Solo: fill = m.soloOn; break;
      case Kind::Arm: fill = m.armOn; break;
      case Kind::Post: fill = m.postOn; break;
    }
    ink = m.activatorText;
  }
  if (v.pressed) fill = paint::pressOf(fill);
  else if (v.hover) fill = paint::hoverOf(fill);
  frame(ctx.dl, r, fill, m.boxBorder);
  if (kind_ == Kind::Arm) {
    const float rad = std::max(1.5f, std::min(ctx.scale.exact(3.0f), 0.28f * static_cast<float>(std::min(r.w, r.h))));
    ctx.dl.addCircleFilled({static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)},
                           rad, on() ? m.activatorText : dim(m.buttonText, 0.35f));
    return;
  }
  const std::string_view text = kind_ == Kind::Post ? std::string_view(on() ? "Post" : "Pre") : std::string_view(label_);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, kind_ == Kind::Solo ? f->bodyStrong : f->numeric, r, text, ink, TextAlign::Center, 0, 0);
}

// --- LiveBox ------------------------------------------------------------------------------------------------------------

LiveBox::LiveBox(std::string name, Kind kind, std::string letter)
    : DragBox(std::move(name), ValueSpec{}, TextAlign::Center), kind_(kind), letter_(std::move(letter)) {
  ValueSpec s;
  switch (kind) {
    case Kind::Volume:
    case Kind::Send:
      s.min = 0.0;
      s.max = 1.0;
      s.def = kind == Kind::Volume ? volumeToFader(0.0) : 0.0;
      s.dragPixels = 160.0;
      s.keyStep = 0.01;
      formatter = [](double pos) { return formatVolume(faderToVolume(pos)); };
      parser = [](std::string_view t) -> std::optional<double> {
        const auto db = parseVolume(t);
        return db ? std::optional<double>(volumeToFader(*db)) : std::nullopt;
      };
      break;
    case Kind::Pan:
      s.min = -1.0;
      s.max = 1.0;
      s.def = 0.0;
      s.step = 1.0 / 50.0;  // Live's pan has 50 steps per side
      s.dragPixels = 120.0;
      formatter = [](double p) { return formatPan(p); };
      parser = [](std::string_view t) { return parsePan(t); };
      break;
  }
  setSpec(s);
  setValue(s.def);
  switch (kind) {
    case Kind::Volume: setInfo("Track Volume", "Drag up / down (Shift: fine), double-click to type a value, Delete for 0 dB."); break;
    case Kind::Pan: setInfo("Track Panning", "Drag up / down (Shift: fine), double-click to type (e.g. 25L), Delete for centre."); break;
    case Kind::Send: setInfo("Send", "How much of this track goes to the return track. Delete for -inf."); break;
  }
}

void LiveBox::paint(PaintContext& ctx) {
  if (editing()) {
    DragBox::paint(ctx);  // the type-in: caret and selection
    return;
  }
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const auto& m = th->mixer;
  const IRect r = rect();
  if (r.empty()) return;
  const VisualState v = visualState();
  frame(ctx.dl, r, v.hover || v.pressed ? paint::hoverOf(m.field) : m.field, v.focus ? th->field.borderFocus : m.boxBorder);
  const IRect in = paint::inset(r, 1);
  const double n = normalized();
  if (!in.empty()) {
    if (kind_ == Kind::Pan) {
      // A thin bar along the bottom edge from the centre to the value (T3.x-fix-look, M3 review L17: the full-height
      // fill and centre marker struck through the text, "C" and "12R" read as "Ȼ" / "1▌R"); the text stays clean.
      const int32_t mid = in.x + in.w / 2;
      const int32_t x = in.x + static_cast<int32_t>(std::lround(n * static_cast<double>(in.w)));
      const int32_t a = std::min(mid, x), b = std::max(mid, x);
      const int32_t bh = std::min(in.h, std::max(2, ctx.scale.px(2.0f)));
      const int32_t by = in.bottom() - bh;
      if (b > a) ctx.dl.addRectFilled(IRect{a, by, b - a, bh}.toRect(), m.panFill);  // centred: no bar ("C" says it)
    } else {
      const int32_t w = static_cast<int32_t>(std::lround(n * static_cast<double>(in.w)));
      if (w > 0) ctx.dl.addRectFilled(IRect{in.x, in.y, w, in.h}.toRect(), kind_ == Kind::Volume ? m.volumeFill : m.sendFill);
    }
  }
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!f) return;
  const Rgba ink = v.pressed ? th->accent.primary : m.fieldText;
  if (!letter_.empty()) {
    const int32_t lw = ctx.scale.px(11.0f);
    paint::text(ctx, f->numeric, {r.x, r.y, lw + ctx.scale.px(2.0f), r.h}, letter_, withAlpha(m.fieldText, 200), TextAlign::Center,
                ctx.scale.px(2.0f), 0);
    paint::text(ctx, f->numeric, {r.x + lw, r.y, r.w - lw, r.h}, valueText(), ink, TextAlign::Center, 0, ctx.scale.px(2.0f));
    return;
  }
  paint::text(ctx, f->numeric, r, valueText(), ink, TextAlign::Center, ctx.scale.px(2.0f), ctx.scale.px(2.0f));
}

// --- Chooser ----------------------------------------------------------------------------------------------------------

Chooser::Chooser(std::string name) : Control(std::move(name)) {
  setInfo("Audio To", "Where the track's output goes: the master, a group track, or nowhere (Sends Only).");
}

void Chooser::setText(std::string_view s) {
  if (text_ == s) return;
  text_ = std::string(s);
  markDirty();
}

bool Chooser::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Left) {
    if (onOpen) onOpen(*this);
    return true;
  }
  return e.type == PointerType::Up && e.button == PointerButton::Left;
}

void Chooser::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const auto& m = th->mixer;
  const IRect r = rect();
  if (r.empty()) return;
  frame(ctx.dl, r, hovered() ? paint::hoverOf(m.field) : m.field, m.boxBorder);
  // ▾ at the right.
  const float a = static_cast<float>(std::max(2, ctx.scale.px(3.0f)));
  const float cx = static_cast<float>(r.right() - ctx.scale.px(7.0f)), cy = static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h);
  ctx.dl.addTriangleFilled({cx - a, cy - a * 0.5f}, {cx + a, cy - a * 0.5f}, {cx, cy + a * 0.7f}, m.fieldText);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, f->small, r, text_, m.fieldText, TextAlign::Left, ctx.scale.px(4.0f), ctx.scale.px(13.0f));
}

// --- labels -----------------------------------------------------------------------------------------------------------

std::string tuningShortName(const doc::TuningDef& d) {
  if (const tuning::Edo* e = d.edo()) return std::to_string(e->n()) + "-EDO";
  if (const tuning::MosParams* m = d.mos()) return std::to_string(m->n * std::max(1, m->periodsPerEquave)) + "-MOS";
  return "Scale";
}

std::string outputLabel(const doc::Project& p, const doc::Track& t) {
  switch (t.mixer.output.kind) {
    case doc::OutputKind::Default: {
      const auto path = doc::findTrack(p, t.id);
      if (path && path->list == doc::TrackList::Main)
        if (const doc::Track* g = doc::parentOf(p, *path)) return g->name;
      return "Master";
    }
    case doc::OutputKind::Master: return "Master";
    case doc::OutputKind::Track: {
      const doc::Track* g = doc::findTrackPtr(p, t.mixer.output.track);
      return g ? g->name : "Master";
    }
    case doc::OutputKind::None: return "Sends Only";
  }
  return "Master";
}

}  // namespace mixer

}  // namespace pq::views
