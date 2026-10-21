// The T0.8 acceptance scene: Inter at the theme sizes (12 / 13 / 14 px, SemiBold titles, a 24 px tabular
// readout), shaping (kerning, contextual alternates, symbols), light-vs-full hinting side by side, text in
// signal colours, dark-on-light, and clipping. Laid out in fixed logical pixels from the top-left (a smaller
// target simply clips). HiDPI (T0.7-fix3): positions are round(logical × scale) device px, fonts are the roles
// resolved at the scale (round(size × scale) px, hinted), measured advances are device px, and the header
// divider stays a 1 device-px hairline.

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pq/ui/text.h"
#include "scenes_internal.h"

namespace pq::ui::detail {

void textScene(DrawList& dl, const SceneTextures& tex, double time) {
  using namespace signal_theme;
  const UiScale& s = tex.scale;
  // Logical px → snapped device px.
  auto P = [&](float v) { return s.pxf(v); };
  auto R = [&](float x0, float y0, float x1, float y1) { return s.rect(x0, y0, x1, y1); };
  const float W = static_cast<float>(dl.targetWidth()), H = static_cast<float>(dl.targetHeight());
  dl.addRectFilled({0, 0, W, H}, kBackground);
  dl.addRectFilled({0, 0, W, P(30)}, kHeader);
  dl.addRectFilled({0, P(30), W, P(30) + 1}, withAlpha(kAccent, 90));
  if (!tex.text) return;
  TextSystem& ts = *tex.text;
  const SceneFonts& f = tex.fonts;
  const Rgba text = rgba(226, 230, 240);     // PQ Meter settings text
  const Rgba textDim = rgba(134, 140, 158);  // PQ Meter settings dim text

  // Header: title left, provenance right-aligned (measure → right edge).
  ts.draw(dl, f.titleSemi, {P(14), P(20)}, "pqdaw · text", text);
  {
    constexpr const char* kInfo = "Inter 4.1 · FreeType 2.14.3 (hinted) · HarfBuzz 14.5.1";
    const int32_t w = ts.measure(f.section, kInfo);
    ts.draw(dl, f.section, {W - P(14) - static_cast<float>(w), P(20)}, kInfo, kHeaderText);
  }

  // Size ladder: label column + sample.
  struct Row {
    FontId font;
    const char* label;
  };
  const Row rows[] = {
      {f.section, "12 px"},     {f.body, "13 px"},          {f.title, "14 px"},
      {f.bodySemi, "13 semi"},  {f.titleSemi, "14 semi"},
  };
  constexpr const char* kSample = "The quick brown fox jumps over the lazy dog — 0123456789 (ABC) {xyz} @#%&";
  float y = 58;  // logical
  for (const Row& r : rows) {
    ts.draw(dl, f.section, {P(14), P(y)}, r.label, textDim);
    ts.draw(dl, r.font, {P(84), P(y)}, kSample, text);
    y += 22;
  }

  // Meter readout: SemiBold 24 px with tabular figures, so the columns stay put as values change. The second
  // value animates in the interactive window (frame 0 is the golden).
  y += 18;
  dl.addRectFilled(R(14, y - 26, 386, y + 10), kPanel);
  char buf[64];
  const float t = static_cast<float>(time);
  std::snprintf(buf, sizeof buf, "%+.1f dB", -12.5 + 6.0 * std::sin(t));
  ts.draw(dl, f.meter, {P(24), P(y)}, buf, kAccent);
  std::snprintf(buf, sizeof buf, "%.2f Hz", 440.0 + 3.0 * std::sin(t * 0.7));
  ts.draw(dl, f.meter, {P(178), P(y)}, buf, kWaveform);
  dl.addRectFilled(R(402, y - 26, 786, y + 10), kPanel);
  ts.draw(dl, f.meter, {P(412), P(y)}, "−∞  1111  8888", kViolet);
  y += 36;

  // Shaping: kerning pairs, Inter's contextual alternates (-> becomes an arrow, case-sensitive forms),
  // tuning text and symbols. Light hinting vs the font's own hints, same string, same size.
  ts.draw(dl, f.section, {P(14), P(y)}, "kerning", textDim);
  ts.draw(dl, f.body, {P(84), P(y)}, "AV Ta To Wa Yo LT P. F, “Quote”  ->  =>  (A)  [H]  1/3  x×y", text);
  y += 22;
  ts.draw(dl, f.section, {P(14), P(y)}, "tuning", textDim);
  ts.draw(dl, f.body, {P(84), P(y)},
          "C4 +13.7¢ · 7/4 · 11/8 · 31-EDO · 5L 2s · √2 ≈ 1.414 · ±1 dB · → ↑ ↓",
          text);
  y += 22;
  ts.draw(dl, f.section, {P(14), P(y)}, "full", textDim);
  ts.draw(dl, f.body, {P(84), P(y)}, "Hinting: Operator  Auto Filter  Saturator  EQ Eight  Wavetable  Glue", text);
  y += 20;
  ts.draw(dl, f.section, {P(14), P(y)}, "light", textDim);
  ts.draw(dl, f.bodyLight, {P(84), P(y)}, "Hinting: Operator  Auto Filter  Saturator  EQ Eight  Wavetable  Glue", text);
  y += 26;

  // Signal-display colours on the panel, and the grid-label token (white at alpha 70). x is device px here:
  // it advances by measured (device) widths plus scaled gaps.
  dl.addRectFilled(R(14, y - 15, 786, y + 33), kPanel);
  float x = P(24);
  const char* bandNames[6] = {"Low", "Low-mid", "Mid", "High-mid", "Presence", "Air"};
  for (int i = 0; i < 6; ++i) x += static_cast<float>(ts.draw(dl, f.bodySemi, {x, P(y)}, bandNames[i], kBands[i])) + P(18);
  ts.draw(dl, f.section, {x + P(8), P(y)}, "-24 dB   -12 dB   0 dB", kGridLabel);
  ts.draw(dl, f.section, {P(24), P(y + 22)}, "100 Hz    1 kHz    10 kHz    grid label token (white α 70)", kGridLabel);
  y += 50;

  // Dark on light, and translucent text over a gradient.
  dl.addRectFilled(R(14, y - 15, 398, y + 33), rgba(206, 208, 214));
  ts.draw(dl, f.bodySemi, {P(24), P(y)}, "Dark on light: Arrangement View", rgba(24, 24, 28));
  ts.draw(dl, f.body, {P(24), P(y + 22)}, "Clip 1 — 4 bars — 120.00 BPM — 4/4", rgba(60, 62, 70));
  dl.addRectFilledMultiColor(R(402, y - 15, 786, y + 33), kAccent, kViolet, kViolet, kAccent);
  ts.draw(dl, f.titleSemi, {P(412), P(y)}, "Text over a gradient", rgba(10, 10, 14));
  ts.draw(dl, f.body, {P(412), P(y + 22)}, "Half-transparent white over the same ramp", rgba(255, 255, 255, 128));
  y += 50;

  // Clipping: the run is cut by the scissor at the box edge (labels in narrow device panels).
  const Rect box = R(14, y - 14, 214, y + 8);
  dl.addRectFilled(box, kHeader);
  dl.pushClip(box);
  ts.draw(dl, f.body, {P(20), P(y)}, "A long device name that does not fit its box", text);
  dl.popClip();
  ts.draw(dl, f.section, {P(226), P(y)}, "← clipped to its 200 px box", textDim);
}

}  // namespace pq::ui::detail
