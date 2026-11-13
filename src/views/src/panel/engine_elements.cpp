// GU.7 (docs/plans/GU.md "Phases" 7): the RICH ELEMENTS of the native engines' faces — the Wavetable's, the Envelope's
// and the analysers' faces split into registered element kinds, each working alone on any panel:
//
//   drawtools    the Wavetable's top row: the Phase / Shape mode switch, the draw tools, the Basic Shapes chooser
//   harmonics    the harmonic editor: the edited keyframe's amplitude and phase bars, their scrollbar
//   wavetable    the table display: the stacked frames, the highlighted frame and the voices' dots (draw mode's cycle and
//                grid), the position slider with the keyframe markers and the gap glyphs
//   warpmode     the warp-mode glyph switch and the oversampling cell (with the fold / sync glyphs of that row)
//   envelope     the envelope editor: the shape, its breakpoints and bends, the voices' dots
//   scope        the oscilloscope (chips, trace, measurement, settings sheet)
//   spectrum     the spectrum display
//   spectrogram  the scrolling spectrogram
//   meter        a level meter (new): the signal's peak and RMS per channel, from its analysis tap; DY.3: or a gain-
//                reduction meter (mode reduction: the signal as dB of reduction, drawn down from the top)
//   noise        NZ.1: the Noise engine's display (the output's trace, the voices' dots, the colour's slope line)
//
// The face's plain controls are generic elements (the Wavetable's phase slider and fold / warp / sync knobs, the
// Envelope's amount slider: pq/document/modules.h enginePanel).
//
// An engine element is a VIEW OF ONE SECTION of the engine's face (the face classes of pq/views/meta, the code that drew
// and edited them before): the face is made by the panel's target (PanelTarget::engineFace) and SHARED by every element
// of the panel bound to the same engine, so the sections are one face's state (a keyframe marker selected on the table
// display is the keyframe the harmonic editor edits; a draw tool chosen in the top row puts the display in draw mode).
// Each element draws the whole face clipped to its section, scaled into its rect (aspect kept, centred), and routes
// pointer events, keys and the wheel to the face in face units, but only where the face has a control inside its
// section: elsewhere a press is the panel's (the block's). Bindings (slot 0):
//   Data     an engine block (doc::dataBinding: its params, settings and DATA — the harmonic table, the keyframes, the
//            display state — edited through the document: one undo step per gesture, no recompile)
//   Signal   (the analysers and the meter) any block's output: the lowering taps it while the element is placed
//            (pq/document/panel.h "Displays tapped by the lowering"); the display state is then the element's own
//
// Costs (UI thread only): a paint draws the face's static layers directly (no tiles: they depend on the engine's data)
// and its live layer, once per section shown; nothing runs while the panel is idle.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "elements_internal.h"
#include "pq/views/devices/analysis.h"

namespace pq::views::panel {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

// ---- SignalWindow (elements_internal.h) ----

devices::SpectrumFeed* SignalWindow::feedOf(PanelTarget& t, const doc::Binding* b) {
  return b != nullptr && b->kind == doc::BindingKind::Signal ? t.signalFeed(*b) : nullptr;
}

void SignalWindow::acquire(PanelTarget& t, const doc::Binding* b) {
  if (feed_ != nullptr) return;
  feed_ = feedOf(t, b);
  if (feed_ == nullptr) return;
  feed_->acquire(devices::kWantScope);
  for (auto& v : buf_) v.assign(kFrames, 0.0f);
}

void SignalWindow::release() {
  if (feed_ != nullptr) feed_->release(devices::kWantScope);
  feed_ = nullptr;
  end_ = 0;
  got_ = 0;
}

bool SignalWindow::poll(PanelTarget& t, const doc::Binding* b) {
  devices::SpectrumFeed* f = feedOf(t, b);
  if (f != feed_) {
    release();
    if (f != nullptr) acquire(t, b);
  }
  if (feed_ == nullptr) return false;
  bool settled = true;
  const uint64_t end = feed_->scopeEnd(settled);
  if (end == end_) return false;
  end_ = end;
  const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(end, kFrames));
  if (n == 0) return false;
  float* ch[4] = {buf_[0].data(), buf_[1].data(), buf_[2].data(), buf_[3].data()};
  got_ = feed_->scopeRead(end - n, n, ch);
  left_ = feed_->scopeChannels() >= 4 ? 2 : 0;
  right_ = feed_->scopeChannels() >= 4 ? 3 : 1;
  return true;
}

std::span<const float> SignalWindow::channel(int c) const noexcept {
  const std::vector<float>& v = buf_[c == 0 ? left_ : right_];
  return {v.data(), std::min<size_t>(got_, v.size())};
}

VoiceTapReader* SignalWindow::reader(PanelTarget& t, const doc::Binding* b) const {
  reader_.feed = feedOf(t, b);
  return reader_.feed != nullptr ? &reader_ : nullptr;
}

bool SignalWindow::live() const noexcept { return feed_ != nullptr && feed_->live(); }
bool SignalWindow::Reader::live() const noexcept { return feed != nullptr && feed->live(); }

namespace {

constexpr std::array<std::string_view, 1> kEngineSlot = {"engine"};
constexpr std::array<std::string_view, 1> kSignalSlot = {"signal"};

// A kind's section of its type's face (face units) and whether its whole rect reacts (an analyser's plot is
// interactive though the face calls it its frame).
struct Section {
  std::string_view kind, label, type;
  Rect r;
  bool display = false;  // a Signal binding is allowed (the analysers)
};
constexpr Section kSections[] = {
    {"drawtools", "Draw Tools", "wavetable", {8.0f, 13.0f, 196.0f, 34.0f}},
    {"harmonics", "Harmonic Editor", "wavetable", {10.0f, 34.0f, 194.0f, 76.0f}},
    {"wavetable", "Table Display", "wavetable", {8.0f, 76.0f, 200.0f, 176.0f}},
    {"warpmode", "Warp Mode", "wavetable", {20.0f, 225.0f, 188.0f, 241.0f}},
    {"envelope", "Envelope Editor", "envelope", {6.0f, 6.0f, 184.0f, 122.0f}},
    {"scope", "Oscilloscope", "oscilloscope", {0.0f, 0.0f, 260.0f, 160.0f}, true},
    {"spectrum", "Spectrum", "spectrum", {0.0f, 0.0f, 240.0f, 150.0f}, true},
    {"spectrogram", "Spectrogram", "spectrogram", {0.0f, 0.0f, 300.0f, 170.0f}, true},
    {"noise", "Noise Display", "noise", {0.0f, 0.0f, 220.0f, 44.0f}},  // NZ.1: the trace, the voices, the slope
};
const Section* sectionOf(std::string_view kind) noexcept {
  for (const Section& s : kSections)
    if (s.kind == kind) return &s;
  return nullptr;
}

class EngineSection final : public Element {
public:
  EngineSection(const doc::PanelElement& d, const Section& s) : Element(d), sec_(s) {}
  ~EngineSection() override {
    if (face_ && pressed_) face_->cancelDrag();
  }

  void bind(PanelTarget& t, FaceHost* host) override {
    host_ = host;
    face_.reset();
    if (const doc::Binding* b = binding()) face_ = t.engineFace(*b, sec_.type, host);
    if (face_) face_->attach(host);
  }

  // ---- painting ----
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    if (!face_) {
      paintMissing(p, env);
      return;
    }
    if (!env.hovered && hover_ != Face::kNone && face_->hoveredPart() == hover_ && !pressed_) {
      (void)face_->setHoveredPart(Face::kNone);
      hover_ = Face::kNone;
    }
    const Fit f = fit();
    const ui::FaceTransform& t = p.transform();
    ui::DrawList& dl = p.drawList();
    dl.pushClip({t.origin.x + f.ox * t.scale, t.origin.y + f.oy * t.scale, t.origin.x + (f.ox + sec_.r.width() * f.k) * t.scale,
                 t.origin.y + (f.oy + sec_.r.height() * f.k) * t.scale});
    ui::FacePainter q = p.at({{t.origin.x + (f.ox - sec_.r.x0 * f.k) * t.scale, t.origin.y + (f.oy - sec_.r.y0 * f.k) * t.scale},
                              t.scale * f.k});
    for (uint32_t layer = 0; layer < face_->staticLayerCount(); ++layer) {
      const Rgba col = face_->staticLayerColour(*env.theme, layer);
      if (ui::alphaOf(col) == 0) continue;
      ui::FacePainter s = q.at(q.transform());
      s.setTint(col);
      face_->paintStatic(s, layer);
    }
    FaceEnv fe;
    fe.theme = env.theme;
    fe.hovered = env.hovered;
    fe.uiScale = env.uiScale;
    face_->paintLive(q, fe);
    dl.popClip();
  }

  // ---- interaction ----
  bool interactive() const noexcept override { return face_ != nullptr; }
  bool hit(Vec2 local) const noexcept override { return part(local, 1.0f) != Face::kNone; }
  bool onPointer(const FacePointer& e, PanelTarget& /*t*/) override {
    if (!face_) return false;
    using K = FacePointer::Kind;
    const Fit f = fit();
    FacePointer x = e;
    x.local = toFace(e.local, f);
    x.pxPerUnit = e.pxPerUnit * f.k;
    switch (e.kind) {
      case K::Down: {
        const int32_t pt = part(e.local, x.pxPerUnit);
        if (pt == Face::kNone) return false;
        (void)face_->setHoveredPart(pt);
        hover_ = pt;
        const bool used = face_->onPointer(x);
        pressed_ = used && face_->dragging();
        return used;
      }
      case K::Move: {
        if (pressed_) return face_->onPointer(x);
        const int32_t pt = part(e.local, x.pxPerUnit);
        bool changed = face_->setHoveredPart(pt);
        hover_ = pt;
        changed = face_->onPointer(x) || changed;  // (a face may track the hover itself)
        return changed;
      }
      case K::Up: {
        const bool was = pressed_;
        pressed_ = false;
        return face_->onPointer(x) || was;
      }
      case K::Wheel: return part(e.local, x.pxPerUnit) != Face::kNone && face_->onPointer(x);
      case K::Leave: {
        bool changed = false;
        if (!pressed_ && hover_ != Face::kNone && face_->hoveredPart() == hover_) changed = face_->setHoveredPart(Face::kNone);
        hover_ = Face::kNone;
        return face_->onPointer(x) || changed;
      }
    }
    return false;
  }
  bool dragging() const noexcept override { return pressed_ && face_ && face_->dragging(); }
  void cancelDrag(PanelTarget& /*t*/) override {
    if (face_ && pressed_) face_->cancelDrag();
    pressed_ = false;
  }
  bool onKey(const ui::KeyEvent& e, PanelTarget& /*t*/) override { return face_ && face_->onKey(e); }
  bool onText(const ui::TextEvent& e, PanelTarget& /*t*/) override { return face_ && face_->onText(e); }
  bool wantsText() const noexcept override { return face_ && face_->wantsText(); }
  void focusLost(PanelTarget& /*t*/) override {
    if (face_) face_->focusLost();
  }
  ui::Cursor cursor() const noexcept override { return face_ ? face_->cursor(face_->hoveredPart()) : ui::Cursor::Inherit; }
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override {
    if (!face_) return Element::describe(t, title, body);
    face_->describe(face_->hoveredPart() != Face::kNone ? face_->hoveredPart() : Face::kFrame, title, body);
  }

  // ---- voices, analysis ----
  VoiceTapReader* voiceReader(PanelTarget& /*t*/) const override { return face_ ? face_->voices() : nullptr; }
  // (A face shared by several sections is polled by each: the first poll of a frame reads the records, the others
  // find nothing new — the reader's serial — and the panel repaints once.)
  bool pollVoices(PanelTarget& /*t*/) override { return face_ && face_->pollVoices(); }
  bool animating() const noexcept override { return face_ && face_->animating(); }

  // ---- wires into the engine's params: they end on the face's control inside this section ----
  const doc::Binding* controlAt(Vec2 local) const override {
    if (!face_ || binding() == nullptr || binding()->kind != doc::BindingKind::Data) return nullptr;
    const std::string param = face_->paramAt(toFace(local, fit()));
    if (param.empty()) return nullptr;
    scratch_ = doc::controlBinding(binding()->device, param, binding()->meta);
    return &scratch_;
  }
  // GU.13: the face's controls inside this section (the drop targets, LEDs and rings of a native engine's params).
  void visitControls(const ControlVisitor& fn) const override {
    const doc::Binding* b = binding();
    if (!face_ || b == nullptr || b->kind != doc::BindingKind::Data) return;
    const Fit f = fit();
    const meta::Type& type = face_->type();
    for (size_t k = 0; k < type.params.size(); ++k) {
      const std::optional<Rect> r = face_->paramRect(k);
      if (!r || !inside({(r->x0 + r->x1) * 0.5f, (r->y0 + r->y1) * 0.5f})) continue;
      const Rect er{f.ox + (r->x0 - sec_.r.x0) * f.k, f.oy + (r->y0 - sec_.r.y0) * f.k, f.ox + (r->x1 - sec_.r.x0) * f.k,
                    f.oy + (r->y1 - sec_.r.y0) * f.k};
      fn(doc::controlBinding(b->device, std::string(type.params[k].name), b->meta), er);
    }
  }
  std::optional<ControlAnchor> anchorFor(const doc::Binding& want, bool /*shownOnly*/) const override {
    const doc::Binding* b = binding();
    if (!face_ || b == nullptr || b->kind != doc::BindingKind::Data || want.kind != doc::BindingKind::Control ||
        want.device != b->device || want.meta != b->meta)
      return std::nullopt;
    std::optional<ControlAnchor> a = face_->controlAnchor(want.name);
    if (!a || !inside(a->at)) return std::nullopt;
    const Fit f = fit();
    a->at = {f.ox + (a->at.x - sec_.r.x0) * f.k, f.oy + (a->at.y - sec_.r.y0) * f.k};
    a->radius *= f.k;
    a->clear *= f.k;
    return a;
  }

private:
  struct Fit {
    float k = 1.0f, ox = 0.0f, oy = 0.0f;
  };
  Fit fit() const noexcept {
    const doc::EmbedFit e = doc::embedFit(desc_.w, desc_.h, sec_.r.width(), sec_.r.height());
    return {e.k, e.x, e.y};
  }
  Vec2 toFace(Vec2 l, const Fit& f) const noexcept { return {sec_.r.x0 + (l.x - f.ox) / f.k, sec_.r.y0 + (l.y - f.oy) / f.k}; }
  bool inside(Vec2 fl) const noexcept { return fl.x >= sec_.r.x0 && fl.x <= sec_.r.x1 && fl.y >= sec_.r.y0 && fl.y <= sec_.r.y1; }
  // The face's part under element-local `l` inside the section (kNone: none of the section's; the frame counts only
  // for a display, whose plot is the frame of its face).
  int32_t part(Vec2 l, float ppu) const noexcept {
    if (!face_) return Face::kNone;
    const Vec2 fl = toFace(l, fit());
    if (!inside(fl)) return Face::kNone;
    const int32_t p = face_->hitTest(fl, ppu);
    if (p == Face::kFrame && !sec_.display) return Face::kNone;
    return p;
  }
  void paintMissing(ui::FacePainter& p, const ElementEnv& env) const {
    const Rect box{0.5f, 0.5f, desc_.w - 0.5f, desc_.h - 0.5f};
    p.roundedRect(box, 6.0f, ui::mulAlpha(env.theme->face.track, 0.35f));
    outlineRect(p, box, 6.0f, 1.0f, env.theme->face.frame);
    p.text({desc_.w * 0.5f, desc_.h * 0.5f}, sec_.label, 7.0f, env.theme->face.ink);
  }
  const Section& sec_;
  std::shared_ptr<MetaFace> face_;
  FaceHost* host_ = nullptr;
  bool pressed_ = false;
  int32_t hover_ = Face::kNone;
  mutable doc::Binding scratch_;
};

template <size_t I>
std::unique_ptr<Element> makeSection(const doc::PanelElement& d) {
  return std::make_unique<EngineSection>(d, kSections[I]);
}

// ---- meter ----------------------------------------------------------------------------------------------------------

// A meter, two bars (L, R) from the last 50 ms of the bound signal's analysis feed (its tap's signal channels), read
// once per frame while the feed moves. Style `mode`:
//   level      the RMS as a bar with the peak as a line, over `floor` … 0 dBFS (the top 6 dB warm)
//   reduction  DY.3: the signal read as dB of gain reduction (≥ 0: a compressor's GR output), its largest value in the
//              window drawn DOWNWARD from the top over `range` dB (12 / 24 / 48), a tick every quarter of the range
class Meter final : public Element {
public:
  using Element::Element;
  static constexpr std::string_view kModes[] = {"level", "reduction"};
  static constexpr StyleSpec kStyle[] = {
      {"mode", StyleSpec::Type::Choice, 0, "level", kModes},
      {"floor", StyleSpec::Type::Number, -60.0, {}, {}, -120.0, -6.0},
      {"range", StyleSpec::Type::Number, 24.0, {}, {}, 12.0, 48.0},
      {"colour", StyleSpec::Type::Token, 0, "signal"},
  };

  uint32_t staticLayerCount() const noexcept override { return reduction() ? 2 : 1; }
  Rgba staticLayerColour(const ui::Theme& th, uint32_t layer) const noexcept override {
    return layer == 0 ? th.face.track : th.face.tick;
  }
  void paintStatic(ui::FacePainter& p, uint32_t layer) const override {
    if (layer == 0) {
      for (int c = 0; c < 2; ++c) p.roundedRect(bar(c), 1.5f, ui::rgba(255, 255, 255));
      return;
    }
    // reduction: a tick every quarter of the range, across both bars
    const Rect b0 = bar(0), b1 = bar(1);
    for (int q = 1; q < 4; ++q) {
      const float y = b0.y0 + (b0.y1 - b0.y0) * static_cast<float>(q) * 0.25f;
      p.line({b0.x0, y}, {b1.x1, y}, 0.6f, ui::rgba(255, 255, 255));
    }
  }
  void paintLive(ui::FacePainter& p, const ElementEnv& env) override {
    const ui::Theme& th = *env.theme;
    if (env.target != nullptr) window_.acquire(*env.target, binding());
    if (reduction()) {
      // (the reduction's colour: the value accent unless the style names one)
      const Rgba col = colour(th, "colour", "value", th.face.value);
      const double range = rangeDb();
      for (int c = 0; c < 2; ++c) {
        const Rect b = bar(c);
        const float f = static_cast<float>(std::clamp(static_cast<double>(gr_[c]) / range, 0.0, 1.0));
        if (f > 0.0f) p.roundedRect({b.x0, b.y0, b.x1, b.y0 + (b.y1 - b.y0) * f}, 1.5f, col);
      }
      return;
    }
    const Rgba col = colour(th, "colour", "signal", th.face.signal);
    for (int c = 0; c < 2; ++c) {
      const Rect b = bar(c);
      const float rms = level(rms_[c]), peak = level(peak_[c]);
      if (rms > 0.0f) p.roundedRect({b.x0, b.y1 - (b.y1 - b.y0) * rms, b.x1, b.y1}, 1.5f, col);
      if (peak > 0.0f) {
        const float y = b.y1 - (b.y1 - b.y0) * peak;
        p.line({b.x0, y}, {b.x1, y}, 1.2f, peak > level(0.5f) ? th.face.valueDriven : th.face.handle);
      }
    }
  }
  VoiceTapReader* voiceReader(PanelTarget& t) const override { return window_.reader(t, binding()); }
  bool pollVoices(PanelTarget& t) override {
    if (!window_.poll(t, binding())) return false;
    for (int c = 0; c < 2; ++c) {
      const std::span<const float> x = window_.channel(c);
      double s = 0.0;
      float pk = 0.0f, gr = 0.0f;
      for (const float v : x) {
        s += static_cast<double>(v) * v;
        pk = std::max(pk, std::fabs(v));
        gr = std::max(gr, v);  // (a NaN is skipped)
      }
      rms_[c] = !x.empty() ? static_cast<float>(std::sqrt(s / static_cast<double>(x.size()))) : 0.0f;
      peak_[c] = pk;
      gr_[c] = gr;
    }
    return true;
  }
  bool animating() const noexcept override { return window_.live(); }

  // Tests (a golden's level without a running engine): both bars' values (level: linear RMS = peak; reduction: dB).
  void show(float a, float b) noexcept {
    rms_[0] = peak_[0] = gr_[0] = a;
    rms_[1] = peak_[1] = gr_[1] = b;
  }

private:
  bool reduction() const noexcept { return txt("mode", "level") == "reduction"; }
  double rangeDb() const noexcept { return std::clamp(num("range", 24.0), 12.0, 48.0); }
  Rect bar(int c) const noexcept {
    const float gap = std::max(1.0f, desc_.w * 0.12f), w = (desc_.w - 3.0f * gap) * 0.5f;
    const float x0 = gap + static_cast<float>(c) * (w + gap);
    return {x0, 2.0f, x0 + w, desc_.h - 2.0f};
  }
  float level(float amp) const noexcept {
    if (!(amp > 0.0f)) return 0.0f;
    const double floor = std::clamp(num("floor", -60.0), -120.0, -6.0);
    const double dbv = 20.0 * std::log10(static_cast<double>(amp));
    return static_cast<float>(std::clamp((dbv - floor) / -floor, 0.0, 1.0));
  }
  SignalWindow window_;
  float rms_[2] = {0.0f, 0.0f}, peak_[2] = {0.0f, 0.0f}, gr_[2] = {0.0f, 0.0f};
};

std::unique_ptr<Element> makeMeter(const doc::PanelElement& d) { return std::make_unique<Meter>(d); }

}  // namespace

namespace test {
bool showMeter(Element& e, float a, float b) {
  auto* m = dynamic_cast<Meter*>(&e);
  if (m == nullptr) return false;
  m->show(a, b);
  return true;
}
}  // namespace test

void registerEngineElements(void (*reg)(std::string_view, std::string_view, std::span<const StyleSpec>, std::span<const std::string_view>,
                                        float, float, ElementFactory)) {
  static_assert(std::size(kSections) == 9);
  constexpr ElementFactory kMake[] = {&makeSection<0>, &makeSection<1>, &makeSection<2>, &makeSection<3>, &makeSection<4>,
                                      &makeSection<5>, &makeSection<6>, &makeSection<7>, &makeSection<8>};
  for (size_t i = 0; i < std::size(kSections); ++i) {
    const Section& s = kSections[i];
    reg(s.kind, s.label, {}, s.display ? std::span<const std::string_view>(kSignalSlot) : std::span<const std::string_view>(kEngineSlot),
        s.r.width(), s.r.height(), kMake[i]);
  }
  reg("meter", "Meter", Meter::kStyle, kSignalSlot, 24, 96, &makeMeter);
}

}  // namespace pq::views::panel
