#pragma once

// Theme tokens (T3.2, SPEC §11 / §15).
//
// Two palettes with separate jobs:
//   - pq::ui::Theme: the chrome of the whole DAW (panels, controls, text, menus, tracks, clips, grid). It is
//     *data*: assets/themes/ableton-dark.json is loaded at startup (loadTheme), every token is required (a
//     missing or unknown token fails loading with the token's name), and dev builds reload it when the file
//     changes (ThemeFileWatcher). The default file approximates Ableton Live 12's dark neutral theme by eye;
//     the values are our own (no Ableton assets).
//   - pq::ui::signal_theme: PQ Meter's palette, copied from PQ Meter's src/ui/theme.h and the level meter so
//     ported visualisers look identical. Only signal displays use it (spectra, scopes, meters, waveforms,
//     modulation rings, wires).
//
// Widgets reach the chrome theme through PaintContext::res.theme and its resolved font roles through
// res.uiFonts (pq/ui/widget.h). Reading a token is a plain member load: no lookup on the paint path.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/ui/color.h"
#include "pq/ui/text.h"
#include "pq/ui/ui_scale.h"

namespace pq::ui {

// --- chrome theme ---------------------------------------------------------------------------------------

enum class ThemeFace : uint8_t { Regular, SemiBold };

// A font role of the theme: face + logical px size (resolved at the UI scale like any FontRole).
struct ThemeFont {
  ThemeFace face = ThemeFace::Regular;
  uint32_t px = 13;
  bool tabular = false;  // 'tnum': equal-width digits (numeric readouts that change under a drag)
  bool operator==(const ThemeFont&) const = default;
};

// Number of clip colours: 14 columns × 5 rows, like Live's clip colour chooser.
inline constexpr uint32_t kClipPaletteColumns = 14;
inline constexpr uint32_t kClipPaletteRows = 5;
inline constexpr uint32_t kClipPaletteSize = kClipPaletteColumns * kClipPaletteRows;

struct Theme {
  std::string name;  // shown in preferences later; free text

  struct Surface {  // panel value steps (low-contrast separation, as in Live)
    Rgba background, panel, raised, sunken, divider, highlight;
    bool operator==(const Surface&) const = default;
  } surface{};
  struct Control {  // buttons, toggles, knobs, faders
    Rgba fill, fillHover, fillPressed, fillDisabled, border, track, value, valueDisabled, thumb, thumbHover, needle;
    bool operator==(const Control&) const = default;
  } control{};
  struct Field {  // text edits, number boxes, choosers
    Rgba fill, fillHover, border, borderFocus;
    bool operator==(const Field&) const = default;
  } field{};
  struct Text {
    Rgba primary, secondary, disabled, onAccent, selection, caret;
    bool operator==(const Text&) const = default;
  } text{};
  struct Accent {  // active states: activator (yellow), on/value (orange), solo (blue), arm (red), focus
    Rgba primary, activator, solo, arm, focus;
    bool operator==(const Accent&) const = default;
  } accent{};
  struct Selection {  // highlighted menu rows, selected list items
    Rgba fill, text;
    bool operator==(const Selection&) const = default;
  } selection{};
  struct Menu {
    Rgba fill, border, separator, shadow;
    bool operator==(const Menu&) const = default;
  } menu{};
  struct Tooltip {
    Rgba fill, border, text;
    bool operator==(const Tooltip&) const = default;
  } tooltip{};
  struct Track {  // track headers and arrangement lanes (T3.8, T3.10)
    Rgba header, headerSelected, lane, laneAlt, laneSelected;
    bool operator==(const Track&) const = default;
  } track{};
  struct Clip {  // clip bodies take their fill from clipPalette
    Rgba text, textLight, border, selectedBorder;  // text: dark, on light clip colours; textLight: on dark ones
    bool operator==(const Clip&) const = default;
  } clip{};
  struct Grid {  // arrangement / piano-roll grid lines (translucent: drawn over lanes of any colour)
    Rgba bar, beat, sub, shade;
    bool operator==(const Grid&) const = default;
  } grid{};
  struct Scrollbar {
    Rgba track, thumb, thumbHover;
    bool operator==(const Scrollbar&) const = default;
  } scrollbar{};
  struct Arrangement {  // arrangement view (T3.8), sampled from Live 12's arrangement (Q47)
    Rgba lane, laneShade, laneSelected, gridLine, separator, background, header, ruler,
        rulerText, loopFill, loopEdge, loopOffFill, loopOffEdge, playhead, startMarker, overviewFrame;
    bool operator==(const Arrangement&) const = default;
  } arrangement{};
  struct Roll {  // piano roll (T3.11): keys, row shading, equave markers, area outside the clip, off-grid notes,
                 // the virtual fundamental and its harmonic guide lines (SPEC §4.3)
    Rgba keyWhite, keyBlack, keyText, rowNatural, rowAccidental, rowScale, rowLine, equave, outside, offGrid, fundamental,
        harmonic;
    bool operator==(const Roll&) const = default;
  } roll{};
  struct MosChart {  // MOS wheel region chart (T3.13): surfaces from the Live 12 look, data colours from mos-explorer
    Rgba background, grid, axis, axisBorder, axisText, axisTick, axisSubTick, periodLine, edoLine, edoText, edoCents,
        edoHardness, marker, markerText, sizeText, bandText, bandSubText, bandTextDim, bandGrayed, bandActive;
    bool operator==(const MosChart&) const = default;
  } mosChart{};

  struct Fonts {  // SPEC §11: 12 / 13 / 14 px roles
    ThemeFont small, body, bodyStrong, title, numeric;
    bool operator==(const Fonts&) const = default;
  } fonts{};
  struct Spacing {  // logical px
    float xs, sm, md, lg;
    bool operator==(const Spacing&) const = default;
  } spacing{};
  struct Radius {  // logical px
    float control, panel, menu, clip;
    bool operator==(const Radius&) const = default;
  } radius{};
  struct Size {  // logical px
    float control, row, menuItem, scrollbar, knob, meter;
    bool operator==(const Size&) const = default;
  } size{};

  std::vector<Rgba> clipPalette;  // kClipPaletteSize colours, row-major (row 0 = the vivid row)

  bool operator==(const Theme&) const = default;
};

// Parses a theme file (JSON, UTF-8). Strict: every token listed by themeTokenNames() must be present exactly
// once, no unknown keys, colours "#RRGGBB" or "#RRGGBBAA". Errors name the token, e.g.
// `theme: missing token "colors.text.disabled"`.
Result<Theme> parseTheme(std::string_view json);
// Reads and parses a theme file. Errors start with the path.
Result<Theme> loadTheme(const std::string& path);
// Canonical text (the format of the committed file): parseTheme(serializeTheme(t)) == t.
std::string serializeTheme(const Theme& t);
// Every token's full name in file order ("colors.surface.background", ..., "fonts.body", "metrics.radius.menu",
// "clipPalette"). For tests and tooling.
std::vector<std::string_view> themeTokenNames();

// The default theme's path: assetsDir()/themes/ableton-dark.json.
std::string defaultThemePath();

// --- contrast (WCAG 2.x) --------------------------------------------------------------------------------
// fg composited over bg when fg is translucent (bg's own alpha is ignored).
Rgba compositeOver(Rgba fg, Rgba bg) noexcept;
double relativeLuminance(Rgba c) noexcept;
double contrastRatio(Rgba fg, Rgba bg) noexcept;
// The clip-name colour for a clip of colour `fill`: clip.text or clip.textLight, whichever contrasts more
// (Live does the same with its clip palette). Views cache it per clip colour; it is not for per-frame use.
Rgba clipTextFor(const Theme& t, Rgba fill) noexcept;

// --- font roles -----------------------------------------------------------------------------------------
// The theme's font roles resolved at a UI scale (integer-px hinted fonts, see FontRole).
struct ThemeFonts {
  FontId small = kNoFont;       // 12: control labels, secondary text
  FontId body = kNoFont;        // 13: menus, text fields, list rows
  FontId bodyStrong = kNoFont;  // 13 SemiBold: toggle letters, headers
  FontId title = kNoFont;       // 14 SemiBold: panel titles
  FontId numeric = kNoFont;     // 12 tabular: number boxes, readouts
};
// Creates (first time) or looks up the fonts. Fonts at a new device size cost a FreeType face + ASCII
// rasterisation (≈1–2 ms each): call on theme load or scale change, never from paint.
Result<ThemeFonts> resolveThemeFonts(TextSystem& text, FaceId regular, FaceId semiBold, const Theme& theme, UiScale scale);

// --- hot reload -----------------------------------------------------------------------------------------
// Dev-build theme hot reload: poll() compares the file's last-write time at most once per `interval` (a
// single metadata query, ≈10 µs; the frame loop wakes for it, nothing redraws unless the file changed), and
// re-parses the file when it changed. A file that fails to parse keeps the current theme and reports the
// error once per change.
class ThemeFileWatcher {
public:
  explicit ThemeFileWatcher(std::string path, double intervalSeconds = 0.5);
  enum class Poll : uint8_t { NotDue, Unchanged, Reloaded, Failed };
  // `now` in seconds (any monotonic clock). On Reloaded `out` holds the new theme; on Failed `error` says why.
  Poll poll(double now, Theme& out, std::string& error);
  double nextCheck() const noexcept { return next_; }
  const std::string& path() const noexcept { return path_; }

private:
  std::string path_;
  double interval_;
  double next_ = 0.0;
  int64_t stamp_ = 0;  // last-write time seen (filesystem clock ticks); 0 = not read yet
  bool primed_ = false;
};

// --- signal-display palette (PQ Meter) ------------------------------------------------------------------
namespace signal_theme {

inline constexpr Rgba kBackground = rgba(10, 10, 14);   // #0A0A0E
inline constexpr Rgba kPanel = rgba(16, 16, 22);        // #101016
inline constexpr Rgba kHeader = rgba(24, 24, 32);       // #181820
inline constexpr Rgba kHeaderText = rgba(140, 140, 160);
inline constexpr Rgba kGrid = rgba(255, 255, 255, 14);
inline constexpr Rgba kGridLabel = rgba(255, 255, 255, 70);
inline constexpr Rgba kAccent = rgba(120, 190, 255);    // #78BEFF
inline constexpr Rgba kViolet = rgba(172, 142, 255);    // #AC8EFF
inline constexpr Rgba kWaveform = rgba(120, 220, 160);
inline constexpr Rgba kWaveformR = rgba(255, 150, 120);

// Multiband band colours, low → high (PQ Meter Theme::bandColor, converted from float to 8-bit).
inline constexpr Rgba kBands[6] = {
    rgba(242, 92, 64), rgba(255, 201, 61), rgba(110, 217, 255),
    rgba(158, 242, 140), rgba(199, 153, 255), rgba(255, 140, 204),
};

// Level meter (PQ Meter src/modules/level_meter.cpp draw()): bar well, RMS body (kWaveform / kWaveformR at
// α200), zone tints above −18 dBFS (warn) and −6 dBFS (hot), the instantaneous-peak cap, the hold tick and
// the clip lamp.
inline constexpr Rgba kMeterWell = rgba(255, 255, 255, 10);
inline constexpr uint32_t kMeterBodyAlpha = 200;
inline constexpr Rgba kMeterWarn = rgba(226, 180, 88);
inline constexpr Rgba kMeterHot = rgba(230, 96, 80);
inline constexpr float kMeterWarnDb = -18.0f;
inline constexpr float kMeterHotDb = -6.0f;
inline constexpr Rgba kMeterPeakCap = rgba(235, 240, 250);
inline constexpr Rgba kMeterHold = rgba(255, 255, 255, 170);
inline constexpr Rgba kMeterLampOn = rgba(240, 70, 60);
inline constexpr Rgba kMeterLampOff = rgba(70, 70, 82, 160);
inline constexpr float kMeterFloorDb = -60.0f;

// PQ Meter's "Magma" colormap stops (t, r, g, b), used by the test pattern's textured ramp.
struct RampStop {
  float t, r, g, b;
};
inline constexpr RampStop kMagma[] = {
    {0.00f, 0.000f, 0.000f, 0.000f}, {0.15f, 0.090f, 0.040f, 0.220f}, {0.35f, 0.360f, 0.070f, 0.430f},
    {0.55f, 0.720f, 0.150f, 0.320f}, {0.75f, 0.960f, 0.470f, 0.130f}, {0.90f, 0.990f, 0.810f, 0.230f},
    {1.00f, 1.000f, 1.000f, 0.930f},
};

}  // namespace signal_theme

}  // namespace pq::ui
