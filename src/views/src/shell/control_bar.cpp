// The shell's control bar (T3.7): tempo, time signature, position, transport, loop, grid display.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "../common/paint.h"
#include "pq/document/commands.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/prefs/audio_prefs.h"
#include "shell_widgets.h"

namespace pq::views::shell {

using namespace pq::ui;

namespace {

constexpr Ticks kSixteenth = kTicksPerWhole / 16;
constexpr double kMinTempo = 20.0, kMaxTempo = 999.0;  // Live's tempo range
constexpr double kMaxLoopSixteenths = 16.0 * 4.0 * 4096.0;
constexpr int32_t kDenominators[] = {1, 2, 4, 8, 16};

int denominatorIndex(int32_t d) noexcept {
  for (int i = 0; i < 5; ++i)
    if (kDenominators[i] == d) return i;
  return 2;
}

// Splits "a.b.c" (spaces ignored) into up to three integers; false on anything else.
bool splitDotted(std::string_view s, int64_t out[3], int& count) {
  count = 0;
  std::string clean;
  for (char c : s)
    if (c != ' ') clean += c;
  std::string_view v = clean;
  while (!v.empty()) {
    if (count == 3) return false;
    const size_t dot = v.find('.');
    const std::string_view part = v.substr(0, dot);
    int64_t n = 0;
    const auto r = std::from_chars(part.data(), part.data() + part.size(), n);
    if (part.empty() || r.ec != std::errc{} || r.ptr != part.data() + part.size()) return false;
    out[count++] = n;
    if (dot == std::string_view::npos) break;
    v.remove_prefix(dot + 1);
    if (v.empty()) return false;
  }
  return count > 0;
}

}  // namespace

// --- formatting -----------------------------------------------------------------------------------------

std::string formatPosition(const TimeSigMap& sigs, Ticks t) {
  const RulerPosition p = rulerPosition(sigs, t);
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld. %d. %d", static_cast<long long>(p.bar), p.beat, p.sixteenth);
  return buf;
}

std::string formatDuration(const TimeSigMap& sigs, Ticks at, Ticks length) {
  const TimeSignature sig = sigs.signatureAt(at);
  const Ticks bar = ticksPerBar(sig), beat = ticksPerBeat(sig);
  const Ticks l = std::max<Ticks>(0, length);
  char buf[48];
  std::snprintf(buf, sizeof buf, "%lld. %lld. %lld", static_cast<long long>(l / bar), static_cast<long long>((l % bar) / beat),
                static_cast<long long>((l % beat) / kSixteenth));
  return buf;
}

std::optional<Ticks> parsePosition(const TimeSigMap& sigs, std::string_view text) {
  int64_t v[3] = {1, 1, 1};
  int n = 0;
  if (!splitDotted(text, v, n)) return std::nullopt;
  if (v[0] < 1 || (n > 1 && v[1] < 1) || (n > 2 && v[2] < 1)) return std::nullopt;
  const int64_t beat = n > 1 ? v[1] : 1, six = n > 2 ? v[2] : 1;
  return sigs.barBeatToTicks({v[0] - 1, beat - 1, (six - 1) * kSixteenth});
}

std::optional<Ticks> parseDuration(const TimeSigMap& sigs, Ticks at, std::string_view text) {
  int64_t v[3] = {0, 0, 0};
  int n = 0;
  if (!splitDotted(text, v, n)) return std::nullopt;
  for (int i = 0; i < n; ++i)
    if (v[i] < 0) return std::nullopt;
  const TimeSignature sig = sigs.signatureAt(at);
  return v[0] * ticksPerBar(sig) + (n > 1 ? v[1] : 0) * ticksPerBeat(sig) + (n > 2 ? v[2] : 0) * kSixteenth;
}

// --- TransportButton ------------------------------------------------------------------------------------

TransportButton::TransportButton(std::string name, Icon icon) : Control(std::move(name)), icon_(icon) {
  setFocusable(false);
  setCursor(ui::Cursor::Pointer);
}

void TransportButton::setOn(bool on) {
  if (on == on_) return;
  on_ = on;
  markDirty();
}

bool TransportButton::onPointer(const PointerEvent& e) {
  if (e.button != PointerButton::Left) return false;
  if (e.type == PointerType::Down) {
    setPressed(true);
    if (onPress) onPress();
    return true;
  }
  if (e.type == PointerType::Up) {
    setPressed(false);
    return true;
  }
  return false;
}

void TransportButton::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect();
  Rgba onCol = th->accent.primary;
  if (icon_ == Icon::Record) onCol = th->accent.arm;
  // On the window-coloured control bar the switches are panel-coloured (Live's transport; Q107.1 option B).
  Rgba fill = v.disabled ? th->surface.panel : on_ ? onCol : th->surface.panel;
  if (!v.disabled) {
    if (v.pressed) fill = on_ ? paint::pressOf(fill) : th->control.fill;
    else if (v.hover) fill = on_ ? paint::hoverOf(fill) : th->surface.raised;
  }
  paint::box(ctx.dl, r, ctx.scale.exact(th->radius.control), fill, th->control.border);
  Rgba ink = v.disabled ? th->text.disabled : on_ ? th->text.onAccent : th->text.primary;
  if (icon_ == Icon::Record && !on_ && !v.disabled) ink = th->accent.arm;
  const float cx = static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f;
  const float cy = static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f;
  const float s = std::floor(std::min(static_cast<float>(r.w), static_cast<float>(r.h)) * 0.5f);  // icon box, device px
  switch (icon_) {
    case Icon::Play: {
      const float h = s * 0.5f, w = s * 0.86f;
      ctx.dl.addTriangleFilled({cx - w * 0.42f, cy - h}, {cx + w * 0.58f, cy}, {cx - w * 0.42f, cy + h}, ink);
      break;
    }
    case Icon::Stop: {
      const float h = std::round(s * 0.42f);
      ctx.dl.addRectFilled({std::round(cx - h), std::round(cy - h), std::round(cx - h) + 2 * h, std::round(cy - h) + 2 * h}, ink);
      break;
    }
    case Icon::Record: ctx.dl.addCircleFilled({cx, cy}, s * 0.45f, ink); break;
    case Icon::Loop: {
      // A loop arrow: an open ring with an arrowhead, like Live's loop switch.
      const float rad = s * 0.42f, t = std::max(1.0f, ctx.scale.exact(1.5f));
      constexpr float kPi = 3.14159265f;
      const float a0 = -0.35f * kPi, a1 = 1.45f * kPi;
      ctx.dl.addArcStroke({cx, cy}, rad, a0, a1, ink, t);
      const float ex = cx + rad * std::cos(a0), ey = cy + rad * std::sin(a0);
      const float hs = std::max(2.5f, s * 0.22f);
      ctx.dl.addTriangleFilled({ex - hs, ey - hs * 0.9f}, {ex + hs * 0.9f, ey + hs * 0.25f}, {ex - hs * 0.6f, ey + hs}, ink);
      break;
    }
  }
}

// --- PositionDisplay ------------------------------------------------------------------------------------

PositionDisplay::PositionDisplay(std::string name) : Control(std::move(name)) {}

void PositionDisplay::setText(std::string text) {
  if (text == text_) return;
  text_ = std::move(text);
  markDirty();
}

void PositionDisplay::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  paint::box(ctx.dl, r, ctx.scale.exact(th->radius.control), th->surface.panel, th->field.border);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, f->numeric, r, text_, th->text.primary, TextAlign::Center, ctx.scale.px(4), ctx.scale.px(4));
}

// --- CpuMeter -------------------------------------------------------------------------------------------

CpuMeter::CpuMeter(ViewContext& ctx) : Control("cpu"), ctx_(ctx) {
  setFocusable(false);
  setInfo("CPU Load Meter", "How much of the audio callback's time the engine uses while it plays (the share of each "
                            "audio buffer spent computing it). It updates twice a second during playback.");
}

void CpuMeter::poll() {
  AudioPrefsPort* port = ctx_.audioPrefs();
  std::string t = "CPU \xE2\x80\x93";
  if (port) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "CPU %.0f %%", std::clamp(port->cpuLoad(), 0.0, 999.0));
    t = buf;
  }
  if (t != text_) {
    text_ = std::move(t);
    markDirty();
  }
}

void CpuMeter::transportChanged() {
  const bool playing = ctx_.transport().playhead().playing;
  if (playing && timer_ == 0) {
    poll();
    timer_ = requestTimer(0.5);
  } else if (!playing && timer_ != 0) {
    cancelTimer(timer_);
    timer_ = 0;
    poll();  // the last reading
  }
}

void CpuMeter::onTimer(uint32_t id) {
  if (id != timer_) return;
  timer_ = 0;
  poll();
  if (ctx_.transport().playhead().playing) timer_ = requestTimer(0.5);
}

void CpuMeter::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  paint::box(ctx.dl, r, ctx.scale.exact(th->radius.control), th->surface.panel, th->field.border);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, f->numeric, r, text_, th->text.primary, TextAlign::Center, ctx.scale.px(4), ctx.scale.px(4));
}

// --- ControlBar -----------------------------------------------------------------------------------------

namespace {

Widget& gap(Widget& row, float w) {
  auto& g = row.add<Widget>();
  g.setWidth(Length::fixed(w));
  g.setHittable(false);
  return g;
}

Widget& spring(Widget& row) {
  auto& g = row.add<Widget>();
  g.setWidth(Length::flex(1));
  g.setHittable(false);
  return g;
}

template <class W>
W& sized(W& w, float width, float height = 20) {
  w.setSize(Length::fixed(width), Length::fixed(height));
  return w;
}

}  // namespace

template <class MakeCommand>
void ControlBar::bindGesture(ValueControl& c, const char* label, MakeCommand make) {
  c.onGestureBegin = [this, label] { (void)ctx_.store().beginGesture(label); };
  c.onChange = [this, make](double v) {
    if (Status s = ctx_.store().dispatch(make(v)); !s) ctx_.setStatus(s.error().message);
  };
  c.onGestureEnd = [this] {
    if (ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
    refreshDocument();  // a value the document refused (clamped) snaps back
  };
  c.onGestureCancel = [this] {  // Esc / window focus lost: nothing recorded
    if (ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
    refreshDocument();
  };
}

ControlBar::ControlBar(ViewContext& ctx) : Widget("control-bar"), ctx_(ctx) {
  setLayout(LayoutKind::Row);
  setHeight(Length::fixed(32));
  setPadding({8, 6, 8, 6});
  setGap(4);
  setAlign(Align::Start, Align::Center);

  // Tempo and time signature (left, as in Live).
  ValueSpec tempo;
  tempo.min = kMinTempo;
  tempo.max = kMaxTempo;
  tempo.def = 120.0;
  tempo.step = 0.01;
  tempo.decimals = 2;
  tempo.dragPixels = kMaxTempo - kMinTempo;  // 1 BPM per logical px, Shift: 0.1
  tempo.keyStep = 1.0;
  tempo_ = &sized(add<DragBox>("tempo", tempo), 58);
  tempo_->setStyle(DragBox::Style::OnWindow);
  tempo_->setInfo("Song Tempo", "The tempo of the song in beats per minute. Drag up or down to change it (Shift: fine), "
                                "double-click to type a value, Delete returns it to 120.");
  bindGesture(*tempo_, "Change Tempo", [this](double v) -> doc::Command {
    std::vector<TempoPoint> pts = ctx_.doc()->timeline->tempo;
    pts.front().bpm = v;
    return doc::SetTempoMap{std::move(pts)};
  });
  gap(*this, 4);

  ValueSpec num;
  num.min = 1;
  num.max = 99;
  num.def = 4;
  num.step = 1;
  num.decimals = 0;
  num.dragPixels = 98 * 6;
  sigNum_ = &sized(add<DragBox>("sig-numerator", num), 24);
  sigNum_->setStyle(DragBox::Style::OnWindow);
  sigNum_->setInfo("Time Signature Numerator", "Beats per bar of the song's time signature (its first signature).");
  auto& slash = sized(add<Label>("sig-slash", "/", FontRoleId::Small, TextTone::Secondary, TextAlign::Center), 8);
  slash.setHittable(false);
  ValueSpec den;
  den.min = 0;
  den.max = 4;
  den.def = 2;
  den.step = 1;
  den.decimals = 0;
  den.dragPixels = 4 * 16;
  sigDen_ = &sized(add<DragBox>("sig-denominator", den), 24);
  sigDen_->setStyle(DragBox::Style::OnWindow);
  sigDen_->formatter = [](double v) { return std::to_string(kDenominators[std::clamp(static_cast<int>(v), 0, 4)]); };
  sigDen_->parser = [](std::string_view t) -> std::optional<double> {
    int d = 0;
    const auto r = std::from_chars(t.data(), t.data() + t.size(), d);
    if (r.ec != std::errc{} || r.ptr != t.data() + t.size()) return std::nullopt;
    for (int i = 0; i < 5; ++i)
      if (kDenominators[i] == d) return static_cast<double>(i);
    return std::nullopt;
  };
  sigDen_->setSpec(sigDen_->spec());  // format with the formatter
  sigDen_->setInfo("Time Signature Denominator", "The note value of one beat: 1, 2, 4, 8 or 16.");
  auto setSig = [this](int32_t numerator, int32_t denominator) -> doc::Command {
    std::vector<TimeSigChange> ch = ctx_.doc()->timeline->signatures;
    ch.front().sig = {numerator, denominator};
    return doc::SetTimeSignatures{std::move(ch)};
  };
  bindGesture(*sigNum_, "Change Time Signature", [this, setSig](double v) {
    return setSig(static_cast<int32_t>(v), ctx_.doc()->timeline->signatures.front().sig.denominator);
  });
  bindGesture(*sigDen_, "Change Time Signature", [this, setSig](double v) {
    return setSig(ctx_.doc()->timeline->signatures.front().sig.numerator, kDenominators[std::clamp(static_cast<int>(v), 0, 4)]);
  });

  spring(*this);

  // Position and transport (centre).
  position_ = &sized(add<PositionDisplay>("position"), 74);
  position_->setInfo("Arrangement Position", "The playback position in bars, beats and sixteenths. While stopped it "
                                             "shows the start marker, where Space starts playback.");
  play_ = &sized(add<TransportButton>("play", TransportButton::Icon::Play), 28);
  play_->setInfo("Play", "Starts playback from the start marker (Space). Shift+Space continues from where playback stopped.");
  play_->onPress = [this] {
    if (!ctx_.playing()) ctx_.togglePlay();
  };
  stop_ = &sized(add<TransportButton>("stop", TransportButton::Icon::Stop), 28);
  stop_->setInfo("Stop", "Stops playback (Space). Press again while stopped to return the start marker to the beginning.");
  stop_->onPress = [this] { ctx_.stopButton(); };
  record_ = &sized(add<TransportButton>("record", TransportButton::Icon::Record), 28);
  record_->setInfo("Arrangement Record", "Recording arrives with milestone M6.");
  record_->setEnabled(false);
  gap(*this, 10);

  // Loop switch, loop start and length.
  loop_ = &sized(add<TransportButton>("loop", TransportButton::Icon::Loop), 28);
  loop_->setInfo("Loop Switch", "Turns the arrangement loop on or off. While it is on, playback repeats the loop brace.");
  loop_->onPress = [this] {
    doc::LoopRegion l = ctx_.doc()->loop;
    l.enabled = !l.enabled;
    if (Status s = ctx_.store().dispatch(doc::SetLoop{l}); !s) ctx_.setStatus(s.error().message);
  };
  ValueSpec ls;
  ls.min = 0;
  ls.max = kMaxLoopSixteenths;
  ls.step = 1;
  ls.decimals = 0;
  ls.dragPixels = kMaxLoopSixteenths;  // one sixteenth per logical px
  ls.keyStep = 4;
  loopStart_ = &sized(add<DragBox>("loop-start", ls), 66);
  loopStart_->setStyle(DragBox::Style::OnWindow);
  loopStart_->setInfo("Loop Start", "Where the arrangement loop begins (bar. beat. sixteenth). Drag or type to move the "
                                    "loop; its length stays.");
  loopStart_->formatter = [this](double v) {
    return formatPosition(ctx_.doc()->timeline->sigMap, static_cast<Ticks>(std::llround(v)) * kSixteenth);
  };
  loopStart_->parser = [this](std::string_view t) -> std::optional<double> {
    const auto ticks = parsePosition(ctx_.doc()->timeline->sigMap, t);
    if (!ticks) return std::nullopt;
    return static_cast<double>(*ticks / kSixteenth);
  };
  bindGesture(*loopStart_, "Move Loop", [this](double v) -> doc::Command {
    doc::LoopRegion l = ctx_.doc()->loop;
    const Ticks len = l.end - l.start;
    l.start = static_cast<Ticks>(std::llround(v)) * kSixteenth;
    l.end = l.start + len;
    return doc::SetLoop{l};
  });
  ValueSpec ll = ls;
  ll.min = 1;
  loopLength_ = &sized(add<DragBox>("loop-length", ll), 66);
  loopLength_->setStyle(DragBox::Style::OnWindow);
  loopLength_->setInfo("Loop Length", "The length of the arrangement loop (bars. beats. sixteenths).");
  loopLength_->formatter = [this](double v) {
    const doc::Project& p = *ctx_.doc();
    return formatDuration(p.timeline->sigMap, p.loop.start, static_cast<Ticks>(std::llround(v)) * kSixteenth);
  };
  loopLength_->parser = [this](std::string_view t) -> std::optional<double> {
    const doc::Project& p = *ctx_.doc();
    const auto ticks = parseDuration(p.timeline->sigMap, p.loop.start, t);
    if (!ticks || *ticks < kSixteenth) return std::nullopt;
    return static_cast<double>(*ticks / kSixteenth);
  };
  bindGesture(*loopLength_, "Change Loop Length", [this](double v) -> doc::Command {
    doc::LoopRegion l = ctx_.doc()->loop;
    l.end = l.start + static_cast<Ticks>(std::llround(v)) * kSixteenth;
    return doc::SetLoop{l};
  });

  spring(*this);

  // Right: the selected track's tuning (tunings are first-class: a chip like the header's), the CPU meter and the grid.
  auto& chip = sized(add<TuningChip>("tuning", ctx_), 132, 17);
  chip.setInfo("Tuning of the Selected Track", "The tuning the selected track plays in (with no track selected: the "
                                               "project's first main tuning). Click to open the MOS wheel for it.");
  tuningChip_ = &chip;
  gap(*this, 6);
  cpu_ = &sized(add<CpuMeter>(ctx_), 64, 17);
  gap(*this, 6);
  grid_ = &sized(add<Label>("grid", "", FontRoleId::Small, TextTone::Secondary, TextAlign::Right), 96);
  grid_->setInfo("Grid", "The editing grid of the arrangement and the clip view. Ctrl+1 / Ctrl+2 make it narrower / "
                         "wider, Ctrl+3 triplet, Ctrl+4 snap, Ctrl+5 fixed or adaptive; right-click an editor for "
                         "the grid menu.");

  docSub_ = ctx_.subscribe([this](const doc::StoreEvent&) { refreshDocument(); });
  viewSub_ = ctx_.observe([this](uint32_t what) {
    if (what & view_change::kTransport) {
      refreshTransport();
      cpu_->transportChanged();
    }
    if (what & view_change::kGrid) grid_->setText(ctx_.gridLabel());
    if (what & (view_change::kSelection | view_change::kProject)) refreshTuningChip();
  });
  refreshDocument();
  refreshTransport();
  refreshTuningChip();
  grid_->setText(ctx_.gridLabel());
}

void ControlBar::refreshTuningChip() {
  auto& chip = static_cast<TuningChip&>(*tuningChip_);
  const doc::Project& p = *ctx_.doc();
  const doc::TrackId t = ctx_.selectedTrack();
  if (t.valid() && doc::findTrackPtr(p, t)) chip.setTarget(TuningTarget::forTrack(t));
  else if (p.tunings && !p.tunings->empty()) chip.setTarget(TuningTarget::forMain(p.tunings->front()->id));
  else chip.setTarget(std::nullopt);
}

ControlBar::~ControlBar() = default;

void ControlBar::refreshDocument() {
  const doc::Project& p = *ctx_.doc();
  if (const doc::TrackId t = ctx_.selectedTrack(); t.valid() && !doc::findTrackPtr(p, t)) refreshTuningChip();  // deleted
  const bool timelineChanged = p.timeline.get() != timeline_;
  if (timelineChanged) {
    timeline_ = p.timeline.get();
    if (!tempo_->inGesture()) tempo_->setValue(p.timeline->tempo.front().bpm);
    const TimeSignature sig = p.timeline->signatures.front().sig;
    if (!sigNum_->inGesture()) sigNum_->setValue(sig.numerator);
    if (!sigDen_->inGesture()) sigDen_->setValue(denominatorIndex(sig.denominator));
  }
  if (timelineChanged || !loopShownValid_ || !(p.loop == loopShown_)) {
    loopShown_ = p.loop;
    loopShownValid_ = true;
    loop_->setOn(p.loop.enabled);
    if (!loopStart_->inGesture()) {
      loopStart_->setValue(static_cast<double>(p.loop.start / kSixteenth));
      loopStart_->setSpec(loopStart_->spec());  // re-format: the signature may have changed
    }
    if (!loopLength_->inGesture()) {
      loopLength_->setValue(static_cast<double>((p.loop.end - p.loop.start) / kSixteenth));
      loopLength_->setSpec(loopLength_->spec());
    }
    if (!ctx_.playing()) position_->setText(formatPosition(p.timeline->sigMap, ctx_.startMarker()));
  }
}

void ControlBar::refreshTransport() {
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  play_->setOn(ph.playing);
  const Ticks t = ph.playing ? ph.tick : ctx_.startMarker();
  position_->setText(formatPosition(ctx_.doc()->timeline->sigMap, t));
  if (ph.playing && timer_ == 0) schedulePositionTimer(ph.tick);
  if (!ph.playing && timer_ != 0) {
    cancelTimer(timer_);
    timer_ = 0;
  }
}

// While playing, the position display changes once per sixteenth: wake then (bounded to 4–100 ms so tempo changes
// and engine report jitter are picked up), never per frame.
void ControlBar::schedulePositionTimer(Ticks tick) {
  const TempoMap& tm = ctx_.doc()->timeline->tempoMap;
  const Ticks next = (tick >= 0 ? tick / kSixteenth + 1 : -((-tick) / kSixteenth)) * kSixteenth;
  const double dt = tm.ticksToSeconds(next) - tm.ticksToSeconds(tick);
  timer_ = requestTimer(std::clamp(dt + 0.0005, 0.004, 0.1));
}

void ControlBar::onTimer(uint32_t id) {
  if (id != timer_) return;
  timer_ = 0;
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  if (!ph.playing) {
    refreshTransport();
    return;
  }
  position_->setText(formatPosition(ctx_.doc()->timeline->sigMap, ph.tick));
  schedulePositionTimer(ph.tick);
}

void ControlBar::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  // Live's transport bar is the window colour with panel-coloured fields (Q107.1 option B, T3.x-fix-look).
  ctx.dl.addRectFilled(r.toRect(), th->surface.background);
}

}  // namespace pq::views::shell
