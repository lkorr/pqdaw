// BR.tuning-block (user 2026-11-13: "the tuning module to be a physical object paired with every midi channel; it should
// be a very small vertical panel thing labeled 'tuning' … but when you open it up itll show the tuning circle and
// everything that you can zoom into; 12 edo by default, and the correct tones light up etc when the note passes
// through"): a MIDI band's tuning block drawn as a FACE (pq/views/meta/meta_face.h Face), hosted by the canvas like the
// mixer's face. It shows and edits the track's tuning (the named tuning it resolves to: TuningTarget::forTrack).
//
//   closed   a narrow vertical plate: the track's colour on its cap, T-U-N-I-N-G stacked down it in the UI's semibold
//            face (letter-spaced capitals in the accent, a soft glow behind them), the tuning's short label ("12")
//            under them when it fits; its notes socket on the bottom edge, its output on the right edge at the spine.
//            Vector painting only (a few primitives per paint, nothing static to cache), no clock.
//   opened   a ui::UiTree of its own (TuningPanel: a header — TUNING, the tuning's name, an EDO entry, the Wheel
//            button — over the MosCircle) laid out at the canvas's device scale with the theme's fonts
//            resolved at that scale, so it zooms with the canvas and its text stays crisp at every settled zoom (the
//            way TX.panel-faces hosted a device panel: retired/src/views/src/devices/panel_face.cpp). The tree paints
//            into a recorded draw list once per change; the list is replayed into the canvas's each frame, scaled while
//            a zoom settles (Face::prepare lays it out again at the new scale). The canvas routes presses on the
//            header's controls and on the circle to the tree in panel px; the header's empty strip is the block's (a
//            double-click there closes it). Keys and text go to the tree while one of its fields holds the focus.
//   wheel    (BR.tuning-outside, user 2026-11-13: "the wheel view … should just expand/open the wheel up on the same
//            module; keeping the mos circle in its same location, and just expanding out to its left") the Wheel button
//            grows the opened block LEFTWARD by kTuningWheelW: the full wheel's region chart (MosChart), a Scala button and
//            the temperament search in a column left of the circle's, which keeps its size and place (the block's right
//            edge stays put: the model anchors it). The button is the canvas's (ProjectCanvas: a press on it toggles
//            PatchNudge::tuningWheel, which re-lays out and so replaces this face — never from inside the face's own
//            dispatch); the face only draws it (tuningFaceWheelButton). The chart and the circle follow each other as in
//            the popover (mos_wheel.h's shared helpers: focusChartSelection, selectTemperament, findTemperament,
//            readScalaTuning).
//   edits    the circle's and chart's drags and buttons, a typed EDO: TuningEdit (pq/views/mos_wheel/mos_wheel.h) — live while
//            dragging, one undo step per gesture, Esc during a drag cancels it. A document change from elsewhere (undo,
//            the wheel, another track sharing the tuning) is shown at once.
//   EDO      an equal division of the octave is shown on the circle as its n equal steps: the n-note MOS of its fifth's
//            generator at that region's equalized end (L = s), so 12-EDO — a new track's default — shows its 12
//            degrees and lights them. A drag of the hardness slider then makes it that MOS, as on the wheel.
//   lit      TU.2's CircleNotes for this track only (CircleNotes's `only`): its playback, the computer keyboard and MIDI
//            in while it listens, its clips' auditions. Frames run only while the face is opened AND on screen: a face
//            whose last two repaints were not painted (scrolled away, the view hidden) parks its tree until it is
//            painted again (no idle frames while nothing plays: CircleNotes asks for frames only while playing or lit).
//
// UI thread only.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string>

#include "../common/paint.h"
#include "../mos_wheel/circle_notes.h"
#include "internal.h"
#include "pq/ui/atlas.h"
#include "pq/ui/controls.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/tile_atlas.h"
#include "pq/tuning/mos_data.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"

namespace pq::views::project_patch {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;
namespace tu = pq::tuning;

namespace {

// The MOS selection a MOS tuning shows (the region of its notes per period containing its generator, the stored hint
// first; `moreLarge`: one with more large steps than small, where the generator is at the boundary of two), as the
// wheel's chart picks it; nullopt when no region of `c`'s set holds it.
std::optional<MosSelection> selectionOf(const MosCircle& c, const tu::MosParams& m, bool moreLarge = false) {
  const tu::MosRegionSet& rs = c.regionSet();
  const tu::MosRegion* best = nullptr;
  for (const tu::MosRegion& r : rs.regions) {
    if (r.perPeriodSize != m.n || m.generator < r.lowCents || m.generator > r.highCents) continue;
    if (m.region && std::fabs(r.lowCents - m.region->lowCents) < 0.01 && std::fabs(r.highCents - m.region->highCents) < 0.01) {
      best = &r;
      break;
    }
    if (best == nullptr || (moreLarge && best->perPeriodKey.nL < best->perPeriodKey.nS && r.perPeriodKey.nL > r.perPeriodKey.nS)) best = &r;
  }
  if (best == nullptr) return std::nullopt;
  MosSelection s;
  s.key = best->key;
  s.perPeriodKey = best->perPeriodKey;
  s.periodsPerEquave = c.config().periodsPerEquave;
  s.period = c.config().period;
  s.generatorCents = std::clamp(m.generator, best->lowCents, best->highCents);
  s.regionLow = best->lowCents;
  s.regionHigh = best->highCents;
  s.mirror = best->isMirror;
  s.modeRotation = m.rotation >= 0 && m.rotation < best->perPeriodSize ? m.rotation : 0;
  for (int32_t n = 1; n <= c.config().maxEdo; ++n) {  // the generator's EDO identity (the chart's, smallest EDO)
    const double step = s.period / static_cast<double>(n);
    const auto k = static_cast<int32_t>(std::llround(s.generatorCents / step));
    if (std::fabs(static_cast<double>(k) * step - s.generatorCents) < 1e-9) {
      s.edo = MosSelection::Edo{k, n};
      break;
    }
  }
  return s;
}

// An n-EDO of the octave as its n equal steps on the circle: the n-note MOS at the equalized end of its region, the
// generator the step nearest the EDO's fifth that is coprime with n (12: 7\12, 7L 5s at L = s: of the two regions meeting
// there, the one with more large steps, the chromatic scale over the diatonic). nullopt: no such MOS in
// `c`'s regions (n < 2, n past the circle's sizes, another equave).
std::optional<MosSelection> edoSelection(const MosCircle& c, const tu::Edo& e) {
  const auto n = static_cast<int32_t>(e.n());
  if (n < 2 || n > c.config().maxSize || std::fabs(e.equaveCents() - 1200.0) > 1e-6 || c.config().periodsPerEquave != 1) return std::nullopt;
  const auto fifth = static_cast<int32_t>(std::lround(static_cast<double>(n) * std::log2(1.5)));
  for (int32_t d = 0; d < n; ++d)
    for (const int32_t k : {fifth - d, fifth + d}) {
      if (k < 1 || k >= n || std::gcd(k, n) != 1) continue;
      tu::MosParams m;
      m.n = n;
      m.generator = 1200.0 * static_cast<double>(k) / static_cast<double>(n);
      if (std::optional<MosSelection> s = selectionOf(c, m, true)) return s;
    }
  return std::nullopt;
}

// The opened face's widget tree root: the header over the circle (the circle's column, kTuningOpenW wide, at the right);
// BR.tuning-outside: with `wheel`, the wheel's column (its header: Scala, the temperament search; the region chart) left of
// it. Reads and edits the track's tuning.
class TuningPanel final : public ui::Widget {
public:
  static constexpr float kHeaderH = metrics::kTuningHeaderH;
  static constexpr float kLabelW = 96.0f;  // the TUNING label's room at the header's left

  TuningPanel(ViewContext& ctx, doc::TrackId track, bool wheel)
      : Widget("tuning-panel"), ctx_(ctx), track_(track), target_(TuningTarget::forTrack(track)), edit_(ctx, target_) {
    setLayout(ui::LayoutKind::Row);
    if (const doc::TuningDef* d = targetTuning(*ctx_.doc(), target_)) draft_ = *d;
    if (wheel) {
      auto& side = add<ui::Widget>("wheel-side");
      side.setLayout(ui::LayoutKind::Column);
      side.setWidth(ui::Length::fixed(metrics::kTuningWheelW));
      auto& wh = side.add<ui::Widget>("wheel-header");
      wh.setLayout(ui::LayoutKind::Row);
      wh.setHeight(ui::Length::fixed(kHeaderH));
      wh.setPadding({static_cast<int16_t>(kLabelW), 4, 8, 4});
      wh.setGap(4);
      wh.setAlign(ui::Align::Start, ui::Align::Center);
      auto& scala = wh.add<ui::Button>("scala", "Scala\xE2\x80\xA6");
      scala.setSize(ui::Length::fixed(52), ui::Length::fixed(18));
      scala.setFocusable(false);
      scala.setInfo("Scala File", "Imports a Scala scale (.scl) as this track's tuning.");
      scala.onClick = [this] { requestScalaFile(); };
      search_ = &wh.add<ui::TextEdit>("temperament");
      search_->setSize(ui::Length::flex(1), ui::Length::fixed(18));
      search_->setPlaceholder("Temperament\xE2\x80\xA6");
      search_->setInfo("Find Temperament", "Type part of a temperament's name (meantone, porcupine, magic\xE2\x80\xA6) and press "
                                           "Enter: its MOS at the middle of its generator range. Several matches open a short list.");
      search_->onCommit = [this](const std::string& t) {
        std::shared_ptr<bool> alive = alive_;
        if (!t.empty() && !findTemperament(*search_, t, [this, alive](int32_t i) {
              if (*alive) (void)chooseTemperament(i);
            }))
          ctx_.setStatus("Temperament: no name contains \xE2\x80\x9C" + t + "\xE2\x80\x9D");
      };
      auto& box = side.add<ui::Widget>("chart-box");
      box.setLayout(ui::LayoutKind::Column);
      box.setHeight(ui::Length::flex(1));
      box.setPadding({8, 4, 8, 8});
      chart_ = &box.add<MosChart>("chart", configFor(draft_));
      chart_->setHeight(ui::Length::flex(1));
      chart_->setOnChange([this](const std::optional<MosSelection>& s, MosGesture g) { onChart(s, g); });
    }
    auto& col = add<ui::Widget>("circle-side");
    col.setLayout(ui::LayoutKind::Column);
    col.setWidth(ui::Length::fixed(metrics::kTuningOpenW));
    auto& header = col.add<ui::Widget>("header");
    header.setLayout(ui::LayoutKind::Row);
    header.setHeight(ui::Length::fixed(kHeaderH));
    // (left: the TUNING label, at the block's left edge: the wheel's header has it when shown; right: clear of the output
    // socket on the edge)
    header.setPadding({static_cast<int16_t>(wheel ? 8.0f : kLabelW), 4, 10, 4});
    header.setGap(4);
    header.setAlign(ui::Align::Start, ui::Align::Center);
    name_ = &header.add<ui::Label>("name", std::string(), ui::FontRoleId::Small, ui::TextTone::Secondary);
    name_->setWidth(ui::Length::flex(1));
    edo_ = &header.add<ui::TextEdit>("edo");
    edo_->setSize(ui::Length::fixed(34), ui::Length::fixed(18));
    edo_->setPlaceholder("EDO");
    edo_->setInfo("EDO", "Type a number of equal divisions of the octave and press Enter: the track's tuning (and every "
                         "track that shares it) retunes at once.");
    edo_->onCommit = [this](const std::string& t) {
      int32_t n = 0;
      if (std::sscanf(t.c_str(), "%d", &n) == 1 && n >= 1 && n <= 9999) (void)chooseEdo(n);
      else showDraft();
    };
    // The canvas takes its presses (TuningFace: tuningFaceWheelButton); the tree draws it and its hover.
    wheel_ = &header.add<ui::Button>("wheel", wheel ? "Circle" : "Wheel");
    wheel_->setSize(ui::Length::fixed(52), ui::Length::fixed(18));
    wheel_->setFocusable(false);
    if (wheel)
      wheel_->setInfo("Circle Only", "Folds the wheel's region chart away: the block is its MOS circle again.");
    else
      wheel_->setInfo("MOS Wheel", "Opens the full MOS wheel here, out to the block's left: the region chart, Scala files, the "
                                   "temperament search. The circle stays where it is.");
    circle_ = &col.add<MosCircle>("circle", configFor(draft_));
    circle_->setHeight(ui::Length::flex(1));
    circle_->setOnChange([this](const std::optional<MosSelection>& s, std::span<const double>, MosGesture g) { onCircle(s, g); });
    circle_->onAudition = [this](double cents) {
      const double hz = draft_.referenceHz * std::exp2(cents / 1200.0);
      ctx_.transport().auditionHz(static_cast<float>(hz), 100.0f, 400.0f);
      circle_->noteOn(CircleNotes::auditionId(0), 0, hz, 0.4);
    };
    docSub_ = ctx_.subscribe([this](const doc::StoreEvent& e) {
      if (applying_ || !TuningChip::tuningMayChange(e)) return;
      const doc::TuningDef* d = targetTuning(*ctx_.doc(), target_);
      if (d == nullptr || *d == draft_ || circle_->dragging() || (chart_ != nullptr && chart_->dragging())) return;
      draft_ = *d;
      showDraft();
    });
    showDraft();
  }
  ~TuningPanel() override {
    *alive_ = false;
    edit_.finish();
  }

  MosCircle& circle() const noexcept { return *circle_; }
  ui::TextEdit& edo() const noexcept { return *edo_; }
  MosChart* chart() const noexcept { return chart_; }  // null: not grown to the wheel
  const ui::Button& wheelButton() const noexcept { return *wheel_; }

  // TU.2: what lights the circle — made once the panel is in its tree (a frame request outside a tree is dropped).
  void startNotes() {
    if (!notes_ && tree() != nullptr) notes_ = std::make_unique<CircleNotes>(ctx_, target_, *circle_, *this, track_);
  }
  void onFrame(double now) override {
    if (notes_) notes_->frame(now);
  }
  void cancelEdit() { edit_.cancel(); }

  void paint(ui::PaintContext& ctx) override {
    if (ctx.res.theme == nullptr) return;
    const ui::Theme& th = *ctx.res.theme;
    const ui::IRect r = rect();
    const float us = ctx.scale.factor;
    const auto hh = static_cast<int32_t>(std::lround(kHeaderH * us));
    ctx.dl.addRectFilled(ui::IRect{r.x, r.y, r.w, hh}.toRect(), th.face.body);
    ctx.dl.addRectFilled(ui::IRect{r.x, r.y + hh - std::max(1, static_cast<int32_t>(us)), r.w, std::max(1, static_cast<int32_t>(us))}.toRect(),
                         th.face.frame);
    // The track's colour, then T U N I N G (letter-spaced capitals in the accent).
    if (const doc::Track* t = doc::findTrackPtr(*ctx_.doc(), track_)) {
      const float s = std::round(3.0f * us);
      ctx.dl.addRectFilled({static_cast<float>(r.x) + std::round(6.0f * us), static_cast<float>(r.y) + std::round(8.0f * us),
                            static_cast<float>(r.x) + std::round(6.0f * us) + s, static_cast<float>(r.y + hh) - std::round(8.0f * us)},
                           ui::hex(t->colour.rgb));
    }
    if (ctx.res.uiFonts != nullptr && ctx.res.text != nullptr)
      (void)paint::text(ctx, ctx.res.uiFonts->bodyStrong,
                        {r.x + static_cast<int32_t>(std::lround(13.0f * us)), r.y, static_cast<int32_t>(std::lround((kLabelW - 13.0f) * us)), hh},
                        "T U N I N G", th.face.value, ui::TextAlign::Left);
    if (chart_ != nullptr) {  // the wheel's column and the circle's: a rule between them under the header
      const auto lw = std::max(1, static_cast<int32_t>(us));
      const int32_t x = r.x + r.w - static_cast<int32_t>(std::lround(metrics::kTuningOpenW * us));
      ctx.dl.addRectFilled(ui::IRect{x, r.y + hh, lw, r.h - hh}.toRect(), th.face.frame);
    }
  }

private:
  static MosChartConfig configFor(const doc::TuningDef& d) {
    MosChartConfig cfg;
    if (const tu::MosParams* m = d.mos()) {
      cfg.period = m->period;
      cfg.periodsPerEquave = std::max(1, m->periodsPerEquave);
      cfg.octaveStretch = m->octaveStretch;
    }
    if (const tu::Edo* e = d.edo()) cfg.maxSize = std::clamp(static_cast<int32_t>(e->n()), cfg.maxSize, 64);  // its n steps
    return cfg;
  }

  // The circle, the name and the EDO entry from the draft (no document change).
  void showDraft() {
    MosChartConfig cfg = configFor(draft_);
    if (const tu::MosParams* m = draft_.mos()) cfg.maxSize = std::max(circle_->config().maxSize, std::clamp(m->n, 2, 64));
    else cfg.maxSize = std::max(cfg.maxSize, circle_->config().maxSize);  // (regions only grow: a size once shown stays)
    circle_->setConfig(cfg);
    if (const tu::MosParams* m = draft_.mos()) {
      circle_->setSelection(selectionOf(*circle_, *m));
      circle_->setModmos(m->modmos);
      edo_->setText({});
    } else if (const tu::Edo* e = draft_.edo()) {
      circle_->setSelection(edoSelection(*circle_, *e));
      circle_->setModmos({});
      edo_->setText(std::to_string(e->n()));
    } else {
      circle_->setSelection(std::nullopt);
      circle_->setModmos({});
      edo_->setText({});
    }
    syncChart();
    std::string name;
    const doc::Project& p = *ctx_.doc();
    if (const doc::TuningSlot* s = doc::findTuning(p, targetTuningId(p, target_))) name = s->name;
    const std::string sum = tuningSummary(draft_);
    name_->setText(name.empty() ? sum : name + " \xC2\xB7 " + sum);
    markDirty();
  }

  bool apply(doc::TuningDef def, MosGesture g, std::string_view label) {
    applying_ = true;
    const bool ok = edit_.apply(def, g, label).ok();
    applying_ = false;
    if (ok) draft_ = std::move(def);
    return ok;
  }

  // The chart shows what the circle does (its sizes, the selected region; zoomed to it, its row scrolled into view a few
  // rows down).
  void syncChart() {
    if (chart_ == nullptr) return;
    if (!(chart_->config() == circle_->config())) chart_->setConfig(circle_->config());
    if (const std::optional<MosSelection>& sel = circle_->selection();
        sel && chart_->select(sel->perPeriodKey, sel->generatorCents, sel->modeRotation)) {
      focusChartSelection(*chart_);
      chart_->setFirstSize(std::max(2, sel->notesPerPeriod() - 4));
    } else {
      chart_->clearSelection();
    }
  }

  void cancelToDocument() {  // the drag's gesture is undone: show the document again
    edit_.cancel();
    if (const doc::TuningDef* d = targetTuning(*ctx_.doc(), target_)) draft_ = *d;
    showDraft();
  }

  // A click or drag in the wheel's chart: the circle takes its MOS, the track's tuning follows.
  void onChart(const std::optional<MosSelection>& sel, MosGesture g) {
    if (g == MosGesture::Cancel) {
      cancelToDocument();
      return;
    }
    circle_->setSelection(sel);
    if (!sel) return;  // nothing selected: the track keeps its tuning
    fromCircle(g);
  }

  void onCircle(const std::optional<MosSelection>& sel, MosGesture g) {
    if (g == MosGesture::Cancel) {
      cancelToDocument();
      return;
    }
    if (!sel) return;
    if (chart_ != nullptr) (void)chart_->select(sel->perPeriodKey, sel->generatorCents, sel->modeRotation);
    fromCircle(g);
  }

  // The circle's MOS as the track's tuning (live while dragging, one undo step at the commit).
  void fromCircle(MosGesture g) {
    const std::optional<tu::MosParams> p = circle_->params();
    if (!p) return;
    doc::TuningDef d;
    d.system = *p;
    d.referenceHz = draft_.referenceHz;
    (void)apply(std::move(d), g, "Change Tuning");
    if (g == MosGesture::Commit) showDraft();
  }

  bool chooseEdo(int32_t n) {
    Result<tu::Edo> e = Error{ErrorCode::InvalidArgument, "", {}};
    const tu::Edo* old = draft_.edo();
    if (old && old->equaveRatio()) e = tu::Edo::make(static_cast<uint32_t>(n), *old->equaveRatio());
    else if (old) e = tu::Edo::makeCents(static_cast<uint32_t>(n), old->equaveCents());
    else e = tu::Edo::make(static_cast<uint32_t>(n));
    if (!e) return false;
    doc::TuningDef d;
    d.system = *e;
    d.referenceHz = draft_.referenceHz;
    const bool ok = apply(std::move(d), MosGesture::Commit, "Change Tuning");
    showDraft();
    return ok;
  }

  bool chooseTemperament(int32_t index) {
    if (chart_ == nullptr || !selectTemperament(ctx_, *chart_, *circle_, index)) {
      showDraft();
      return false;
    }
    search_->setText(std::string(tu::temperaments()[static_cast<size_t>(index)].name));
    fromCircle(MosGesture::Commit);
    return true;
  }

  void requestScalaFile() {
    FileDialogs* d = ctx_.fileDialogs();
    if (d == nullptr) {
      ctx_.setStatus("Scala: no file dialog available");
      return;
    }
    // The dialog may answer after the block closed (a re-layout replaced this face): the token says whether we exist.
    std::shared_ptr<bool> alive = alive_;
    d->chooseScalaFile([this, alive](std::string path) {
      if (!*alive || path.empty()) return;
      Result<doc::TuningDef> def = readScalaTuning(ctx_, path, draft_.referenceHz);
      if (!def) return;
      (void)apply(std::move(*def), MosGesture::Commit, "Change Tuning");
      showDraft();
    });
  }

  ViewContext& ctx_;
  doc::TrackId track_;
  TuningTarget target_;
  doc::TuningDef draft_;
  TuningEdit edit_;
  MosCircle* circle_ = nullptr;
  MosChart* chart_ = nullptr;        // BR.tuning-outside: the wheel's (null: the circle alone)
  ui::TextEdit* search_ = nullptr;   // … its temperament search
  ui::Label* name_ = nullptr;
  ui::TextEdit* edo_ = nullptr;
  ui::Button* wheel_ = nullptr;
  std::unique_ptr<CircleNotes> notes_;
  ViewContext::Subscription docSub_;
  bool applying_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false once destroyed (late dialog / menu callbacks)
};

constexpr float kCorner = 4.0f;

class TuningFace final : public Face, private ui::TreeHost {
public:
  static constexpr float kMinScale = 0.5f;  // device px per world unit: below it the recording is drawn shrunk
  static constexpr float kMaxScale = 8.0f;  // above it, stretched

  TuningFace(ViewContext& ctx, doc::TrackId track, bool open, bool wheel, Vec2 size)
      : ctx_(ctx), track_(track), open_(open), size_(size) {
    if (!open_) return;
    panel_ = &tree_.emplaceRoot<TuningPanel>(ctx, track, wheel);
    tree_.setHost(this);
  }
  ~TuningFace() override { tree_.setHost(nullptr); }

  TuningPanel* panel() const noexcept { return panel_; }
  float layoutScale() const noexcept { return layScale_; }
  // The Wheel button's rect (world units from the block's top-left; none: closed, not laid out yet).
  std::optional<Rect> wheelButton() const noexcept {
    if (panel_ == nullptr || layScale_ == 0.0f) return std::nullopt;
    const ui::IRect r = panel_->wheelButton().rect();
    return Rect{static_cast<float>(r.x) / layScale_, static_cast<float>(r.y) / layScale_, static_cast<float>(r.right()) / layScale_,
                static_cast<float>(r.bottom()) / layScale_};
  }

  // ---- geometry ----
  Vec2 worldSize() const noexcept override { return size_; }
  FaceShape shape() const noexcept override { return {false, {0.0f, 0.0f}, kCorner}; }
  float jackInset() const noexcept override { return 0.0f; }

  // ---- painting ----
  void paintJacks(ui::FacePainter& p, const FaceEnv& env) const override {
    const ui::Theme& th = *env.theme;
    for (const FaceJack& j : env.jacks) {
      if (!j.visible) continue;
      const Vec2 c = j.at;  // on the edge itself: the notes in on the bottom, out on the right
      p.disc(c, 3.4f, th.face.jack);
      p.ring(c, 2.75f, 1.1f, th.face.jackRing);
      p.disc(c, 1.25f, j.connected ? th.circuit.wireNotes : ui::mulAlpha(th.face.jackRing, 0.35f));
    }
  }

  void paintLive(ui::FacePainter& p, const FaceEnv& env) override {
    awaiting_ = 0;
    if (parked_) {  // on screen again: its tree catches up at the next update
      parked_ = false;
      if (host_ != nullptr) host_->faceNeedsUpdate(*this);
    }
    if (env.theme == nullptr) return;
    const ui::Theme& th = *env.theme;
    if (open_) paintOpen(p, env);
    else paintClosed(p, th);
    if (env.selected || env.hovered) {
      const float sc = p.transform().scale;
      const Vec2 o = p.transform().origin;
      const float t = std::max(1.0f, std::round((env.selected ? 2.0f : 1.0f) * env.uiScale));
      p.drawList().addRectStroke({std::round(o.x) - t, std::round(o.y) - t, std::round(o.x + size_.x * sc) + t, std::round(o.y + size_.y * sc) + t},
                                 env.selected ? th.circuit.selected : th.circuit.hover, t);
    }
  }
  void liveRects(std::vector<Rect>& out) const override { out.push_back({0.0f, 0.0f, size_.x, size_.y}); }

  // ---- interaction ----
  int32_t hitTest(Vec2 local, float /*pxPerUnit*/) const noexcept override {
    if (local.x < 0.0f || local.y < 0.0f || local.x > size_.x || local.y > size_.y) return kNone;
    if (!open_ || layScale_ == 0.0f) return kFrame;  // closed: the block (a double-click opens it)
    const ui::Widget* w = tree_.hitTest(toPanel(local));
    // The headers' empty strips and the TUNING label are the block's (a double-click there closes it).
    if (w == nullptr || w == panel_) return kFrame;
    for (const std::string_view n : {"header", "name", "wheel-header", "wheel-side", "chart-box", "circle-side"})
      if (w->name() == n) return kFrame;
    return 0;
  }

  bool onPointer(const FacePointer& e) override {
    if (!open_ || layScale_ == 0.0f) return false;
    ui::PointerEvent pe;
    pe.mods = e.mods;
    pe.clickCount = e.clicks;
    // A drag reads the pointer's movement in device px (at most 1:1 with the panel's px): the slider laid out at
    // kMinScale and drawn smaller moves as it does at its own scale; zoomed in past kMaxScale it follows the pointer.
    auto dragPos = [&](Vec2 device) {
      const float f = downK_ >= 1.0f ? 1.0f / downK_ : 1.0f;
      return Vec2{downPanel_.x + (device.x - downDevice_.x) * f, downPanel_.y + (device.y - downDevice_.y) * f};
    };
    switch (e.kind) {
      case FacePointer::Kind::Down: {
        pe.type = ui::PointerType::Down;
        pe.button = e.button;
        pe.pos = toPanel(e.local);
        quiet_ = true;
        tree_.update(hostNow());  // due timers first, the layout current for the hit test
        quiet_ = false;
        downPanel_ = pe.pos;
        downDevice_ = e.device;
        downK_ = e.pxPerUnit / layScale_;
        if (!tree_.dispatchPointer(pe)) {
          // Nothing took it (a right press): the release goes to the tree at once, the press is the host's (its menu).
          ui::PointerEvent up = pe;
          up.type = ui::PointerType::Up;
          (void)tree_.dispatchPointer(up);
          pressed_ = false;
          return false;
        }
        pressed_ = tree_.captured() != nullptr;
        return true;
      }
      case FacePointer::Kind::Move:
        pe.type = ui::PointerType::Move;
        pe.pos = pressed_ ? dragPos(e.device) : toPanel(e.local);
        (void)tree_.dispatchPointer(pe);
        return tree_.needsPaint();
      case FacePointer::Kind::Up:
        pe.type = ui::PointerType::Up;
        pe.button = e.button;
        pe.pos = pressed_ ? dragPos(e.device) : toPanel(e.local);
        (void)tree_.dispatchPointer(pe);
        pressed_ = false;
        return true;
      case FacePointer::Kind::Leave:
        pe.type = ui::PointerType::Leave;
        (void)tree_.dispatchPointer(pe);
        return tree_.needsPaint();
      case FacePointer::Kind::Wheel: {
        // Over the wheel's chart, the chart's (it zooms its cents, Shift scrolls its rows, Ctrl scales them: as in the
        // popover); elsewhere the canvas's (it pans and zooms).
        const MosChart* chart = panel_ != nullptr ? panel_->chart() : nullptr;
        if (chart == nullptr || !chart->rect().contains(toPanel(e.local))) return false;
        pe.type = ui::PointerType::Wheel;
        pe.pos = toPanel(e.local);
        pe.wheel = e.wheel;
        (void)tree_.dispatchPointer(pe);
        return true;
      }
    }
    return false;
  }
  bool dragging() const noexcept override { return pressed_ && tree_.captured() != nullptr; }
  bool onKey(const ui::KeyEvent& e) override {
    if (!open_ || tree_.focus() == nullptr || e.key == ui::key::kTab) return false;
    const ui::KeyResult r = tree_.dispatchKey(e);
    return r == ui::KeyResult::Widget || r == ui::KeyResult::TextSwallowed;
  }
  bool onText(const ui::TextEvent& e) override { return open_ && tree_.dispatchText(e); }
  bool wantsText() const noexcept override { return open_ && tree_.focus() != nullptr && tree_.focus()->acceptsTextInput(); }
  void focusLost() override {
    if (tree_.focus() != nullptr) tree_.setFocus(nullptr);
  }
  void cancelDrag() override {
    if (pressed_) tree_.cancelInteraction();
    pressed_ = false;
  }
  ui::Cursor cursor(int32_t part) const noexcept override {
    if (!open_ || (part < 0 && !dragging())) return ui::Cursor::Inherit;
    const ui::Cursor c = tree_.cursor();
    return c == ui::Cursor::Arrow ? ui::Cursor::Inherit : c;
  }
  void describe(int32_t part, std::string& title, std::string& body) const override {
    if (open_ && part >= 0) {
      const ui::InfoText t = ui::infoFor(tree_.hovered());
      title.assign(t.title);
      body.assign(t.body);
      if (!title.empty()) return;
    }
    title = "Tuning";
    body = open_ ? "The track's tuning on the MOS circle: drag the hardness slider, step through its modes, type an EDO; Wheel "
                   "opens the region chart beside it. Its degrees light as the track's notes sound. Double-click the header "
                   "to close it."
                 : "The track's tuning: its notes pass through here to the instrument. Double-click to open its MOS circle.";
  }
  bool setHoveredPart(int32_t part) noexcept override {
    bool changed = Face::setHoveredPart(part);
    if (open_ && part == kNone && !pressed_) {
      ui::PointerEvent pe;
      pe.type = ui::PointerType::Leave;
      (void)tree_.dispatchPointer(pe);
      changed = changed || tree_.needsPaint();
    }
    return changed;
  }

  // ---- its own clock ----
  void attach(FaceHost* host) override {
    host_ = host;
    if (host_ != nullptr && panel_ != nullptr && layScale_ > 0.0f) panel_->startNotes();
  }
  bool update(double now) override {
    if (!open_ || parked_) return false;
    // Two repaints asked for and neither painted: off screen (scrolled away, the view hidden). Park until painted.
    if (awaiting_ >= 2) {
      parked_ = true;
      return false;
    }
    quiet_ = true;
    tree_.update(now);
    quiet_ = false;
    const bool dirty = tree_.needsPaint();
    if (dirty) ++awaiting_;
    return dirty;
  }
  double nextWake() const noexcept override {
    if (!open_ || parked_) return std::numeric_limits<double>::infinity();
    if (tree_.framesRequested()) return -std::numeric_limits<double>::infinity();
    return tree_.nextWakeTime();
  }
  bool needsPrepare(float pxPerUnit) const noexcept override {
    if (!open_) return false;
    if (clampScale(pxPerUnit) != layScale_) return true;
    const ui::PaintResources* hr = hostResources();  // a theme reload, another text system: fonts again
    return hr != nullptr && (from_.text != hr->text || from_.theme != hr->theme || from_.tiles != hr->tiles);
  }
  bool prepare(float pxPerUnit) override { return open_ && layoutAt(pxPerUnit); }

private:
  // ui::TreeHost
  ui::Widget* hostWidget() const noexcept override { return host_ != nullptr ? host_->hostWidget() : nullptr; }
  ui::IRect toHost(const ui::IRect& r) const noexcept override {
    const float k = layScale_ > 0.0f ? lastScale_ / layScale_ : 1.0f;
    const float x0 = std::round(lastOrigin_.x + static_cast<float>(r.x) * k), y0 = std::round(lastOrigin_.y + static_cast<float>(r.y) * k);
    const float x1 = std::round(lastOrigin_.x + static_cast<float>(r.right()) * k), y1 = std::round(lastOrigin_.y + static_cast<float>(r.bottom()) * k);
    return {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)};
  }
  void treeNeedsUpdate() noexcept override {
    if (!quiet_ && !parked_ && host_ != nullptr) host_->faceNeedsUpdate(*this);
  }

  static float clampScale(float s) noexcept { return std::clamp(s, kMinScale, kMaxScale); }
  Vec2 toPanel(Vec2 local) const noexcept { return {local.x * layScale_, local.y * layScale_}; }
  const ui::PaintResources* hostResources() const noexcept {
    const ui::Widget* w = hostWidget();
    return w != nullptr && w->tree() != nullptr ? w->tree()->resources() : nullptr;
  }
  double hostNow() const noexcept {
    const ui::Widget* w = hostWidget();
    return w != nullptr && w->tree() != nullptr ? w->tree()->now() : 0.0;
  }

  // Lays the tree out at scale `s` (the theme's fonts resolved at it). False when nothing changed.
  bool layoutAt(float s) {
    s = clampScale(s);
    const ui::PaintResources* hr = hostResources();
    const bool sameRes = hr == nullptr || (from_.text == hr->text && from_.theme == hr->theme && from_.tiles == hr->tiles);
    if (s == layScale_ && sameRes) return false;
    if (hr != nullptr) from_ = *hr;
    res_ = hr != nullptr ? *hr : ui::PaintResources{};
    res_.uiFonts = nullptr;
    ui::UiScale us;
    us.factor = s;  // not UiScale's window range: a canvas zooms past it (fonts stay within the text system's sizes)
    if (res_.text != nullptr && res_.theme != nullptr) {
      // Faces 0 and 1 are the UI's regular and semibold faces (pq/ui/scene_textures.h loads them first, in that order).
      if (auto f = ui::resolveThemeFonts(*res_.text, 0, 1, *res_.theme, us)) {
        fonts_ = *f;
        res_.uiFonts = &fonts_;
      } else {
        res_.text = nullptr;  // no glyphs rather than a font that does not exist
      }
    }
    tree_.setResources(&res_);
    if (ui::Widget* w = hostWidget(); w != nullptr && w->tree() != nullptr) tree_.setClipboard(&w->tree()->clipboard());
    quiet_ = true;
    tree_.setViewport(static_cast<uint32_t>(std::ceil(size_.x * s)), static_cast<uint32_t>(std::ceil(size_.y * s)), us);
    tree_.layoutNow();
    quiet_ = false;
    const bool first = layScale_ == 0.0f;
    layScale_ = s;
    recValid_ = false;
    if (first && host_ != nullptr && panel_ != nullptr) panel_->startNotes();
    return true;
  }

  void record() {
    rec_.reset(tree_.width(), tree_.height());
    quiet_ = true;
    tree_.paint(rec_, res_, hostNow());
    quiet_ = false;
    recValid_ = true;
    recTiles_ = false;
    for (const ui::DrawCmd& c : rec_.commands()) recTiles_ = recTiles_ || c.texture >= ui::kFirstTileTexture;
    recEvictions_ = res_.text != nullptr && res_.text->atlas() ? res_.text->atlas()->evictions() : 0;
  }

  void paintOpen(ui::FacePainter& p, const FaceEnv& env) {
    const float sc = p.transform().scale;
    if (layScale_ == 0.0f) (void)layoutAt(sc);  // the first paint lays out at once (later scales: prepare, settled)
    lastOrigin_ = p.transform().origin;
    lastScale_ = sc;
    if (layScale_ == 0.0f) return;
    const ui::Atlas* glyphs = res_.text != nullptr && res_.text->atlas() ? res_.text->atlas().get() : nullptr;
    // Re-record only when the panel changed (or what the recording samples may have moved: an evicted glyph page).
    if (!recValid_ || tree_.needsPaint() || tree_.framesRequested() || recTiles_ || (glyphs != nullptr && glyphs->evictions() != recEvictions_)) {
      quiet_ = true;
      tree_.layoutNow();
      quiet_ = false;
      record();
    }
    ui::DrawList& dl = p.drawList();
    dl.appendTransformed(rec_, lastOrigin_, sc / layScale_);
    for (const ui::DrawCmd& c : rec_.commands()) {  // the pages the recording samples stay resident this frame
      if (ui::isAtlasPageTexture(c.texture) && res_.text != nullptr && res_.text->atlas()) res_.text->atlas()->touch(c.texture - 1);
      else if (c.texture >= ui::kFirstTileTexture && res_.tiles != nullptr) res_.tiles->touch(c.texture - ui::kFirstTileTexture);
    }
    // The plate's edge over the recording (its corners: the canvas's background shows through).
    const ui::Theme& th = *env.theme;
    outline(p, th.face.frame, 1.0f);
  }

  // A rounded rect's outline (the painter fills rounded rects only).
  void outline(ui::FacePainter& p, Rgba col, float t) const {
    const float W = size_.x, H = size_.y, r = kCorner, h = t * 0.5f;
    constexpr float kPi = 3.14159265f;
    p.line({r, h}, {W - r, h}, t, col);
    p.line({r, H - h}, {W - r, H - h}, t, col);
    p.line({h, r}, {h, H - r}, t, col);
    p.line({W - h, r}, {W - h, H - r}, t, col);
    p.arc({r, r}, r - h, kPi, 1.5f * kPi, t, col);
    p.arc({W - r, r}, r - h, -0.5f * kPi, 0.0f, t, col);
    p.arc({W - r, H - r}, r - h, 0.0f, 0.5f * kPi, t, col);
    p.arc({r, H - r}, r - h, 0.5f * kPi, kPi, t, col);
  }

  // The closed plate: the track's colour on its cap, T-U-N-I-N-G down it, the tuning's short label under them.
  void paintClosed(ui::FacePainter& p, const ui::Theme& th) {
    const float W = size_.x, H = size_.y;
    p.roundedRect({0.0f, 0.0f, W, H}, kCorner, th.face.body);
    const Vec2 sheen[2] = {{1.5f, 1.0f}, {W - 1.5f, 1.0f}};
    p.fillUnder(sheen, H * 0.5f, ui::rgba(255, 255, 255, 14), ui::rgba(255, 255, 255, 0));
    outline(p, th.face.frame, 1.0f);
    const doc::Project& pr = *ctx_.doc();
    const doc::Track* t = doc::findTrackPtr(pr, track_);
    if (t != nullptr) p.roundedRect({3.0f, 3.0f, W - 3.0f, 5.5f}, 1.2f, ui::hex(t->colour.rgb));
    // The letters: the UI's semibold face, stacked and spaced, a faint glow behind the column.
    const ui::PaintResources* res = hostResources();
    ui::FacePainter q = p.at(p.transform());
    q.setText(res != nullptr ? res->text : nullptr, 1);
    constexpr std::string_view kWord = "TUNING";
    constexpr float kTop = 12.0f, kPitch = 8.0f, kPx = 6.5f;
    q.beginGlow();
    q.glow({W * 0.5f, kTop + kPitch * 2.5f}, 16.0f, ui::mulAlpha(th.face.glow, 0.35f));
    q.endGlow();
    for (size_t i = 0; i < kWord.size(); ++i) q.text({W * 0.5f, kTop + kPitch * static_cast<float>(i)}, kWord.substr(i, 1), kPx, th.face.value);
    // Under them (clear of the notes plug on the bottom edge) its tuning's shortest label when it fits ("12", "31").
    const float ly = kTop + kPitch * static_cast<float>(kWord.size()) + 3.0f;
    p.line({4.0f, ly - 5.0f}, {W - 4.0f, ly - 5.0f}, 0.8f, ui::mulAlpha(th.face.ink, 0.45f));
    if (t != nullptr)
      if (const doc::TuningDef* d = targetTuning(pr, TuningTarget::forTrack(track_))) {
        const std::string s = compactTuningLabel(*d);
        if (s.size() <= 3) q.text({W * 0.5f, ly + 1.5f}, s, 5.5f, th.face.ink);
      }
  }

  ViewContext& ctx_;
  doc::TrackId track_;
  bool open_ = false;
  Vec2 size_{0.0f, 0.0f};
  FaceHost* host_ = nullptr;
  ui::UiTree tree_;
  TuningPanel* panel_ = nullptr;
  ui::PaintResources res_{};   // what the tree paints with (the host's, fonts at layScale_)
  ui::PaintResources from_{};  // the host's resources it was built from
  ui::ThemeFonts fonts_{};
  float layScale_ = 0.0f;               // the tree's scale (0: never laid out)
  Vec2 lastOrigin_{0.0f, 0.0f};         // where the last paint put the block (host device px)
  float lastScale_ = 1.0f;              // ... at this many host device px per world unit
  ui::DrawList rec_;                    // the tree's last paint, in panel px
  bool recValid_ = false;
  bool recTiles_ = false;
  uint64_t recEvictions_ = 0;
  bool quiet_ = false;                  // inside our own update / paint: the tree's notifications are ours
  bool pressed_ = false;                // a press went to the tree and it holds the pointer
  Vec2 downPanel_{0.0f, 0.0f}, downDevice_{0.0f, 0.0f};
  float downK_ = 1.0f;
  uint32_t awaiting_ = 0;               // repaints asked for (update) since the last paint
  bool parked_ = false;                 // off screen: no updates, no frames, until painted again
};

}  // namespace

// BR.tuning-outside: an opened block wider than the circle's column is grown to the wheel (the model's only reason).
bool tuningWheelShown(const Node& n) noexcept { return n.kind == NodeKind::Tuning && n.expanded && n.w > metrics::kTuningOpenW + 0.5f; }

std::string tuningFacePath(const Node& n) {
  return std::string(kTuningFacePrefix) + (!n.expanded ? "closed" : tuningWheelShown(n) ? "wheel" : "open");
}

std::unique_ptr<Face> makeTuningFace(ProjectPatchView& view, const ProjectPatch& pp, const Node& n) {
  if (n.kind != NodeKind::Tuning || n.band >= pp.bands.size()) return nullptr;
  return std::make_unique<TuningFace>(view.context(), pp.bands[n.band].track, n.expanded, tuningWheelShown(n), Vec2{n.w, n.h});
}

std::optional<Rect> tuningFaceWheelButton(const Face& f) {
  const auto* t = dynamic_cast<const TuningFace*>(&f);
  return t != nullptr ? t->wheelButton() : std::nullopt;
}

MosChart* tuningFaceChart(Face& f) {
  auto* t = dynamic_cast<TuningFace*>(&f);
  return t != nullptr && t->panel() != nullptr && t->layoutScale() > 0.0f ? t->panel()->chart() : nullptr;
}

MosCircle* tuningFaceCircle(Face& f) {
  auto* t = dynamic_cast<TuningFace*>(&f);
  return t != nullptr && t->panel() != nullptr && t->layoutScale() > 0.0f ? &t->panel()->circle() : nullptr;
}

ui::TextEdit* tuningFaceEdo(Face& f) {
  auto* t = dynamic_cast<TuningFace*>(&f);
  return t != nullptr && t->panel() != nullptr && t->layoutScale() > 0.0f ? &t->panel()->edo() : nullptr;
}

}  // namespace pq::views::project_patch
