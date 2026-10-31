// The audio Clip View (T5.6): see pq/views/audio_clip/audio_clip_view.h.

#include "pq/views/audio_clip/audio_clip_view.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "../common/paint.h"
#include "../common/waveform_data.h"
#include "pq/document/audio_clip.h"
#include "pq/document/commands.h"
#include "pq/media/probe.h"
#include "pq/ui/theme.h"
#include "pq/ui/waveform.h"
#include "pq/views/waveforms.h"

namespace pq::views {

using namespace pq::ui;

namespace {

constexpr float kBoxWidth = 352.0f;   // logical px: two property columns (M5 review UI 2)
constexpr float kRulerH = 16.0f;
constexpr float kGrab = 5.0f;          // logical px around the marker / brace ends

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool parseNumber(std::string_view s, double& out) {
  s = trim(s);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc{} && p == s.data() + s.size();
}

std::filesystem::path utf8Path(std::string_view s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace

std::optional<doc::AudioTranspose> parseTranspose(std::string_view text) {
  std::string_view s = trim(text);
  if (s.empty()) return std::nullopt;
  doc::AudioTranspose t;
  auto endsWith = [&](std::string_view suf) { return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf; };
  double v = 0.0;
  if (endsWith("\xC2\xA2") || endsWith("c")) {  // cents
    const std::string_view num = endsWith("c") ? s.substr(0, s.size() - 1) : s.substr(0, s.size() - 2);
    if (!parseNumber(num, v)) return std::nullopt;
    t.unit = doc::TransposeUnit::Cents;
    t.cents = v;
    return t;
  }
  if (endsWith("st")) {  // semitones
    if (!parseNumber(s.substr(0, s.size() - 2), v)) return std::nullopt;
    t.unit = doc::TransposeUnit::Cents;
    t.cents = 100.0 * v;
    return t;
  }
  if (const size_t slash = s.find('/'); slash != std::string_view::npos) {
    int64_t n = 0, d = 0;
    const std::string_view a = trim(s.substr(0, slash)), b = trim(s.substr(slash + 1));
    const auto ra = std::from_chars(a.data(), a.data() + a.size(), n);
    const auto rb = std::from_chars(b.data(), b.data() + b.size(), d);
    if (ra.ec != std::errc{} || rb.ec != std::errc{} || ra.ptr != a.data() + a.size() || rb.ptr != b.data() + b.size() || n <= 0 ||
        d <= 0)
      return std::nullopt;
    t.unit = doc::TransposeUnit::Ratio;
    t.ratio = tuning::Rational(n, d);
    return t;
  }
  int64_t steps = 0;
  std::string_view num = s;
  if (!num.empty() && num.front() == '+') num.remove_prefix(1);
  const auto [p, ec] = std::from_chars(num.data(), num.data() + num.size(), steps);
  if (ec != std::errc{} || p != num.data() + num.size()) return std::nullopt;
  t.unit = doc::TransposeUnit::Steps;
  t.steps = static_cast<int32_t>(std::clamp<int64_t>(steps, -doc::kMaxTransposeSteps, doc::kMaxTransposeSteps));
  return t;
}

std::string formatTranspose(const doc::AudioTranspose& t) {
  char buf[48];
  switch (t.unit) {
    case doc::TransposeUnit::Steps: std::snprintf(buf, sizeof buf, "%d", t.steps); break;
    case doc::TransposeUnit::Cents: std::snprintf(buf, sizeof buf, "%gc", t.cents); break;
    case doc::TransposeUnit::Ratio:
      std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(t.ratio.num()), static_cast<long long>(t.ratio.den()));
      break;
  }
  return buf;
}

double sourceBpmFor(double fileBpm, double durationSeconds) {
  if (fileBpm >= kMinBpm && fileBpm <= kMaxBpm) return fileBpm;
  if (!(durationSeconds > 0.0)) return 120.0;
  // Bars of 4/4 at 120 BPM (2 s each), rounded to a whole number (at least one); the tempo that makes it exact.
  const double bars = std::max(1.0, std::round(durationSeconds / 2.0));
  return std::clamp(bars * 4.0 * 60.0 / durationSeconds, kMinBpm, kMaxBpm);
}

// ---- the sample display ------------------------------------------------------------------------------------------

namespace audio_clip {

class SampleDisplay : public Control {
public:
  explicit SampleDisplay(AudioClipView& v) : Control("sample"), v_(v) {
    setFocusable(true);  // a press focuses it, so Esc reaches the view during a drag (M5 review M8)
    setInfo("Sample", "The whole sample, the part this clip plays lit. Drag the start marker in the strip above it to "
                      "choose where the clip starts in the file; in Re-Pitch, drag the loop brace or its ends (1/16 notes; "
                      "Shift: free). Each drag is one undo step.");
  }
  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  void onCaptureLost() override { cancel(); }
  Cursor cursorAt(Vec2 p) const override {
    if (drag_ != Drag::None) return drag_ == Drag::Brace ? Cursor::Move : Cursor::ResizeEW;
    const Drag d = hit(p);
    return d == Drag::None ? Cursor::Arrow : d == Drag::Brace ? Cursor::Move : Cursor::ResizeEW;
  }
  bool dragging() const noexcept { return drag_ != Drag::None; }
  bool cancel() {
    if (drag_ == Drag::None) return false;
    drag_ = Drag::None;
    if (v_.ctx_.store().inGesture()) (void)v_.ctx_.store().cancelGesture();
    markDirty();
    return true;
  }

  // Geometry (device px), from the last layout.
  IRect ruler() const noexcept {
    const IRect r = rect();
    const int32_t h = tree() ? tree()->uiScale().px(kRulerH) : static_cast<int32_t>(kRulerH);
    return {r.x, r.y, r.w, std::min(h, r.h)};
  }
  IRect wave() const noexcept {
    const IRect r = rect(), u = ruler();
    return {r.x, u.bottom(), r.w, std::max(0, r.bottom() - u.bottom())};
  }
  double frames() const noexcept {
    const doc::SampleRef* s = sample();
    return s ? static_cast<double>(std::max<int64_t>(1, s->frames)) : 1.0;
  }
  float xOf(double frame) const noexcept {
    const IRect w = wave();
    return static_cast<float>(w.x) + static_cast<float>(frame / frames() * static_cast<double>(w.w));
  }
  double frameAt(float x) const noexcept {
    const IRect w = wave();
    return w.w > 0 ? (static_cast<double>(x) - w.x) / static_cast<double>(w.w) * frames() : 0.0;
  }
  const doc::SampleRef* sample() const noexcept {
    return v_.clip_ && v_.snap_ ? doc::findSample(*v_.snap_, v_.clip_->sample) : nullptr;
  }
  double fpt() const noexcept {
    const doc::SampleRef* s = sample();
    return s && v_.clip_ ? doc::framesPerContentTick(s->rate, v_.clip_->sourceBpm) : 0.0;
  }
  // The source window the clip plays: [a, b) frames.
  void region(double& a, double& b) const noexcept {
    const doc::AudioClip& c = *v_.clip_;
    const doc::SampleRef* s = sample();
    if (!s) {
      a = b = 0;
      return;
    }
    if (c.warp == doc::AudioWarp::Off) {
      a = c.contentStartFrame;
      b = a + c.seconds * s->rate * speedRatio();
    } else {
      a = static_cast<double>(c.contentStart) * fpt();
      const Ticks end = contentLooped(doc::contentLoop(c)) ? c.loop.end : c.contentStart + c.length;
      b = static_cast<double>(end) * fpt();
    }
  }
  double speedRatio() const noexcept {
    if (!v_.clip_ || !v_.snap_) return 1.0;
    const std::optional<doc::TrackPath> path = doc::findTrack(*v_.snap_, v_.ref_.track);
    return path ? doc::audioRateRatio(*v_.snap_, *path, *v_.clip_) : 1.0;
  }
  double markerFrame() const noexcept {
    if (!v_.clip_) return 0.0;
    const doc::AudioClip& c = *v_.clip_;
    return c.warp == doc::AudioWarp::Off ? c.contentStartFrame : static_cast<double>(c.contentStart) * fpt();
  }

private:
  enum class Drag : uint8_t { None, Marker, LoopStart, LoopEnd, Brace };
  Drag hit(Vec2 p) const {
    if (!v_.clip_ || !sample()) return Drag::None;
    const IRect u = ruler();
    const float g = static_cast<float>(tree() ? tree()->uiScale().px(kGrab) : static_cast<int32_t>(kGrab));
    if (p.y < static_cast<float>(u.y) || p.y >= static_cast<float>(rect().bottom())) return Drag::None;
    const doc::AudioClip& c = *v_.clip_;
    if (c.warp == doc::AudioWarp::RePitch && c.loop.enabled && p.y < static_cast<float>(u.bottom())) {
      const float xa = xOf(static_cast<double>(c.loop.start) * fpt()), xb = xOf(static_cast<double>(c.loop.end) * fpt());
      if (std::abs(p.x - xa) <= g) return Drag::LoopStart;
      if (std::abs(p.x - xb) <= g) return Drag::LoopEnd;
      if (p.x > xa && p.x < xb && std::abs(p.x - xOf(markerFrame())) > g) return Drag::Brace;
    }
    if (std::abs(p.x - xOf(markerFrame())) <= g) return Drag::Marker;
    return Drag::None;
  }
  void dragTo(const PointerEvent& e);

  AudioClipView& v_;
  Drag drag_ = Drag::None;
  float pressX_ = 0.0f;
  doc::AudioClipPtr pressClip_;
};

void SampleDisplay::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  DrawList& dl = ctx.dl;
  const IRect r = rect();
  dl.addRectFilled(r.toRect(), th->surface.sunken);
  const IRect u = ruler(), w = wave();
  dl.addRectFilled(u.toRect(), th->surface.panel);
  if (!v_.clip_) return;
  const doc::AudioClip& c = *v_.clip_;
  const doc::SampleRef* s = sample();
  const Rgba colour = hex(c.colour.rgb);
  const FontId font = ctx.res.uiFonts ? ctx.res.uiFonts->small : kNoFont;
  if (!s || !v_.ctx_.sampleAvailable(c.sample)) {
    dl.addRectFilled(w.toRect(), rgba(200, 40, 35, 90));
    paint::text(ctx, font, w, "Sample missing: " + (s ? s->path : std::string("?")), th->text.primary, TextAlign::Center, 8, 8);
    return;
  }
  // The region the clip plays, lit in the clip colour; the rest of the file dimmed.
  double a = 0, b = 0;
  region(a, b);
  const float xa = std::clamp(std::round(xOf(a)), static_cast<float>(w.x), static_cast<float>(w.right()));
  const float xb = std::clamp(std::round(xOf(b)), static_cast<float>(w.x), static_cast<float>(w.right()));
  if (xb > xa) dl.addRectFilled({xa, static_cast<float>(w.y), xb, static_cast<float>(w.bottom())}, lerp(colour, rgba(30, 30, 30), 0.30f));
  if (const media::WaveformOverview* ov = v_.ctx_.waveforms().overview(c.sample)) {
    WaveformLevel lv[kMaxOverviewLevels];
    WaveformData data = waveformData(*ov, nullptr, lv);
    WaveformDraw d;
    d.frame0 = 0.0;
    d.frame1 = frames();
    d.rect = w.toRect();
    // M5 review UI 2: the sample at unity (the clip gain scaled +6 dB into the box edges and looked clipped); the gain
    // is a readout in the corner instead.
    d.gain = 1.0f;
    d.colour = lerp(colour, rgba(0, 0, 0), 0.72f);
    if (waveformLevelFor(data, frames() / std::max(1, w.w)) < 0)
      if (const media::SampleWindow* sw = v_.ctx_.waveforms().window(c.sample, 0, static_cast<int64_t>(frames())))
        data = waveformData(*ov, sw, lv);
    (void)drawWaveform(dl, data, d);
  } else {
    paint::text(ctx, font, w, "Loading waveform\xE2\x80\xA6", th->text.secondary, TextAlign::Center, 8, 8);
  }
  // Region edges.
  const Rgba edge = withAlpha(th->text.primary, 150);
  if (xb > xa) {
    dl.addRectFilled({xa, static_cast<float>(w.y), xa + 1.0f, static_cast<float>(w.bottom())}, edge);
    dl.addRectFilled({xb - 1.0f, static_cast<float>(w.y), xb, static_cast<float>(w.bottom())}, edge);
  }
  // Re-Pitch: the loop brace in the strip.
  if (c.warp == doc::AudioWarp::RePitch) {
    const float la = std::round(xOf(static_cast<double>(c.loop.start) * fpt())), lb = std::round(xOf(static_cast<double>(c.loop.end) * fpt()));
    const Rgba braceCol = c.loop.enabled ? th->accent.primary : withAlpha(th->text.disabled, 160);
    const float by0 = static_cast<float>(u.y + u.h / 4), by1 = static_cast<float>(u.bottom() - u.h / 4);
    if (lb > la) {
      dl.addRectFilled({la, by0, lb, by1}, withAlpha(braceCol, c.loop.enabled ? 200 : 110));
      dl.addRectFilled({la, by0, la + 2.0f, static_cast<float>(u.bottom())}, braceCol);
      dl.addRectFilled({lb - 2.0f, by0, lb, static_cast<float>(u.bottom())}, braceCol);
    }
  }
  // The clip gain as a readout (the waveform is drawn at unity).
  if (c.gainDb != 0.0f && font != kNoFont) {
    char g[32];
    if (c.gainDb <= doc::kMinClipGainDb) std::snprintf(g, sizeof g, "Gain -inf dB");
    else std::snprintf(g, sizeof g, "Gain %+.1f dB", static_cast<double>(c.gainDb));
    paint::text(ctx, font, {w.x, w.y + ctx.scale.px(4.0f), w.w, ctx.scale.px(16.0f)}, g, th->text.primary, TextAlign::Right,
                ctx.scale.px(8.0f), ctx.scale.px(8.0f));
  }
  // The start marker (M5 review UI 2: a 1-px line was indistinguishable from the region's edge): a flag in the accent
  // colour filling the strip, and a 2-px line over the waveform.
  const float mx = std::round(xOf(markerFrame()));
  const float fh = static_cast<float>(u.h);
  const float sz = std::max(static_cast<float>(ctx.scale.px(9.0f)), fh * 0.75f);
  const float lw = static_cast<float>(std::max(2, ctx.scale.px(2.0f)));
  const Rgba flag = th->accent.primary;
  dl.addTriangleFilled({mx, static_cast<float>(u.y)}, {mx + sz, static_cast<float>(u.y) + 0.5f * fh}, {mx, static_cast<float>(u.bottom())}, flag);
  dl.addRectFilled({mx, static_cast<float>(u.y), mx + lw, static_cast<float>(w.bottom())}, flag);
}

bool SampleDisplay::onPointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Down: {
      if (e.button != PointerButton::Left) return false;
      const Drag d = hit(e.pos);
      if (d == Drag::None) return true;
      drag_ = d;
      pressX_ = e.pos.x;
      pressClip_ = v_.clip_;
      (void)v_.ctx_.store().beginGesture(d == Drag::Marker ? "Clip Start" : "Clip Loop");
      return true;
    }
    case PointerType::Move:
      if (drag_ != Drag::None) dragTo(e);
      return drag_ != Drag::None;
    case PointerType::Up:
      if (drag_ == Drag::None) return false;
      drag_ = Drag::None;
      if (v_.ctx_.store().inGesture()) (void)v_.ctx_.store().commitGesture();
      return true;
    default: return false;
  }
}

void SampleDisplay::dragTo(const PointerEvent& e) {
  if (!pressClip_ || !v_.snap_) return;
  const doc::AudioClip& c0 = *pressClip_;
  const doc::SampleRef* s = sample();
  if (!s) return;
  const double df = frameAt(e.pos.x) - frameAt(pressX_);
  const bool free = (e.mods & mod::kShift) != 0;
  const Ticks grid = kPpq / 4;  // 1/16 note of content
  auto snapTicks = [&](double t) {
    const auto v = static_cast<Ticks>(std::llround(t));
    return free ? v : (v + (v >= 0 ? grid / 2 : -grid / 2)) / grid * grid;
  };
  const doc::Snapshot snap = v_.ctx_.doc();
  if (drag_ == Drag::Marker) {
    doc::AudioClip n = *v_.clip_;
    if (c0.warp == doc::AudioWarp::Off) {
      const double span = c0.seconds * s->rate * speedRatio();
      n.contentStartFrame = std::clamp(c0.contentStartFrame + df, 0.0, std::max(0.0, static_cast<double>(s->frames) - span));
      if (n.contentStartFrame == v_.clip_->contentStartFrame) return;
    } else {
      const double f = fpt();
      if (!(f > 0.0)) return;
      const auto fileTicks = static_cast<Ticks>(std::floor(static_cast<double>(s->frames) / f));
      n.contentStart = std::clamp<Ticks>(snapTicks(static_cast<double>(c0.contentStart) + df / f), 0, std::max<Ticks>(0, fileTicks - 1));
      if (n.contentStart == v_.clip_->contentStart) return;
    }
    doc::Batch b;
    b.label = "Clip Start";
    b.commands.push_back(doc::SetAudioClip{v_.ref_, std::make_shared<const doc::AudioClip>(std::move(n))});
    if (Status st = v_.ctx_.store().dispatch(doc::Command(std::move(b))); !st) v_.ctx_.setStatus("Clip Start: " + st.error().message);
    return;
  }
  const double f = fpt();
  if (!(f > 0.0)) return;
  const auto dt = static_cast<Ticks>(std::llround(df / f));
  doc::ClipLoop loop = c0.loop;
  const Ticks minLen = free ? 1 : grid;
  // L12: bounds ordered (a Shift-made loop can be shorter than minLen), and nothing past the file's end.
  const auto fileTicks = std::max<Ticks>(minLen, static_cast<Ticks>(std::floor(static_cast<double>(s->frames) / f)));
  if (drag_ == Drag::LoopStart) {
    loop.start = std::clamp<Ticks>(snapTicks(static_cast<double>(c0.loop.start + dt)), 0, std::max<Ticks>(0, loop.end - minLen));
  } else if (drag_ == Drag::LoopEnd) {
    loop.end = std::clamp<Ticks>(snapTicks(static_cast<double>(c0.loop.end + dt)), loop.start + minLen,
                                 std::max(fileTicks, loop.start + minLen));
  } else {
    const Ticks len = c0.loop.end - c0.loop.start;
    loop.start = std::clamp<Ticks>(snapTicks(static_cast<double>(c0.loop.start + dt)), 0, std::max<Ticks>(0, fileTicks - len));
    loop.end = loop.start + len;
  }
  if (loop == v_.clip_->loop) return;
  (void)v_.dispatch(doc::ops::setAudioClipLoop(*snap, v_.ref_, loop), "Clip Loop");
}

}  // namespace audio_clip

// ---- the view ---------------------------------------------------------------------------------------------------

bool AudioClipView::dragging() const noexcept { return display_ != nullptr && display_->dragging(); }

bool AudioClipView::onKey(const KeyEvent& e) {
  if (e.down && e.key == key::kEscape && display_ != nullptr && display_->cancel()) {
    if (tree() && tree()->captured()) tree()->releaseCapture();
    return true;
  }
  return false;
}

AudioClipView::AudioClipView(ViewContext& ctx) : Widget("audio-clip-view"), ctx_(ctx) {
  setLayout(LayoutKind::Custom);
  setClipChildren(true);
  box_ = &add<Widget>("audio-box");
  box_->setLayout(LayoutKind::Column);
  box_->setAlign(Align::Start, Align::Start);
  box_->setPadding({8, 0, 8, 6});
  name_ = &box_->add<Label>("name", "", FontRoleId::BodyStrong, TextTone::Primary);
  name_->setHeight(Length::fixed(20));
  // An empty label: the space of the title strip, which the view paints in the clip colour with the name.
  file_ = &box_->add<Label>("file", "", FontRoleId::Small, TextTone::Secondary);
  file_->setHeight(Length::fixed(16));
  format_ = &box_->add<Label>("format", "", FontRoleId::Small, TextTone::Secondary);
  format_->setHeight(Length::fixed(18));
  // M5 review UI 2: the properties in two groups side by side, as Live's Sample and Clip boxes (one long form ran
  // down the panel): Clip (gain, transpose, detune, reverse, fades) and Sample (warp, tempo, loop).
  Widget& props = box_->add<Widget>("props");
  props.setLayout(LayoutKind::Row);
  props.setGap(12);
  props.setHeight(Length::flex(1));
  props.setAlign(Align::Start, Align::Start);
  Widget* clipCol = &props.add<Widget>("clip");
  Widget* sampleCol = &props.add<Widget>("sample");
  for (Widget* col : {clipCol, sampleCol}) {
    col->setLayout(LayoutKind::Column);
    col->setWidth(Length::flex(1));
    col->setAlign(Align::Start, Align::Start);
  }
  Widget* rowParent = clipCol;
  auto row = [&rowParent](const char* name, const char* text) -> Widget& {
    Widget& w = rowParent->add<Widget>(name);
    w.setLayout(LayoutKind::Row);
    w.setHeight(Length::fixed(20));
    w.setPadding({0, 2, 0, 0});
    w.add<Label>("label", text, FontRoleId::Small, TextTone::Secondary).setWidth(Length::flex(1));
    return w;
  };
  auto box = [](Widget& r, const char* name, ValueSpec spec) -> DragBox& {
    DragBox& b = r.add<DragBox>(name, std::move(spec));
    b.setWidth(Length::fixed(84));
    return b;
  };
  ValueSpec gs;
  gs.min = doc::kMinClipGainDb;
  gs.max = doc::kMaxClipGainDb;
  gs.def = 0.0;
  gs.dragPixels = 400.0;
  gs.keyStep = 0.5;
  gs.decimals = 1;
  gs.unit = "dB";
  gain_ = &box(row("gain-row", "Gain"), "gain", gs);
  gain_->formatter = [](double v) {
    if (v <= doc::kMinClipGainDb) return std::string("-inf dB");
    char b[32];
    std::snprintf(b, sizeof b, "%.1f dB", v);
    return std::string(b);
  };
  gain_->setSpec(gs);
  gain_->setInfo("Clip Gain", "The clip's gain (−inf … +24 dB). The waveform follows it.");
  bindGesture(*gain_, "Clip Gain", [this](double v) { (void)setGain(static_cast<float>(v)); });

  Widget& tr = row("transpose-row", "Transpose");
  transpose_ = &tr.add<TextEdit>("transpose");
  transpose_->setWidth(Length::fixed(84));
  transpose_->setInfo("Transpose", "Type 7 for 7 steps of the clip's tuning, 700c for cents, +7st for semitones, 3/2 for a "
                                   "ratio. Warp Off only (Re-Pitch follows the tempo).");
  transpose_->onCommit = [this](const std::string& t) {
    if (!setTranspose(t) && clip_) transpose_->setText(formatTranspose(clip_->transpose));
  };
  ValueSpec ds;
  ds.min = -doc::kMaxDetuneCents;
  ds.max = doc::kMaxDetuneCents;
  ds.def = 0.0;
  ds.dragPixels = 300.0;
  ds.keyStep = 1.0;
  ds.decimals = 0;
  ds.unit = "\xC2\xA2";
  detune_ = &box(row("detune-row", "Detune"), "detune", ds);
  detune_->setInfo("Detune", "Fine pitch in cents (±50). Warp Off only.");
  bindGesture(*detune_, "Clip Detune", [this](double v) { (void)setDetune(static_cast<float>(v)); });

  reverse_ = &row("reverse-row", "Reverse").add<Toggle>("reverse", "Rev");
  reverse_->setWidth(Length::fixed(84));
  reverse_->setInfo("Reverse", "Plays the clip backwards (the file is not changed).");
  reverse_->onToggle = [this](bool on) { (void)setReverse(on); };

  rowParent = sampleCol;
  warp_ = &row("warp-row", "Warp").add<Dropdown>("warp", std::vector<std::string>{"Off", "Re-Pitch"}, 0);
  warp_->setWidth(Length::fixed(84));
  warp_->setFocusable(false);
  warp_->setInfo("Warp", "Off: the clip plays at the file's own speed and keeps its duration in seconds. Re-Pitch: it "
                         "follows the song tempo like a tape (speed and pitch change together).");
  warp_->onChange = [this](int32_t i) { (void)setWarp(i == 1 ? doc::AudioWarp::RePitch : doc::AudioWarp::Off); };
  ValueSpec bs;
  bs.min = 20.0;
  bs.max = 999.0;
  bs.def = 120.0;
  bs.dragPixels = 1000.0;
  bs.keyStep = 1.0;
  bs.decimals = 2;
  bpm_ = &box(row("bpm-row", "BPM"), "bpm", bs);
  bpm_->setInfo("Source Tempo", "Re-Pitch: the tempo the file was recorded at (from its tempo tag or name, else its length).");
  bindGesture(*bpm_, "Clip Tempo", [this](double v) { (void)setSourceBpm(v); });
  loop_ = &row("loop-row", "Loop").add<Toggle>("loop", "Loop");
  loop_->setWidth(Length::fixed(84));
  loop_->setInfo("Loop", "Re-Pitch: repeats the part under the loop brace.");
  loop_->onToggle = [this](bool on) { (void)setLoopEnabled(on); };

  rowParent = clipCol;
  ValueSpec fs;
  fs.min = 0.0;
  fs.max = 60000.0;
  fs.def = 0.0;
  fs.dragPixels = 2000.0;
  fs.keyStep = 1.0;
  fs.decimals = 0;
  fs.unit = "ms";
  fadeIn_ = &box(row("fade-in-row", "Fade In"), "fade-in", fs);
  fadeIn_->setInfo("Fade In", "The fade at the clip start, in milliseconds (or drag its handle in the arrangement).");
  bindGesture(*fadeIn_, "Clip Fade", [this](double v) { (void)setFadesMs(v, fadeOut_->value()); });
  fadeOut_ = &box(row("fade-out-row", "Fade Out"), "fade-out", fs);
  fadeOut_->setInfo("Fade Out", "The fade at the clip end, in milliseconds.");
  bindGesture(*fadeOut_, "Clip Fade", [this](double v) { (void)setFadesMs(fadeIn_->value(), v); });

  display_ = &add<audio_clip::SampleDisplay>(*this);

  docSub_ = ctx_.subscribe([this](const doc::StoreEvent&) { refresh(); });
  viewSub_ = ctx_.observe([this](uint32_t bits) {
    if (bits & (view_change::kDetailClip | view_change::kSamples)) refresh();
    if (bits & (view_change::kWaveforms | view_change::kSamples)) display_->markDirty();
  });
  refresh();
}

AudioClipView::~AudioClipView() = default;

void AudioClipView::bindGesture(ValueControl& c, std::string label, std::function<void(double)> apply) {
  c.onGestureBegin = [this, label] { (void)ctx_.store().beginGesture(label); };
  c.onChange = std::move(apply);
  c.onGestureEnd = [this] {
    if (ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
  };
  c.onGestureCancel = [this] {
    if (ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
    refresh();
  };
}

void AudioClipView::refresh() {
  const doc::Snapshot cur = ctx_.doc();
  const doc::ClipRef want = ctx_.detailClip();
  doc::AudioClipPtr c;
  if (want.clip.valid())
    if (const doc::Track* t = doc::findTrackPtr(*cur, want.track)) {
      const int32_t i = doc::findClipIndex(*t, want.clip);
      if (i >= 0) c = t->clips[static_cast<size_t>(i)].audio;
    }
  const bool changed = c != clip_ || cur != snap_ || !(want.clip == ref_.clip && want.track == ref_.track);
  ref_ = want;
  clip_ = c;
  snap_ = cur;
  setVisible(clip_ != nullptr);
  if (!clip_) return;
  if (!changed) return;
  const doc::AudioClip& a = *clip_;
  const doc::SampleRef* s = doc::findSample(*cur, a.sample);
  const std::string path = ctx_.samplePath(a.sample);
  if (path != probedPath_) {  // headers only (≤ a few reads), once per file: bits, tempo tag
    probedPath_ = path;
    bits_ = 0;
    fileBpm_ = 0.0;
    container_.clear();
    if (!path.empty())
      if (Result<media::SampleInfo> info = media::probe(utf8Path(path))) {
        bits_ = info->bitsPerSample;
        fileBpm_ = info->bpm;
        container_ = std::string(media::containerName(info->container));
      }
  }
  if (s) {
    const std::u8string f = utf8Path(s->path).filename().u8string();
    const std::string fileName(reinterpret_cast<const char*>(f.data()), f.size());
    file_->setText(path.empty() ? fileName + " (missing)" : fileName);
    char fmt[128];
    const double secs = static_cast<double>(s->frames) / std::max(1.0, s->rate);
    if (bits_ > 0)
      std::snprintf(fmt, sizeof fmt, "%.1f kHz \xC2\xB7 %u-bit \xC2\xB7 %s \xC2\xB7 %.2f s", s->rate / 1000.0, bits_,
                    s->channels == 1 ? "mono" : s->channels == 2 ? "stereo" : "multi", secs);
    else
      std::snprintf(fmt, sizeof fmt, "%.1f kHz \xC2\xB7 %s \xC2\xB7 %.2f s", s->rate / 1000.0,
                    s->channels == 1 ? "mono" : s->channels == 2 ? "stereo" : "multi", secs);
    format_->setText(fmt);
  }
  if (!gain_->inGesture()) gain_->setValue(a.gainDb);
  if (!transpose_->focused()) transpose_->setText(formatTranspose(a.transpose));
  if (!detune_->inGesture()) detune_->setValue(a.detuneCents);
  reverse_->setOn(a.reverse);
  warp_->setIndex(a.warp == doc::AudioWarp::RePitch ? 1 : 0);
  if (!bpm_->inGesture()) bpm_->setValue(a.sourceBpm);
  loop_->setOn(a.loop.enabled);
  const bool off = a.warp == doc::AudioWarp::Off;
  transpose_->setEnabled(off);  // Re-Pitch ignores transpose and detune (Q145 item 2): greyed, as in Live
  detune_->setEnabled(off);
  bpm_->setEnabled(!off);
  loop_->setEnabled(!off);
  const TempoMap& tm = cur->timeline->tempoMap;
  const double t0 = tm.ticksToSeconds(a.start), t1 = tm.ticksToSeconds(a.start + a.length);
  const Ticks fi = std::clamp<Ticks>(a.fadeIn.length, 0, a.length), fo = std::clamp<Ticks>(a.fadeOut.length, 0, a.length - fi);
  if (!fadeIn_->inGesture()) fadeIn_->setValue(std::round((tm.ticksToSeconds(a.start + fi) - t0) * 1000.0));
  if (!fadeOut_->inGesture()) fadeOut_->setValue(std::round((t1 - tm.ticksToSeconds(a.start + a.length - fo)) * 1000.0));
  box_->markDirty();
  display_->markDirty();
}

bool AudioClipView::dispatch(const doc::ops::EditResult& e, std::string_view what) {
  if (!e) {
    ctx_.setStatus(std::string(what) + ": " + e.error().message);
    return false;
  }
  if (e->empty()) return false;
  if (Status s = ctx_.store().dispatch(e->command); !s) {
    ctx_.setStatus(std::string(what) + ": " + s.error().message);
    return false;
  }
  return true;
}

bool AudioClipView::setGain(float db) {
  if (!clip_ || db == clip_->gainDb) return false;
  return dispatch(doc::ops::setAudioClipGain(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), db), "Clip Gain");
}

bool AudioClipView::setTranspose(std::string_view text) {
  if (!clip_) return false;
  const std::optional<doc::AudioTranspose> t = parseTranspose(text);
  if (!t) {
    ctx_.setStatus("Transpose: type 7 (steps), 700c (cents), +7st (semitones) or 3/2 (a ratio)");
    return false;
  }
  if (*t == clip_->transpose) return false;
  return dispatch(doc::ops::setAudioClipTranspose(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), *t), "Transpose");
}

bool AudioClipView::setDetune(float cents) {
  if (!clip_ || cents == clip_->detuneCents) return false;
  return dispatch(doc::ops::setAudioClipDetune(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), cents), "Detune");
}

bool AudioClipView::setReverse(bool on) {
  if (!clip_ || on == clip_->reverse) return false;
  return dispatch(doc::ops::setAudioClipsReverse(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), on), "Reverse");
}

bool AudioClipView::setWarp(doc::AudioWarp warp) {
  if (!clip_ || warp == clip_->warp) return false;
  double bpm = clip_->sourceBpm;
  if (warp == doc::AudioWarp::RePitch) {
    const doc::SampleRef* s = doc::findSample(*ctx_.doc(), clip_->sample);
    bpm = sourceBpmFor(fileBpm_, s ? static_cast<double>(s->frames) / std::max(1.0, s->rate) : 0.0);
  }
  return dispatch(doc::ops::setAudioClipWarp(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), warp, bpm), "Warp");
}

bool AudioClipView::setSourceBpm(double bpm) {
  if (!clip_ || clip_->warp != doc::AudioWarp::RePitch || bpm == clip_->sourceBpm) return false;
  return dispatch(doc::ops::setAudioClipWarp(*ctx_.doc(), std::span<const doc::ClipRef>(&ref_, 1), doc::AudioWarp::RePitch, bpm),
                  "Clip Tempo");
}

bool AudioClipView::setLoopEnabled(bool on) {
  if (!clip_ || clip_->warp != doc::AudioWarp::RePitch || on == clip_->loop.enabled) return false;
  doc::ClipLoop l = clip_->loop;
  l.enabled = on;
  if (l.end <= l.start) {  // a fresh loop: the clip's own span of content
    l.start = clip_->contentStart;
    l.end = clip_->contentStart + clip_->length;
  }
  return dispatch(doc::ops::setAudioClipLoop(*ctx_.doc(), ref_, l), "Loop");
}

bool AudioClipView::setFadesMs(double inMs, double outMs) {
  if (!clip_) return false;
  const doc::AudioClip& a = *clip_;
  const TempoMap& tm = ctx_.doc()->timeline->tempoMap;
  const double t0 = tm.ticksToSeconds(a.start), t1 = tm.ticksToSeconds(a.start + a.length);
  doc::AudioFade fi = a.fadeIn, fo = a.fadeOut;
  fi.length = std::clamp<Ticks>(static_cast<Ticks>(std::llround(tm.secondsToTicks(t0 + std::max(0.0, inMs) / 1000.0))) - a.start, 0, a.length);
  fo.length = std::clamp<Ticks>(a.start + a.length - static_cast<Ticks>(std::llround(tm.secondsToTicks(t1 - std::max(0.0, outMs) / 1000.0))),
                                0, a.length);
  if (fi == a.fadeIn && fo == a.fadeOut) return false;
  return dispatch(doc::ops::setAudioClipFades(*ctx_.doc(), ref_, fi, fo), "Clip Fade");
}

IRect AudioClipView::sampleRect() const noexcept { return display_->wave(); }
IRect AudioClipView::rulerRect() const noexcept { return display_->ruler(); }
float AudioClipView::xOfFrame(double frame) const noexcept { return display_->xOf(frame); }
double AudioClipView::startMarkerFrame() const noexcept { return display_->markerFrame(); }

void AudioClipView::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  ctx.dl.addRectFilled(rect().toRect(), th->surface.background);
  const IRect b = box_->rect();
  ctx.dl.addRectFilled(b.toRect(), th->surface.panel);
  if (!clip_) return;
  // The title strip in the clip colour with its name (as the piano roll's clip box).
  const Rgba colour = hex(clip_->colour.rgb);
  const IRect strip{b.x, b.y, b.w, ctx.scale.px(20.0f)};
  ctx.dl.addRectFilled(strip.toRect(), colour);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, f->bodyStrong, strip, clip_->name, clipTextFor(*th, colour), TextAlign::Left, ctx.scale.px(6.0f), ctx.scale.px(6.0f));
}

void AudioClipView::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const int32_t boxW = std::min(pass.px(kBoxWidth), r.w / 2);
  const int32_t gap = 1;
  pass.place(*box_, {r.x, r.y, boxW, r.h});
  pass.place(*display_, {r.x + boxW + gap, r.y, std::max(0, r.w - boxW - gap), r.h});
}

AudioClipView& installAudioClipView(Shell& shell) {
  return shell.slot(ShellSlot::ClipDetail).add<AudioClipView>(shell.context());
}

AudioClipView* findAudioClipView(Shell& shell) {
  return dynamic_cast<AudioClipView*>(shell.slot(ShellSlot::ClipDetail).find("audio-clip-view"));
}

}  // namespace pq::views
