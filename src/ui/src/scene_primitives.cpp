// The T0.7-fix2 acceptance scene: the widget-grade draw-list primitives as a sampler of Ableton-style chrome
// on dark grey. Crisp, pixel-snapped strokes (panel borders, separators, button outlines, fader ticks) next to
// antialiased curves (knob rings with a modulation arc, LEDs, pill toggles, disclosure/transport triangles),
// a bilinear 4-corner gradient XY pad, and a row of raw primitive checks (circle and ring sizes, sub-pixel
// rings, butt vs round caps, triangles at fractional positions, rect/rounded-rect stroke widths).
//
// Laid out in fixed device pixels from the top-left (like the text scene): crisp primitives are defined on the
// pixel grid, so scaling the layout would defeat what the scene shows. A smaller target simply clips.
//
// The colours below approximate Ableton Live's default dark theme. They are scene-local on purpose: the real
// chrome token set arrives with the widget toolkit (T3.x).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "pq/ui/text.h"
#include "scenes_internal.h"

namespace pq::ui::detail {

namespace {

constexpr Rgba kBg = hex(0x1E1E1E);          // window background behind panels
constexpr Rgba kHeaderBg = hex(0x171717);    // top strip
constexpr Rgba kPanel = hex(0x2C2C2C);       // device panel
constexpr Rgba kPanelTitle = hex(0x363636);  // device title bar
constexpr Rgba kBorder = hex(0x111111);      // panel borders, dark separators
constexpr Rgba kHighlight = hex(0x424242);   // light separators (bevel side)
constexpr Rgba kControl = hex(0x4D4D4D);     // knob tracks, off toggles, fader slots
constexpr Rgba kControlHi = hex(0x5E5E5E);   // knob caps, fader thumbs
constexpr Rgba kText = hex(0xC8C8C8);
constexpr Rgba kTextDim = hex(0x8C8C8C);
constexpr Rgba kAccent = hex(0xFFA82E);      // value arcs, on toggles (Ableton orange)
constexpr Rgba kMod = hex(0x4CB8E8);         // modulation ring (blue)
constexpr Rgba kLedOn = hex(0x7ADB4C);
constexpr Rgba kSolo = hex(0x5AA5E0);
constexpr Rgba kRecord = hex(0xE5534B);

constexpr float kPi = std::numbers::pi_v<float>;
// Knob travel: 7 o'clock to 5 o'clock, clockwise (screen angles: 0 = +x, increasing towards +y).
constexpr float kKnobStart = 0.75f * kPi;
constexpr float kKnobSweep = 1.5f * kPi;

struct Ctx {
  DrawList& dl;
  TextSystem* ts;
  const SceneFonts& f;
  void text(FontId font, Vec2 p, const char* s, Rgba c) const {
    if (ts) ts->draw(dl, font, p, s, c);
  }
  // Text centred horizontally on x.
  void textC(FontId font, float x, float y, const char* s, Rgba c) const {
    if (!ts) return;
    const int32_t w = ts->measure(font, s);
    ts->draw(dl, font, {std::round(x - 0.5f * static_cast<float>(w)), y}, s, c);
  }
};

// Device panel: fill, title bar, 1 px border inside the rect, title separator, disclosure arrow + title.
void devicePanel(const Ctx& c, Rect r, const char* title, bool expanded) {
  DrawList& dl = c.dl;
  dl.addRoundedRectFilled(r, 4, kPanel);
  dl.addRoundedRectFilled({r.x0, r.y0, r.x1, r.y0 + 22}, 4, kPanelTitle);
  dl.addRectFilled({r.x0, r.y0 + 12, r.x1, r.y0 + 22}, kPanelTitle);  // square off the title bar's bottom
  dl.addHLine(r.x0, r.x1, r.y0 + 22, kBorder);
  dl.addRoundedRectStroke(r, 4, kBorder);
  const float ax = r.x0 + 12, ay = r.y0 + 11;
  if (expanded)
    dl.addTriangleFilled({ax - 4, ay - 2.5f}, {ax + 4, ay - 2.5f}, {ax, ay + 2.5f}, kTextDim);
  else
    dl.addTriangleFilled({ax - 2.5f, ay - 4}, {ax + 2.5f, ay}, {ax - 2.5f, ay + 4}, kTextDim);
  c.text(c.f.bodySemi, {r.x0 + 24, r.y0 + 16}, title, kText);
}

// Ableton-style knob: track arc, value arc, a thinner modulation arc outside it, cap with a pointer.
void knob(const Ctx& c, Vec2 p, float value, float modDepth, const char* label, const char* valueText) {
  DrawList& dl = c.dl;
  constexpr float kR = 17.0f;
  const float a0 = kKnobStart, aVal = kKnobStart + kKnobSweep * value;
  dl.addArcStroke(p, kR, a0, a0 + kKnobSweep, kControl, 3.0f);
  dl.addArcStroke(p, kR, a0, aVal, kAccent, 3.0f);
  if (modDepth != 0.0f) {
    const float aMod = std::clamp(aVal + kKnobSweep * modDepth, a0, a0 + kKnobSweep);
    dl.addArcStroke(p, kR + 4.5f, aVal, aMod, kMod, 2.0f, LineCap::Round);
  }
  dl.addCircleFilled(p, 11.0f, kControlHi);
  dl.addCircleStroke(p, 11.0f, kBorder, 1.0f);
  // Pointer: a 2 px wide quad from near the centre to the cap's rim, as a convex polygon.
  const Vec2 d{std::cos(aVal), std::sin(aVal)}, n{-d.y, d.x};
  const Vec2 q0{p.x + d.x * 3.0f, p.y + d.y * 3.0f}, q1{p.x + d.x * 10.0f, p.y + d.y * 10.0f};
  const Vec2 quad[4] = {{q0.x + n.x, q0.y + n.y}, {q1.x + n.x, q1.y + n.y}, {q1.x - n.x, q1.y - n.y}, {q0.x - n.x, q0.y - n.y}};
  dl.addConvexPolyFilled(quad, kText);
  c.textC(c.f.section, p.x, p.y + 36, label, kTextDim);
  c.textC(c.f.section, p.x, p.y + 52, valueText, kText);
}

// Pill switch: off = grey with the dot left, on = accent with the dot right.
void pillToggle(DrawList& dl, Vec2 p, bool on) {
  const Rect r{p.x, p.y, p.x + 30, p.y + 14};
  dl.addRoundedRectFilled(r, 7, on ? kAccent : kControl);
  dl.addRoundedRectStroke(r, 7, kBorder);
  dl.addCircleFilled({on ? r.x1 - 7 : r.x0 + 7, p.y + 7}, 4.5f, on ? hex(0x1E1E1E) : kTextDim);
}

// Square text button (Ableton's track activator / solo / arm): crisp 1 px border, flat fill when on.
void squareButton(const Ctx& c, Rect r, const char* label, bool on, Rgba onColour) {
  DrawList& dl = c.dl;
  dl.addRectFilled(r, on ? onColour : kControl);
  dl.addRectStroke(r, kBorder);
  if (label == nullptr)
    dl.addCircleFilled({0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}, 4.0f, on ? hex(0x111111) : kText);
  else
    c.textC(c.f.bodySemi, 0.5f * (r.x0 + r.x1), r.y1 - 5, label, on ? hex(0x111111) : kText);
}

// Vertical fader: slot, dB ticks (crisp), level fill and a thumb.
void fader(const Ctx& c, float x, float y0, float y1, float value, const char* label) {
  DrawList& dl = c.dl;
  for (int i = 0; i <= 8; ++i) {
    const float y = std::round(y0 + (y1 - y0) * static_cast<float>(i) / 8.0f);
    dl.addHLine(x - 14, x - 8 + (i % 2 == 0 ? 2.0f : 0.0f), y, i % 2 == 0 ? kTextDim : kControl);
  }
  dl.addRectFilled({x - 2, y0, x + 2, y1}, kBorder);
  const float yv = std::round(y1 - (y1 - y0) * value);
  dl.addRectFilled({x - 1, yv, x + 1, y1}, kAccent);
  const Rect thumb{x - 9, yv - 5, x + 9, yv + 5};
  dl.addRoundedRectFilled(thumb, 2, kControlHi);
  dl.addRoundedRectStroke(thumb, 2, kBorder);
  dl.addHLine(thumb.x0 + 3, thumb.x1 - 3, yv, kText);
  c.textC(c.f.section, x, y1 + 18, label, kTextDim);
}

}  // namespace

void primitivesScene(DrawList& dl, const SceneTextures& tex, double time) {
  const float W = static_cast<float>(dl.targetWidth()), H = static_cast<float>(dl.targetHeight());
  const Ctx c{dl, tex.text.get(), tex.fonts};
  const float t = static_cast<float>(time);
  char buf[32];

  dl.addRectFilled({0, 0, W, H}, kBg);
  // Header strip: title, a 1 px dark separator plus a 1 px highlight below it (Ableton's bevel look).
  dl.addRectFilled({0, 0, W, 28}, kHeaderBg);
  dl.addHLine(0, W, 28, kBorder);
  dl.addHLine(0, W, 29, kHighlight);
  c.text(c.f.titleSemi, {14, 19}, "pqdaw · primitives", kText);
  // Transport in the header: play (triangle), stop (square), record (circle), loop (ring + arrow).
  {
    const float x = 640, y = 14;
    dl.addTriangleFilled({x - 5, y - 6}, {x + 6, y}, {x - 5, y + 6}, kLedOn);
    dl.addRectFilled({x + 18, y - 5, x + 28, y + 5}, kText);
    dl.addCircleFilled({x + 46, y}, 5.5f, kRecord);
    dl.addArcStroke({x + 70, y}, 5.5f, -0.35f * kPi, 1.3f * kPi, kTextDim, 1.5f);
    dl.addTriangleFilled({x + 72.5f, y - 9}, {x + 76.5f, y - 5.5f}, {x + 71.5f, y - 3}, kTextDim);
    dl.addVLine(x + 90, 6, 23, kHighlight);
    dl.addVLine(x - 18, 6, 23, kHighlight);
  }

  // --- device: four knobs with value + modulation arcs ---------------------------------------------------
  const Rect dev{12, 40, 412, 236};
  devicePanel(c, dev, "Auto Filter", true);
  struct Knob {
    const char* label;
    float value, mod;
    const char* fmt;
    float scale;
  };
  const Knob knobs[4] = {{"Freq", 0.62f + 0.2f * std::sin(t * 0.8f), 0.18f, "%.0f Hz", 2000.0f},
                         {"Res", 0.35f, -0.22f, "%.0f %%", 100.0f},
                         {"Drive", 0.0f, 0.0f, "%.1f dB", 24.0f},
                         {"Mix", 1.0f, -0.4f, "%.0f %%", 100.0f}};
  for (int i = 0; i < 4; ++i) {
    const float x = dev.x0 + 50 + 100 * static_cast<float>(i);
    std::snprintf(buf, sizeof buf, knobs[i].fmt, static_cast<double>(knobs[i].value * knobs[i].scale));
    knob(c, {x, 106}, knobs[i].value, knobs[i].mod, knobs[i].label, buf);
    if (i > 0) dl.addVLine(x - 50, dev.y0 + 34, dev.y0 + 134, kBorder);
  }
  // Footer row inside the device: separators at 1 and 2 px, pill toggles, LEDs.
  dl.addHLine(dev.x0 + 1, dev.x1 - 1, 188, kBorder);
  dl.addHLine(dev.x0 + 1, dev.x1 - 1, 189, kHighlight);
  for (int i = 0; i < 4; ++i) {
    const float x = dev.x0 + 16 + 98 * static_cast<float>(i);
    const bool on = (i % 2) == 0;
    pillToggle(dl, {x, 203}, on);
    dl.addCircleFilled({x + 48, 210}, 3.5f, on ? kLedOn : kControl);
    if (!on) dl.addCircleStroke({x + 48, 210}, 3.5f, kBorder, 1.0f);
    c.text(c.f.section, {x + 58, 214}, on ? "On" : "Off", on ? kText : kTextDim);
  }

  // --- mixer strip: square buttons, collapsed device header, faders ------------------------------------
  const Rect mix{424, 40, 788, 236};
  devicePanel(c, mix, "Mixer", true);
  const char* labels[4] = {"1", "S", nullptr, "M"};  // nullptr: the arm button, drawn as a dot
  const Rgba onCols[4] = {kAccent, kSolo, kRecord, kAccent};
  for (int i = 0; i < 4; ++i) {
    const float x = mix.x0 + 14 + 30 * static_cast<float>(i);
    squareButton(c, {x, 76, x + 24, 94}, labels[i], i != 3, onCols[i]);
  }
  // Collapsed chain entries: right-pointing disclosure arrows, crisp row separators.
  const char* chain[3] = {"EQ Eight", "Compressor", "Utility"};
  for (int i = 0; i < 3; ++i) {
    const float y = 108 + 26 * static_cast<float>(i);
    dl.addRectFilled({mix.x0 + 10, y, mix.x0 + 140, y + 22}, i == 1 ? kPanelTitle : kPanel);
    dl.addRectStroke({mix.x0 + 10, y, mix.x0 + 140, y + 22}, i == 1 ? kAccent : kBorder);
    const float ax = mix.x0 + 22, ay = y + 11;
    dl.addTriangleFilled({ax - 2.5f, ay - 4}, {ax + 2.5f, ay}, {ax - 2.5f, ay + 4}, kTextDim);
    c.text(c.f.body, {mix.x0 + 32, y + 15}, chain[i], kText);
  }
  dl.addVLine(mix.x0 + 152, mix.y0 + 23, mix.y1 - 1, kBorder);
  dl.addVLine(mix.x0 + 153, mix.y0 + 23, mix.y1 - 1, kHighlight);
  const float faderVals[4] = {0.75f, 0.5f + 0.25f * std::sin(t), 0.3f, 0.9f};
  const char* faderLabels[4] = {"A", "B", "C", "Main"};
  for (int i = 0; i < 4; ++i) fader(c, mix.x0 + 194 + 46 * static_cast<float>(i), 76, 196, faderVals[i], faderLabels[i]);

  // --- bottom-left: XY pad with a bilinear 4-corner gradient -------------------------------------------
  const Rect pad{12, 250, 252, 466};
  dl.addRectFilledMultiColor(pad, hex(0xE5534B), hex(0xFFA82E), hex(0x4CB8E8), hex(0x7A4CE0));
  dl.addRectStroke(pad, kBorder, 2.0f);
  const Vec2 h{pad.x0 + 150 + 30 * std::sin(t * 0.6f), pad.y0 + 80};
  dl.addHLine(pad.x0 + 2, pad.x1 - 2, h.y, rgba(255, 255, 255, 90));
  dl.addVLine(h.x, pad.y0 + 2, pad.y1 - 2, rgba(255, 255, 255, 90));
  dl.addCircleFilled(h, 7, rgba(255, 255, 255, 60));
  dl.addCircleStroke(h, 7, rgba(255, 255, 255), 1.5f);

  // --- bottom-right: raw primitive checks on a panel ---------------------------------------------------
  const Rect raw{264, 250, 788, 466};
  dl.addRectFilled(raw, kPanel);
  dl.addRectStroke(raw, kBorder);
  // Row A: discs r = 1.5…24 and rings of thickness 0.5…4 (sub-pixel rings keep coverage via alpha).
  const float discR[7] = {1.5f, 2.5f, 4, 6, 9, 14, 20};
  float x = raw.x0 + 14;
  for (float r : discR) {
    dl.addCircleFilled({x + r, 294}, r, kText);
    x += 2 * r + 10;
  }
  const float ringT[6] = {0.5f, 1, 1.5f, 2, 3, 5};
  x = raw.x0 + 172 + 120;
  for (float th : ringT) {
    dl.addCircleStroke({x, 294}, 15, kAccent, th);
    x += 40;
  }
  dl.addHLine(raw.x0 + 1, raw.x1 - 1, 324, kBorder);
  // Row B: 6 px arcs with butt vs round caps; a 270° ring sampler at fractional centres; thin arcs.
  x = raw.x0 + 40;
  for (int i = 0; i < 4; ++i) {
    const float a0 = -0.5f * kPi + 0.3f * static_cast<float>(i), a1 = a0 + kPi * (0.6f + 0.25f * static_cast<float>(i));
    dl.addArcStroke({x + 0.37f * static_cast<float>(i), 362.5f}, 20, a0, a1, kMod, 6, i % 2 ? LineCap::Round : LineCap::Butt);
    x += 64;
  }
  for (int i = 0; i < 4; ++i) {
    const float r = 6 + 5 * static_cast<float>(i);
    dl.addArcStroke({raw.x0 + 330 + 0.25f * static_cast<float>(i), 362}, r, kKnobStart, kKnobStart + kKnobSweep,
                    i % 2 ? kText : kTextDim, 1.0f + 0.5f * static_cast<float>(i));
  }
  for (int i = 0; i < 3; ++i) {
    const Vec2 p{raw.x0 + 400 + 44 * static_cast<float>(i), 362};
    dl.addArcStroke(p, 16, kKnobStart, kKnobStart + kKnobSweep, kControl, 4);
    dl.addArcStroke(p, 16, kKnobStart, kKnobStart + kKnobSweep * (0.3f + 0.3f * static_cast<float>(i)), kAccent, 4);
  }
  dl.addHLine(raw.x0 + 1, raw.x1 - 1, 398, kBorder);
  // Row C: triangles (four orientations, fractional positions), a hexagon, then crisp stroke widths and
  // rounded stroke radii, then 1 px separators at integer and half-pixel y (identical by the snapping rule).
  x = raw.x0 + 20;
  for (int i = 0; i < 4; ++i) {
    const float cx = x + 0.3f * static_cast<float>(i), cy = 432.5f;
    const Vec2 tri[4][3] = {{{cx - 6, cy - 7}, {cx + 7, cy}, {cx - 6, cy + 7}},
                            {{cx - 7, cy - 5}, {cx + 7, cy - 5}, {cx, cy + 7}},
                            {{cx + 6, cy - 7}, {cx - 7, cy}, {cx + 6, cy + 7}},
                            {{cx - 7, cy + 5}, {cx, cy - 7}, {cx + 7, cy + 5}}};
    dl.addTriangleFilled(tri[i][0], tri[i][1], tri[i][2], kText);
    x += 24;
  }
  {
    Vec2 hexagon[6];
    for (int i = 0; i < 6; ++i) {
      const float a = kPi / 3.0f * static_cast<float>(i) + kPi / 6.0f;
      hexagon[i] = {x + 12 + 11 * std::cos(a), 432.5f + 11 * std::sin(a)};
    }
    dl.addConvexPolyFilled(hexagon, kLedOn);
    x += 40;
  }
  for (int i = 0; i < 3; ++i) {
    const float th = static_cast<float>(i + 1);
    dl.addRectStroke({x, 414, x + 36, 452}, kText, th);
    x += 46;
  }
  const float radii[4] = {2, 4, 8, 19};
  for (int i = 0; i < 4; ++i) {
    dl.addRoundedRectFilled({x, 414, x + 38, 452}, radii[i], kControl);
    dl.addRoundedRectStroke({x, 414, x + 38, 452}, radii[i], i == 3 ? kAccent : kText, i == 2 ? 2.0f : 1.0f);
    x += 48;
  }
  for (int i = 0; i < 4; ++i) {
    const float y = 418.0f + 8.0f * static_cast<float>(i) + (i % 2 ? 0.5f : 0.0f);
    dl.addHLine(x, raw.x1 - 12, y, kText, i == 3 ? 2.0f : 1.0f);
  }
}

}  // namespace pq::ui::detail
