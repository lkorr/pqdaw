// The Oscilloscope face (TX.scope; SPEC §15 2026-11-01 "analysis modules"): the module IS its scope. PQ Meter's
// oscilloscope (pq meter/src/modules/oscilloscope.cpp) is the reference for what it does and how it looks; the
// analysis is pq/dsp/scope.h, the trace primitives pq/ui/lines.h (PQ Meter's scopeTrace / scopeEnvelope).
//
//   chips      top-left: the timebase (FIXED / PITCH / BEAT) and the channel (MID / L / R / SIDE / L·R lanes), click to
//              cycle (Shift: backwards); top-right: FREEZE (the picture holds, the signal still passes) and SET (the
//              settings sheet over the plot: every option below as a row — a click cycles a choice or toggles, a
//              vertical drag sets a number, Shift fine, a double-click resets it; RESET puts them all back)
//   trace      Catmull-Rom through the samples at two points per device px, then the screen-space spline, glow passes
//              under the core (scopeTrace); more than 4 samples per px: the min / max envelope (scopeEnvelope);
//              stereo lanes L (green) and R (salmon) in two half-height lanes; persistence: up to 24 earlier traces
//              fading; auto gain (0.72 of the visible peak, eased 0.08 per frame) or a fixed gain; log amplitude
//              (−60 … 0 dB, sign kept)
//   grid       the centre line, ± half scale, 10 time divisions (Beat: beat rules, bars brighter)
//   readouts   bottom-left: the pitch, cycles, span and gain (Fixed: ms/div; Beat: span, tempo, "stopped"), bottom-right
//              the note name with cents (C3 = 261.626 Hz naming; faded by the clarity; "note": the voice's own pitch)
//   zoom       Alt+wheel zooms the time axis about the pointer, a drag pans while zoomed, a double-click resets
//   measure    Ctrl+drag over the trace pins a span: Δt (ms, samples), the frequency of a cycle that long and its note,
//              peak and RMS (dBFS), crest factor, peak-to-peak, DC and zero crossings; its edges stay draggable; Esc or
//              a Ctrl+click clears it
//
// Display state is the instance's data (a record of numbers: pq/meta/oscilloscope.h), so it is saved and undoable and
// never recompiles; a read-only target keeps it for the face's life. Zoom, the measurement and the sheet are view state.
//
// The signal: the host binds the instance's analysis feed (Face::setAnalysis: a chain Oscilloscope device's tap); the
// face acquires its history (kWantScope: no spectrum is computed) while painted, reads the window its timebase needs at
// each poll (≤ 60 Hz while the feed moves) and lets go when its host stops painting it. World units 260 × 160.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "faces.h"
#include "option_sheet.h"
#include "pq/dsp/scope.h"
#include "pq/ui/lines.h"
#include "pq/views/devices/analysis.h"

namespace pq::views {

namespace {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;
using Tb = dsp::ScopeSettings::Timebase;

constexpr float kW = 260.0f, kH = 160.0f;
constexpr float kRadius = 10.0f;
constexpr Rect kPlot{8.0f, 8.0f, kW - 8.0f, kH - 8.0f};
constexpr float kChipH = 10.0f, kChipY = 12.0f;
constexpr Rect kTbChip{12.0f, kChipY, 44.0f, kChipY + kChipH};
constexpr Rect kChChip{47.0f, kChipY, 75.0f, kChipY + kChipH};
constexpr Rect kFreezeChip{kW - 74.0f, kChipY, kW - 41.0f, kChipY + kChipH};
constexpr Rect kSetChip{kW - 38.0f, kChipY, kW - 12.0f, kChipY + kChipH};
constexpr float kLabelPx = 7.0f;           // world units
constexpr float kLabelMinDevicePx = 6.0f;  // below this the text and chips are left out (the trace stays)
constexpr int kMaxTrails = 24;
constexpr uint32_t kTrailPoints = 256;
constexpr double kEnvelopeSpp = 4.0;       // more samples per px than this: the min / max envelope

enum Part : int32_t { kPlotPart = 0, kTbPart = 1, kChPart = 2, kFreezePart = 3, kSetPart = 4, kSheetPart = 5, kRowBase = 100 };

// PQ Meter's fixed signal colours (SPEC §11.1: its palette for signal displays).
constexpr Rgba kTraceA = ui::signal_theme::kWaveform, kTraceB = ui::signal_theme::kWaveformR;
constexpr Rgba kGrid = ui::signal_theme::kGrid, kGridLabel = ui::signal_theme::kGridLabel;
constexpr Rgba kAccent = ui::signal_theme::kAccent;
constexpr Rgba kFrozen = ui::rgba(255, 180, 90, 230);
constexpr Rgba kPad = ui::rgba(8, 8, 12, 200);
constexpr Rgba kReadoutText = ui::rgba(150, 162, 166);  // TMM.13-fix2: #96A2A6

bool inside(const Rect& r, Vec2 p) noexcept { return p.x >= r.x0 && p.x <= r.x1 && p.y >= r.y0 && p.y <= r.y1; }

// ---- options (the instance's data) -----------------------------------------------------------------------------------

enum class Ch : uint8_t { Mid, Left, Right, Side, Stereo };
constexpr const char* kTbNames[] = {"FIXED", "PITCH", "BEAT"};
constexpr const char* kChNames[] = {"MID", "L", "R", "SIDE", "L/R"};
constexpr double kBeatSpans[] = {1.0, 2.0, 4.0, 8.0, 16.0};
constexpr const char* kBeatSpanNames[] = {"1 beat", "2 beats", "1 bar", "2 bars", "4 bars"};

using sheet::Opt;
using sheet::kOnOff;
enum O : uint32_t {
  oTimebase, oLock, oWindow, oCycles, oSource, oMinHz, oMaxHz, oClarity, oTrigger, oLevel, oHyst, oSpan,
  oChannel, oAutoGain, oGain, oLog, oPersist, oLine, oGlow, oGrid, oReadout, oNote, oLanes, oCount
};
constexpr const char* kLockNames[] = {"Stable", "Tracking", "Constant"};
constexpr const char* kSourceNames[] = {"Note, else detect", "Detect"};
constexpr const char* kTrigNames[] = {"Off", "Rising", "Falling"};
constexpr const char* kTbLong[] = {"Fixed", "Pitch", "Beat"};
constexpr const char* kChLong[] = {"Mid", "Left", "Right", "Side", "L / R lanes"};
// PQ Meter's defaults (its kDefault* constants): a constant 52.2 ms span aligned by the pitch, 20 … 7770 Hz, clarity
// 0.80, a 1.4 px line, glow 0.35, auto gain.
const Opt kOpts[oCount] = {
    {"tb", "Timebase", 1, 0, 2, Opt::Choice, kTbLong},
    {"lock", "Pitch lock", 2, 0, 2, Opt::Choice, kLockNames},
    {"win", "Window", 52.2, 1, 2000, Opt::LogNumber, nullptr, "%.1f ms"},
    {"cyc", "Cycles", 1, 1, 16, Opt::Number, nullptr, "%.0f", 1.0},
    {"src", "Pitch from", 0, 0, 1, Opt::Choice, kSourceNames},
    {"lo", "Lowest", 20, 20, 500, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"hi", "Highest", 7770, 200, 8000, Opt::LogNumber, nullptr, "%.0f Hz"},
    {"clar", "Clarity", 0.8, 0.1, 0.95, Opt::Number, nullptr, "%.2f"},
    {"trig", "Trigger", 1, 0, 2, Opt::Choice, kTrigNames},
    {"lvl", "Level", 0, -1, 1, Opt::Number, nullptr, "%+.2f"},
    {"hys", "Hysteresis", 0.02, 0.001, 0.2, Opt::LogNumber, nullptr, "%.3f"},
    {"span", "Span", 2, 0, 4, Opt::Choice, kBeatSpanNames},
    {"ch", "Channel", 0, 0, 4, Opt::Choice, kChLong},
    {"agc", "Auto gain", 1, 0, 1, Opt::Toggle, kOnOff},
    {"gain", "Gain", 1, 0.1, 32, Opt::LogNumber, nullptr, "%.2fx"},
    {"log", "Log amplitude", 0, 0, 1, Opt::Toggle, kOnOff},
    {"pers", "Persistence", 0, 0, 1, Opt::Number, nullptr, "%.2f"},
    {"lw", "Line width", 1.4, 0.5, 4, Opt::Number, nullptr, "%.1f px"},
    {"glow", "Glow", 0.35, 0, 1, Opt::Number, nullptr, "%.2f"},
    {"grid", "Grid", 1, 0, 1, Opt::Toggle, kOnOff},
    {"read", "Scale readout", 1, 0, 1, Opt::Toggle, kOnOff},
    {"note", "Note readout", 1, 0, 1, Opt::Toggle, kOnOff},
    {"lanes", "Lane letters", 1, 0, 1, Opt::Toggle, kOnOff},
};

struct Options : sheet::Values {
  Options() : Values(kOpts) {}
  Tb timebase() const noexcept { return static_cast<Tb>(std::clamp(choice(oTimebase), 0, 2)); }
  Ch channel() const noexcept { return static_cast<Ch>(std::clamp(choice(oChannel), 0, 4)); }
  dsp::ScopeSettings settings() const noexcept {
    dsp::ScopeSettings s;
    s.timebase = timebase();
    s.lock = static_cast<dsp::ScopeSettings::Lock>(std::clamp(choice(oLock), 0, 2));
    s.source = static_cast<dsp::ScopeSettings::PitchSource>(std::clamp(choice(oSource), 0, 1));
    s.windowMs = static_cast<float>((*this)[oWindow]);
    s.cycles = choice(oCycles);
    s.minHz = static_cast<float>((*this)[oMinHz]);
    s.maxHz = static_cast<float>((*this)[oMaxHz]);
    s.clarity = static_cast<float>((*this)[oClarity]);
    s.trigger = static_cast<dsp::ScopeSettings::Trigger>(std::clamp(choice(oTrigger), 0, 2));
    s.level = static_cast<float>((*this)[oLevel]);
    s.hysteresis = static_cast<float>((*this)[oHyst]);
    return s;
  }
};

// Which rows a sheet tab shows for these options (in order): ANALYSIS what is measured and how the trace is held
// still, DISPLAY only how it is drawn.
void visibleRows(const Options& o, int tab, std::vector<uint32_t>& out) {
  out.clear();
  if (tab == sheet::kDisplay) {
    out.push_back(oAutoGain);
    if (!o.on(oAutoGain)) out.push_back(oGain);
    for (const uint32_t i : {oLog, oPersist, oLine, oGlow, oGrid, oReadout, oNote}) out.push_back(i);
    if (o.channel() == Ch::Stereo) out.push_back(oLanes);
    return;
  }
  const Tb tb = o.timebase();
  out.push_back(oTimebase);
  if (tb == Tb::Pitch) {
    out.push_back(oLock);
    out.push_back(oSource);
  }
  if (tb != Tb::Beat) out.push_back(oWindow);
  if (tb == Tb::Pitch && o.choice(oLock) != 2) out.push_back(oCycles);
  if (tb == Tb::Pitch) {
    out.push_back(oMinHz);
    out.push_back(oMaxHz);
    out.push_back(oClarity);
  }
  if (tb != Tb::Beat) {
    out.push_back(oTrigger);
    if (o.choice(oTrigger) != 0) {
      out.push_back(oLevel);
      out.push_back(oHyst);
    }
  }
  if (tb == Tb::Beat) out.push_back(oSpan);
  out.push_back(oChannel);
}

// The nearest 12-EDO note of hz (C3 = 261.626 Hz, the spectrum display's naming) and its cents.
void noteName(double hz, char* out, size_t n) {
  static const char* const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (!(hz > 8.0)) {
    out[0] = '\0';
    return;
  }
  const double semis = 60.0 + 12.0 * std::log2(hz / 261.6256);
  const long midi = std::lround(semis);
  const int cents = static_cast<int>(std::lround((semis - static_cast<double>(midi)) * 100.0));
  const long octave = midi / 12 - 2 - (midi < 0 && midi % 12 != 0 ? 1 : 0);
  std::snprintf(out, n, "%s%ld %+d", kNames[((midi % 12) + 12) % 12], octave, cents);
}

// The hosts poll a face's voice reader; this one stands for the feed's history.
class FeedFrames final : public VoiceTapReader {
public:
  uint32_t read(std::span<FaceVoice>) override { return 0; }
  uint64_t serial() const noexcept override { return serial_; }
  bool live() const noexcept override { return live_; }
  uint64_t serial_ = 0;
  bool live_ = false;
};

class ScopeFace final : public MetaFace {
public:
  explicit ScopeFace(const meta::Type& t) : MetaFace(t) { tracker_.setSettings(opts_.settings()); }
  ~ScopeFace() override { letGo(); }

  Vec2 worldSize() const noexcept override { return {kW, kH}; }
  float inputY(size_t, size_t) const noexcept override { return 0.5f * kH; }
  float jackInset() const noexcept override { return 0.0f; }  // TMM.13-fix2 (L23): on the frame's edge, off the plot
  float outputY(size_t, size_t) const noexcept override { return 0.5f * kH; }

  void setAnalysis(std::function<devices::SpectrumFeed*()> feedOf, std::function<void()> wakeHost) override {
    feedOf_ = std::move(feedOf);
    wakeHost_ = std::move(wakeHost);
    syncFeed();
  }
  bool wantsAnalysis() const noexcept override { return true; }
  VoiceTapReader* voices() const noexcept override { return feedOf_ ? &frames_ : nullptr; }
  bool animating() const noexcept override { return frames_.live_; }

  bool pollVoices() override {
    bound_ = true;  // MetaFace::bind sets the target, then polls
    syncOptions();
    if (!syncFeed()) return true;  // no tap yet: one repaint (the host asks again at its next wake)
    const double now = clockNow();
    if (now - lastPaint_ > 0.5) {  // not painted lately: its host is hidden — after one unanswered repaint, let go
      if (probe_ == 0.0) {
        probe_ = now;
        return true;
      }
      if (lastPaint_ < probe_ && now - probe_ > 0.05) {
        letGo();
        frames_.live_ = false;
        probe_ = 0.0;
        return false;
      }
    }
    if (frozen_ || !acquired_) {
      frames_.live_ = false;
      return false;
    }
    const bool changed = analyse();
    frames_.live_ = !settled_;
    if (changed) ++frames_.serial_;
    return changed;
  }

  // ---- static layers: body; the plot's well; frame ----
  uint32_t staticLayerCount() const noexcept override { return 3; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.body : (layer == 1 ? th.device.display : th.face.frame);
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    const Rgba w = ui::rgba(255, 255, 255);
    if (layer == 0) p.roundedRect({1.0f, 1.0f, kW - 1.0f, kH - 1.0f}, kRadius, w);
    else if (layer == 1) p.roundedRect(kPlot, 4.0f, w);
    else outline(p, w);
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    const ui::Theme& th = *env.theme;
    syncOptions();
    syncFeed();
    lastPaint_ = clockNow();
    probe_ = 0.0;
    if (feed_ != nullptr && !acquired_) {
      acquire();
      if (!frozen_) analyse();  // a display that just appeared draws its first frame complete
      if (wakeHost_) wakeHost_();
    } else if (feed_ != nullptr && !hasFrame_ && !frozen_) {
      analyse();
    }
    const ui::FaceTransform& t = p.transform();
    const float scale = t.scale;
    const Vec2 a = t.map({kPlot.x0, kPlot.y0}), b = t.map({kPlot.x1, kPlot.y1});
    const Rect r{a.x, a.y, b.x, b.y};
    ui::DrawList& dl = p.drawList();
    const bool labels = kLabelPx * scale >= kLabelMinDevicePx * env.uiScale;
    dl.pushClip(r);
    paintTrace(dl, r, scale);
    paintMeasure(p, dl, r, scale, labels && !sheet_);  // the sheet covers the card
    dl.popClip();
    if (labels) {
      paintText(p, r);
      chip(p, kTbChip, kTbNames[static_cast<int>(opts_.timebase())], opts_.timebase() != Tb::Fixed, th);
      chip(p, kChChip, kChNames[static_cast<int>(opts_.channel())], false, th);
      chip(p, kFreezeChip, "FREEZE", frozen_, th, frozen_ ? kFrozen : 0);
      chip(p, kSetChip, "SET", sheet_, th);
      if (sheet_) paintSheet(p, th);
    }
    for (const FaceJack& j : env.jacks) {  // leads in and out: the signal passes through unchanged
      if (!j.visible) continue;
      const float x0 = j.input ? 5.0f : kPlot.x1, x1 = j.input ? kPlot.x0 : kW - 5.0f;
      p.line({x0, j.at.y}, {x1, j.at.y}, 1.25f, j.connected ? th.face.signal : th.face.jackRing);
    }
    if (env.selected || (env.hovered && hovered_ == kFrame)) outline(p, env.selected ? th.circuit.selected : th.circuit.hover);
  }

  void liveRects(std::vector<Rect>& out) const override { out.push_back({0.0f, 0.0f, kW, kH}); }

  int32_t hitTest(Vec2 l, float) const noexcept override {
    if (l.x < 0.0f || l.y < 0.0f || l.x > kW || l.y > kH) return kNone;
    if (inside(kTbChip, l)) return kTbPart;
    if (inside(kChChip, l)) return kChPart;
    if (inside(kFreezeChip, l)) return kFreezePart;
    if (inside(kSetChip, l)) return kSetPart;
    if (sheet_) {
      const int32_t part = panel_.hit(l, rows_.size());
      if (part >= 0) return part == sheet::kBody ? kSheetPart : part;  // the sheet's background: nothing under it reacts
    }
    if (inside(kPlot, l)) return kPlotPart;
    return kFrame;
  }

  bool onPointer(const FacePointer& e) override {
    using K = FacePointer::Kind;
    const bool shift = (e.mods & ui::mod::kShift) != 0;
    switch (e.kind) {
      case K::Down: {
        if (e.button != ui::PointerButton::Left) return false;
        const int32_t part = hitTest(e.local, e.pxPerUnit);
        if (part == kTbPart || part == kChPart) {
          const uint32_t o = part == kTbPart ? oTimebase : oChannel;
          setOption(o, sheet::cycled(opts_, o, shift ? -1 : 1));
          return true;
        }
        if (part == kFreezePart) {
          frozen_ = !frozen_;
          if (!frozen_) {
            analyse();
            if (wakeHost_) wakeHost_();
          }
          return true;
        }
        if (part == kSetPart) {
          sheet_ = !sheet_;
          return true;
        }
        if (part >= kRowBase) return rowDown(static_cast<uint32_t>(part - kRowBase), e);
        if (part == sheet::kTabBase || part == sheet::kTabBase + 1) {
          panel_.tab = part - sheet::kTabBase;
          visibleRows(opts_, panel_.tab, rows_);
          return true;
        }
        if (part == sheet::kSave) {
          meta::saveUserDefaultData(type().id, opts_.data());
          return true;
        }
        if (part == sheet::kReset) {  // every option back to its default, one step
          opts_.reset();
          writeOptions();
          return true;
        }
        if (part == kSheetPart) return true;
        if (part != kPlotPart) return false;
        // The plot: Ctrl+drag measures, a drag pans while zoomed, a double-click resets the zoom.
        if (e.mods & ui::mod::kCtrl) {
          const double s = sampleAt(e.local.x);
          const float tol = 4.0f / std::max(e.pxPerUnit, 1e-3f);
          if (measure_ && e.clicks == 1) {
            if (std::fabs(e.local.x - worldXOf(mA_)) <= tol) std::swap(mA_, mB_);  // grab an edge: mA_ stays
            else if (std::fabs(e.local.x - worldXOf(mB_)) > tol) mA_ = s;
          } else {
            mA_ = s;
          }
          mB_ = s;
          measure_ = true;
          drag_ = Drag::Measure;
          pressX_ = e.device.x;
          moved_ = false;
          return true;
        }
        if (zoomed()) {
          if (e.clicks >= 2) {
            zoomStart_ = 0.0;
            zoomSpan_ = 1.0;
            return true;
          }
          drag_ = Drag::Pan;
          pressX_ = e.device.x;
          pressZoom_ = zoomStart_;
          pxPerUnit_ = e.pxPerUnit;
          return true;
        }
        return false;  // the host's (select, move)
      }
      case K::Move: {
        if (drag_ == Drag::Measure) {
          mB_ = sampleAt(e.local.x);
          if (shift) mB_ = std::round(mB_);
          moved_ = moved_ || std::fabs(e.device.x - pressX_) > 3.0f;
          return true;
        }
        if (drag_ == Drag::Pan) {
          const double plotPx = (kPlot.x1 - kPlot.x0) * pxPerUnit_;
          zoomStart_ = std::clamp(pressZoom_ - (e.device.x - pressX_) / plotPx * zoomSpan_, 0.0, 1.0 - zoomSpan_);
          return true;
        }
        if (drag_ == Drag::Row) {
          const Opt& op = kOpts[dragOpt_];
          const double k = (shift ? 0.1 : 1.0) / 160.0;
          const double t = std::clamp(pressTravel_ + (pressY_ - e.device.y) * k, 0.0, 1.0);
          setOption(dragOpt_, sheet::fromTravel(op, t));
          return true;
        }
        const float x = inside(kPlot, e.local) && !sheet_ && (e.mods & ui::mod::kCtrl) ? e.local.x : -1.0f;
        if (x == hoverX_) return false;
        hoverX_ = x;
        return true;
      }
      case K::Up: {
        const Drag d = drag_;
        drag_ = Drag::None;
        if (d == Drag::Measure && !moved_ && std::fabs(mB_ - mA_) < 0.5) measure_ = false;  // a Ctrl+click clears
        if (d == Drag::Row && editing_) {
          target().end();
          editing_ = false;
        }
        return d != Drag::None;
      }
      case K::Wheel: {
        if (!(e.mods & ui::mod::kAlt) || !inside(kPlot, e.local) || sheet_) return false;
        const double f = std::pow(0.85, static_cast<double>(e.wheel.y));
        const double at = std::clamp((e.local.x - kPlot.x0) / (kPlot.x1 - kPlot.x0), 0.0f, 1.0f);
        const double focus = zoomStart_ + at * zoomSpan_;
        zoomSpan_ = std::clamp(zoomSpan_ * f, minZoom(), 1.0);
        zoomStart_ = std::clamp(focus - at * zoomSpan_, 0.0, 1.0 - zoomSpan_);
        return true;
      }
      case K::Leave:
        if (hoverX_ < 0.0f) return false;
        hoverX_ = -1.0f;
        return true;
    }
    return false;
  }
  bool dragging() const noexcept override { return drag_ != Drag::None; }
  void cancelDrag() override {
    if (drag_ == Drag::Row && editing_) {
      target().cancel();
      editing_ = false;
      syncOptions(true);
    }
    drag_ = Drag::None;
  }
  bool onKey(const ui::KeyEvent& e) override {
    if (!e.down || e.key != ui::key::kEscape) return false;
    if (measure_) {
      measure_ = false;
      return true;
    }
    if (sheet_) {
      sheet_ = false;
      return true;
    }
    return false;
  }
  ui::Cursor cursor(int32_t part) const noexcept override {
    if (part == kTbPart || part == kChPart || part == kFreezePart || part == kSetPart) return ui::Cursor::Pointer;
    if (part >= kRowBase) {
      const auto i = static_cast<size_t>(part - kRowBase);
      if (i < rows_.size() && sheet::numeric(kOpts[rows_[i]])) return ui::Cursor::ResizeNS;
      return ui::Cursor::Pointer;
    }
    if (part >= sheet::kTabBase && part <= sheet::kReset) return ui::Cursor::Pointer;
    if (part == kPlotPart && drag_ == Drag::Measure) return ui::Cursor::ResizeEW;
    return ui::Cursor::Inherit;
  }

  void describe(int32_t part, std::string& title, std::string& body) const override {
    switch (part) {
      case kTbPart:
        title = "Timebase";
        body = "Click to cycle (Shift: back). PITCH holds the trace still by the signal's period (the note's own pitch when "
               "the module runs per voice, else a detected one); FIXED shows a set span from an edge trigger; BEAT spans "
               "a musical length of the transport, the downbeat on the left. More in SET.";
        return;
      case kChPart:
        title = "Channel";
        body = "Click to cycle (Shift: back): MID (L + R) / 2, L, R, SIDE (L − R) / 2, or L/R in two lanes.";
        return;
      case kFreezePart:
        title = "Freeze";
        body = frozen_ ? "Frozen: the picture holds (the signal still passes). Measure and zoom it; click to follow again."
                       : "Click to hold the picture (the signal still passes).";
        return;
      case kSetPart:
        title = "Settings";
        body = sheet_ ? "Click to close the settings." : "Click for every option: lock, window, cycles, pitch range, trigger, "
                                                       "gain, persistence, line, glow, readouts.";
        return;
      case kPlotPart:
        title = "Oscilloscope";
        body = feed_ != nullptr ? "The signal passing through (it is not changed). Ctrl+drag measures a span; Alt+wheel "
                                  "zooms time, drag pans while zoomed, double-click resets."
                                : "The signal passing through (it is not changed). Shown when the module plays in a track's "
                                  "chain.";
        return;
      case sheet::kTabBase:
        title = "Analysis";
        body = "The options of what is measured and how the trace is held still: timebase, pitch lock, window, pitch "
               "range, trigger, channel.";
        return;
      case sheet::kTabBase + 1:
        title = "Display";
        body = "Only how it is drawn: gain, log amplitude, persistence, line, glow, grid, readouts.";
        return;
      case sheet::kSave:
        title = "Save as default";
        body = "New Oscilloscopes start with these options (every tab). Saved with pqdaw's settings; existing ones keep "
               "theirs.";
        return;
      case sheet::kReset:
        title = "Reset";
        body = "Every option back to the factory default.";
        return;
      default:
        if (part >= kRowBase && static_cast<size_t>(part - kRowBase) < rows_.size()) {
          const Opt& op = kOpts[rows_[static_cast<size_t>(part - kRowBase)]];
          title = op.label;
          body = sheet::numeric(op)
                     ? "Drag up / down to set (Shift: fine); double-click resets."
                     : "Click to change (Shift: back); double-click resets.";
          return;
        }
        MetaFace::describe(part, title, body);
    }
  }

  // ---- tests ----
  void shown(ScopeShown& out) const {
    out.hasFrame = hasFrame_;
    out.start = frameAbs_;
    out.span = frame_.span;
    out.pitchHz = frame_.pitchHz;
    out.note = frame_.note;
    out.gain = gain_;
    out.trails = static_cast<uint32_t>(trailsA_.size());
    out.frozen = frozen_;
    out.acquired = acquired_;
    out.timebase = static_cast<uint32_t>(opts_.timebase());
    out.channel = static_cast<uint32_t>(opts_.channel());
    out.beatPhase = beat_.phase();
    out.samples = shownA_;
  }
  void setOptionForTest(std::string_view key, double v) {
    for (uint32_t i = 0; i < oCount; ++i)
      if (key == kOpts[i].key) setOption(i, v);
  }
  void setFrozenForTest(bool f) { frozen_ = f; }
  void setSheetForTest(bool s) { sheet_ = s; }
  void setMeasureForTest(double a, double b) {
    measure_ = true;
    mA_ = a;
    mB_ = b;
  }

private:
  enum class Drag : uint8_t { None, Measure, Pan, Row };

  // ---- options ----
  void syncOptions(bool force = false) {
    const meta::DataPtr d = hasTarget() ? target().data() : nullptr;
    if (!force && d == data_ && optionsLoaded_) return;
    data_ = d;
    optionsLoaded_ = true;
    if (!local_) opts_.load(d.get());
    applyOptions();
  }
  void applyOptions() {
    const dsp::ScopeSettings s = opts_.settings();
    const dsp::ScopeSettings& o = tracker_.settings();
    if (s.timebase != o.timebase || s.lock != o.lock || s.source != o.source) tracker_.reset();
    const double span = kBeatSpans[std::clamp(opts_.choice(oSpan), 0, 4)];
    if (s.timebase != o.timebase || span != beat_.span()) {
      beat_.setSpan(span);
      beat_.clear();
      lastEnd_ = 0;  // the beat slots re-read the whole history (a stopped transport still shows its last pass)
    }
    tracker_.setSettings(s);
    const uint32_t keep = keepTrails();
    while (trailsA_.size() > keep) trailsA_.erase(trailsA_.begin());
    while (trailsB_.size() > keep) trailsB_.erase(trailsB_.begin());
    visibleRows(opts_, panel_.tab, rows_);
    hasFrame_ = false;  // recompute with the new options at the next poll / paint
  }
  // Writes an option (one undo step, or part of the open drag's gesture).
  void setOption(uint32_t o, double v) {
    opts_.set(o, v);
    writeOptions();
  }
  void writeOptions() {
    if (hasTarget() && target().editable()) {
      data_ = opts_.data();
      target().setData(data_);
      data_ = target().data();  // the document's pointer: unchanged until an undo or another edit
    } else {
      local_ = true;  // a read-only target: the face keeps its options itself
    }
    applyOptions();
    if (!frozen_ && acquired_) analyse();
    if (wakeHost_) wakeHost_();
  }
  bool rowDown(uint32_t row, const FacePointer& e) {
    if (row >= rows_.size()) return false;
    const uint32_t o = rows_[row];
    const Opt& op = kOpts[o];
    if (e.clicks >= 2) {
      setOption(o, op.def);
      return true;
    }
    if (op.kind == Opt::Choice || op.kind == Opt::Toggle) {
      setOption(o, sheet::cycled(opts_, o, (e.mods & ui::mod::kShift) ? -1 : 1));
      return true;
    }
    drag_ = Drag::Row;
    dragOpt_ = o;
    pressY_ = e.device.y;
    pressTravel_ = sheet::toTravel(op, opts_[o]);
    if (hasTarget() && target().editable()) {
      target().begin(op.label);
      editing_ = true;
    }
    return true;
  }

  // ---- the feed ----
  bool syncFeed() {
    devices::SpectrumFeed* f = feedOf_ ? feedOf_() : nullptr;
    if (f != feed_) {
      letGo();
      feed_ = f;
      lastEnd_ = 0;
      hasFrame_ = false;
      tracker_.reset();
      beat_.clear();
    }
    return feed_ != nullptr;
  }
  void acquire() {
    if (acquired_ || feed_ == nullptr) return;
    feed_->acquire(devices::kWantScope);
    feed_->addWaker(this, [this] {
      frames_.live_ = true;
      if (wakeHost_) wakeHost_();
    });
    acquired_ = true;
  }
  void letGo() {
    if (!acquired_) return;
    feed_->removeWaker(this);
    feed_->release(devices::kWantScope);
    acquired_ = false;
  }
  uint32_t keepTrails() const noexcept {
    const double p = opts_[oPersist];
    return p > 0.001 ? static_cast<uint32_t>(std::max(1.0, p * kMaxTrails)) : 0u;
  }

  // Reads what the timebase needs and computes the frame (UI thread, once per displayed frame). True: it changed.
  bool analyse() {
    if (feed_ == nullptr) return false;
    bool settled = false;
    const uint64_t end = feed_->scopeEnd(settled);
    settled_ = settled;
    if (end == lastEnd_ && hasFrame_) return false;
    const double sr = feed_->config().sampleRate;
    rate_ = sr;
    const uint32_t nch = feed_->scopeChannels();
    const bool four = nch >= 4;
    const uint32_t cL = four ? 2 : 0, cR = four ? 3 : (nch > 1 ? 1 : 0);
    const Ch ch = opts_.channel();
    if (opts_.timebase() == Tb::Beat) {
      // Everything new since the last read (at most the history), through the beat slots.
      const uint64_t from = std::max(lastEnd_, end > devices::SpectrumFeed::kScopeFrames ? end - devices::SpectrumFeed::kScopeFrames : 0);
      const auto n = static_cast<uint32_t>(end - from);
      bufBeat_.resize(n);
      bufL_.resize(n);
      bufR_.resize(n);
      float* outs[4] = {four ? bufBeat_.data() : nullptr, nullptr, nullptr, nullptr};
      outs[cL] = bufL_.data();
      if (cR != cL) outs[cR] = bufR_.data();
      feed_->scopeRead(from, n, outs);
      if (cR == cL) bufR_ = bufL_;
      if (!four) std::fill(bufBeat_.begin(), bufBeat_.end(), std::nanf(""));
      beat_.ingest(bufBeat_.data(), bufL_.data(), bufR_.data(), n);
      lastEnd_ = end;
      hasFrame_ = true;
      updateGain();
      return true;
    }
    const size_t need = std::min<size_t>(tracker_.need(sr), devices::SpectrumFeed::kScopeFrames);
    const uint64_t from = end > need ? end - need : 0;
    const auto n = static_cast<uint32_t>(end - from);
    bufL_.resize(n);
    bufR_.resize(n);
    bufNote_.resize(1);
    float* outs[4] = {nullptr, nullptr, nullptr, nullptr};
    outs[cL] = bufL_.data();
    if (cR != cL) outs[cR] = bufR_.data();
    feed_->scopeRead(from, n, outs);
    if (cR == cL) bufR_ = bufL_;
    float noteHz = 0.0f;
    if (four && end > 0) {
      float* o2[4] = {nullptr, bufNote_.data(), nullptr, nullptr};
      feed_->scopeRead(end - 1, 1, o2);
      noteHz = std::isfinite(bufNote_[0]) && bufNote_[0] > 0.0f ? bufNote_[0] : 0.0f;
    }
    // The traced signal (and the second lane's).
    bufA_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      const float l = bufL_[i], r = bufR_[i];
      bufA_[i] = ch == Ch::Left || ch == Ch::Stereo ? l : ch == Ch::Right ? r : ch == Ch::Side ? 0.5f * (l - r) : 0.5f * (l + r);
    }
    frame_ = tracker_.frame(bufA_, sr, noteHz, false);
    frameAbs_ = from + frame_.start;
    // What is shown: the frame's samples plus one either side (the cubic reads its neighbours).
    const size_t s0 = frame_.start > 0 ? frame_.start - 1 : 0;
    const size_t s1 = std::min<size_t>(bufA_.size(), frame_.start + frame_.span + 2);
    lead_ = static_cast<uint32_t>(frame_.start - s0);
    shownA_.assign(bufA_.begin() + static_cast<std::ptrdiff_t>(s0), bufA_.begin() + static_cast<std::ptrdiff_t>(s1));
    if (ch == Ch::Stereo) shownB_.assign(bufR_.begin() + static_cast<std::ptrdiff_t>(s0), bufR_.begin() + static_cast<std::ptrdiff_t>(s1));
    else shownB_.clear();
    lastEnd_ = end;
    const bool first = !hasFrame_;
    hasFrame_ = true;
    updateGain(first);
    pushTrails();
    return true;
  }
  // The visible window: [vs, vs + vn) of the shown samples (after the zoom).
  void visible(size_t& vs, size_t& vn) const noexcept {
    const size_t span = frame_.span;
    vs = lead_ + static_cast<size_t>(zoomStart_ * static_cast<double>(span));
    vn = std::max<size_t>(2, static_cast<size_t>(zoomSpan_ * static_cast<double>(span)));
    if (vs + vn > shownA_.size()) vn = shownA_.size() > vs ? shownA_.size() - vs : 0;
  }
  void updateGain(bool snap = false) {
    if (!opts_.on(oAutoGain)) {
      gain_ = static_cast<float>(opts_[oGain]);
      return;
    }
    float peak = 1e-4f;
    if (opts_.timebase() == Tb::Beat) {
      beat_.columns(beatChannel(), 512, colMn_, colMx_, colOk_);
      for (size_t c = 0; c < colOk_.size(); ++c)
        if (colOk_[c]) peak = std::max({peak, std::fabs(colMn_[c]), std::fabs(colMx_[c])});
      if (opts_.channel() == Ch::Stereo) {
        beat_.columns(dsp::ScopeBeat::kR, 512, colMn_, colMx_, colOk_);
        for (size_t c = 0; c < colOk_.size(); ++c)
          if (colOk_[c]) peak = std::max({peak, std::fabs(colMn_[c]), std::fabs(colMx_[c])});
      }
      if (peak <= 1e-4f) return;
      const float target = std::clamp(0.85f / peak, 0.1f, 40.0f);  // PQ Meter's beat view: 0.85
      gain_ = snap || !hasGain_ ? target : gain_ + (target - gain_) * 0.08f;
      hasGain_ = true;
      return;
    }
    size_t vs = 0, vn = 0;
    visible(vs, vn);
    for (size_t i = vs; i < vs + vn; ++i) {
      peak = std::max(peak, std::fabs(shownA_[i]));
      if (!shownB_.empty()) peak = std::max(peak, std::fabs(shownB_[i]));
    }
    // 0.72, not 0.9: the gain lags, so a transient louder than the recent past is on screen before it moves (PQ Meter).
    if (peak <= 1e-4f) return;  // nothing visible: the gain holds (the first sound sets it at once)
    const float target = std::clamp(0.72f / peak, 0.1f, 40.0f);
    gain_ = snap || !hasGain_ ? target : gain_ + (target - gain_) * 0.08f;
    hasGain_ = true;
  }
  uint32_t beatChannel() const noexcept {
    switch (opts_.channel()) {
      case Ch::Left: case Ch::Stereo: return dsp::ScopeBeat::kL;
      case Ch::Right: return dsp::ScopeBeat::kR;
      case Ch::Side: return dsp::ScopeBeat::kS;
      default: return dsp::ScopeBeat::kM;
    }
  }
  // A trail: this frame's trace, kTrailPoints points of (x 0 … 1, the scaled value), oldest first.
  void pushTrails() {
    const uint32_t keep = keepTrails();
    if (keep == 0) {
      trailsA_.clear();
      trailsB_.clear();
      return;
    }
    auto push = [&](std::vector<std::vector<Vec2>>& trails, const std::vector<float>& src) {
      size_t vs = 0, vn = 0;
      visible(vs, vn);
      if (vn < 2) return;
      // Reuse the oldest trail's buffer once the count is full (no allocation per frame).
      std::vector<Vec2> t;
      if (trails.size() >= keep && !trails.empty()) {
        t = std::move(trails.front());
        trails.erase(trails.begin());
      }
      t.resize(kTrailPoints);
      for (uint32_t k = 0; k < kTrailPoints; ++k) {
        const double u = static_cast<double>(k) / (kTrailPoints - 1);
        t[k] = {static_cast<float>(u), shape(cubic(src, static_cast<double>(vs) + u * static_cast<double>(vn - 1)))};
      }
      trails.push_back(std::move(t));
      while (trails.size() > keep) trails.erase(trails.begin());
    };
    push(trailsA_, shownA_);
    if (!shownB_.empty()) push(trailsB_, shownB_);
    else trailsB_.clear();
  }
  // The displayed amplitude of a sample: gain, then log amplitude if chosen (−60 … 0 dB, the sign kept), clamped.
  float shape(float v) const noexcept {
    if (opts_.on(oLog)) {
      const float a = std::fabs(v) * gain_;
      const float db = a > 1e-6f ? 20.0f * std::log10(a) : -120.0f;
      return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f) * (v < 0.0f ? -1.0f : 1.0f);
    }
    return std::clamp(v * gain_, -1.5f, 1.5f);
  }
  static float cubic(const std::vector<float>& s, double pos) noexcept {
    const auto i0 = static_cast<size_t>(std::max(pos, 0.0));
    if (s.empty()) return 0.0f;
    if (i0 + 1 >= s.size()) return s.back();
    const double f = pos - static_cast<double>(i0);
    const float s1 = s[i0], s2 = s[i0 + 1];
    const float s0 = i0 > 0 ? s[i0 - 1] : s1;
    const float s3 = i0 + 2 < s.size() ? s[i0 + 2] : s2;
    const double f2 = f * f, f3 = f2 * f;
    return static_cast<float>(s0 * (-0.5 * f3 + f2 - 0.5 * f) + s1 * (1.5 * f3 - 2.5 * f2 + 1.0) +
                              s2 * (-1.5 * f3 + 2.0 * f2 + 0.5 * f) + s3 * (0.5 * f3 - 0.5 * f2));
  }
  bool zoomed() const noexcept { return zoomSpan_ < 0.999; }
  double minZoom() const noexcept { return frame_.span > 32 ? 16.0 / static_cast<double>(frame_.span) : 1.0; }
  // The sample (relative to the frame's start, fractional) under world x, and back.
  double sampleAt(float worldX) const noexcept {
    const double u = std::clamp((worldX - kPlot.x0) / (kPlot.x1 - kPlot.x0), 0.0f, 1.0f);
    return (zoomStart_ + u * zoomSpan_) * static_cast<double>(frame_.span);
  }
  float worldXOf(double sample) const noexcept {
    const double u = (sample / std::max<double>(1.0, static_cast<double>(frame_.span)) - zoomStart_) / zoomSpan_;
    return kPlot.x0 + static_cast<float>(u) * (kPlot.x1 - kPlot.x0);
  }

  // ---- painting ----
  void paintTrace(ui::DrawList& dl, const Rect& r, float scale) {
    const bool stereo = opts_.channel() == Ch::Stereo;
    const int lanes = stereo ? 2 : 1;
    const float h = r.y1 - r.y0, laneH = h / static_cast<float>(lanes);
    const float inset = 2.0f * scale;
    auto laneMid = [&](int lane) { return r.y0 + laneH * (static_cast<float>(lane) + 0.5f); };
    auto yOf = [&](float shaped, int lane) { return laneMid(lane) - shaped * (laneH * 0.5f - inset); };
    const float lw = std::max(0.25f, static_cast<float>(opts_[oLine]) * scale);
    const float glow = static_cast<float>(opts_[oGlow]);
    const bool beat = opts_.timebase() == Tb::Beat;
    if (opts_.on(oGrid)) {
      for (int lane = 0; lane < lanes; ++lane) {
        const float mid = laneMid(lane);
        dl.addHLine(r.x0, r.x1, mid, kGridLabel, scale);
        if (!beat)
          for (const float s : {-0.5f, 0.5f}) dl.addHLine(r.x0, r.x1, mid - s * (laneH * 0.5f - inset), kGrid, scale);
      }
      if (beat) {
        const int beats = static_cast<int>(kBeatSpans[std::clamp(opts_.choice(oSpan), 0, 4)]);
        const int divs = beats == 1 ? 4 : beats;
        for (int d = 1; d < divs; ++d) {
          const float x = r.x0 + (r.x1 - r.x0) * static_cast<float>(d) / static_cast<float>(divs);
          const bool bar = beats >= 8 && d % 4 == 0;
          dl.addVLine(x, r.y0, r.y1, bar ? ui::smoothAlphaMul(kGridLabel, 0.6f) : kGrid, scale);
        }
      } else {
        for (int d = 1; d < 10; ++d) dl.addVLine(r.x0 + (r.x1 - r.x0) * static_cast<float>(d) / 10.0f, r.y0, r.y1, kGrid, scale);
      }
    }
    if (!hasFrame_) return;
    if (beat) {
      paintBeat(dl, r, lanes, yOf, lw, glow, scale);
      return;
    }
    // Persistence trails under the trace, oldest faintest.
    const uint32_t keep = keepTrails();
    auto trails = [&](const std::vector<std::vector<Vec2>>& tr, int lane, Rgba col) {
      // The last trail is this frame's trace (drawn below as the trace itself).
      for (size_t t = 0; t + 1 < tr.size(); ++t) {
        const float age = static_cast<float>(tr.size() - 1 - t) / static_cast<float>(keep + 1);
        const float alpha = 140.0f * (1.0f - age) * static_cast<float>(opts_[oPersist]);
        if (alpha <= 2.0f) continue;
        pts_.resize(tr[t].size());
        for (size_t k = 0; k < tr[t].size(); ++k) pts_[k] = {r.x0 + tr[t][k].x * (r.x1 - r.x0), yOf(tr[t][k].y, lane)};
        ui::smoothPolyline(dl, pts_, ui::withAlpha(col, static_cast<uint32_t>(alpha)), std::max(0.75f * scale, lw * 0.85f));
      }
    };
    if (keep > 0) {
      trails(trailsA_, 0, kTraceA);
      if (stereo) trails(trailsB_, 1, kTraceB);
    }
    size_t vs = 0, vn = 0;
    visible(vs, vn);
    if (vn < 2) return;
    const float w = r.x1 - r.x0;
    const double spp = static_cast<double>(vn) / std::max(1.0f, w);
    auto lane = [&](const std::vector<float>& src, int ln, Rgba col) {
      if (spp > kEnvelopeSpp) {
        // Dense: the min / max envelope, one column per device px.
        const int cols = std::max(2, static_cast<int>(w));
        up_.clear();
        dn_.clear();
        for (int c = 0; c < cols; ++c) {
          const size_t a = vs + static_cast<size_t>(static_cast<double>(c) * spp);
          size_t b = vs + static_cast<size_t>(static_cast<double>(c + 1) * spp);
          if (a >= src.size()) break;
          b = std::min(b, src.size());
          if (a >= b) continue;
          float lo = src[a], hi = src[a];
          for (size_t i = a + 1; i < b; ++i) {
            lo = std::min(lo, src[i]);
            hi = std::max(hi, src[i]);
          }
          float yHi = yOf(shape(hi), ln), yLo = yOf(shape(lo), ln);
          if (yHi > yLo) std::swap(yHi, yLo);
          if (yLo - yHi < 0.5f) {
            const float m = 0.5f * (yHi + yLo);
            yHi = m - 0.25f;
            yLo = m + 0.25f;
          }
          const float x = r.x0 + static_cast<float>(c) + 0.5f;
          up_.push_back({x, yHi});
          dn_.push_back({x, yLo});
        }
        if (up_.size() >= 2) ui::scopeEnvelope(dl, up_.data(), dn_.data(), static_cast<uint32_t>(up_.size()), col, lw, glow);
        return;
      }
      // A curve through the samples: Catmull-Rom at two points per device px, then the screen-space spline.
      const int steps = std::max(2, static_cast<int>(w * 2.0f));
      pts_.clear();
      for (int s = 0; s <= steps; ++s) {
        const double u = static_cast<double>(s) / steps;
        const double pos = static_cast<double>(vs) + static_cast<double>(vn - 1) * u;
        pts_.push_back({r.x0 + static_cast<float>(u) * w, yOf(shape(cubic(src, pos)), ln)});
      }
      ui::splinePath(pts_, smooth_, 1.25f);
      ui::scopeTrace(dl, smooth_, col, lw, glow);
    };
    lane(shownA_, 0, kTraceA);
    if (stereo && !shownB_.empty()) lane(shownB_, 1, kTraceB);
  }

  template <class YOf>
  void paintBeat(ui::DrawList& dl, const Rect& r, int lanes, const YOf& yOf, float lw, float glow, float scale) {
    const float w = r.x1 - r.x0;
    const auto cols = static_cast<uint32_t>(std::clamp(static_cast<int>(w), 2, 1024));
    auto lane = [&](uint32_t channel, int ln, Rgba col) {
      beat_.columns(channel, cols, colMn_, colMx_, colOk_);
      uint32_t c = 0;
      while (c < cols) {  // one band per run of filled columns (an unvisited stretch is a hole, not a zero line)
        while (c < cols && !colOk_[c]) ++c;
        up_.clear();
        dn_.clear();
        while (c < cols && colOk_[c]) {
          float yHi = yOf(shape(colMx_[c]), ln), yLo = yOf(shape(colMn_[c]), ln);
          if (yHi > yLo) std::swap(yHi, yLo);
          if (yLo - yHi < 0.5f) {
            const float m = 0.5f * (yHi + yLo);
            yHi = m - 0.25f;
            yLo = m + 0.25f;
          }
          const float x = r.x0 + (static_cast<float>(c) + 0.5f) / static_cast<float>(cols) * w;
          up_.push_back({x, yHi});
          dn_.push_back({x, yLo});
          ++c;
        }
        if (up_.size() >= 2) ui::scopeEnvelope(dl, up_.data(), dn_.data(), static_cast<uint32_t>(up_.size()), col, lw, glow);
      }
    };
    lane(beatChannel(), 0, kTraceA);
    if (lanes == 2) lane(dsp::ScopeBeat::kR, 1, kTraceB);
    if (beat_.phase() >= 0.0) {  // the playhead; dim while the transport is stopped
      const float x = r.x0 + static_cast<float>(beat_.phase()) * w;
      dl.addVLine(x, r.y0, r.y1, ui::smoothAlphaMul(kAccent, beat_.moving() && !frozen_ ? 0.85f : 0.30f), scale);
    }
  }

  void paintText(ui::FacePainter& p, const Rect&) const {
    const float y = kPlot.y1 - 6.0f;
    auto left = [&](const char* s, Rgba col) {
      p.textAligned({kPlot.x0 + 4.0f, y}, s, kLabelPx, col, 0.0f);
    };
    char info[128];
    info[0] = '\0';
    const bool stereo = opts_.channel() == Ch::Stereo;
    const double sr = rate_ > 0.0 ? rate_ : 48000.0;
    const float gain = gain_;
    const char* agc = opts_.on(oAutoGain) ? " auto" : "";
    const double spanMs = static_cast<double>(frame_.span) * zoomSpan_ * 1000.0 / sr;
    if (opts_.timebase() == Tb::Beat) {
      const double bpm = beat_.bpm(sr);
      const int span = std::clamp(opts_.choice(oSpan), 0, 4);
      if (bpm > 20.0 && bpm < 999.0)
        std::snprintf(info, sizeof info, "%s  %.1f bpm  %.0f ms  %.2fx%s%s", kBeatSpanNames[span], bpm,
                      kBeatSpans[span] * 60000.0 / bpm, gain, agc, beat_.moving() ? "" : "  stopped");
      else
        std::snprintf(info, sizeof info, "%s  no clock  %.2fx%s", kBeatSpanNames[span], gain, agc);
    } else if (opts_.timebase() == Tb::Pitch && frame_.pitchHz > 0.0f) {
      const long cyc = std::lround(static_cast<double>(frame_.span) / std::max(frame_.period, 1.0));
      std::snprintf(info, sizeof info, "%.1f Hz  %ld cyc  %.1f ms  %.2fx%s", frame_.pitchHz, cyc, spanMs, gain, agc);
    } else if (opts_.timebase() == Tb::Pitch) {
      std::snprintf(info, sizeof info, "no pitch  %.1f ms/div  %.2fx%s", spanMs / 10.0, gain, agc);
    } else {
      std::snprintf(info, sizeof info, "%.1f ms/div  %.2fx%s", spanMs / 10.0, gain, agc);
    }
    // TMM.13-fix2 (MM review contrast table: 2.6:1): the readout in a lifted grey (≥ 4.5:1 on the plot), not the grid's.
    if (opts_.on(oReadout) && hasFrame_) left(info, kReadoutText);
    // The note, faded by the clarity (a marginal detection reads as marginal).
    if (opts_.timebase() == Tb::Pitch && opts_.on(oNote) && frame_.pitchHz > 0.0f && !stereo) {
      char note[32], text[48];
      noteName(frame_.pitchHz, note, sizeof note);
      std::snprintf(text, sizeof text, "%s%s", note, frame_.note ? "  note" : "");
      const auto a = static_cast<uint32_t>(90.0f + 140.0f * std::clamp(frame_.clarity, 0.0f, 1.0f));
      p.textAligned({kPlot.x1 - 4.0f, y}, text, kLabelPx, ui::withAlpha(kTraceA, a), 1.0f);
    }
    if (stereo && opts_.on(oLanes)) {
      // Each lane's letter at its top-right (the upper one under the chips), on a backing pad.
      const float laneH = (kPlot.y1 - kPlot.y0) * 0.5f;
      const float ys[2] = {kChipY + kChipH + 7.0f, kPlot.y0 + laneH + 8.0f};
      const char* letters[2] = {"L", "R"};
      for (int i = 0; i < 2; ++i) {
        p.roundedRect({kPlot.x1 - 13.0f, ys[i] - 5.0f, kPlot.x1 - 3.0f, ys[i] + 5.0f}, 2.0f, kPad);
        p.text({kPlot.x1 - 8.0f, ys[i]}, letters[i], kLabelPx, i == 0 ? kTraceA : kTraceB);
      }
    }
    if (zoomed()) {
      char z[24];
      std::snprintf(z, sizeof z, "%.1fx", 1.0 / zoomSpan_);
      p.text({0.5f * (kPlot.x0 + kPlot.x1), kChipY + 0.5f * kChipH}, z, kLabelPx, kGridLabel);
    }
  }

  // The measurement: a wash over the span, its edges, and a card of its numbers docked top-left under the chips.
  void paintMeasure(ui::FacePainter& p, ui::DrawList& dl, const Rect& r, float scale, bool labels) {
    if (!hasFrame_ || opts_.timebase() == Tb::Beat) return;
    // Ctrl held over the plot: a faint crosshair where a measurement would start.
    if (!measure_ && hoverX_ >= 0.0f) dl.addVLine(p.transform().map({hoverX_, 0.0f}).x, r.y0, r.y1, ui::withAlpha(kTraceA, 90), scale);
    if (!measure_) return;
    const double a = std::min(mA_, mB_), b = std::max(mA_, mB_);
    const float xa = std::clamp(p.transform().map({worldXOf(a), 0.0f}).x, r.x0, r.x1);
    const float xb = std::clamp(p.transform().map({worldXOf(b), 0.0f}).x, r.x0, r.x1);
    dl.addRectFilled({xa, r.y0, xb, r.y1}, ui::withAlpha(kTraceA, 22));
    dl.addVLine(xa, r.y0, r.y1, ui::withAlpha(kTraceA, 120), scale);
    dl.addVLine(xb, r.y0, r.y1, ui::withAlpha(kTraceA, 120), scale);
    if (!labels) return;
    // Level statistics of the visible channel(s) inside the span.
    const double sr = rate_ > 0.0 ? rate_ : 48000.0;
    const double dt = (b - a) / sr;
    float pk = 0.0f, lo = 0.0f, hi = 0.0f;
    double sum = 0.0, sq = 0.0;
    long cnt = 0;
    int zx = 0;
    float prev = 0.0f;
    const auto i0 = static_cast<size_t>(std::max(0.0, std::floor(a))) + lead_;
    const auto i1 = static_cast<size_t>(std::max(0.0, std::ceil(b))) + lead_;
    for (size_t i = i0; i <= i1 && i < shownA_.size(); ++i) {
      const float v = shownA_[i];
      pk = std::max(pk, std::fabs(v));
      if (!shownB_.empty()) pk = std::max(pk, std::fabs(shownB_[i]));
      if (cnt == 0) lo = hi = v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
      sum += v;
      sq += static_cast<double>(v) * v;
      if (cnt > 0 && ((prev < 0.0f) != (v < 0.0f))) ++zx;
      prev = v;
      ++cnt;
    }
    char lines[6][48];
    int n = 0;
    std::snprintf(lines[n++], 48, "%.3f ms  %.0f smp", dt * 1000.0, b - a);
    if (dt > 0.0) {
      char note[32];
      noteName(1.0 / dt, note, sizeof note);
      std::snprintf(lines[n++], 48, "%.2f Hz  %s", 1.0 / dt, note);
    }
    if (cnt > 0) {
      const double rms = std::sqrt(sq / static_cast<double>(cnt));
      const double pkDb = pk > 1e-9f ? 20.0 * std::log10(pk) : -120.0, rmsDb = rms > 1e-9 ? 20.0 * std::log10(rms) : -120.0;
      std::snprintf(lines[n++], 48, "peak %.1f  rms %.1f dB", pkDb, rmsDb);
      std::snprintf(lines[n++], 48, "crest %.1f dB  p-p %.3f", pkDb - rmsDb, static_cast<double>(hi - lo));
      std::snprintf(lines[n++], 48, "dc %+.4f  %d zero x", sum / static_cast<double>(cnt), zx);
    }
    const float lh = kLabelPx + 2.0f;
    const Rect card{kPlot.x0 + 3.0f, kChipY + kChipH + 3.0f, kPlot.x0 + 112.0f, kChipY + kChipH + 5.0f + lh * static_cast<float>(n)};
    p.roundedRect(card, 3.0f, kPad);
    for (int i = 0; i < n; ++i) {
      p.textAligned({card.x0 + 3.0f, card.y0 + 1.0f + lh * (static_cast<float>(i) + 0.5f)}, lines[i], kLabelPx,
                    ui::rgba(255, 255, 255, 210), 0.0f);
    }
  }

  // ---- the settings sheet (option_sheet.h: two tabs over the plot) ----
  void paintSheet(ui::FacePainter& p, const ui::Theme& th) const {
    Options saved;
    const bool isSaved = sheet::savedDefault(type().id, saved) && saved == opts_;
    panel_.paint(p, th, opts_, rows_, hovered_, isSaved);
  }

  static void outline(ui::FacePainter& p, Rgba col) {
    const Vec2 pts[5] = {{1.0f, 1.0f}, {kW - 1.0f, 1.0f}, {kW - 1.0f, kH - 1.0f}, {1.0f, kH - 1.0f}, {1.0f, 1.0f}};
    p.polyline(pts, 1.25f, col);
  }
  void chip(ui::FacePainter& p, const Rect& r, const char* label, bool on, const ui::Theme& th, Rgba onCol = 0) const {
    const Rgba c = onCol != 0 ? onCol : th.face.value;
    p.roundedRect(r, 3.0f, on ? ui::mulAlpha(c, 0.3f) : ui::rgba(8, 8, 12, 160));
    p.text({0.5f * (r.x0 + r.x1), 0.5f * (r.y0 + r.y1)}, label, 6.0f, on ? c : th.device.displayText);
  }

  bool hasTarget() const noexcept { return bound_; }

  devices::SpectrumFeed* feed_ = nullptr;
  bool acquired_ = false, settled_ = true;
  mutable FeedFrames frames_;
  std::function<devices::SpectrumFeed*()> feedOf_;
  std::function<void()> wakeHost_;
  double lastPaint_ = 0.0, probe_ = 0.0, rate_ = 0.0;
  bool bound_ = false;

  Options opts_;
  meta::DataPtr data_;
  bool optionsLoaded_ = false, local_ = false;
  std::vector<uint32_t> rows_;

  dsp::ScopeTracker tracker_;
  dsp::ScopeBeat beat_;
  dsp::ScopeFrame frame_;
  uint64_t lastEnd_ = 0, frameAbs_ = 0;
  uint32_t lead_ = 0;
  bool hasFrame_ = false, hasGain_ = false, frozen_ = false, sheet_ = false;
  sheet::Sheet panel_{{kPlot.x0 + 1.0f, kChipY + kChipH + 3.0f, kPlot.x1 - 1.0f, kPlot.y1 - 1.0f}, 6.0f};
  float gain_ = 1.0f;
  std::vector<float> bufL_, bufR_, bufA_, bufBeat_, bufNote_, shownA_, shownB_;
  std::vector<std::vector<Vec2>> trailsA_, trailsB_;

  double zoomStart_ = 0.0, zoomSpan_ = 1.0, pressZoom_ = 0.0;
  bool measure_ = false, moved_ = false;
  double mA_ = 0.0, mB_ = 0.0;
  Drag drag_ = Drag::None;
  uint32_t dragOpt_ = 0;
  bool editing_ = false;
  float pressX_ = 0.0f, pressY_ = 0.0f, pxPerUnit_ = 1.0f, hoverX_ = -1.0f;
  double pressTravel_ = 0.0;

  // Paint scratch.
  std::vector<Vec2> pts_, smooth_, up_, dn_;
  std::vector<float> colMn_, colMx_;
  std::vector<uint8_t> colOk_;
};

}  // namespace

std::unique_ptr<MetaFace> makeScopeFace(const meta::Type& t) { return std::make_unique<ScopeFace>(t); }

bool scopeShown(const MetaFace& f, ScopeShown& out) {
  const auto* s = dynamic_cast<const ScopeFace*>(&f);
  if (s == nullptr) return false;
  s->shown(out);
  return true;
}

bool scopeTestControl(MetaFace& f, std::string_view key, double value) {
  auto* s = dynamic_cast<ScopeFace*>(&f);
  if (s == nullptr) return false;
  if (key == "freeze") s->setFrozenForTest(value != 0.0);
  else if (key == "sheet") s->setSheetForTest(value != 0.0);
  else s->setOptionForTest(key, value);
  return true;
}

bool scopeTestMeasure(MetaFace& f, double a, double b) {
  auto* s = dynamic_cast<ScopeFace*>(&f);
  if (s == nullptr) return false;
  s->setMeasureForTest(a, b);
  return true;
}

}  // namespace pq::views
