#include "pq/views/view_scenes.h"

#include <algorithm>
#include <string>

#include "pq/ui/draw_list.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/document/edit_ops.h"

namespace pq::views {

namespace {

struct SceneEntry {
  std::string_view name;
  uint32_t width, height;  // default logical size
  // Extra setup after the shell is built (nullptr: the shell as loaded).
  Status (*setup)(Shell& shell);
};

ArrangementView* arrangementOf(Shell& shell) {
  ui::Widget* w = shell.slot(ShellSlot::TrackArea).find("arrangement");
  return dynamic_cast<ArrangementView*>(w);
}

// T3.8: the arrangement at its default zoom, the start marker at bar 5 (tests/views/fixtures/arrangement).
Status setupArrangementBasic(Shell& shell) {
  if (!arrangementOf(shell)) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  shell.context().setStartMarker(4 * kTicksPerWhole);
  return Ok{};
}

// T3.8: zoomed out to 14 logical px per 4/4 bar (adaptive grid and previews at a coarse level).
Status setupArrangementZoomedOut(Shell& shell) {
  ArrangementView* a = arrangementOf(shell);
  if (!a) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  a->setTimeView(0.0, 14.0 / static_cast<double>(kTicksPerWhole));
  return Ok{};
}

// T3.9: a time selection dragged over Piano … Strings (bars 5–7; it selects the clips it touches), then those clips
// Alt-dragged (copied) two rows up and two bars later, the drag still in progress: the selected clips' outline, the
// selected rows, the time selection and the drag preview in one picture.
Status setupArrangementSelection(Shell& shell) {
  ArrangementView* a = arrangementOf(shell);
  if (!a) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  ui::UiTree& tree = shell.tree();
  tree.layoutNow();
  int32_t piano = -1, strings = -1, bass = -1;
  const auto rows = a->rows();
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].track->name == "Piano") piano = static_cast<int32_t>(i);
    if (rows[i].track->name == "Strings") strings = static_cast<int32_t>(i);
    if (rows[i].track->name == "Bass") bass = static_cast<int32_t>(i);
  }
  if (piano < 0 || strings < 0 || bass < 0) return Error{ErrorCode::NotFound, "arrangement-selection: needs the arrangement fixture", {}};
  auto send = [&](ui::PointerType t, ui::Vec2 p, ui::Mods m) {
    ui::PointerEvent e;
    e.type = t;
    e.pos = p;
    e.mods = m;
    e.button = (t == ui::PointerType::Down || t == ui::PointerType::Up) ? ui::PointerButton::Left : ui::PointerButton::None;
    e.clickCount = 1;
    tree.dispatchPointer(e);
  };
  const double bar = static_cast<double>(kTicksPerWhole);
  auto bodyY = [&](int32_t row) { const ui::IRect r = a->rowLaneRect(row); return static_cast<float>(r.bottom() - 8); };
  auto titleY = [&](int32_t row) { return static_cast<float>(a->rowLaneRect(row).y + 7); };
  const ui::Vec2 from{a->xOfTick(4.0 * bar) + 9.0f, bodyY(piano)};  // clear of the clip edge (that resizes)
  send(ui::PointerType::Move, from, ui::mod::kNone);
  send(ui::PointerType::Down, from, ui::mod::kNone);
  send(ui::PointerType::Move, {a->xOfTick(5.0 * bar), bodyY(piano)}, ui::mod::kNone);
  send(ui::PointerType::Move, {a->xOfTick(6.0 * bar) + 2.0f, bodyY(strings)}, ui::mod::kNone);
  send(ui::PointerType::Up, {a->xOfTick(6.0 * bar) + 2.0f, bodyY(strings)}, ui::mod::kNone);
  const ui::Vec2 grab{a->xOfTick(4.5 * bar), titleY(piano)};
  send(ui::PointerType::Move, grab, ui::mod::kAlt);
  send(ui::PointerType::Down, grab, ui::mod::kAlt);
  send(ui::PointerType::Move, {a->xOfTick(5.5 * bar), titleY(bass)}, ui::mod::kAlt);
  send(ui::PointerType::Move, {a->xOfTick(6.5 * bar), titleY(bass)}, ui::mod::kAlt);
  if (!a->dragging()) return Error{ErrorCode::Unknown, "arrangement-selection: the drag did not start", {}};
  return Ok{};
}

// T3.11: the first clip of the project in the clip view, the detail view raised so the roll is the main subject.
Status openFirstClip(Shell& shell) {
  ViewContext& ctx = shell.context();
  const doc::Project& p = *ctx.doc();
  doc::ClipRef ref{};
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (!ref.clip.valid() && t.kind == doc::TrackKind::Midi && !t.clips.empty()) ref = {t.id, t.clips.front().id};
  });
  if (!ref.clip.valid()) return Error{ErrorCode::NotFound, "pianoroll scene: the project has no MIDI clip", {}};
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(520.0f);
  ctx.setDetailClip(ref);
  return Ok{};
}

Status pianoRollVelocity(Shell& shell) {
  if (Status s = openFirstClip(shell); !s) return s;
  // Every fourth note selected, to show selected stems.
  auto* view = dynamic_cast<ClipView*>(shell.tree().find("main/body/bottom/detail/clip/clip-view"));
  if (!view) return Error{ErrorCode::NotFound, "pianoroll-velocity: no clip view", {}};
  const doc::Project& p = *shell.context().doc();
  const doc::Track* t = doc::findTrackPtr(p, view->clip().track);
  const int32_t ci = t ? doc::findClipIndex(*t, view->clip().clip) : -1;
  if (ci < 0) return Error{ErrorCode::NotFound, "pianoroll-velocity: clip not found", {}};
  std::vector<doc::NoteId> ids;
  size_t i = 0;
  for (const doc::Note& n : t->clips[static_cast<size_t>(ci)].clip->notes)
    if (i++ % 4 == 2) ids.push_back(n.id);
  view->select(ids);
  return Ok{};
}

// T3.12: scale / MOS layouts over tests/views/fixtures/pianoroll-5L2s (… -near-collapse, pianoroll-scala).
ClipView* openScaleClip(Shell& shell) {
  if (!openFirstClip(shell)) return nullptr;
  return dynamic_cast<ClipView*>(shell.tree().find("main/body/bottom/detail/clip/clip-view"));
}

Status pianoRollProportional(Shell& shell) {
  if (!openScaleClip(shell)) return Error{ErrorCode::NotFound, "pianoroll scale scene: no clip view", {}};
  return Ok{};
}

Status pianoRollEquidistant(Shell& shell) {
  ClipView* v = openScaleClip(shell);
  if (!v) return Error{ErrorCode::NotFound, "pianoroll-5L2s-equi: no clip view", {}};
  v->setScaleLayout(roll::RowLayout::Equidistant);
  return Ok{};
}

Status pianoRollOver31(Shell& shell) {
  ClipView* v = openScaleClip(shell);
  if (!v) return Error{ErrorCode::NotFound, "pianoroll-5L2s-over-31edo: no clip view", {}};
  v->setOverlayEdo(31);
  return Ok{};
}

// T3.13: the MOS region chart alone, filling the window: opened in the shell's overlay layer (not dismissable), so
// the scene is the chart widget exactly as a popover would host it.
MosChart* openMosChart(Shell& shell, const MosChartConfig& config) {
  ui::OverlayLayer& layer = shell.overlay();
  ui::UiTree* tree = layer.tree();
  if (!tree) return nullptr;
  auto chart = std::make_unique<MosChart>("mos-chart", config);
  const float sc = tree->scale();
  chart->setSize(ui::Length::fixed(static_cast<float>(tree->width()) / sc), ui::Length::fixed(static_cast<float>(tree->height()) / sc));
  ui::OverlayOptions o;
  o.placement = ui::Placement::AtPoint;
  o.dismissOnOutsideClick = false;
  o.takeFocus = false;
  auto& w = static_cast<MosChart&>(layer.open(std::move(chart), std::move(o)));
  tree->layoutNow();
  return &w;
}

// T3.13: 5L 2s selected at 7\12 (700 ¢), EDO markers up to 31, the whole octave.
Status setupMosChartDefault(Shell& shell) {
  MosChartConfig cfg;
  cfg.maxEdo = 31;
  MosChart* c = openMosChart(shell, cfg);
  if (!c) return Error{ErrorCode::NotFound, "no overlay layer for the MOS chart", {}};
  if (!c->select({5, 2}, 700.0)) return Error{ErrorCode::NotFound, "5L 2s not in the chart", {}};
  return Ok{};
}

// T3.13: zoomed to 655–745 ¢ around 5L 2s (generator 18\31), EDO markers and temperament ranges legible, the pointer
// resting on 7L 2s (size 9) so the hover card shows.
Status setupMosChartZoomed(Shell& shell) {
  MosChartConfig cfg;
  cfg.maxEdo = 31;
  MosChart* c = openMosChart(shell, cfg);
  if (!c) return Error{ErrorCode::NotFound, "no overlay layer for the MOS chart", {}};
  c->setView(655.0, 745.0);
  if (!c->select({5, 2}, 18.0 * 1200.0 / 31.0)) return Error{ErrorCode::NotFound, "5L 2s not in the chart", {}};
  ui::UiTree& tree = *shell.overlay().tree();
  tree.layoutNow();
  const ui::IRect row = c->rowRect(9);
  if (row.empty()) return Error{ErrorCode::NotFound, "row 9 not visible", {}};
  ui::PointerEvent e;
  e.type = ui::PointerType::Move;
  e.pos = {c->xOfCents(676.0), static_cast<float>(row.y + row.h / 2)};
  tree.dispatchPointer(e);
  return Ok{};
}

// T3.14: the MOS circle alone, filling the window (opened like the chart above). The selection comes from a chart
// built off-tree (the circle takes the chart's MosSelection, as in the popover).
MosCircle* openMosCircle(Shell& shell, tuning::MosKey key, double gen, int32_t rotation, std::vector<double> modmos) {
  ui::OverlayLayer& layer = shell.overlay();
  ui::UiTree* tree = layer.tree();
  if (!tree) return nullptr;
  MosChartConfig cfg;
  cfg.maxEdo = 31;
  MosChart chart("chart", cfg);
  if (!chart.select(key, gen, rotation)) return nullptr;
  auto circle = std::make_unique<MosCircle>("mos-circle", cfg);
  circle->setSelection(chart.selection());
  circle->setModmos(std::move(modmos));
  const float sc = tree->scale();
  circle->setSize(ui::Length::fixed(static_cast<float>(tree->width()) / sc), ui::Length::fixed(static_cast<float>(tree->height()) / sc));
  ui::OverlayOptions o;
  o.placement = ui::Placement::AtPoint;
  o.dismissOnOutsideClick = false;
  o.takeFocus = false;
  auto& w = static_cast<MosCircle&>(layer.open(std::move(circle), std::move(o)));
  tree->layoutNow();
  return &w;
}

// T3.14: 5L 2s at 18\31 (meantone), mode rotation 0 (Lydian-side brightest root), the whole wheel.
Status setupMosCircle5L2s(Shell& shell) {
  if (!openMosCircle(shell, {5, 2}, 18.0 * 1200.0 / 31.0, 0, {}))
    return Error{ErrorCode::NotFound, "mos-circle-5L2s: no overlay or no 5L 2s region", {}};
  return Ok{};
}

// T3.14: 5L 2s at 7\12 in the Aeolian mode (rotation 2) with its 7th degree raised one chroma (harmonic minor:
// the explorer's "#" degree, the green step pattern, the 300-cent step "M", the reset button).
Status setupMosCircleModmos(Shell& shell) {
  if (!openMosCircle(shell, {5, 2}, 700.0, 2, {0, 0, 0, 0, 0, 0, 1}))
    return Error{ErrorCode::NotFound, "mos-circle-modmos: no overlay or no 5L 2s region", {}};
  return Ok{};
}

// T3.14: a MIDI track with a 12-EDO melody in the clip view and the MOS wheel open for that clip (from the clip box's
// tuning chip target), 5L 2s at 18\31 chosen: the roll behind shows the live preview (7 rows per octave).
Status setupMosWheelPopover(Shell& shell) {
  ViewContext& ctx = shell.context();
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) -> std::optional<doc::ops::Edit> {
    if (!e.ok() || !store.dispatch(e->command).ok()) return std::nullopt;
    return *e;
  };
  const auto t = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Lead"));
  if (!t) return Error{ErrorCode::Unknown, "mos-wheel-popover: createTrack failed", {}};
  const auto c = run(doc::ops::insertEmptyClip(*store.current(), store.ids(), t->tracks[0], 0, 2 * kTicksPerWhole, "Melody"));
  if (!c) return Error{ErrorCode::Unknown, "mos-wheel-popover: insertEmptyClip failed", {}};
  std::vector<doc::Note> notes;
  const int32_t degrees[] = {0, 2, 4, 5, 7, 9, 11, 12, 11, 9, 7, 5, 4, 2, 0, 7};
  for (size_t i = 0; i < std::size(degrees); ++i) {
    doc::Note n;
    n.start = static_cast<Ticks>(i) * (kTicksPerWhole / 8);
    n.length = kTicksPerWhole / 8;
    n.pitch = tuning::PitchId::fromDegree(degrees[i] % 12, degrees[i] / 12);
    n.velocity = 100.0f;
    notes.push_back(n);
  }
  if (!run(doc::ops::addNotes(*store.current(), store.ids(), c->clips[0], notes)))
    return Error{ErrorCode::Unknown, "mos-wheel-popover: addNotes failed", {}};
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(640.0f);
  ctx.setDetailClip(c->clips[0]);
  shell.tree().layoutNow();
  const ui::UiScale s(shell.tree().scale());
  const ui::IRect anchor{static_cast<int32_t>(shell.tree().width()) - s.px(MosWheelPopover::kWidth + 12.0f), s.px(36.0f), 1, 1};
  MosWheelPopover* w = openMosWheel(shell.overlay(), ctx, TuningTarget::forClip(c->clips[0]), anchor);
  if (!w) return Error{ErrorCode::NotFound, "mos-wheel-popover: no overlay layer", {}};
  if (!w->chooseMos({5, 2}, 18.0 * 1200.0 / 31.0, 1)) return Error{ErrorCode::NotFound, "mos-wheel-popover: 5L 2s", {}};
  return Ok{};
}

// T3.10: fixed meter levels for the mixer scenes, by track name (a static source: the scene renders once).
Status setupMixerCommon(Shell& shell) {
  MixerView* mixer = findMixer(shell);
  ArrangementView* a = arrangementOf(shell);
  if (!mixer || !a) return Error{ErrorCode::NotFound, "mixer scenes: the arrangement and the mixer must be installed", {}};
  static FixedMeterSource meters;
  meters = FixedMeterSource{};
  auto lv = [](float rms, float peak, bool clip = false) {
    ui::MeterLevels l;
    l.rmsDb[0] = rms;
    l.rmsDb[1] = rms - 2.0f;
    l.peakDb[0] = peak;
    l.peakDb[1] = peak - 1.5f;
    l.holdDb[0] = peak + 1.0f;
    l.holdDb[1] = peak;
    l.clip[0] = clip;
    return l;
  };
  struct Level {
    const char* name;
    float rms, peak;
    bool clip;
  };
  const Level levels[] = {{"Drums", -14.0f, -6.0f, false}, {"Bass", -20.0f, -12.0f, false}, {"Keys", -16.0f, -9.0f, false},
                          {"Piano", -18.0f, -10.0f, false}, {"Lead", -9.0f, -2.0f, true},   {"A Reverb", -26.0f, -18.0f, false},
                          {"B Delay", -30.0f, -22.0f, false}, {"Master", -8.0f, -1.0f, false}};
  doc::forEachTrack(*shell.context().doc(), [&](const doc::Track& t, const doc::Track*) {
    for (const Level& l : levels)
      if (t.name == l.name) meters.set(t.id, lv(l.rms, l.peak, l.clip));
  });
  mixer->setMeterSource(&meters);
  // Piano selected (its header tinted), as after a click on its title.
  for (const ArrangementRow& r : a->rows())
    if (r.track->name == "Piano") a->selectTrack(r.id);
  return Ok{};
}

// T3.10: the arrangement's headers with their mixer sections, returns and master pinned at the bottom
// (tests/views/fixtures/mixer).
Status setupTracksMixer(Shell& shell) { return setupMixerCommon(shell); }

// T3.10: the mixer view (Ctrl+Alt+M) over the same project.
Status setupMixerView(Shell& shell) {
  if (Status s = setupMixerCommon(shell); !s) return s;
  findMixer(shell)->setShown(true);
  findMixer(shell)->wakeMeters();
  return Ok{};
}

// One entry per scene. T3.8+ append theirs (arrangement-basic, pianoroll-31edo, mos-chart-default, …).
constexpr SceneEntry kScenes[] = {
    {"shell", 1280, 800, nullptr},
    {"arrangement-basic", 1280, 800, setupArrangementBasic},
    {"arrangement-zoomed-out", 1280, 800, setupArrangementZoomedOut},
    {"arrangement-selection", 1280, 800, setupArrangementSelection},
    {"pianoroll-12edo", 1280, 800, openFirstClip},
    {"pianoroll-31edo", 1280, 800, openFirstClip},
    {"pianoroll-velocity", 1280, 800, pianoRollVelocity},
    {"pianoroll-5L2s-prop", 1280, 800, pianoRollProportional},
    {"pianoroll-5L2s-equi", 1280, 800, pianoRollEquidistant},
    {"pianoroll-5L2s-near-collapse", 1280, 800, pianoRollProportional},
    {"pianoroll-5L2s-over-31edo", 1280, 800, pianoRollOver31},
    {"pianoroll-scala", 1280, 800, pianoRollProportional},
    {"mos-chart-default", 800, 600, setupMosChartDefault},
    {"mos-chart-zoomed", 800, 600, setupMosChartZoomed},
    {"mos-circle-5L2s", 440, 560, setupMosCircle5L2s},
    {"mos-circle-modmos", 440, 560, setupMosCircleModmos},
    {"mos-wheel-popover", 1600, 900, setupMosWheelPopover},
    {"tracks-mixer", 1280, 800, setupTracksMixer},
    {"mixer-view", 1280, 800, setupMixerView},
};

std::string_view kNames[std::size(kScenes)] = {};

const SceneEntry* findScene(std::string_view name) noexcept {
  for (const SceneEntry& e : kScenes)
    if (e.name == name) return &e;
  return nullptr;
}

}  // namespace

std::span<const std::string_view> viewSceneNames() noexcept {
  for (size_t i = 0; i < std::size(kScenes); ++i) kNames[i] = kScenes[i].name;
  return kNames;
}

bool isViewScene(std::string_view scene) noexcept { return findScene(scene) != nullptr; }

void viewSceneDefaultSize(std::string_view scene, uint32_t& width, uint32_t& height) noexcept {
  const SceneEntry* e = findScene(scene);
  width = e ? e->width : 1280;
  height = e ? e->height : 800;
}

Result<std::unique_ptr<Shell>> buildViewScene(std::string_view scene, ui::UiTree& tree, ViewContext& ctx) {
  const SceneEntry* e = findScene(scene);
  if (!e) return Error{ErrorCode::NotFound, "unknown view scene '" + std::string(scene) + "'", {}};
  auto shell = Shell::create(tree, ctx);
  if (!shell) return std::move(shell).error();
  if (e->setup)
    if (Status s = e->setup(**shell); !s) return std::move(s).error();
  tree.layoutNow();
  return shell;
}

Result<ui::Image> renderViewScene(std::string_view scene, std::string_view projectDir, uint32_t width, uint32_t height,
                                  ui::UiScale scale, const ui::GpuOptions& options) {
  if (!isViewScene(scene)) return Error{ErrorCode::NotFound, "unknown view scene '" + std::string(scene) + "'", {}};
  if (width == 0 || height == 0) return Error{ErrorCode::InvalidArgument, "renderViewScene: zero size", {}};
  FakeTransport transport;
  ViewContext ctx(transport);
  if (!projectDir.empty())
    if (Status s = ctx.openProject(projectDir); !s) return std::move(s).error();
  ctx.setStatus({});

  auto gpu = ui::GpuContext::create(options);
  if (!gpu) return std::move(gpu).error();
  auto renderer = ui::Renderer::create(**gpu, ui::OffscreenTarget::kFormat, 1);
  if (!renderer) return std::move(renderer).error();
  const auto dw = static_cast<uint32_t>(std::max(1, scale.px(static_cast<float>(width))));
  const auto dh = static_cast<uint32_t>(std::max(1, scale.px(static_cast<float>(height))));
  auto target = ui::OffscreenTarget::create(**gpu, dw, dh);
  if (!target) return std::move(target).error();
  auto textures = ui::createSceneTextures(**renderer, scale);
  if (!textures) return std::move(textures).error();
  const ui::PaintResources res = ui::paintResources(*textures);

  ui::UiTree tree;
  tree.setViewport(dw, dh, scale);
  tree.setResources(&res);
  auto shell = buildViewScene(scene, tree, ctx);
  if (!shell) return std::move(shell).error();
  // Settle before the screenshot: run the frames and timers that change what is drawn (progressive work such as the
  // arrangement's note previews, a zoom settling), as the window would within its first frames.
  double now = 0.0;
  tree.update(now);
  ui::DrawList dl;
  for (int i = 0; i < 256 && (tree.needsFrame() || tree.nextWakeTime() <= now + 1.0); ++i) {
    dl.reset(dw, dh);
    textures->text->beginFrame();
    tree.paint(dl, res, now);
    now = std::max(now + 1.0 / 60.0, std::min(tree.nextWakeTime(), now + 1.0));
    tree.update(now);
  }
  dl.reset(dw, dh);
  textures->text->beginFrame();
  tree.paint(dl, res, now);
  if (Status s = (*target)->draw(**renderer, dl, ui::kSceneClearColor); !s) return std::move(s).error();
  auto img = (*target)->read();
  if (img && (*gpu)->validationErrorCount() != 0)
    return Error{ErrorCode::Device, std::to_string((*gpu)->validationErrorCount()) + " Vulkan validation error(s)", {}};
  return img;  // the shell and the tree are destroyed before the context they subscribe to (declaration order)
}

}  // namespace pq::views
