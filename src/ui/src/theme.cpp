// Theme file: strict JSON reader, canonical writer, contrast helpers, font-role resolution, hot reload.

#include "pq/ui/theme.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace pq::ui {

namespace {

using Json = nlohmann::json;

// --- token tables ---------------------------------------------------------------------------------------
// One row per token: its name inside its section and an accessor. The tables fix the file order.
struct ColorToken {
  std::string_view name;
  Rgba& (*get)(Theme&);
};
#define PQ_COLOR(section, field) \
  ColorToken { #section "." #field, [](Theme& t) -> Rgba& { return t.section.field; } }
const ColorToken kColors[] = {
    PQ_COLOR(surface, background), PQ_COLOR(surface, panel), PQ_COLOR(surface, raised), PQ_COLOR(surface, sunken),
    PQ_COLOR(surface, divider), PQ_COLOR(surface, highlight),
    PQ_COLOR(control, fill), PQ_COLOR(control, fillHover), PQ_COLOR(control, fillPressed), PQ_COLOR(control, fillDisabled),
    PQ_COLOR(control, border), PQ_COLOR(control, track), PQ_COLOR(control, value), PQ_COLOR(control, valueDisabled),
    PQ_COLOR(control, thumb), PQ_COLOR(control, thumbHover), PQ_COLOR(control, needle),
    PQ_COLOR(field, fill), PQ_COLOR(field, fillHover), PQ_COLOR(field, border), PQ_COLOR(field, borderFocus),
    PQ_COLOR(text, primary), PQ_COLOR(text, secondary), PQ_COLOR(text, disabled), PQ_COLOR(text, onAccent),
    PQ_COLOR(text, selection), PQ_COLOR(text, caret),
    PQ_COLOR(accent, primary), PQ_COLOR(accent, activator), PQ_COLOR(accent, solo), PQ_COLOR(accent, arm),
    PQ_COLOR(accent, focus),
    PQ_COLOR(selection, fill), PQ_COLOR(selection, text),
    PQ_COLOR(menu, fill), PQ_COLOR(menu, border), PQ_COLOR(menu, separator), PQ_COLOR(menu, shadow),
    PQ_COLOR(tooltip, fill), PQ_COLOR(tooltip, border), PQ_COLOR(tooltip, text),
    PQ_COLOR(track, header), PQ_COLOR(track, headerSelected), PQ_COLOR(track, lane), PQ_COLOR(track, laneAlt),
    PQ_COLOR(track, laneSelected),
    PQ_COLOR(clip, text), PQ_COLOR(clip, textLight), PQ_COLOR(clip, border), PQ_COLOR(clip, selectedBorder),
    PQ_COLOR(grid, bar), PQ_COLOR(grid, beat), PQ_COLOR(grid, sub), PQ_COLOR(grid, shade),
    PQ_COLOR(scrollbar, track), PQ_COLOR(scrollbar, thumb), PQ_COLOR(scrollbar, thumbHover),
    PQ_COLOR(arrangement, lane), PQ_COLOR(arrangement, laneShade), PQ_COLOR(arrangement, laneSelected), PQ_COLOR(arrangement, gridLine),
    PQ_COLOR(arrangement, separator), PQ_COLOR(arrangement, background), PQ_COLOR(arrangement, header), PQ_COLOR(arrangement, ruler),
    PQ_COLOR(arrangement, rulerText), PQ_COLOR(arrangement, loopFill), PQ_COLOR(arrangement, loopEdge), PQ_COLOR(arrangement, loopOffFill),
    PQ_COLOR(arrangement, loopOffEdge), PQ_COLOR(arrangement, playhead), PQ_COLOR(arrangement, startMarker), PQ_COLOR(arrangement, overviewFrame),
    PQ_COLOR(mosChart, background), PQ_COLOR(mosChart, grid), PQ_COLOR(mosChart, axis), PQ_COLOR(mosChart, axisBorder),
    PQ_COLOR(mosChart, axisText), PQ_COLOR(mosChart, axisTick), PQ_COLOR(mosChart, axisSubTick), PQ_COLOR(mosChart, periodLine),
    PQ_COLOR(mosChart, edoLine), PQ_COLOR(mosChart, edoText), PQ_COLOR(mosChart, edoCents), PQ_COLOR(mosChart, edoHardness),
    PQ_COLOR(mosChart, marker), PQ_COLOR(mosChart, markerText), PQ_COLOR(mosChart, sizeText), PQ_COLOR(mosChart, bandText),
    PQ_COLOR(mosChart, bandSubText), PQ_COLOR(mosChart, bandTextDim), PQ_COLOR(mosChart, bandGrayed), PQ_COLOR(mosChart, bandActive),
};
#undef PQ_COLOR

struct FontToken {
  std::string_view name;
  ThemeFont& (*get)(Theme&);
};
#define PQ_FONT(field) \
  FontToken { #field, [](Theme& t) -> ThemeFont& { return t.fonts.field; } }
const FontToken kFonts[] = {PQ_FONT(small), PQ_FONT(body), PQ_FONT(bodyStrong), PQ_FONT(title), PQ_FONT(numeric)};
#undef PQ_FONT

struct MetricToken {
  std::string_view name;
  float& (*get)(Theme&);
};
#define PQ_METRIC(section, field) \
  MetricToken { #section "." #field, [](Theme& t) -> float& { return t.section.field; } }
const MetricToken kMetrics[] = {
    PQ_METRIC(spacing, xs), PQ_METRIC(spacing, sm), PQ_METRIC(spacing, md), PQ_METRIC(spacing, lg),
    PQ_METRIC(radius, control), PQ_METRIC(radius, panel), PQ_METRIC(radius, menu), PQ_METRIC(radius, clip),
    PQ_METRIC(size, control), PQ_METRIC(size, row), PQ_METRIC(size, menuItem), PQ_METRIC(size, scrollbar),
    PQ_METRIC(size, knob), PQ_METRIC(size, meter),
};
#undef PQ_METRIC

constexpr std::string_view kSections[] = {"name", "colors", "fonts", "metrics", "clipPalette"};

Error themeError(std::string msg) { return Error{ErrorCode::Parse, "theme: " + std::move(msg), {}}; }

int hexDigit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseColor(std::string_view s, Rgba& out) noexcept {
  if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
  uint32_t v[4] = {0, 0, 0, 255};
  for (size_t i = 0; i * 2 + 1 < s.size(); ++i) {
    const int hi = hexDigit(s[1 + 2 * i]), lo = hexDigit(s[2 + 2 * i]);
    if (hi < 0 || lo < 0) return false;
    v[i] = static_cast<uint32_t>(hi * 16 + lo);
  }
  out = rgba(v[0], v[1], v[2], v[3]);
  return true;
}

std::string formatColor(Rgba c) {
  char buf[16];
  if (alphaOf(c) == 255)
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", redOf(c), greenOf(c), blueOf(c));
  else
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", redOf(c), greenOf(c), blueOf(c), alphaOf(c));
  return buf;
}

std::string formatNumber(float v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

// Checks that `obj` has exactly the keys `names` (in any order); names the first missing or unknown one.
template <class Table>
Status checkKeys(const Json& obj, const Table& table, std::string_view section) {
  if (!obj.is_object()) return themeError("\"" + std::string(section) + "\" must be an object");
  for (const auto& tok : table)
    if (!obj.contains(std::string(tok.name)))
      return themeError("missing token \"" + std::string(section) + "." + std::string(tok.name) + "\"");
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    const bool known = std::any_of(std::begin(table), std::end(table), [&](const auto& t) { return t.name == it.key(); });
    if (!known) return themeError("unknown token \"" + std::string(section) + "." + it.key() + "\"");
  }
  return Ok{};
}

}  // namespace

std::vector<std::string_view> themeTokenNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> n;
    n.emplace_back("name");
    for (const auto& c : kColors) n.push_back("colors." + std::string(c.name));
    for (const auto& f : kFonts) n.push_back("fonts." + std::string(f.name));
    for (const auto& m : kMetrics) n.push_back("metrics." + std::string(m.name));
    n.emplace_back("clipPalette");
    return n;
  }();
  return {names.begin(), names.end()};
}

Result<Theme> parseTheme(std::string_view text) {
  // Duplicate keys: nlohmann keeps the last one silently, so the parser callback tracks keys per object.
  std::vector<std::set<std::string>> keys;
  std::vector<std::string> path;
  std::string duplicate;
  auto cb = [&](int /*depth*/, Json::parse_event_t ev, Json& parsed) {
    switch (ev) {
      case Json::parse_event_t::object_start: keys.emplace_back(); break;
      case Json::parse_event_t::object_end:
        if (!keys.empty()) keys.pop_back();
        break;
      case Json::parse_event_t::key:
        if (!keys.empty() && !keys.back().insert(parsed.get<std::string>()).second && duplicate.empty())
          duplicate = parsed.get<std::string>();
        break;
      default: break;
    }
    return true;
  };
  Json j;
  try {
    j = Json::parse(text.begin(), text.end(), cb);
  } catch (const Json::exception& e) {  // parsing boundary: exceptions stop here
    return themeError(std::string("invalid JSON: ") + e.what());
  }
  if (!duplicate.empty()) return themeError("duplicate key \"" + duplicate + "\"");
  if (!j.is_object()) return themeError("the file must be a JSON object");
  for (const auto s : kSections)
    if (!j.contains(std::string(s))) return themeError("missing token \"" + std::string(s) + "\"");
  for (auto it = j.begin(); it != j.end(); ++it)
    if (std::find(std::begin(kSections), std::end(kSections), it.key()) == std::end(kSections))
      return themeError("unknown token \"" + it.key() + "\"");

  Theme t;
  if (!j["name"].is_string()) return themeError("\"name\" must be a string");
  t.name = j["name"].get<std::string>();

  const Json& colors = j["colors"];
  if (Status s = checkKeys(colors, kColors, "colors"); !s) return std::move(s).error();
  for (const auto& c : kColors) {
    const Json& v = colors[std::string(c.name)];
    if (!v.is_string() || !parseColor(v.get<std::string>(), c.get(t)))
      return themeError("token \"colors." + std::string(c.name) + "\" must be \"#RRGGBB\" or \"#RRGGBBAA\"");
  }

  const Json& fonts = j["fonts"];
  if (Status s = checkKeys(fonts, kFonts, "fonts"); !s) return std::move(s).error();
  for (const auto& f : kFonts) {
    const Json& v = fonts[std::string(f.name)];
    const std::string tok = "fonts." + std::string(f.name);
    if (!v.is_object() || v.size() != 3 || !v.contains("face") || !v.contains("px") || !v.contains("tabular"))
      return themeError("token \"" + tok + "\" must be {\"face\", \"px\", \"tabular\"}");
    ThemeFont& out = f.get(t);
    const Json& face = v["face"];
    if (face == "regular") out.face = ThemeFace::Regular;
    else if (face == "semibold") out.face = ThemeFace::SemiBold;
    else return themeError("token \"" + tok + ".face\" must be \"regular\" or \"semibold\"");
    if (!v["px"].is_number_integer() || v["px"].get<int64_t>() < 6 || v["px"].get<int64_t>() > 64)
      return themeError("token \"" + tok + ".px\" must be an integer in 6..64");
    out.px = static_cast<uint32_t>(v["px"].get<int64_t>());
    if (!v["tabular"].is_boolean()) return themeError("token \"" + tok + ".tabular\" must be true or false");
    out.tabular = v["tabular"].get<bool>();
  }

  const Json& metrics = j["metrics"];
  if (Status s = checkKeys(metrics, kMetrics, "metrics"); !s) return std::move(s).error();
  for (const auto& m : kMetrics) {
    const Json& v = metrics[std::string(m.name)];
    if (!v.is_number() || !(v.get<double>() >= 0.0) || v.get<double>() > 512.0)
      return themeError("token \"metrics." + std::string(m.name) + "\" must be a number in 0..512");
    m.get(t) = v.get<float>();
  }

  const Json& pal = j["clipPalette"];
  if (!pal.is_array() || pal.size() != kClipPaletteSize)
    return themeError("token \"clipPalette\" must be an array of " + std::to_string(kClipPaletteSize) + " colours");
  t.clipPalette.resize(kClipPaletteSize);
  for (uint32_t i = 0; i < kClipPaletteSize; ++i)
    if (!pal[i].is_string() || !parseColor(pal[i].get<std::string>(), t.clipPalette[i]))
      return themeError("token \"clipPalette[" + std::to_string(i) + "]\" must be \"#RRGGBB\" or \"#RRGGBBAA\"");
  return t;
}

Result<Theme> loadTheme(const std::string& path) {
  std::ifstream f(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!f) return Error{ErrorCode::Io, path + ": cannot open theme file", {}};
  std::ostringstream ss;
  ss << f.rdbuf();
  auto t = parseTheme(ss.str());
  if (!t) return Error{t.error().code, path + ": " + t.error().message, {}};
  return t;
}

std::string serializeTheme(const Theme& theme) {
  Theme& t = const_cast<Theme&>(theme);  // the accessors are shared with the reader; nothing is written
  std::string o;
  o += "{\n  \"name\": " + Json(t.name).dump() + ",\n  \"colors\": {\n";
  for (size_t i = 0; i < std::size(kColors); ++i)
    o += "    \"" + std::string(kColors[i].name) + "\": \"" + formatColor(kColors[i].get(t)) + "\"" +
         (i + 1 < std::size(kColors) ? ",\n" : "\n");
  o += "  },\n  \"fonts\": {\n";
  for (size_t i = 0; i < std::size(kFonts); ++i) {
    const ThemeFont& f = kFonts[i].get(t);
    o += "    \"" + std::string(kFonts[i].name) + "\": {\"face\": \"" +
         (f.face == ThemeFace::Regular ? "regular" : "semibold") + "\", \"px\": " + std::to_string(f.px) +
         ", \"tabular\": " + (f.tabular ? "true" : "false") + "}" + (i + 1 < std::size(kFonts) ? ",\n" : "\n");
  }
  o += "  },\n  \"metrics\": {\n";
  for (size_t i = 0; i < std::size(kMetrics); ++i)
    o += "    \"" + std::string(kMetrics[i].name) + "\": " + formatNumber(kMetrics[i].get(t)) +
         (i + 1 < std::size(kMetrics) ? ",\n" : "\n");
  o += "  },\n  \"clipPalette\": [\n";
  for (size_t i = 0; i < t.clipPalette.size(); ++i) {
    if (i % kClipPaletteColumns == 0) o += "    ";
    o += "\"" + formatColor(t.clipPalette[i]) + "\"";
    if (i + 1 < t.clipPalette.size()) o += (i % kClipPaletteColumns == kClipPaletteColumns - 1) ? ",\n" : ", ";
  }
  o += "\n  ]\n}\n";
  return o;
}

std::string defaultThemePath() { return assetsDir() + "/themes/ableton-dark.json"; }

// --- contrast -------------------------------------------------------------------------------------------

Rgba compositeOver(Rgba fg, Rgba bg) noexcept {
  const uint32_t a = alphaOf(fg);
  if (a == 255) return withAlpha(fg, 255);
  auto ch = [a](uint32_t f, uint32_t b) { return (f * a + b * (255 - a) + 127) / 255; };
  return rgba(ch(redOf(fg), redOf(bg)), ch(greenOf(fg), greenOf(bg)), ch(blueOf(fg), blueOf(bg)), 255);
}

double relativeLuminance(Rgba c) noexcept {
  auto lin = [](uint32_t v) {
    const double s = static_cast<double>(v) / 255.0;
    return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * lin(redOf(c)) + 0.7152 * lin(greenOf(c)) + 0.0722 * lin(blueOf(c));
}

double contrastRatio(Rgba fg, Rgba bg) noexcept {
  const double a = relativeLuminance(compositeOver(fg, bg)), b = relativeLuminance(withAlpha(bg, 255));
  return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

Rgba clipTextFor(const Theme& t, Rgba fill) noexcept {
  return contrastRatio(t.clip.text, fill) >= contrastRatio(t.clip.textLight, fill) ? t.clip.text : t.clip.textLight;
}

// --- fonts ----------------------------------------------------------------------------------------------

Result<ThemeFonts> resolveThemeFonts(TextSystem& text, FaceId regular, FaceId semiBold, const Theme& theme, UiScale scale) {
  struct Want {
    FontId ThemeFonts::*id;
    const ThemeFont* spec;
  };
  const Want wants[] = {{&ThemeFonts::small, &theme.fonts.small},
                        {&ThemeFonts::body, &theme.fonts.body},
                        {&ThemeFonts::bodyStrong, &theme.fonts.bodyStrong},
                        {&ThemeFonts::title, &theme.fonts.title},
                        {&ThemeFonts::numeric, &theme.fonts.numeric}};
  ThemeFonts out;
  for (const Want& w : wants) {
    FontRole role;
    role.face = w.spec->face == ThemeFace::Regular ? regular : semiBold;
    role.logicalPx = w.spec->px;
    role.options.tabularFigures = w.spec->tabular;
    auto f = text.font(role, scale);
    if (!f) return std::move(f).error();
    out.*w.id = *f;
  }
  return out;
}

// --- hot reload -----------------------------------------------------------------------------------------

namespace {
int64_t fileStamp(const std::string& path) noexcept {
  std::error_code ec;
  const auto t = std::filesystem::last_write_time(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
  return ec ? -1 : static_cast<int64_t>(t.time_since_epoch().count());
}
}  // namespace

ThemeFileWatcher::ThemeFileWatcher(std::string path, double intervalSeconds)
    : path_(std::move(path)), interval_(intervalSeconds), stamp_(fileStamp(path_)), primed_(true) {}

ThemeFileWatcher::Poll ThemeFileWatcher::poll(double now, Theme& out, std::string& error) {
  if (now < next_) return Poll::NotDue;
  next_ = now + interval_;
  const int64_t s = fileStamp(path_);
  if (primed_ && s == stamp_) return Poll::Unchanged;
  primed_ = true;
  stamp_ = s;
  if (s < 0) {
    error = path_ + ": cannot read theme file";
    return Poll::Failed;
  }
  auto t = loadTheme(path_);
  if (!t) {
    error = t.error().message;
    return Poll::Failed;
  }
  out = std::move(*t);
  return Poll::Reloaded;
}

}  // namespace pq::ui
