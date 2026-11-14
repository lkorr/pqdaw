#include "pq/views/view_scenes.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pq/dsp/kernel.h"
#include "pq/graph/inspect.h"
#include "pq/graph/types.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/media/probe.h"
#include "pq/meta/envelope.h"
#include "pq/document/modules.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/browser/browser_panel.h"
#include "pq/views/browser/demo_index.h"
#include "pq/views/browser/library.h"
#include "pq/views/circuit/node_menu.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/devices/analysis.h"
#include "pq/document/modules.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/export/export_window.h"
#include "pq/views/fake_transport.h"
#include "pq/views/meta/face_targets.h"
#include "meta/faces.h"  // TX.spectrogram: the face's analysis request (gramRequest)
#include "pq/views/mixer/mixer.h"
#include "pq/views/piano_roll/clip_view.h"
#include "piano_roll/roll_state.h"  // T7.4: the JI scenes create objects as the JI tool does
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "project_patch/internal.h"
#include "pq/views/tunings/tunings_panel.h"
#include "pq/document/audio_clip.h"
#include "pq/document/devices.h"
#include "pq/document/lower.h"
#include "pq/document/edit_ops.h"
#include "pq/document/racks.h"
#include "pq/document/main_path.h"
#include "shell/shell_widgets.h"

namespace pq::views {

namespace {

// GU.4: the scenes show each built-in module (a Phase Accumulator, a VCA: a group preset, open by default in the view)
// closed — one block drawn as its panel, where the old meta faces were.
void closeModules(project_patch::ProjectPatchView& v) {
  std::vector<doc::DeviceId> modules;
  doc::forEachTrack(*v.context().doc(), [&](const doc::Track& t, const doc::Track*) {
    for (const doc::DevicePtr& d : t.devices)
      if (doc::isGroupDevice(*d) && doc::findModule(d->name) != nullptr) modules.push_back(d->id);
  });
  for (const doc::DeviceId g : modules) v.setGroupOpen(g, false);
}
// TMM.13-fix2 (MM review "UI and aesthetic" 8): synthetic Phase accumulator voices play an A major chord (A3 C#4 E4
// A4 E3 C#5): increments at 48 kHz, so each voice is drawn in the colour of a real note (they were all 0 Hz: red).
constexpr double kSceneRate = 48000.0;
constexpr double kSceneNoteInc[] = {220.0 / kSceneRate, 277.183 / kSceneRate, 329.628 / kSceneRate,
                                    440.0 / kSceneRate, 164.814 / kSceneRate, 554.365 / kSceneRate};

struct SceneEntry {
  std::string_view name;
  uint32_t width, height;  // default logical size
  // Extra setup after the shell is built (nullptr: the shell as loaded).
  Status (*setup)(Shell& shell);
  // T4.2: scenes over a patch file instead of a project (renderViewScene's `projectDir` is then the .pqp path).
  Status (*setupPatch)(Shell& shell, std::string_view patchFile) = nullptr;
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

// T3.15 helpers: a track of the automation fixture by name, and its lane / row geometry.
const doc::Track* namedTrack(const doc::Project& p, std::string_view name) {
  const doc::Track* found = nullptr;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (!found && t.name == name) found = &t;
  });
  return found;
}

// The arrangement of the automation fixture prepared for a screenshot: the detail view hidden (the tracks are the
// subject), the 8 bars filling the lanes.
Status prepareAutomationScene(Shell& shell, ArrangementView*& a, const doc::Track*& drums, const doc::Track*& bass,
                              const doc::Track*& piano, const doc::Track*& pad, const doc::Track*& lead) {
  a = arrangementOf(shell);
  if (!a) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  ViewContext& ctx = shell.context();
  const doc::Project& p = *ctx.doc();
  drums = namedTrack(p, "Drums");
  bass = namedTrack(p, "Bass");
  piano = namedTrack(p, "Piano");
  pad = namedTrack(p, "Pad");
  lead = namedTrack(p, "Lead");
  if (!drums || !bass || !piano || !pad || !lead) return Error{ErrorCode::NotFound, "automation scenes: need the automation fixture", {}};
  ctx.setDetailVisible(false);
  shell.tree().layoutNow();
  const float sc = shell.tree().scale();
  a->setTimeView(0.0, static_cast<double>(a->lanesViewport().w) / sc / (8.25 * static_cast<double>(kTicksPerWhole)));
  return Ok{};
}

// T3.15: lanes under Drums (volume: curves and a jump), Bass (pan, overridden by hand: grey, "Re-enable Automation"
// shows), Piano (the 5L 2s generator), Pad (volume, not automated: the dashed value) and Lead (volume, send A,
// activator); a time selection over bars 2–4 of the drums' volume with its breakpoints selected.
Status setupAutomationLanes(Shell& shell) {
  ArrangementView* a = nullptr;
  const doc::Track *drums = nullptr, *bass = nullptr, *piano = nullptr, *pad = nullptr, *lead = nullptr;
  if (Status s = prepareAutomationScene(shell, a, drums, bass, piano, pad, lead); !s) return s;
  ViewContext& ctx = shell.context();
  ArrangementAutomation& au = a->automation();
  for (const doc::Track* t : {drums, bass, piano, pad, lead}) au.showLanes(t->id, true);
  ctx.overrideAutomation(doc::ParamRef::track(doc::ParamKind::TrackPan, bass->id));
  shell.tree().layoutNow();
  const doc::ParamRef vol = doc::ParamRef::track(doc::ParamKind::TrackVolume, drums->id);
  const TickRange range{kTicksPerWhole, 3 * kTicksPerWhole};
  std::vector<doc::BreakpointId> ids;
  for (const doc::LanePtr& l : drums->lanes)
    if (l->param == vol)
      for (const doc::Breakpoint& b : l->points)
        if (b.tick >= range.start && b.tick <= range.end) ids.push_back(b.id);
  au.select(drums->id, vol, std::move(ids), range);
  return Ok{};
}

// T3.15: automation mode (A): every track's chosen envelope over its clips (Drums volume, Bass pan overridden: grey,
// the Keys group's pan, Piano's 5L 2s generator, Pad's volume not automated: dashed, Lead volume), the header
// choosers, and a breakpoint of Lead's volume being dragged (the value readout).
Status setupAutomationMode(Shell& shell) {
  ArrangementView* a = nullptr;
  const doc::Track *drums = nullptr, *bass = nullptr, *piano = nullptr, *pad = nullptr, *lead = nullptr;
  if (Status s = prepareAutomationScene(shell, a, drums, bass, piano, pad, lead); !s) return s;
  ViewContext& ctx = shell.context();
  ArrangementAutomation& au = a->automation();
  au.setMode(true);
  ctx.overrideAutomation(doc::ParamRef::track(doc::ParamKind::TrackPan, bass->id));
  ui::UiTree& tree = shell.tree();
  tree.layoutNow();
  const doc::ParamRef vol = doc::ParamRef::track(doc::ParamKind::TrackVolume, lead->id);
  const std::optional<AutomationBand> band = au.band(lead->id, vol, true);
  if (!band || !band->lane || band->lane->points.size() < 2) return Error{ErrorCode::NotFound, "automation-mode-over-clips: Lead's volume band", {}};
  const std::optional<ui::Vec2> at = au.pointPos(*band, band->lane->points[1].id);  // the point at bar 4
  if (!at) return Error{ErrorCode::NotFound, "automation-mode-over-clips: breakpoint", {}};
  auto send = [&](ui::PointerType t, ui::Vec2 pos) {
    ui::PointerEvent e;
    e.type = t;
    e.pos = pos;
    e.button = (t == ui::PointerType::Down || t == ui::PointerType::Up) ? ui::PointerButton::Left : ui::PointerButton::None;
    e.clickCount = 1;
    tree.dispatchPointer(e);
  };
  send(ui::PointerType::Move, *at);
  send(ui::PointerType::Down, *at);
  const float bar = a->xOfTick(static_cast<double>(kTicksPerWhole)) - a->xOfTick(0.0);
  send(ui::PointerType::Move, {at->x + bar * 0.25f, at->y + 4.0f});
  send(ui::PointerType::Move, {at->x + bar * 0.5f, at->y + 8.0f});
  if (!au.dragging()) return Error{ErrorCode::Unknown, "automation-mode-over-clips: the drag did not start", {}};
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

// TU.2: 5L 2s at 18\31 with four notes sounding (held through the settle frames: full glow), each lit in the colour
// of its frequency: the tonic at 130 Hz (violet), degree 2 at 330 Hz (blue), degree 4 at 620 Hz (cyan), degree 5 an
// octave up at 1400 Hz (yellow).
Status setupMosCircleLit(Shell& shell) {
  MosCircle* c = openMosCircle(shell, {5, 2}, 18.0 * 1200.0 / 31.0, 0, {});
  if (!c) return Error{ErrorCode::NotFound, "mos-circle-lit: no overlay or no 5L 2s region", {}};
  c->noteOn(1, 0, 130.0);
  c->noteOn(2, 2, 330.0);
  c->noteOn(3, 4, 620.0);
  c->noteOn(4, 7 + 5, 1400.0);
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
  // TU.3: the clip on its own named tuning (what the wheel then edits), as "New Tuning" in its chip's menu leaves it.
  const auto tb = run(doc::ops::addMainTuning(*store.current(), store.ids(), "Tuning B", doc::TuningDef{}, doc::Colour{0x8FC24A}));
  if (!tb || !store.dispatch(doc::SetClipTuning{c->clips[0], doc::TuningAssignment::mainTuning(tb->tunings[0])}).ok())
    return Error{ErrorCode::Unknown, "mos-wheel-popover: the clip's tuning failed", {}};
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

// T3.18: the preferences window (Audio tab) over the shell, from a scripted device list (FakeAudioPrefsPort::studio:
// a Focusrite-like interface with ASIO and WASAPI endpoints). The port is static: the scene renders once.
Status openAudioPrefs(Shell& shell, bool wasapi) {
  static FakeAudioPrefsPort port;
  port = FakeAudioPrefsPort::studio();
  if (wasapi) {
    AudioSettings s = port.current;
    s.driver = AudioDriverType::WasapiExclusive;
    s.outputDevice = "{0.0.0.00000000}.{a}";
    s.outputName = "Speakers (Focusrite USB Audio)";
    s.inputDevice.clear();
    s.bufferFrames = 192;
    port.apply(s);
    port.applied.clear();
    port.running.deviceName = s.outputName;
    port.running.inputChannels = 0;  // no input device: no inputs open
    port.running.inputLatencyMs = 0.0;
    port.running.outputLatencyMs = 8.0;
    port.running.state = AudioStatus::State::Suspended;
    port.load = 0.0;
  }
  shell.context().setAudioPrefs(&port);
  if (!openPreferences(shell.overlay(), port, &shell.context())) return Error{ErrorCode::NotFound, "audio-prefs: no overlay layer", {}};
  return Ok{};
}
Status setupAudioPrefs(Shell& shell) { return openAudioPrefs(shell, false); }
Status setupAudioPrefsWasapi(Shell& shell) { return openAudioPrefs(shell, true); }

// T5.x-library-prefs: the Library tab (a moved User Library, three added folders, the second selected).
Status setupLibraryPrefs(Shell& shell) {
  Library& lib = shell.context().library();
  lib.setUserLibrary("D:\\Music\\pqdaw User Library");
  lib.setFolders({"C:\\Samples\\Drums", "D:\\Splice\\sounds", "E:\\Field Recordings\\2026"});
  if (Status s = openAudioPrefs(shell, false); !s) return s;
  PreferencesWindow* w = openPreferencesWindow(shell.overlay());
  if (!w) return Error{ErrorCode::NotFound, "library-prefs: no window", {}};
  w->showTab(PreferencesWindow::Tab::Library);
  w->selectFolder(1);
  return Ok{};
}

// T6.4: the MIDI tab (three ports: one on, one off, one remembered but not connected).
Status setupMidiPrefs(Shell& shell) {
  if (Status s = openAudioPrefs(shell, false); !s) return s;
  PreferencesWindow* w = openPreferencesWindow(shell.overlay());
  if (!w) return Error{ErrorCode::NotFound, "midi-prefs: no window", {}};
  w->showTab(PreferencesWindow::Tab::Midi);
  return Ok{};
}

// M6 review L22: the Warp tab (T6.8) and the Record tab (T6.7b) at their defaults.
Status setupPrefsTab(Shell& shell, PreferencesWindow::Tab tab, const char* scene) {
  if (Status s = openAudioPrefs(shell, false); !s) return s;
  PreferencesWindow* w = openPreferencesWindow(shell.overlay());
  if (!w) return Error{ErrorCode::NotFound, std::string(scene) + ": no window", {}};
  w->showTab(tab);
  return Ok{};
}
Status setupWarpPrefs(Shell& shell) { return setupPrefsTab(shell, PreferencesWindow::Tab::Warp, "warp-prefs"); }
Status setupRecordPrefs(Shell& shell) { return setupPrefsTab(shell, PreferencesWindow::Tab::Record, "record-prefs"); }

// EX.2: the Export Audio window over a scripted port: All Individual Tracks, Song, 24-bit TPDF, normalized, a fixed
// folder (the golden must not show the machine's paths). Over a project with clips (the goldens pass the arrangement
// fixture) the range readout shows the song's length.
Status setupExportAudio(Shell& shell) {
  static FakeExportPort port;
  port = FakeExportPort{};
  ViewContext& ctx = shell.context();
  ctx.setExportPort(&port);
  ViewPrefs v = ctx.viewPrefs();
  v.exportAudio = {};
  v.exportAudio.tracks = ExportTracks::AllTracks;
  v.exportAudio.normalize = true;
  v.exportAudio.folder = "D:\\Music\\Exports";
  ctx.setViewPrefs(v);
  if (!openExportWindow(shell.overlay(), port, ctx, {})) return Error{ErrorCode::NotFound, "export-audio: no overlay layer", {}};
  return Ok{};
}

// ---- T3.19: menu bar, Tunings panel, ratio notes ------------------------------------------------------------------

// T3.19: the menu bar's Create menu open over the empty project (Insert MIDI / Return Track, Insert MIDI Clip, New
// Main Tuning).
Status setupMenuBar(Shell& shell) {
  auto* bar = dynamic_cast<pq::views::shell::MenuBar*>(shell.tree().find("main/menu-bar"));
  if (!bar) return Error{ErrorCode::NotFound, "menu-bar: no menu bar", {}};
  shell.tree().layoutNow();
  if (!bar->open(2)) return Error{ErrorCode::NotFound, "menu-bar: no overlay layer", {}};
  return Ok{};
}

namespace {

// Dispatches an edit; nullopt (and nothing changed) on failure.
// GU.11: the block named `name` inside group `g` (a module's value block: "Drive", "Width" …), or none.
doc::DeviceId memberNamed(const doc::Project& p, doc::DeviceId g, std::string_view name) {
  const doc::DeviceLoc l = doc::findDevice(p, g);
  if (l.track == nullptr) return {};
  for (const doc::Device* d : doc::groupMembersDeep(l.track->devices, g))
    if (d->name == name) return d->id;
  return {};
}
// GU.11: the block heading track `t`'s main path (its instrument group on a MIDI track), or none.
doc::DeviceId pathHead(const doc::Project& p, doc::TrackId t) {
  const doc::Track* tr = doc::findTrackPtr(p, t);
  if (tr == nullptr) return {};
  const doc::MainPath path = doc::mainPath(p, *tr);
  return path.blocks.empty() ? doc::DeviceId{} : path.blocks.front()->id;
}
// GU.11: a top-level block of track `t` named `name` (a module's group), or none.
doc::DeviceId topNamed(const doc::Project& p, doc::TrackId t, std::string_view name) {
  if (const doc::Track* tr = doc::findTrackPtr(p, t))
    for (const doc::DevicePtr& d : tr->devices)
      if (!d->group.valid() && d->name == name) return d->id;
  return {};
}

std::optional<doc::ops::Edit> runEdit(doc::DocumentStore& store, const doc::ops::EditResult& e) {
  if (!e.ok() || !store.dispatch(e->command).ok()) return std::nullopt;
  return *e;
}

doc::Note scnNote(Ticks start, Ticks length, tuning::PitchId pitch, float velocity = 100.0f) {
  doc::Note n;
  n.start = start;
  n.length = length;
  n.pitch = pitch;
  n.velocity = velocity;
  return n;
}

}  // namespace

// T3.19: three main tunings (the default 12-EDO, "Tuning B: 22edo", "Tuning C: 5L 2s" with its generator automated on
// Lead), tracks assigned to them, the Tunings panel shown left of the arrangement, Lead's generator lane open.
Status setupTuningsPanel(Shell& shell) {
  ViewContext& ctx = shell.context();
  doc::DocumentStore& store = ctx.store();
  auto* panelPtr = dynamic_cast<TuningsPanel*>(shell.slot(ShellSlot::Tunings).find("tunings-panel"));
  ArrangementView* a = arrangementOf(shell);
  if (!panelPtr || !a) return Error{ErrorCode::NotFound, "tunings-panel: the panel and the arrangement must be installed", {}};
  const Error fail{ErrorCode::Unknown, "tunings-panel: an edit failed", {}};
  doc::TuningDef b;
  b.system = *tuning::Edo::make(22);
  const auto tb = runEdit(store, doc::ops::addMainTuning(*store.current(), store.ids(), "Tuning B: 22edo", b, doc::Colour{0xE0A43C}));
  doc::TuningDef c;
  tuning::MosParams m;
  m.n = 7;
  m.period = 1200.0;
  m.generator = 696.77;
  c.system = m;
  const auto tc = runEdit(store, doc::ops::addMainTuning(*store.current(), store.ids(), "Tuning C: 5L 2s", c, doc::Colour{0x8FC24A}));
  if (!tb || !tc) return fail;
  const doc::TuningId B = tb->tunings[0], C = tc->tunings[0];
  struct T {
    const char* name;
    doc::TuningAssignment a;
    uint32_t colour;
  };
  const T tracks[] = {{"Bass", doc::TuningAssignment::mainTuning(B), 0xE68632},
                      {"Lead", doc::TuningAssignment::mainTuning(C), 0x6FB04A},
                      {"Pad", doc::TuningAssignment::inherit(), 0x5E9BD6}};
  doc::TrackId lead;
  doc::TrackId after;
  for (const T& t : tracks) {
    const auto e = runEdit(store, doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, after, t.name, doc::Colour{t.colour}));
    if (!e) return fail;
    after = e->tracks[0];
    if (!store.dispatch(doc::SetTrackTuning{after, t.a}).ok()) return fail;
    if (std::string_view(t.name) == "Lead") lead = after;
    const auto clip = runEdit(store, doc::ops::insertEmptyClip(*store.current(), store.ids(), after, 0, 4 * kTicksPerWhole, t.name));
    if (!clip) return fail;
    std::vector<doc::Note> notes;
    for (int i = 0; i < 8; ++i)
      notes.push_back(scnNote(i * (kTicksPerWhole / 2), kTicksPerWhole / 2, tuning::PitchId::fromDegree((i * 3) % 7, 0)));
    if (!runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), clip->clips[0], notes))) return fail;
  }
  // Lead automates Tuning C's generator: a ramp across the region (meantone → superpyth).
  auto lane = std::make_shared<doc::AutomationLane>();
  lane->id = store.ids().lane();
  lane->param = doc::ParamRef::tuning(doc::ParamKind::MosGenerator, C);
  lane->points = {{0, 690.0f, 0.0f, store.ids().breakpoint()},
                  {2 * kTicksPerWhole, 705.0f, 0.0f, store.ids().breakpoint()},
                  {4 * kTicksPerWhole, 696.77f, 0.0f, store.ids().breakpoint()}};
  if (!store.dispatch(doc::AddAutomationLane{lead, lane}).ok()) return fail;
  ctx.setTuningsVisible(true);
  ctx.setDetailVisible(false);
  shell.tree().layoutNow();
  if (!panelPtr->showAutomationLane(doc::ParamRef::tuning(doc::ParamKind::MosGenerator, C))) return fail;
  a->selectTrack(lead);
  shell.tree().layoutNow();
  a->setTimeView(0.0, static_cast<double>(a->lanesViewport().w) / shell.tree().scale() / (4.5 * static_cast<double>(kTicksPerWhole)));
  return Ok{};
}

namespace {

// T3.19: a MIDI track with one clip of Ratio notes (the 4:5:6:7 chord over F = 1/4, a 9/4 and a 7/4 melody), a Degree
// note and a Cents note; the roll raised and its rows labelled with ratios. Returns the clip view.
ClipView* buildRatioScene(Shell& shell) {
  ViewContext& ctx = shell.context();
  doc::DocumentStore& store = ctx.store();
  const auto t = runEdit(store, doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Harmonics", doc::Colour{0xE68632}));
  if (!t) return nullptr;
  const auto c = runEdit(store, doc::ops::insertEmptyClip(*store.current(), store.ids(), t->tracks[0], 0, 2 * kTicksPerWhole, "Otonal"));
  if (!c) return nullptr;
  using tuning::PitchId;
  using tuning::Rational;
  const Ticks q = kPpq;
  std::vector<doc::Note> notes = {
      scnNote(0, 4 * q, PitchId::fromRatio(Rational(1))),          scnNote(0, 4 * q, PitchId::fromRatio(Rational(5, 4))),
      scnNote(0, 4 * q, PitchId::fromRatio(Rational(3, 2))),       scnNote(0, 4 * q, PitchId::fromRatio(Rational(7, 4))),
      scnNote(4 * q, 2 * q, PitchId::fromRatio(Rational(9, 4))),   scnNote(6 * q, q, PitchId::fromRatio(Rational(2))),
      scnNote(7 * q, q, PitchId::fromRatio(Rational(7, 4))),       scnNote(4 * q, 2 * q, PitchId::fromDegree(4, 0), 80.0f),
      scnNote(6 * q, 2 * q, PitchId::fromCents(-31.0), 70.0f)};
  if (!runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), c->clips[0], notes))) return nullptr;
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(560.0f);
  ctx.setDetailClip(c->clips[0]);
  shell.tree().layoutNow();
  auto* view = dynamic_cast<ClipView*>(shell.tree().find("main/body/bottom/detail/clip/clip-view"));
  if (!view) return nullptr;
  view->setRowLabels(roll::RowLabels::Ratio);
  view->setRowHeight(18);
  view->centreOnDegree(9);
  shell.tree().layoutNow();
  return view;
}

// The centre of a note's rect (device px), or nullopt.
std::optional<ui::Vec2> noteCentre(ClipView& v, const doc::Note& n) {
  const ui::IRect r = v.noteRect(n.id);
  if (r.empty()) return std::nullopt;
  return ui::Vec2{static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f};
}

const doc::Note* sceneNote(const ViewContext& ctx, const ClipView& v, tuning::PitchId pitch, Ticks start) {
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), v.clip().track);
  const int32_t ci = t ? doc::findClipIndex(*t, v.clip().clip) : -1;
  if (ci < 0) return nullptr;
  for (const doc::Note& n : t->clips[static_cast<size_t>(ci)].clip->notes)
    if (n.pitch == pitch && n.start == start) return &n;
  return nullptr;
}

}  // namespace

// T3.19: ratio labels on the notes ("7/4 · 7": the ratio and its harmonic over F = 1/4), rows labelled with ratios, the
// 7/4 of the chord selected, and the pointer resting on it: the info view reads its identity and pitch.
Status setupPianoRollRatios(Shell& shell) {
  ClipView* v = buildRatioScene(shell);
  if (!v) return Error{ErrorCode::Unknown, "pianoroll-ratios: building the clip failed", {}};
  const doc::Note* n = sceneNote(shell.context(), *v, tuning::PitchId::fromRatio(tuning::Rational(7, 4)), 0);
  if (!n) return Error{ErrorCode::NotFound, "pianoroll-ratios: 7/4", {}};
  const doc::NoteId id = n->id;
  v->select(std::span<const doc::NoteId>(&id, 1));
  const std::optional<ui::Vec2> at = noteCentre(*v, *n);
  if (!at) return Error{ErrorCode::NotFound, "pianoroll-ratios: 7/4 not visible", {}};
  ui::PointerEvent e;
  e.type = ui::PointerType::Move;
  e.pos = *at;
  shell.tree().dispatchPointer(e);
  return Ok{};
}

// ---- T7.4: JI objects and notes along pitch curves in the piano roll -------------------------------------------

namespace {

// A MIDI track with one empty two-bar clip in the roll, the detail view tall enough for a chord's morph.
struct JiScene {
  ClipView* view = nullptr;
  doc::ClipRef clip{};
};
std::optional<JiScene> buildJiScene(Shell& shell, std::string_view name, int32_t rowPx, int64_t centre) {
  ViewContext& ctx = shell.context();
  doc::DocumentStore& store = ctx.store();
  const auto t = runEdit(store, doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "JI", doc::Colour{0x5FA8D8}));
  if (!t) return std::nullopt;
  const auto c = runEdit(store, doc::ops::insertEmptyClip(*store.current(), store.ids(), t->tracks[0], 0, 2 * kTicksPerWhole, std::string(name)));
  if (!c) return std::nullopt;
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(480.0f);  // the detail view at its tallest
  ctx.setDetailClip(c->clips[0]);
  shell.tree().layoutNow();
  auto* view = dynamic_cast<ClipView*>(shell.tree().find("main/body/bottom/detail/clip/clip-view"));
  if (!view) return std::nullopt;
  view->setRowHeight(rowPx);
  view->centreOnDegree(centre);
  view->setZoom(92.0 / static_cast<double>(kPpq), 0.0);  // the two bars across the note area
  view->scrollToTick(-0.35 * static_cast<double>(kPpq));
  shell.tree().layoutNow();
  return JiScene{view, c->clips[0]};
}

}  // namespace

// A four-stop morph (7:8:9:10:11:12:13 → 5:6:7:8:9 → 4:5:6:7 → 3:4:5) over two bars on C1, riding a curve: the box,
// the seven voices merging as the chords shrink, harmonic numbers, the header, the stop markers and their proportions,
// the ride line.
Status setupPianoRollJiMorph(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Morph", 11, 8);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-ji-morph: building the clip failed", {}};
  doc::DocumentStore& store = shell.context().store();
  const Ticks q = kPpq;
  const int64_t s0[] = {7, 8, 9, 10, 11, 12, 13}, s1[] = {5, 6, 7, 8, 9}, s2[] = {4, 5, 6, 7}, s3[] = {3, 4, 5};
  const auto e = runEdit(store, doc::ops::createJiObject(*store.current(), store.ids(), sc->clip, 0, 8 * q,
                                                         tuning::PitchId::fromDegree(0, -2), s0));
  if (!e || e->jiObjects.empty()) return Error{ErrorCode::Unknown, "pianoroll-ji-morph: createJiObject", {}};
  const doc::JiObjectId id = e->jiObjects[0];
  if (!runEdit(store, doc::ops::addJiStop(*store.current(), store.ids(), sc->clip, id, 2 * q, s1, q)) ||
      !runEdit(store, doc::ops::addJiStop(*store.current(), store.ids(), sc->clip, id, 4 * q, s2, q)) ||
      !runEdit(store, doc::ops::addJiStop(*store.current(), store.ids(), sc->clip, id, 6 * q, s3, 3 * q / 2)))
    return Error{ErrorCode::Unknown, "pianoroll-ji-morph: addJiStop", {}};
  const doc::Track* t = doc::findTrackPtr(*store.current(), sc->clip.track);
  const int32_t ci = t ? doc::findClipIndex(*t, sc->clip.clip) : -1;
  const doc::JiObject* o = ci >= 0 ? doc::findJiObject(*t->clips[static_cast<size_t>(ci)].clip, id) : nullptr;
  if (!o) return Error{ErrorCode::NotFound, "pianoroll-ji-morph: the object", {}};
  doc::JiObject ride = *o;
  ride.ride = {{0, 0.0f, 0.0f, {}}, {2 * q, -45.0f, 0.0f, {}}, {9 * q / 2, 35.0f, 0.0f, {}}, {13 * q / 2, -20.0f, 0.0f, {}}, {8 * q, 0.0f, 0.0f, {}}};
  if (!runEdit(store, doc::ops::setJiObject(*store.current(), store.ids(), sc->clip, ride)))
    return Error{ErrorCode::Unknown, "pianoroll-ji-morph: the ride curve", {}};
  shell.tree().layoutNow();
  return Ok{};
}

// An undertone object (1/4:1/5:1/6:1/7) and an overtone one (4:5:6:7) made as the JI tool makes them (the lowest
// voice on the box's bottom row); the second is selected (solid border, edge handles).
Status setupPianoRollJiUndertone(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Undertone", 14, 8);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-ji-undertone: building the clip failed", {}};
  roll::RollState& st = sc->view->state();
  const Ticks q = kPpq;
  std::string why;
  if (!st.createJiObject({0, 4 * q, 0, 12}, "1/4:1/5:1/6:1/7", &why) || !st.createJiObject({9 * q / 2, 3 * q, 2, 14}, "4:5:6:7", &why))
    return Error{ErrorCode::Unknown, "pianoroll-ji-undertone: " + why, {}};
  shell.tree().layoutNow();
  return Ok{};
}

// "Tune to JI" as the document builds it (ops::jiFromNotes): C E G A# as written in the tuning (a tuning stop, labelled
// with the tuning's name) gliding to the nearest proportion; the four notes stay as the object's inner notes and are
// drawn along their voices.
Status setupPianoRollJiTuning(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Tune to JI", 16, 5);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-ji-tuning: building the clip failed", {}};
  doc::DocumentStore& store = shell.context().store();
  using tuning::PitchId;
  const Ticks q = kPpq;
  const std::vector<doc::Note> notes = {scnNote(0, 6 * q, PitchId::fromDegree(0, 0)), scnNote(0, 6 * q, PitchId::fromDegree(4, 0)),
                                        scnNote(0, 6 * q, PitchId::fromDegree(7, 0)), scnNote(0, 6 * q, PitchId::fromDegree(10, 0))};
  const auto added = runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, notes));
  if (!added) return Error{ErrorCode::Unknown, "pianoroll-ji-tuning: addNotes", {}};
  doc::ops::JiFromNotesOptions opt;
  opt.glide = 2 * q;
  if (!runEdit(store, doc::ops::jiFromNotes(*store.current(), store.ids(), sc->clip, added->notes, opt)))
    return Error{ErrorCode::Unknown, "pianoroll-ji-tuning: jiFromNotes", {}};
  shell.tree().layoutNow();
  return Ok{};
}

// Notes with pitch curves, in no object: a bend up a whole tone, a scoop into the note, a vibrato (selected), next to
// plain notes.
Status setupPianoRollCurveNotes(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Bends", 16, 5);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-curve-notes: building the clip failed", {}};
  doc::DocumentStore& store = shell.context().store();
  using tuning::PitchId;
  const Ticks q = kPpq;
  const std::vector<doc::Note> notes = {scnNote(0, 4 * q, PitchId::fromDegree(0, 0)), scnNote(0, 2 * q, PitchId::fromDegree(7, 0)),
                                        scnNote(2 * q, 2 * q, PitchId::fromDegree(9, 0)), scnNote(4 * q, 4 * q, PitchId::fromDegree(4, 0)),
                                        scnNote(4 * q, 2 * q, PitchId::fromDegree(-3, 0), 80.0f)};
  const auto added = runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, notes));
  if (!added || added->notes.size() != notes.size()) return Error{ErrorCode::Unknown, "pianoroll-curve-notes: addNotes", {}};
  auto keyAt = [&](Ticks start, PitchId p) -> std::optional<doc::NoteKey> {
    const doc::Note* n = sceneNote(shell.context(), *sc->view, p, start);
    return n ? std::optional<doc::NoteKey>(doc::keyOf(*n)) : std::nullopt;
  };
  const std::optional<doc::NoteKey> bend = keyAt(0, PitchId::fromDegree(0, 0)), scoop = keyAt(2 * q, PitchId::fromDegree(9, 0)),
                                    vib = keyAt(4 * q, PitchId::fromDegree(4, 0));
  if (!bend || !scoop || !vib) return Error{ErrorCode::NotFound, "pianoroll-curve-notes: the notes", {}};
  std::vector<doc::Breakpoint> wobble;
  for (int i = 0; i <= 32; ++i)
    wobble.push_back({i * (4 * q) / 32, i < 8 ? 0.0f : 35.0f * std::sin(static_cast<float>(i - 8) * 1.5707963f) * std::min(1.0f, static_cast<float>(i - 8) / 8.0f), 0.0f, {}});
  if (!runEdit(store, doc::ops::setNoteCurve(*store.current(), sc->clip, *bend, {{0, 0.0f, 0.0f, {}}, {q, 0.0f, 0.0f, {}}, {5 * q / 2, 200.0f, 0.0f, {}}, {4 * q, 200.0f, 0.0f, {}}})) ||
      !runEdit(store, doc::ops::setNoteCurve(*store.current(), sc->clip, *scoop, {{0, -200.0f, 0.0f, {}}, {q, 0.0f, 0.0f, {}}})) ||
      !runEdit(store, doc::ops::setNoteCurve(*store.current(), sc->clip, *vib, wobble)))
    return Error{ErrorCode::Unknown, "pianoroll-curve-notes: setNoteCurve", {}};
  const doc::NoteId sel = vib->id;
  sc->view->select(std::span<const doc::NoteId>(&sel, 1));
  shell.tree().layoutNow();
  return Ok{};
}

// ---- T7.5: the inner roll, Tune to JI, the fundamental with the mouse ----------------------------------------------

namespace {

std::vector<doc::NoteId> sceneNoteIds(const ViewContext& ctx, const ClipView& v) {
  std::vector<doc::NoteId> ids;
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), v.clip().track);
  const int32_t ci = t ? doc::findClipIndex(*t, v.clip().clip) : -1;
  if (ci >= 0)
    for (const doc::Note& n : t->clips[static_cast<size_t>(ci)].clip->notes) ids.push_back(n.id);
  return ids;
}

// The scene's grid: quarter notes (the glide "Tune to JI" gives is one grid step).
void quarterGrid(ViewContext& ctx) {
  GridSpec g;
  g.mode = GridMode::Fixed;
  g.fixedExp = 2;
  ctx.setGrid(g);
}

}  // namespace

// An entered object (4:5:6:7 morphing to 5:6:7:8, one voice on a bezier with its handles): its voices are the rows
// (lanes), its inner notes lie on them through the morph, the stop is selected, and the plain notes around it are
// dimmed.
Status setupPianoRollJiEntered(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Inner Roll", 14, 8);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-ji-entered: building the clip failed", {}};
  quarterGrid(shell.context());
  doc::DocumentStore& store = shell.context().store();
  roll::RollState& st = sc->view->state();
  using tuning::PitchId;
  const Ticks q = kPpq;
  std::string why;
  if (!st.createJiObject({q, 6 * q, 0, 12}, "4:5:6:7", &why)) return Error{ErrorCode::Unknown, "pianoroll-ji-entered: " + why, {}};
  const doc::JiObjectId id = st.jiSelection();
  const int64_t next[] = {5, 6, 7, 8};
  if (!runEdit(store, doc::ops::addJiStop(*store.current(), store.ids(), sc->clip, id, 7 * q / 2, next, 3 * q / 2)))
    return Error{ErrorCode::Unknown, "pianoroll-ji-entered: addJiStop", {}};
  if (!st.setJiVoiceShape(id, 1, 2, doc::JiShape::Bezier)) return Error{ErrorCode::Unknown, "pianoroll-ji-entered: the bezier", {}};
  const std::vector<doc::Note> notes = {scnNote(q, 2 * q, PitchId::fromHarmonic(id.value, 0)),     scnNote(2 * q, 3 * q, PitchId::fromHarmonic(id.value, 1)),
                                        scnNote(3 * q, 3 * q, PitchId::fromHarmonic(id.value, 2)), scnNote(5 * q, 2 * q, PitchId::fromHarmonic(id.value, 3)),
                                        scnNote(5 * q, 2 * q, PitchId::fromHarmonic(id.value, 0), 70.0f),
                                        scnNote(0, q, PitchId::fromDegree(7, 0)),                  scnNote(7 * q, q, PitchId::fromDegree(4, 0)),
                                        scnNote(0, q, PitchId::fromDegree(0, 1)),                  scnNote(7 * q, q, PitchId::fromDegree(2, 1))};
  if (!runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, notes)))
    return Error{ErrorCode::Unknown, "pianoroll-ji-entered: addNotes", {}};
  shell.tree().layoutNow();
  st.enterJiObject(id);
  st.selectJiStop(id, 1);
  if (st.jiEntered() != id) return Error{ErrorCode::Unknown, "pianoroll-ji-entered: entering", {}};
  shell.tree().layoutNow();
  return Ok{};
}

// "Tune to JI" as the roll does it (RollState::tuneSelectionToJi, one grid step of glide): C E G -> 4:5:6, and D F A ->
// 10:12:15 that goes back to the tuning three beats in ("Back to Tuning"). The second is selected: its glide grips show.
Status setupPianoRollTuneToJi(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Tune to JI", 16, 5);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji: building the clip failed", {}};
  quarterGrid(shell.context());
  doc::DocumentStore& store = shell.context().store();
  roll::RollState& st = sc->view->state();
  using tuning::PitchId;
  const Ticks q = kPpq;
  const std::vector<doc::Note> first = {scnNote(0, 7 * q / 2, PitchId::fromDegree(0, 0)), scnNote(0, 7 * q / 2, PitchId::fromDegree(4, 0)),
                                        scnNote(0, 7 * q / 2, PitchId::fromDegree(7, 0))};
  const auto a = runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, first));
  if (!a) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji: addNotes", {}};
  std::vector<doc::NoteId> ids;
  for (const doc::NoteKey& k : a->notes) ids.push_back(k.id);
  sc->view->select(ids);
  if (!st.tuneSelectionToJi()) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji: the first chord", {}};
  const std::vector<doc::Note> second = {scnNote(4 * q, 4 * q, PitchId::fromDegree(2, 0)), scnNote(4 * q, 4 * q, PitchId::fromDegree(5, 0)),
                                         scnNote(4 * q, 4 * q, PitchId::fromDegree(9, 0))};
  const auto b = runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, second));
  if (!b) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji: addNotes", {}};
  ids.clear();
  for (const doc::NoteKey& k : b->notes) ids.push_back(k.id);
  sc->view->select(ids);
  if (!st.tuneSelectionToJi()) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji: the second chord", {}};
  const doc::JiObjectId id = st.jiSelection();
  if (!st.addJiTuningStopAt(id, 3 * q)) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji: back to the tuning", {}};
  st.selectJiStop(id, -1);
  shell.tree().layoutNow();
  return Ok{};
}

// The same in another active tuning: a 31-EDO clip, its 4:5:6:7 chord (steps 0, 10, 18, 25) tuned to JI. The first
// stop is labelled with the tuning's name.
Status setupPianoRollTuneToJi31(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Tune to JI", 9, 14);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji-31edo: building the clip failed", {}};
  quarterGrid(shell.context());
  doc::DocumentStore& store = shell.context().store();
  roll::RollState& st = sc->view->state();
  using tuning::PitchId;
  const Ticks q = kPpq;
  doc::TuningDef def;
  if (auto e = tuning::Edo::make(31)) def.system = *e;
  const auto tb = runEdit(store, doc::ops::addMainTuning(*store.current(), store.ids(), "31-EDO", def, doc::Colour{0x8FC24A}));
  if (!tb || !store.dispatch(doc::SetClipTuning{sc->clip, doc::TuningAssignment::mainTuning(tb->tunings[0])}).ok())
    return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji-31edo: the clip's tuning", {}};
  shell.tree().layoutNow();
  sc->view->centreOnDegree(14);
  const std::vector<doc::Note> notes = {scnNote(q, 6 * q, PitchId::fromDegree(0, 0)), scnNote(q, 6 * q, PitchId::fromDegree(10, 0)),
                                        scnNote(q, 6 * q, PitchId::fromDegree(18, 0)), scnNote(q, 6 * q, PitchId::fromDegree(25, 0))};
  const auto a = runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, notes));
  if (!a) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji-31edo: addNotes", {}};
  std::vector<doc::NoteId> ids;
  for (const doc::NoteKey& k : a->notes) ids.push_back(k.id);
  sc->view->select(ids);
  if (!st.tuneSelectionToJi()) return Error{ErrorCode::Unknown, "pianoroll-tune-to-ji-31edo: Tune to JI", {}};
  st.selectJiObject({});
  shell.tree().layoutNow();
  return Ok{};
}

// "Is Harmonic… 5" on a 12-EDO E (T7.5b): the E is the anchor, exactly on guide 5 of the series drawn around it
// (numbered at the right edge), and the notes drawn afterwards are exact ratios to it on guides 4, 6, 7 and 9.
Status setupPianoRollFundamentalLock(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Fundamental", 14, 12);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-fundamental-lock: building the clip failed", {}};
  doc::DocumentStore& store = shell.context().store();
  roll::RollState& st = sc->view->state();
  using tuning::PitchId;
  using tuning::Rational;
  const Ticks q = kPpq;
  const std::vector<doc::Note> notes = {scnNote(0, 4 * q, PitchId::fromDegree(4, 0))};
  if (!runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, notes)))
    return Error{ErrorCode::Unknown, "pianoroll-fundamental-lock: addNotes", {}};
  const std::vector<doc::NoteId> ids = sceneNoteIds(shell.context(), *sc->view);
  if (ids.size() != 1 || !st.lockFundamentalAt(ids[0], 5)) return Error{ErrorCode::Unknown, "pianoroll-fundamental-lock: the lock", {}};
  const auto on = [&](int64_t k) { return PitchId::fromRatio(Rational(k, 5), tuning::AnchorKind::Note, ids[0].value); };
  const std::vector<doc::Note> drawn = {scnNote(2 * q, 2 * q, on(6)), scnNote(4 * q, 2 * q, on(7)), scnNote(4 * q, 4 * q, on(4)),
                                        scnNote(6 * q, 2 * q, on(9))};
  if (!runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, drawn)))
    return Error{ErrorCode::Unknown, "pianoroll-fundamental-lock: the drawn notes", {}};
  sc->view->select(std::span<const doc::NoteId>(ids.data(), 1));
  shell.tree().layoutNow();
  return Ok{};
}

// ---- T7.6: bends between notes, vibrato, a ride curve ---------------------------------------------------------------

namespace {

// Alt + the note's edge, as the roll does it (RollState::bendEdit): the selected notes glide in / out over `length`.
bool sceneBend(doc::DocumentStore& store, roll::RollState& st, bool in, Ticks length) {
  st.drag = {};
  st.drag.kind = in ? roll::DragKind::ResizeStart : roll::DragKind::ResizeEnd;
  st.drag.bend = true;
  st.drag.dt = in ? length : -length;
  const doc::ops::EditResult e = st.bendEdit(st.drag);
  st.drag = {};
  return e && !e->empty() && runEdit(store, e).has_value();
}

}  // namespace

// A melody whose notes glide into each other: Ctrl+G on the first four (each bends out into the next over one grid
// step), a long scoop into the last from the note before it (Alt + its left edge), made straight; that note is selected
// and shows its breakpoints.
Status setupPianoRollBends(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Bends", 16, 6);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-bends: building the clip failed", {}};
  quarterGrid(shell.context());
  doc::DocumentStore& store = shell.context().store();
  roll::RollState& st = sc->view->state();
  using tuning::PitchId;
  const Ticks q = kPpq;
  const std::vector<doc::Note> notes = {scnNote(0, 3 * q / 2, PitchId::fromDegree(0, 0)), scnNote(3 * q / 2, 3 * q / 2, PitchId::fromDegree(4, 0)),
                                        scnNote(3 * q, q, PitchId::fromDegree(7, 0)),     scnNote(4 * q, 3 * q / 2, PitchId::fromDegree(5, 0)),
                                        scnNote(11 * q / 2, 5 * q / 2, PitchId::fromDegree(9, 0)), scnNote(0, 8 * q, PitchId::fromDegree(-5, 0), 70.0f)};
  if (!runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, notes)))
    return Error{ErrorCode::Unknown, "pianoroll-bends: addNotes", {}};
  shell.tree().layoutNow();
  std::vector<doc::NoteId> melody;
  for (int i = 0; i < 4; ++i)
    if (const doc::Note* n = sceneNote(shell.context(), *sc->view, notes[static_cast<size_t>(i)].pitch, notes[static_cast<size_t>(i)].start)) melody.push_back(n->id);
  const doc::Note* last = sceneNote(shell.context(), *sc->view, notes[4].pitch, notes[4].start);
  if (melody.size() != 4 || !last) return Error{ErrorCode::NotFound, "pianoroll-bends: the notes", {}};
  const doc::NoteId lastId = last->id;
  sc->view->select(melody);
  if (!st.glideSelection()) return Error{ErrorCode::Unknown, "pianoroll-bends: the glide", {}};
  sc->view->select(std::span<const doc::NoteId>(&lastId, 1));
  if (!sceneBend(store, st, true, q)) return Error{ErrorCode::Unknown, "pianoroll-bends: the bend in", {}};
  sc->view->select(std::span<const doc::NoteId>(&lastId, 1));
  if (!st.setBendShape(-1, doc::JiShape::LogLinear)) return Error{ErrorCode::Unknown, "pianoroll-bends: the shape", {}};
  sc->view->select(std::span<const doc::NoteId>(&lastId, 1));
  shell.tree().layoutNow();
  return Ok{};
}

// Vibrato: a long note with 30 ¢ at 6 Hz ("Add Vibrato…"), one that scoops in from the note before it and then
// swings 60 ¢ at 4 Hz, a plain note under them.
Status setupPianoRollVibrato(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Vibrato", 16, 5);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-vibrato: building the clip failed", {}};
  quarterGrid(shell.context());
  doc::DocumentStore& store = shell.context().store();
  roll::RollState& st = sc->view->state();
  using tuning::PitchId;
  const Ticks q = kPpq;
  const std::vector<doc::Note> notes = {scnNote(0, 4 * q, PitchId::fromDegree(7, 0)), scnNote(4 * q, 4 * q, PitchId::fromDegree(9, 0)),
                                        scnNote(0, 8 * q, PitchId::fromDegree(0, 0), 70.0f)};
  if (!runEdit(store, doc::ops::addNotes(*store.current(), store.ids(), sc->clip, notes)))
    return Error{ErrorCode::Unknown, "pianoroll-vibrato: addNotes", {}};
  shell.tree().layoutNow();
  const doc::Note* a = sceneNote(shell.context(), *sc->view, notes[0].pitch, 0);
  const doc::Note* b = sceneNote(shell.context(), *sc->view, notes[1].pitch, 4 * q);
  if (!a || !b) return Error{ErrorCode::NotFound, "pianoroll-vibrato: the notes", {}};
  const doc::NoteId ida = a->id, idb = b->id;
  sc->view->select(std::span<const doc::NoteId>(&ida, 1));
  if (!st.addVibratoToSelection(30.0, 6.0)) return Error{ErrorCode::Unknown, "pianoroll-vibrato: the vibrato", {}};
  sc->view->select(std::span<const doc::NoteId>(&idb, 1));
  if (!sceneBend(store, st, true, q)) return Error{ErrorCode::Unknown, "pianoroll-vibrato: the bend in", {}};
  sc->view->select(std::span<const doc::NoteId>(&idb, 1));
  if (!st.addVibratoToSelection(60.0, 4.0)) return Error{ErrorCode::Unknown, "pianoroll-vibrato: the second vibrato", {}};
  sc->view->select(std::span<const doc::NoteId>(&ida, 1));
  shell.tree().layoutNow();
  return Ok{};
}

// Ride curves: 4:5:6:7 riding a drawn rise of a whole tone and back (selected: its breakpoints on the lowest voice,
// the diamonds that bend its segments), and 4:5:6 under a vibrato; every voice follows.
Status setupPianoRollRide(Shell& shell) {
  const std::optional<JiScene> sc = buildJiScene(shell, "Ride", 14, 7);
  if (!sc) return Error{ErrorCode::Unknown, "pianoroll-ride: building the clip failed", {}};
  quarterGrid(shell.context());
  roll::RollState& st = sc->view->state();
  const Ticks q = kPpq;
  std::string why;
  if (!st.createJiObject({4 * q + q / 2, 3 * q, 0, 7}, "4:5:6", &why)) return Error{ErrorCode::Unknown, "pianoroll-ride: " + why, {}};
  const doc::JiObjectId second = st.jiSelection();
  if (!st.createJiObject({0, 4 * q, 0, 12}, "4:5:6:7", &why)) return Error{ErrorCode::Unknown, "pianoroll-ride: " + why, {}};
  const doc::JiObjectId first = st.jiSelection();
  const roll::CurveTarget ride{{}, first};
  if (!st.commitCurve(ride, {{0, 0.0f, 0.0f, {}}, {q, 0.0f, 0.6f, {}}, {2 * q, 200.0f, 0.0f, {}}, {3 * q, 200.0f, -0.6f, {}}, {4 * q, 0.0f, 0.0f, {}}}, "Ride"))
    return Error{ErrorCode::Unknown, "pianoroll-ride: the ride", {}};
  const roll::CurveTarget wobble{{}, second};
  const roll::RollState::Vibrato whole{q / 2, 3 * q, 0.0, 0.0};
  if (!st.setVibrato(wobble, whole, 40.0, 5.0))
    return Error{ErrorCode::Unknown, "pianoroll-ride: the vibrato", {}};
  st.selectJiObject(first);
  shell.tree().layoutNow();
  return Ok{};
}

// T4.16b: the pointer resting on the end of Bass B (bars 5–8): the edge a drag would trim is marked (Live).
Status setupArrangementHover(Shell& shell) {
  ArrangementView* a = arrangementOf(shell);
  if (!a) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  shell.tree().layoutNow();
  doc::ClipRef ref{};
  doc::forEachTrack(*shell.context().doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Bass" && t.clips.size() > 1) ref = {t.id, t.clips[1].id};
  });
  if (!ref.clip.valid()) return Error{ErrorCode::NotFound, "arrangement-hover: needs the arrangement fixture", {}};
  const ui::IRect box = a->clipRect(ref);
  ui::PointerEvent e;
  e.type = ui::PointerType::Move;
  e.pos = {static_cast<float>(box.right()) - 2.0f, static_cast<float>(box.y + box.h / 2)};
  shell.tree().dispatchPointer(e);
  if (!a->clipHover().clip.clip.valid()) return Error{ErrorCode::Unknown, "arrangement-hover: no hover", {}};
  return Ok{};
}

// T5.6: audio clips (tests/views/fixtures/audio): waveforms (stereo, mono, gain, reverse, Re-Pitch loop), fades, a
// crossfade, badges, the missing overlay, a MIDI row between; the first clip selected (its handles shown). Waveforms
// are built synchronously (no UI poster in scenes).
Status setupArrangementAudio(Shell& shell) {
  ArrangementView* a = arrangementOf(shell);
  if (!a) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  a->setTimeView(-0.1 * static_cast<double>(kTicksPerWhole), 210.0 / static_cast<double>(kTicksPerWhole));
  doc::ClipRef ref{};
  doc::forEachTrack(*shell.context().doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Drums" && !t.clips.empty()) ref = {t.id, t.clips[0].id};
  });
  if (!ref.clip.valid()) return Error{ErrorCode::NotFound, "arrangement-audio: needs the audio fixture", {}};
  a->selectClips(std::span<const doc::ClipRef>(&ref, 1), false);
  shell.tree().layoutNow();
  return Ok{};
}

// T6.10: take lanes on the audio fixture (tests/views/fixtures/audio): a new audio track "Takes" with three takes of
// beat.wav (each 1.6 s from bar 1.25, reading 8000 frames further in than the one above) and two note takes on Keys.
// `comp`: four regions comped from the three audio takes (the last the comp selection's neighbour), half a bar of the
// Keys' second take comped in (the comp selection). Otherwise Keys auditions its second take (lit, main lane grey).
Status setupArrangementTakes(Shell& shell, bool comp) {
  ArrangementView* a = arrangementOf(shell);
  if (!a) return Error{ErrorCode::NotFound, "arrangement view not installed", {}};
  ViewContext& ctx = shell.context();
  doc::DocumentStore& st = ctx.store();
  const doc::Project& p0 = *ctx.doc();
  doc::SampleId beat{};
  if (p0.samples)
    for (const doc::SampleRef& s : *p0.samples)
      if (s.displayName == "beat") beat = s.id;
  doc::TrackId keys{}, lost{};
  doc::forEachTrack(p0, [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Keys") keys = t.id;
    if (t.name == "Lost") lost = t.id;
  });
  if (!beat.valid() || !keys.valid() || !lost.valid()) return Error{ErrorCode::NotFound, "arrangement-takes: needs the audio fixture", {}};
  auto run = [&](const doc::ops::EditResult& e) -> Status {
    if (!e) return e.error();
    return st.dispatch(e->command);
  };
  const Ticks bar = kTicksPerWhole;
  auto t = doc::ops::createTrack(*ctx.doc(), st.ids(), doc::TrackKind::Audio, lost, "Takes", doc::Colour{0x46A5E8});
  if (Status s = run(t); !s) return s;
  const doc::TrackId takes = t->tracks.at(0);
  std::vector<doc::LaneId> audioLanes;
  for (int k = 0; k < 3; ++k) {
    doc::AudioClip c;
    c.id = st.ids().clip();
    c.name = "beat";
    c.colour = doc::Colour{0x46A5E8};
    c.start = bar / 4;
    c.sample = beat;
    c.seconds = 1.6;
    c.contentStartFrame = 8000.0 * k;
    c = doc::syncedClip(*ctx.doc()->timeline, c);
    const doc::ClipSlot slot{c.id, nullptr, std::make_shared<const doc::AudioClip>(std::move(c))};
    if (Status s = run(doc::ops::addTakeLane(*ctx.doc(), st.ids(), takes, std::span<const doc::ClipSlot>(&slot, 1))); !s) return s;
    audioLanes.push_back(doc::findTrackPtr(*ctx.doc(), takes)->takes.back()->id);
  }
  std::vector<doc::LaneId> noteLanes;
  for (int k = 0; k < 2; ++k) {
    std::vector<doc::Note> notes;
    for (int i = 0; i < 8; ++i) {
      doc::Note n;
      n.id = st.ids().note();
      n.start = i * bar / 8;
      n.length = bar / 16 + k * bar / 32;
      n.pitch = tuning::PitchId::fromDegree(k == 0 ? (i * 2) % 7 : 7 - (i * 3) % 8, 0);
      n.velocity = 100.0f;
      notes.push_back(n);
    }
    auto seq = doc::NoteSeq::fromNotes(std::move(notes));
    if (!seq) return seq.error();
    doc::NoteClip c;
    c.id = st.ids().clip();
    c.name = k == 0 ? "take 1" : "take 2";
    c.colour = doc::Colour{0x7BC47F};
    c.start = 0;
    c.length = bar;
    c.loop.end = bar;
    c.notes = std::move(*seq);
    const doc::ClipSlot slot{c.id, std::make_shared<const doc::NoteClip>(std::move(c)), nullptr};
    if (Status s = run(doc::ops::addTakeLane(*ctx.doc(), st.ids(), keys, std::span<const doc::ClipSlot>(&slot, 1))); !s) return s;
    noteLanes.push_back(doc::findTrackPtr(*ctx.doc(), keys)->takes.back()->id);
  }
  a->setTimeView(-0.1 * static_cast<double>(bar), 450.0 / static_cast<double>(bar));
  shell.tree().layoutNow();
  if (comp) {
    const Ticks q = bar / 8;
    const std::pair<int, std::pair<Ticks, Ticks>> regions[] = {
        {0, {2 * q, 4 * q}}, {1, {4 * q, 6 * q}}, {2, {6 * q, 7 * q}}, {1, {7 * q, bar / 4 + (bar * 4) / 5}}};
    for (const auto& [k, r] : regions)
      if (!a->comp(takes, audioLanes[static_cast<size_t>(k)], r.first, r.second))
        return Error{ErrorCode::Unknown, "arrangement-comp: a comp failed", {}};
    if (!a->comp(keys, noteLanes[1], bar / 2, bar)) return Error{ErrorCode::Unknown, "arrangement-comp: the note comp failed", {}};
  } else if (!a->auditionTake(keys, noteLanes[1])) {
    return Error{ErrorCode::Unknown, "arrangement-takes: audition failed", {}};
  }
  shell.tree().layoutNow();
  return Ok{};
}

// T5.10: the browser over browserDemoIndex (the audio fixture's samples as the User Library): a folder open, a sample
// selected with its waveform in the preview strip, sorted by name.
Status setupBrowserPanel(Shell& shell) {
  ViewContext& ctx = shell.context();
  ctx.library().setIndex(browserDemoIndex(ctx.projectDir() + "/samples"));
  ctx.setBrowserVisible(true);
  shell.tree().layoutNow();
  BrowserPanel* b = findBrowser(shell);
  if (!b) return Error{ErrorCode::NotFound, "browser-panel: no browser", {}};
  b->setSource({BrowserSource::Kind::Place, 0});
  for (size_t i = 0; i < b->rows().size(); ++i)
    if (b->rows()[i].dir && b->index()->dirName(b->rows()[i].id) == "Drums") b->setFolderOpen(b->rows()[i].id, true);
  for (size_t i = 0; i < b->rows().size(); ++i)
    if (!b->rows()[i].dir && b->index()->name(b->rows()[i].id) == "vox.wav") b->select(static_cast<int32_t>(i), false);
  shell.tree().setFocus(&b->listWidget());
  shell.tree().layoutNow();
  return Ok{};
}

// T5.10: a search ("vo") in the Samples category over every place: best matches first, the first selected.
Status setupBrowserSearch(Shell& shell) {
  ViewContext& ctx = shell.context();
  ctx.library().setIndex(browserDemoIndex(ctx.projectDir() + "/samples"));
  ctx.setBrowserVisible(true);
  shell.tree().layoutNow();
  BrowserPanel* b = findBrowser(shell);
  if (!b) return Error{ErrorCode::NotFound, "browser-search: no browser", {}};
  b->setSource({BrowserSource::Kind::Category, static_cast<uint32_t>(library::Category::Samples)});
  b->setQuery("vo");
  b->select(0, false);
  shell.tree().setFocus(&b->searchField());
  shell.tree().layoutNow();
  return Ok{};
}

// T5.11: hot-swap mode for an Operator (the demo index's Presets/Operator folder): the hot-swap bar over the device's
// presets, the list focused.
Status setupBrowserHotSwap(Shell& shell) {
  ViewContext& ctx = shell.context();
  ctx.library().setIndex(browserDemoIndex(ctx.projectDir() + "/samples"));
  ctx.setBrowserVisible(true);
  shell.tree().layoutNow();
  BrowserPanel* b = findBrowser(shell);
  if (!b) return Error{ErrorCode::NotFound, "browser-hotswap: no browser", {}};
  HotSwapTarget t;
  t.kind = HotSwapTarget::Kind::Device;
  t.patch = "operator";
  t.label = "Operator";
  b->setHotSwap(t);
  shell.tree().setFocus(&b->listWidget());
  shell.tree().layoutNow();
  return Ok{};
}

// T5.6: the audio Clip View on the Re-Pitch clip (tests/views/fixtures/audio): the box (gain, transpose greyed,
// warp, BPM, loop, fades), the whole sample with the clip's region, the start marker and the loop brace.
Status setupClipViewAudio(Shell& shell) {
  ViewContext& ctx = shell.context();
  doc::ClipRef ref{};
  doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Vox" && !t.clips.empty()) ref = {t.id, t.clips[0].id};
  });
  if (!ref.clip.valid()) return Error{ErrorCode::NotFound, "clip-view-audio: needs the audio fixture", {}};
  if (ArrangementView* a = arrangementOf(shell)) a->selectClips(std::span<const doc::ClipRef>(&ref, 1), true);
  ctx.setDetailClip(ref);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Clip);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(440.0f);
  shell.tree().layoutNow();
  return Ok{};
}

// T6.8: the audio Clip View on the first Drums clip (beat.wav: four kicks and hats, 2 s) warped. `markers`: Beats with
// Preserve = Transients (the mode rows, the faint transients, the beat grid on the content axis); else Complex Pro with
// three warp markers, the middle one moved half a beat later (the waveform stretched before it, squeezed after).
// `panel`: the bottom panel's height (AC.fix: 240, the default, flows the cards into columns).
Status setupAudioClipWarp(Shell& shell, bool markers, float panel = 440.0f) {
  ViewContext& ctx = shell.context();
  doc::ClipRef ref{};
  doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Drums" && !t.clips.empty() && !ref.clip.valid()) ref = {t.id, t.clips[0].id};
  });
  if (!ref.clip.valid()) return Error{ErrorCode::NotFound, "audio-clip-warp: needs the audio fixture", {}};
  const std::span<const doc::ClipRef> one(&ref, 1);
  auto apply = [&](const doc::ops::EditResult& e) -> Status {
    if (!e) return e.error();
    return ctx.store().dispatch(e->command);
  };
  if (Status st = apply(doc::ops::setAudioClipWarp(*ctx.doc(), one, markers ? doc::AudioWarp::ComplexPro : doc::AudioWarp::Beats,
                                                   120.0));
      !st)
    return st;
  auto clip = [&]() -> const doc::AudioClip& {
    const doc::Track* t = doc::findTrackPtr(*ctx.doc(), ref.track);
    return *t->clips[static_cast<size_t>(doc::findClipIndex(*t, ref.clip))].audio;
  };
  if (!markers) {
    doc::WarpParams w = clip().warpParams;
    w.beatsGranulation = 0;
    if (Status st = apply(doc::ops::setAudioClipWarpParams(*ctx.doc(), one, w)); !st) return st;
  } else {
    const Ticks c0 = clip().contentStart;
    for (const Ticks t : {c0 + kPpq, c0 + 2 * kPpq, c0 + 3 * kPpq})
      if (Status st = apply(doc::ops::addWarpMarker(*ctx.doc(), ref, t)); !st) return st;
    if (Status st = apply(doc::ops::moveWarpMarker(*ctx.doc(), ref, 1, c0 + 2 * kPpq + kPpq / 2)); !st) return st;
  }
  if (ArrangementView* a = arrangementOf(shell)) a->selectClips(one, true);
  ctx.setDetailClip(ref);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Clip);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(panel);
  shell.tree().layoutNow();
  return Ok{};
}

// T4.16b: the pointer resting on the end of an unselected note: the note brightened, its end marked.
Status setupPianoRollHover(Shell& shell) {
  if (Status s = openFirstClip(shell); !s) return s;
  shell.tree().layoutNow();
  auto* v = dynamic_cast<ClipView*>(shell.tree().find("main/body/bottom/detail/clip/clip-view"));
  if (!v) return Error{ErrorCode::NotFound, "pianoroll-hover: no clip view", {}};
  const ui::IRect r = v->noteRect(doc::NoteId{100});
  if (r.empty()) return Error{ErrorCode::NotFound, "pianoroll-hover: needs the pianoroll-12edo fixture", {}};
  ui::PointerEvent e;
  e.type = ui::PointerType::Move;
  e.pos = {static_cast<float>(r.right()) - 2.0f, static_cast<float>(r.y + r.h / 2)};
  shell.tree().dispatchPointer(e);
  if (v->noteHover().zone != 1) return Error{ErrorCode::Unknown, "pianoroll-hover: no edge hover", {}};
  return Ok{};
}

// T3.19: the note menu (right-click on the chord's 3/2): convert to Degree / Ratio / Cents, Set Pitch…, Lock
// Fundamental ▸, Row Labels ▸.
Status setupPianoRollNoteMenu(Shell& shell) {
  ClipView* v = buildRatioScene(shell);
  if (!v) return Error{ErrorCode::Unknown, "pianoroll-note-menu: building the clip failed", {}};
  const doc::Note* n = sceneNote(shell.context(), *v, tuning::PitchId::fromRatio(tuning::Rational(3, 2)), 0);
  if (!n) return Error{ErrorCode::NotFound, "pianoroll-note-menu: 3/2", {}};
  const std::optional<ui::Vec2> at = noteCentre(*v, *n);
  if (!at) return Error{ErrorCode::NotFound, "pianoroll-note-menu: 3/2 not visible", {}};
  ui::PointerEvent e;
  e.type = ui::PointerType::Move;
  e.pos = *at;
  shell.tree().dispatchPointer(e);
  e.type = ui::PointerType::Down;
  e.button = ui::PointerButton::Right;
  e.clickCount = 1;
  shell.tree().dispatchPointer(e);
  e.type = ui::PointerType::Up;
  shell.tree().dispatchPointer(e);
  if (shell.overlay().openCount() == 0) return Error{ErrorCode::Unknown, "pianoroll-note-menu: no menu", {}};
  return Ok{};
}

// T4.2: the circuit viewer over the whole window, on a patch of a .pqp file (the first one, or `patch`).
Status openPatchScene(Shell& shell, std::string_view file, std::string_view patch) {
  if (file.empty()) return Error{ErrorCode::InvalidArgument, "circuit scenes need a .pqp file (--project <file.pqp>)", {}};
  auto doc = circuit::loadPatchDocument(std::string(file));
  if (!doc) return std::move(doc).error();
  if (!circuit::openCircuitViewer(shell.overlay(), *doc, patch))
    return Error{ErrorCode::NotFound, "no patch '" + std::string(patch) + "' in " + std::string(file), {}};
  shell.tree().layoutNow();
  return Ok{};
}
Status setupCircuit(Shell& shell, std::string_view file) { return openPatchScene(shell, file, {}); }
Status setupCircuitSvf(Shell& shell, std::string_view file) { return openPatchScene(shell, file, "svf_multi"); }
Status setupCircuitSvfCore(Shell& shell, std::string_view file) { return openPatchScene(shell, file, "svf_core"); }
Status setupCircuitSymbols(Shell& shell) {
  if (!circuit::openCircuitViewer(shell.overlay(), circuit::symbolSheet())) return Error{ErrorCode::NotFound, "no overlay", {}};
  shell.tree().layoutNow();
  return Ok{};
}

// T4.4: the circuit editor (canvas + text pane) over a .pqp file's first patch, with its first atom selected.
Result<circuit::CircuitEditor*> openEditorScene(Shell& shell, std::string_view file) {
  if (file.empty()) return Error{ErrorCode::InvalidArgument, "circuit scenes need a .pqp file (--project <file.pqp>)", {}};
  auto doc = circuit::PatchEditor::open(std::string(file));
  if (!doc) return std::move(doc).error();
  circuit::CircuitEditor* e = circuit::openCircuitEditor(shell.overlay(), std::move(*doc));
  if (!e) return Error{ErrorCode::NotFound, "no patch in " + std::string(file), {}};
  shell.tree().layoutNow();
  return e;
}
Status setupCircuitEditor(Shell& shell, std::string_view file) {
  auto e = openEditorScene(shell, file);
  if (!e) return std::move(e).error();
  const graph::Patch& p = (*e)->document().drawn((*e)->shownIndex());
  for (graph::NodeIndex v = 0; v < p.nodeCount(); ++v)
    if (p.node(v).kind == graph::NodeKind::Atom) {
      const uint32_t one[1] = {v};
      (*e)->canvas().select(one, {});
      break;
    }
  (*e)->checkNow();
  shell.tree().layoutNow();
  return Ok{};
}
// T4.15: branches switched off by structural constants — the multi-stage envelope (--project
// stdlib/envelopes/msenv.pqp; GU.11: was the retired stock Saturator's curve) at its defaults (mode 0 None, soft 1):
// with Inactive off only the active logic is drawn (the Loop / Beat / Sync / Trigger retriggers and the hard start are
// hidden, the visible nodes laid out on their own); with it on, all of it, the switched-off nodes dimmed.
Result<circuit::CircuitEditor*> openInactiveScene(Shell& shell, std::string_view file, bool showInactive) {
  auto e = openEditorScene(shell, file);
  if (!e) return std::move(e).error();
  (*e)->setShowInactive(showInactive);
  if ((*e)->structure().inactiveNodes == 0)
    return Error{ErrorCode::NotFound, "circuit-inactive: nothing is switched off by a structural constant", {}};
  shell.tree().layoutNow();
  (*e)->canvas().fitToRead();  // M4 review M15: as the editor opens it
  shell.tree().layoutNow();
  return *e;
}
Status setupCircuitInactiveHidden(Shell& shell, std::string_view file) {
  auto e = openInactiveScene(shell, file, false);
  return e ? Status(Ok{}) : Status(std::move(e).error());
}
Status setupCircuitInactiveShown(Shell& shell, std::string_view file) {
  auto e = openInactiveScene(shell, file, true);
  return e ? Status(Ok{}) : Status(std::move(e).error());
}
// The same with Inactive on, zoomed in on the `done` threshold 0.0001 and the pointer resting on it: the constant's
// tooltip with its recognised form (−80 dB).
Status setupCircuitTooltip(Shell& shell, std::string_view file) {
  auto e = openInactiveScene(shell, file, true);
  if (!e) return std::move(e).error();
  circuit::CircuitEditor& ed = **e;
  const graph::Patch& p = ed.document().drawn(ed.shownIndex());
  for (graph::NodeIndex v = 0; v < p.nodeCount(); ++v) {
    if (p.node(v).kind != graph::NodeKind::Const || std::fabs(p.constant(v).literal - 1e-4) > 1e-12) continue;
    const circuit::SceneNode& n = ed.canvas().scene()->nodes[v];
    const float s = shell.tree().scale();
    const ui::IRect cr = ed.canvas().rect();
    circuit::CircuitCanvas::View view;
    view.zoom = 1.25;
    view.x = n.x + n.w * 0.5 - cr.w / s * 0.5 / view.zoom;
    view.y = n.y + n.h * 0.5 - cr.h / s * 0.5 / view.zoom;
    ed.canvas().setView(view);
    shell.tree().layoutNow();
    const ui::Vec2 at = ed.canvas().toDevice({n.x + n.w * 0.5f, n.y + n.h * 0.5f});
    if (!ed.showNodeTooltip(v, {at.x + 10.0f * s, at.y + 18.0f * s})) break;
    shell.tree().layoutNow();
    return Ok{};
  }
  return Error{ErrorCode::NotFound, "circuit-tooltip: no 0.0001 constant", {}};
}

// The same with a typing slip in the text pane: the inline error, and the canvas veiled over the last good version.
Status setupCircuitEditorError(Shell& shell, std::string_view file) {
  auto e = openEditorScene(shell, file);
  if (!e) return std::move(e).error();
  circuit::PatchEditor& d = (*e)->document();
  // The first binding line of the patch (not a declaration, not a comment).
  size_t at = std::string::npos;
  for (size_t line = d.parsed().patchSpans.empty() ? 0 : d.parsed().patchSpans.front().begin; line < d.text().size();) {
    const size_t end = std::min(d.text().find('\n', line), d.text().size());
    const std::string_view l = std::string_view(d.text()).substr(line, end - line);
    const size_t ind = l.find_first_not_of(' ');
    const std::string_view body = ind == std::string_view::npos ? std::string_view() : l.substr(ind);
    const size_t eq = l.find(" = ");
    if (eq != std::string_view::npos && !body.starts_with("param") && !body.starts_with("//") && !body.starts_with("in ") &&
        !body.starts_with("out ") && !body.starts_with("export")) {
      at = line + eq;
      break;
    }
    line = end + 1;
  }
  if (at == std::string::npos) return Error{ErrorCode::NotFound, "circuit-editor-error: no binding to break", {}};
  d.replaceText(static_cast<uint32_t>(at + 1), 1, ":", false);  // `x : …` where `x = …` was
  (*e)->textPane().setCaret(static_cast<uint32_t>(at + 2));
  shell.tree().setFocus(&(*e)->textPane());
  shell.tree().layoutNow();
  return Ok{};
}

// T4.5: a stand-in player for the probe and cost scenes (views have no engine): it "plays" any schedule, gives every
// probe of it deterministic data — a decaying 500 Hz tone for real signals, a 30 %-duty square for bools, levels lit for
// two of every three bool probes — and reports a cost of 0.3 ns per unit of graph::opCostWeight per region.
class ScenePlayer final : public circuit::PatchPlayer {
public:
  void reset() {
    playing_ = metering_ = false;
    gen_ = 0;
    rings_.clear();
    levels_.clear();
    signals_.clear();
  }
  double sampleRate() const override { return 48000.0; }
  Status play(const graph::Schedule& s, const graph::TypedPatch&) override {
    playing_ = true;
    ++gen_;
    rings_.clear();
    levels_.clear();
    constexpr uint32_t kFrames = 1024;
    std::vector<float> buf(kFrames);
    for (const graph::ProbeRef& p : s.probes) {
      const bool b = p.kind == graph::Kind::Bool;
      for (uint32_t k = 0; k < kFrames; ++k) {
        const double t = static_cast<double>(k);
        buf[k] = b ? (k % 200 < 60 ? dsp::boolMask(true) : 0.0f)
                   : static_cast<float>(0.8 * std::sin(2.0 * 3.141592653589793 * t / 96.0) * (0.35 + 0.65 * std::exp(-t / 500.0)));
      }
      if (std::find(signals_.begin(), signals_.end(), p.id) != signals_.end()) {
        auto ring = std::make_unique<TapRing>(1, 4096, 1024);
        ring->addReader();
        const float* ch = buf.data();
        ring->write(&ch, 1, kFrames);
        rings_.emplace_back(p.id, std::move(ring));
      }
      auto lv = std::make_unique<ProbeLevel>();
      const float high = b ? (p.id % 3 != 0 ? 1.0f : 0.0f) : buf.back();
      lv->write(high, b ? 0.0f : -0.8f, b ? high : 0.8f);
      levels_.emplace_back(p.id, std::move(lv));
    }
    cost_ = CostReading{};
    cost_.generation = gen_;
    cost_.units.resize(s.regions.size());
    cost_.regionNsPerFrame.assign(s.regions.size(), 0.0);
    for (uint32_t r = 0; r < s.regions.size(); ++r) {
      cost_.units[r] = r;
      for (uint32_t i = s.regions[r].firstOp; i < s.regions[r].firstOp + s.regions[r].numOps; ++i)
        cost_.regionNsPerFrame[r] += 0.3 * graph::opCostWeight(s, i);
      cost_.totalNsPerFrame += cost_.regionNsPerFrame[r];
    }
    cost_.totalNsPerFrame += 1.5;
    return Ok{};
  }
  void stop() override { playing_ = false; }
  bool playing() const override { return playing_; }
  const graph::StateLayout* stateLayout() const override { return nullptr; }
  void setProbeSignals(std::span<const uint32_t> ids) override { signals_.assign(ids.begin(), ids.end()); }
  TapRing* probeSignal(uint32_t id) noexcept override {
    for (auto& [k, r] : rings_)
      if (k == id) return r.get();
    return nullptr;
  }
  const ProbeLevel* probeLevel(uint32_t id) const noexcept override {
    for (const auto& [k, l] : levels_)
      if (k == id) return l.get();
    return nullptr;
  }
  void setMetering(bool on) override { metering_ = on; }
  bool readCost(CostReading& out) override {
    if (!metering_ || !playing_) return false;
    out = cost_;
    return true;
  }
  uint64_t generation() const noexcept override { return gen_; }

private:
  bool playing_ = false, metering_ = false;
  uint64_t gen_ = 0;
  std::vector<std::pair<uint32_t, std::unique_ptr<TapRing>>> rings_;
  std::vector<std::pair<uint32_t, std::unique_ptr<ProbeLevel>>> levels_;
  std::vector<uint32_t> signals_;
  CostReading cost_;
};

ScenePlayer& scenePlayer() {
  static ScenePlayer p;
  return p;
}

// The editor over `file`, playing on the scene player.
Result<circuit::CircuitEditor*> openPlayingEditor(Shell& shell, std::string_view file) {
  if (file.empty()) return Error{ErrorCode::InvalidArgument, "circuit scenes need a .pqp file (--project <file.pqp>)", {}};
  auto doc = circuit::PatchEditor::open(std::string(file));
  if (!doc) return std::move(doc).error();
  scenePlayer().reset();
  circuit::CircuitEditor* e = circuit::openCircuitEditor(shell.overlay(), std::move(*doc), {}, &scenePlayer());
  if (!e) return Error{ErrorCode::NotFound, "no patch in " + std::string(file), {}};
  shell.tree().layoutNow();
  if (!e->togglePlay()) return Error{ErrorCode::Unknown, "the scene's patch does not play: " + e->status(), {}};
  shell.tree().layoutNow();
  return e;
}

// T4.5: live probes on the stock ADSR — the bool wires' LED trace (some lit) and the readout of the hovered output wire
// (its value and mini-scope).
Status setupCircuitEditorProbe(Shell& shell, std::string_view file) {
  auto e = openPlayingEditor(shell, file);
  if (!e) return std::move(e).error();
  circuit::CircuitEditor& ed = **e;
  const auto& sc = ed.canvas().scene();
  const graph::Patch* p = &ed.document().drawn(ed.shownIndex());
  const auto out = p->findByName("out");
  if (!sc || !out) return Error{ErrorCode::NotFound, "circuit-editor-probe: no `out` to probe", {}};
  uint32_t wire = UINT32_MAX;
  for (uint32_t w = 0; w < sc->wires.size() && wire == UINT32_MAX; ++w)
    if (sc->pins[sc->wires[w].toPin].node == *out) wire = w;
  if (wire == UINT32_MAX) return Error{ErrorCode::NotFound, "circuit-editor-probe: `out` is not wired", {}};
  ed.probeWire(wire);
  ed.pollProbes();
  const circuit::SceneWire& sw = ed.canvas().scene()->wires[wire];
  const graph::Point2 a = ed.canvas().scene()->points[sw.firstPoint + (sw.pointCount - 1) / 2];
  const graph::Point2 b = ed.canvas().scene()->points[sw.firstPoint + (sw.pointCount - 1) / 2 + 1];
  ed.canvas().setPointer(ed.canvas().toDevice({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}));
  shell.tree().layoutNow();
  return Ok{};
}

// T4.5: the cost overlay on the stock SVF (its cores as subcircuit blocks): ns per sample per node and in total.
Status setupCircuitEditorCost(Shell& shell, std::string_view file) {
  auto e = openPlayingEditor(shell, file);
  if (!e) return std::move(e).error();
  (*e)->setCostShown(true);
  (*e)->pollCost();
  shell.tree().layoutNow();
  return Ok{};
}

// T4.1: the Device view of a MIDI track: Basic Synth → Saturator (Digital Clip, drive +6 dB, selected) → Utility (off).
// GU.11: each a module (its group's panel). Needs the device library (renderViewScene's `devices`).
Status setupDeviceChain(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-chain: no device library", {}};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) { return runEdit(store, e); };
  const auto t = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Keys"));
  if (!t) return Error{ErrorCode::Unknown, "device-chain: createTrack failed", {}};
  const doc::TrackId track = t->tracks[0];
  const auto sat = run(doc::ops::addDevice(*store.current(), store.ids(), track, "module:saturator", doc::DeviceKind::AudioEffect));
  const auto util = run(doc::ops::addDevice(*store.current(), store.ids(), track, "module:utility", doc::DeviceKind::AudioEffect));
  if (!sat || !util) return Error{ErrorCode::Unknown, "device-chain: addDevice failed", {}};
  const doc::DeviceId satId = sat->devices[0], utilId = util->devices[0];
  const doc::Project& p = *store.current();
  const std::string vp(doc::kValueParam);
  if (!run(doc::ops::setDeviceParamValue(p, *ctx.deviceLibrary(), memberNamed(p, satId, "Type"), vp, 6.0)) ||
      !run(doc::ops::setDeviceParamValue(*store.current(), *ctx.deviceLibrary(), memberNamed(*store.current(), satId, "Drive"), vp,
                                         1.9952623149688795)) ||  // +6 dB
      !run(doc::ops::setDevicesActive(*store.current(), std::vector<doc::DeviceId>{utilId}, false)))
    return Error{ErrorCode::Unknown, "device-chain: device edits failed", {}};
  ctx.setSelectedTrack(track);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-chain: the Device view is not installed", {}};
  const doc::DeviceId one[1] = {satId};
  v->select(one);
  shell.tree().layoutNow();
  return Ok{};
}

// T4.7: cross-device wiring. Track "Mod" holds an LFO; "Keys" holds Basic Synth → Utility with a free LFO beside them
// (GU.11: modules: the LFO's Out is its signal). Wires: Keys' LFO into Utility's Width and Balance (cables into the
// group's value blocks), into its own Rate (a delay-free loop: its badge shows z⁻¹), and Mod's LFO into Utility's Gain
// across tracks. The rings show fixed sweeps (a FixedModulationSource; the app reads the engine's taps).
Status setupDeviceWiring(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-wiring: no device library", {}};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) { return runEdit(store, e); };
  const auto mod = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Mod"));
  const auto keys = mod ? run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, mod->tracks[0], "Keys"))
                        : std::nullopt;
  if (!mod || !keys) return Error{ErrorCode::Unknown, "device-wiring: createTrack failed", {}};
  const doc::TrackId modT = mod->tracks[0], keysT = keys->tracks[0];
  const auto lfoA = run(doc::ops::addDevice(*store.current(), store.ids(), modT, "module:lfo", doc::DeviceKind::AudioEffect, doc::ops::kOffPath));
  const auto lfoB = run(doc::ops::addDevice(*store.current(), store.ids(), keysT, "module:lfo", doc::DeviceKind::AudioEffect, doc::ops::kOffPath));
  const auto util = run(doc::ops::addDevice(*store.current(), store.ids(), keysT, "module:utility", doc::DeviceKind::AudioEffect));
  if (!lfoA || !lfoB || !util) return Error{ErrorCode::Unknown, "device-wiring: addDevice failed", {}};
  const doc::DeviceId a = lfoA->devices[0], b = lfoB->devices[0], u = util->devices[0];
  const doc::Project& p = *store.current();
  const doc::DeviceId width = memberNamed(p, u, "Width"), balance = memberNamed(p, u, "Balance"), gain = memberNamed(p, u, "Gain"),
                      rate = memberNamed(p, b, "Rate");
  const std::string vp(doc::kValueParam);
  if (!run(doc::ops::connectWire(*store.current(), store.ids(), b, "y", width, vp)) ||
      !run(doc::ops::connectWire(*store.current(), store.ids(), b, "y", balance, vp)) ||
      !run(doc::ops::connectTravelWire(*store.current(), store.ids(), b, "y", rate, vp, 0.2)) ||
      !run(doc::ops::connectWire(*store.current(), store.ids(), a, "y", gain, vp)))
    return Error{ErrorCode::Unknown, "device-wiring: connectWire failed", {}};
  static FixedModulationSource sweeps;
  sweeps = FixedModulationSource();
  sweeps.set(width, vp, ModSweep{true, 0.2f, 0.05f});
  sweeps.set(balance, vp, ModSweep{true, 0.1f, 0.45f});
  sweeps.set(gain, vp, ModSweep{true, 0.3f, 0.3f});
  ctx.setModulationSource(&sweeps);
  ctx.setSelectedTrack(keysT);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-wiring: the Device view is not installed", {}};
  shell.tree().layoutNow();
  sweeps.setLive(true);
  v->pollRings();  // one read; then no source: the rings keep it and nothing polls (a still screenshot)
  ctx.setModulationSource(nullptr);
  shell.tree().layoutNow();
  return Ok{};
}

// FZ.3: a scripted freeze source (the playback link's render statuses): a fixed status per track.
struct SceneFreezeSource {
  std::vector<std::pair<doc::TrackId, FreezeView>> status;
  uint64_t gen = 1;
  FreezeSource source() {
    return {[this] { return gen; },
            [this](doc::TrackId t) {
              for (const auto& [id, v] : status)
                if (id == t) return v;
              return FreezeView{};
            },
            [](doc::TrackId) { return graph::kNone; }};
  }
};
FreezeView freezeView(FreezeView::State state, float progress = 0.0f, std::string error = {}) {
  FreezeView v;
  v.state = state;
  v.progress = progress;
  v.error = std::move(error);
  return v;
}

// FZ.3: the frozen look in the arrangement (tests/views/fixtures/mixer): Bass frozen with its render playing, the Keys
// group frozen (its members: Piano's render at 42 %, Pad's failed), the status line after a refused edit (Unfreeze).
Status setupTracksFrozen(Shell& shell) {
  if (Status s = setupMixerCommon(shell); !s) return s;
  ViewContext& ctx = shell.context();
  const doc::Track* bass = namedTrack(*ctx.doc(), "Bass");
  const doc::Track* keys = namedTrack(*ctx.doc(), "Keys");
  if (bass == nullptr || keys == nullptr) return Error{ErrorCode::NotFound, "tracks-frozen: no Bass / Keys track", {}};
  const doc::TrackId ids[2] = {bass->id, keys->id};
  if (!ctx.setTracksFrozen(ids, true)) return Error{ErrorCode::Unknown, "tracks-frozen: freeze failed", {}};
  const doc::Track* piano = namedTrack(*ctx.doc(), "Piano");
  const doc::Track* pad = namedTrack(*ctx.doc(), "Pad");
  if (piano == nullptr || pad == nullptr) return Error{ErrorCode::NotFound, "tracks-frozen: no Piano / Pad track", {}};
  static SceneFreezeSource src;
  src = SceneFreezeSource{};
  src.status = {{ids[0], freezeView(FreezeView::State::Ready, 1.0f)},
                {piano->id, freezeView(FreezeView::State::Rendering, 0.42f)},
                {pad->id, freezeView(FreezeView::State::Failed, 0.0f, "track 4 has no own bus")}};
  ctx.setFreezeSource(src.source());
  ctx.refuseFrozen(ids[0]);
  shell.tree().layoutNow();
  return Ok{};
}

// FZ.3: the Device view of a frozen track with one block kept running: "Keys" (its instrument → Saturator → Utility)
// frozen, the Saturator's output cabled into the Gain of Utility on "Mod" (so it and the instrument feeding it keep
// running), its render playing.
Status setupDeviceFrozen(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-frozen: no device library", {}};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) { return runEdit(store, e); };
  const auto mod = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Mod"));
  const auto keys = mod ? run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, mod->tracks[0], "Keys"))
                        : std::nullopt;
  if (!mod || !keys) return Error{ErrorCode::Unknown, "device-frozen: createTrack failed", {}};
  const doc::TrackId modT = mod->tracks[0], keysT = keys->tracks[0];
  const auto modUtil = run(doc::ops::addDevice(*store.current(), store.ids(), modT, "module:utility", doc::DeviceKind::AudioEffect));
  const auto sat = run(doc::ops::addDevice(*store.current(), store.ids(), keysT, "module:saturator", doc::DeviceKind::AudioEffect));
  const auto util = run(doc::ops::addDevice(*store.current(), store.ids(), keysT, "module:utility", doc::DeviceKind::AudioEffect));
  if (!modUtil || !sat || !util) return Error{ErrorCode::Unknown, "device-frozen: addDevice failed", {}};
  const doc::DeviceId gain = memberNamed(*store.current(), modUtil->devices[0], "Gain");
  if (!run(doc::ops::connectWire(*store.current(), store.ids(), sat->devices[0], std::string(doc::kMainPort), gain,
                                 std::string(doc::kValueParam))))
    return Error{ErrorCode::Unknown, "device-frozen: connectWire failed", {}};
  const doc::TrackId one[1] = {keysT};
  if (!ctx.setTracksFrozen(one, true)) return Error{ErrorCode::Unknown, "device-frozen: freeze failed", {}};
  static SceneFreezeSource src;
  src = SceneFreezeSource{};
  src.status = {{keysT, freezeView(FreezeView::State::Ready, 1.0f)}};
  ctx.setFreezeSource(src.source());
  ctx.setStatus({});
  ctx.setSelectedTrack(keysT);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  if (findDeviceChain(shell) == nullptr) return Error{ErrorCode::NotFound, "device-frozen: the Device view is not installed", {}};
  shell.tree().layoutNow();
  return Ok{};
}

// T4.13: the project patch view over the user's sketch project (project_patch::buildSketchProject): the whole project
// (overview), and zoomed onto the Keys band with its mixer strip expanded (band).
Status setupProjectPatch(Shell& shell, bool band) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-patch: no device library", {}};
  if (Status s = project_patch::buildSketchProject(ctx.store()); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-patch: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  if (band) {
    const doc::Track* keys = namedTrack(*ctx.doc(), "Keys");
    if (keys == nullptr) return Error{ErrorCode::NotFound, "project-patch: no Keys track", {}};
    v->setMixerExpanded(keys->id, true);
    v->focusBand(keys->id, false);
  } else {
    v->fitAll(false);
  }
  shell.tree().layoutNow();
  return Ok{};
}
Status setupProjectPatchOverview(Shell& shell) { return setupProjectPatch(shell, false); }
Status setupProjectPatchBand(Shell& shell) { return setupProjectPatch(shell, true); }
// TX.mixer-look (bug report 2026-11-03): a mixer as its face, close up — the sketch project's Keys mixer at the top right
// of its band, expanded (its send's level), panned 25L at −4.5 dB, its send to Space at −12 dB, soloed.
Status setupProjectMixer(Shell& shell) {
  if (Status s = setupProjectPatch(shell, true); !s) return s;
  ViewContext& ctx = shell.context();
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  const doc::Track* keys = namedTrack(*ctx.doc(), "Keys");
  const doc::Track* space = namedTrack(*ctx.doc(), "Space");
  if (keys == nullptr || space == nullptr) return Error{ErrorCode::NotFound, "project-mixer: no Keys / Space", {}};
  const doc::TrackId k = keys->id, sp = space->id;
  doc::DocumentStore& st = ctx.store();
  if (!st.dispatch(doc::SetTrackPan{k, -0.5f}).ok() || !st.dispatch(doc::SetTrackVolume{k, -4.5f}).ok() ||
      !st.dispatch(doc::SetSendLevel{k, sp, -12.0f}).ok() || !st.dispatch(doc::SetTrackSolo{k, true}).ok())
    return Error{ErrorCode::Unknown, "project-mixer: the mixer edits failed", {}};
  // Playing: a fixed level on its meter (RMS −14 / −16 dB, peaks −5 / +1.5 dB: the right channel over 0 dB).
  static FixedMeterSource meters;
  meters = FixedMeterSource{};
  ui::MeterLevels l;
  l.rmsDb[0] = -14.0f;
  l.rmsDb[1] = -16.0f;
  l.peakDb[0] = -5.0f;
  l.peakDb[1] = 1.5f;
  meters.set(k, l);
  meters.setLive(true);
  v->setMeterSource(&meters);
  shell.tree().layoutNow();
  const project_patch::ProjectPatch& pp = v->patch();
  const uint32_t b = pp.bandOf(k);
  if (b == project_patch::kNone || pp.bands[b].mixer == project_patch::kNone) return Error{ErrorCode::NotFound, "project-mixer: no Keys mixer", {}};
  const project_patch::Node& m = pp.nodes[pp.bands[b].mixer];
  const ui::IRect r = v->canvas().rect();
  const float us = shell.tree().scale();
  circuit::CircuitCanvas::View view;
  view.zoom = 3.0;
  view.x = m.x + 0.5 * m.w - 0.5 * r.w / (view.zoom * us);
  view.y = m.y + 0.5 * m.h - 0.5 * r.h / (view.zoom * us);
  v->canvas().setView(view);
  shell.tree().layoutNow();
  v->wakeFaces();  // the meter read (the hosts poll visible faces)
  shell.tree().layoutNow();
  return Ok{};
}
// BR.tuning-block: the MIDI bands' tuning blocks hitched outside their bands' left edge (BR.tuning-outside) — Keys's
// opened to its MOS circle (12-EDO, the new track's default) with C, E and G sounding (lit in their frequencies'
// colours), the band under it closed (the narrow TUNING panel), each with its notes cable out of the lane's left end and
// on across the band's edge into the instrument. `wheel`: Keys's grown leftward to the full wheel (the region chart
// beside the circle, which stays where it was).
Status setupProjectPatchTuningAt(Shell& shell, bool wheel, const char* scene) {
  const std::string name(scene);
  if (Status s = setupProjectPatch(shell, true); !s) return s;
  ViewContext& ctx = shell.context();
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  const doc::Track* keys = namedTrack(*ctx.doc(), "Keys");
  if (keys == nullptr) return Error{ErrorCode::NotFound, name + ": no Keys track", {}};
  v->setTuningOpen(keys->id, true);
  if (wheel) v->setTuningWheel(keys->id, true);
  shell.tree().layoutNow();
  const project_patch::ProjectPatch& pp = v->patch();
  const uint32_t b = pp.bandOf(keys->id);
  if (b == project_patch::kNone || pp.bands[b].tuning == project_patch::kNone)
    return Error{ErrorCode::NotFound, name + ": Keys has no tuning block", {}};
  const project_patch::Node& tn = pp.nodes[pp.bands[b].tuning];
  const ui::IRect r = v->canvas().rect();
  const float us = shell.tree().scale();
  circuit::CircuitCanvas::View view;
  view.zoom = std::min({1.0, static_cast<double>(r.h) / (static_cast<double>(us) * (tn.h + 420.0)),
                        static_cast<double>(r.w) / (static_cast<double>(us) * (tn.w + 520.0))});
  view.x = tn.x - 24.0;
  view.y = tn.y - 52.0;
  v->canvas().setView(view);
  shell.tree().layoutNow();
  const uint32_t sn = pp.bands[b].tuning < v->scene().sceneNode.size() ? v->scene().sceneNode[pp.bands[b].tuning] : project_patch::kNone;
  Face* f = sn != project_patch::kNone ? v->canvas().panelOn(sn) : nullptr;
  if (f == nullptr) return Error{ErrorCode::NotFound, name + ": no tuning face", {}};
  (void)f->prepare(v->canvas().deviceScale());  // laid out now (the canvas would at its first settled frame)
  MosCircle* c = project_patch::tuningFaceCircle(*f);
  if (c == nullptr) return Error{ErrorCode::NotFound, name + ": the face has no circle", {}};
  if (wheel && project_patch::tuningFaceChart(*f) == nullptr) return Error{ErrorCode::NotFound, name + ": the face has no chart", {}};
  c->noteOn(1, 0, 261.63);  // C
  c->noteOn(2, 4, 329.63);  // E
  c->noteOn(3, 7, 392.0);   // G
  shell.tree().layoutNow();
  return Ok{};
}
Status setupProjectPatchTuning(Shell& shell) { return setupProjectPatchTuningAt(shell, false, "project-patch-tuning"); }
Status setupProjectPatchTuningWheel(Shell& shell) { return setupProjectPatchTuningAt(shell, true, "project-patch-tuning-wheel"); }
// T5.x-circuit-chain: zooming into a band of the project patch view (a double-click on it): the sketch project's Keys
// band opened — Operator → Saturator, each opened to its atoms in its labelled frame, wired through to the mixer.
Status setupProjectPatchZoom(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-patch-zoom: no device library", {}};
  if (Status s = project_patch::buildSketchProject(ctx.store()); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-patch-zoom: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  const doc::Track* keys = namedTrack(*ctx.doc(), "Keys");
  if (keys == nullptr) return Error{ErrorCode::NotFound, "project-patch-zoom: no Keys track", {}};
  v->openBand(keys->id, false);
  shell.tree().layoutNow();
  return Ok{};
}
// EQ.4b: the EQ's curve after its circuit is opened and closed (the user's report: "no curve" until a band moved). A
// MIDI track's EQ (two bands boosted / cut) opened in the circuit view as "Open in Circuit View" does, the group
// opened and closed there, the circuit view left: the Device view's EQ panel draws its curve.
Status setupEqCircuitRoundTrip(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "eq-circuit-roundtrip: no device library", {}};
  doc::DocumentStore& store = ctx.store();
  const auto t = runEdit(store, doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Keys"));
  if (!t) return Error{ErrorCode::Unknown, "eq-circuit-roundtrip: createTrack failed", {}};
  const doc::TrackId track = t->tracks[0];
  const auto eq = runEdit(store, doc::ops::addDevice(*store.current(), store.ids(), track, "module:eq", doc::DeviceKind::AudioEffect));
  if (!eq) return Error{ErrorCode::Unknown, "eq-circuit-roundtrip: addDevice failed", {}};
  const doc::DeviceId eqId = eq->devices[0];
  const std::optional<doc::EqInfo> info = doc::eqInfo(*store.current(), eqId);
  if (!info || info->bands[0].size() < 3) return Error{ErrorCode::Unknown, "eq-circuit-roundtrip: no bands", {}};
  const std::string vp(doc::kValueParam);
  if (!runEdit(store, doc::ops::setDeviceParamValue(*store.current(), *ctx.deviceLibrary(), info->bands[0][1].gain, vp, 1.9952623149688795)) ||
      !runEdit(store, doc::ops::setDeviceParamValue(*store.current(), *ctx.deviceLibrary(), info->bands[0][2].gain, vp, 0.5011872336272722)))
    return Error{ErrorCode::Unknown, "eq-circuit-roundtrip: band edits failed", {}};
  ctx.setSelectedTrack(track);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "eq-circuit-roundtrip: the circuit view is not installed", {}};
  if (!ctx.openProjectCircuit({}, eqId)) return Error{ErrorCode::Unknown, "eq-circuit-roundtrip: did not open", {}};
  shell.tree().layoutNow();
  v->setDeviceExpanded(eqId, false);
  shell.tree().layoutNow();
  v->setDeviceExpanded(eqId, true);
  shell.tree().layoutNow();
  v->setDeviceExpanded(eqId, false);
  shell.tree().layoutNow();
  v->setShown(false, false);
  shell.tree().layoutNow();
  return Ok{};
}

// T5.x-one-circuit: "Open in Circuit View" on the Keys band's Saturator (the sketch project): the project patch view
// with the band opened, framed on the Saturator (outlined: the focus), then zoomed out to the whole project with the
// band still open: one circuit from the device's atoms to the master's Out.
Status setupProjectCircuitDevice(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-circuit-device: no device library", {}};
  if (Status s = project_patch::buildSketchProject(ctx.store()); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-circuit-device: the view is not installed", {}};
  const doc::Track* keys = namedTrack(*ctx.doc(), "Keys");
  if (keys == nullptr || keys->devices.size() < 2) return Error{ErrorCode::NotFound, "project-circuit-device: no Keys chain", {}};
  shell.tree().layoutNow();
  if (!ctx.openProjectCircuit({}, keys->devices[1]->id)) return Error{ErrorCode::Unknown, "project-circuit-device: did not open", {}};
  shell.tree().layoutNow();
  v->canvas().fitAll();  // the canvas's fit (the view's F would close the band): the whole project, the band open
  shell.tree().layoutNow();
  return Ok{};
}

// TL.1: the band lanes (--project tests/views/fixtures/audio: audio tracks with stereo, mono, reversed,
// Re-Pitch and missing clips and a MIDI row): the windows' column at a readable zoom, 8 bars (twice the default span,
// so twice the default width), the bar lines, clip names, waveforms and the MIDI row's notes.
Status setupProjectPatchLanes(Shell& shell) {
  ViewContext& ctx = shell.context();
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-patch-lanes: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->setLaneTime(0.0, static_cast<double>(ctx.doc()->timeline->sigMap.barToTicks(8)));  // TL.1: 8 bars on the lanes
  const project_patch::ProjectPatch& pp = v->patch();
  float x = 0.0f, y = 0.0f;
  bool found = false;
  for (const project_patch::Band& b : pp.bands)
    if (project_patch::hasClipRow(b) && !found) {
      x = pp.laneX0;
      y = b.laneY - 60.0f;
      found = true;
    }
  if (!found) return Error{ErrorCode::NotFound, "project-patch-lanes: no lane (needs the audio fixture)", {}};
  circuit::CircuitCanvas::View view;
  view.zoom = 1.5;
  view.x = static_cast<double>(x) - 24.0;
  view.y = static_cast<double>(y) - 8.0;
  v->canvas().setView(view);
  (void)ctx;
  shell.tree().layoutNow();
  return Ok{};
}

// TX.lane-edit: the band lanes as an editing surface (the sketch project): a time selection dragged over Keys and Pad
// (bar 2 … 3.5: it selects the clips it touches, outlined, the windows lighter), and automation mode with Pad's volume
// automated (a ramp and a curved fall) drawn over its clips, a breakpoint selected; the arrangement's painters throughout.
// TL.2: Pad's lanes shown (its volume's sub-lane under its lane, the same envelope and selected point).
Status setupProjectPatchLaneEdit(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-patch-lane-edit: no device library", {}};
  if (Status s = project_patch::buildSketchProject(ctx.store()); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  auto* arr = dynamic_cast<ArrangementView*>(shell.slot(ShellSlot::TrackArea).find("arrangement"));
  if (v == nullptr || arr == nullptr) return Error{ErrorCode::NotFound, "project-patch-lane-edit: the views are not installed", {}};
  const doc::Track* keys = namedTrack(*ctx.doc(), "Keys");
  const doc::Track* pad = namedTrack(*ctx.doc(), "Pad");
  if (keys == nullptr || pad == nullptr) return Error{ErrorCode::NotFound, "project-patch-lane-edit: no Keys / Pad", {}};
  const doc::TrackId padId = pad->id, keysId = keys->id;
  doc::DocumentStore& store = ctx.store();
  const Ticks bar = ticksPerBar(ctx.doc()->timeline->sigMap.signatureAt(0));
  auto lane = std::make_shared<doc::AutomationLane>();
  lane->id = store.ids().lane();
  lane->param = doc::ParamRef::track(doc::ParamKind::TrackVolume, padId);
  lane->points = {{0, -24.0f, 0.0f, store.ids().breakpoint()},
                  {bar + bar / 2, 0.0f, 0.0f, store.ids().breakpoint()},
                  {3 * bar, -6.0f, -0.6f, store.ids().breakpoint()},
                  {4 * bar, -30.0f, 0.0f, store.ids().breakpoint()}};
  const doc::BreakpointId picked = lane->points[1].id;
  if (!store.dispatch(doc::AddAutomationLane{padId, lane}).ok()) return Error{ErrorCode::Unknown, "project-patch-lane-edit: lane", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->setLaneTime(0.0, 8.0 * static_cast<double>(bar));  // TL.1: 8 bars on the lanes
  const project_patch::ProjectPatch& pp = v->patch();
  const uint32_t kb = pp.bandOf(keysId), pb = pp.bandOf(padId);
  if (kb == project_patch::kNone || pb == project_patch::kNone) return Error{ErrorCode::NotFound, "project-patch-lane-edit: no bands", {}};
  const project_patch::Node& kn = pp.nodes[pp.bands[kb].source];
  const project_patch::Node& pn = pp.nodes[pp.bands[pb].source];
  circuit::CircuitCanvas::View view;
  view.zoom = 1.0;
  view.x = static_cast<double>(kn.x) - 12.0;
  view.y = static_cast<double>(std::min(kn.y, pn.y)) - 60.0;
  v->canvas().setView(view);
  shell.tree().layoutNow();
  // The range over both windows (as a drag would leave it), then the automation (its own selection: the arrangement
  // keeps one at a time, so the clips' range is set after it).
  ArrangementAutomation& au = arr->automation();
  au.showLanes(padId, true);  // TL.2: Pad's automated volume as a sub-lane under its lane too (the arrangement's lanes)
  au.choose(padId, lane->param);
  au.setMode(true);
  au.select(padId, lane->param, {picked});
  ArrangementView::Selection sel;
  sel.tracks = {keysId, padId};
  sel.time = TickRange{bar, 3 * bar + bar / 2};
  sel.range = true;
  for (const doc::Track* t : {keys, pad}) {
    const doc::Track* now = doc::findTrackPtr(*ctx.doc(), t->id);
    for (const doc::ClipSlot& c : now->clips)
      if (c.start() < sel.time->end && c.end() > sel.time->start) sel.clips.push_back({now->id, c.id});
  }
  arr->setSelection(std::move(sel), false);
  shell.tree().layoutNow();
  return Ok{};
}

// TL.3: the automation wire (the sketch project): the first control of Keys' blocks with automation of its own pressed
// (its sub-lane shown under Keys' lane): one thin red lead from it to the nearest point of that sub-lane, at zoom 1.
Status setupProjectPatchAutoWire(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-patch-auto-wire: no device library", {}};
  if (Status s = project_patch::buildSketchProject(ctx.store()); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-patch-auto-wire: the view is not installed", {}};
  const doc::Track* keys = namedTrack(*ctx.doc(), "Keys");
  if (keys == nullptr) return Error{ErrorCode::NotFound, "project-patch-auto-wire: no Keys", {}};
  const doc::TrackId keysId = keys->id;
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  const Ticks bar = ticksPerBar(ctx.doc()->timeline->sigMap.signatureAt(0));
  v->setLaneTime(0.0, 8.0 * static_cast<double>(bar));
  const project_patch::ProjectPatch& pp = v->patch();
  const uint32_t kb = pp.bandOf(keysId);
  if (kb == project_patch::kNone) return Error{ErrorCode::NotFound, "project-patch-auto-wire: no band", {}};
  circuit::CircuitCanvas::View view;
  view.zoom = 1.0;
  view.x = static_cast<double>(pp.bands[kb].x0) - 12.0;
  view.y = static_cast<double>(pp.bands[kb].y) - 50.0;
  v->canvas().setView(view);
  shell.tree().layoutNow();
  // The first control of Keys' devices drawn as a knob (a rim on the canvas) with automation of its own: pressed.
  std::vector<doc::DeviceId> devices;
  doc::forEachDeviceIn(keys->devices, [&](const doc::Device& d) { devices.push_back(d.id); });
  bool shown = false;
  for (const doc::DeviceId d : devices)
    for (const doc::Binding& b : v->controlsOf(d)) {
      if (shown || b.kind != doc::BindingKind::Control) continue;
      const auto at = v->controlDevice(b);
      if (!at || at->second < 3.0f || !v->showControlAutomation(b)) continue;
      v->setSelectedControl(b);
      shown = true;
    }
  if (!shown) return Error{ErrorCode::NotFound, "project-patch-auto-wire: no control of Keys with automation", {}};
  shell.tree().layoutNow();
  return Ok{};
}

// GU.9 v2: racks in the Device view (a rack is a set of parallel groups, pq/document/racks.h). audio-rack (the T4.8
// scene rebuilt from blocks): Keys = Basic Synth → an Audio Effect Rack [Dry: Utility | Crunch: Saturator (shown) |
// Wide: Utility] with chain zones (a crossfade over the chain selector), chain mixers, macros Drive / Width / Blend
// (Blend drives the selector), the zone editor on its Chain tab. GU.11: the chains' devices are modules; the macros map
// their value blocks.
Status setupAudioRack(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "audio-rack: no device library", {}};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) { return runEdit(store, e); };
  const auto t = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Keys"));
  if (!t) return Error{ErrorCode::Unknown, "audio-rack: createTrack failed", {}};
  const doc::TrackId track = t->tracks[0];
  const auto rack = run(doc::ops::addRack(*store.current(), store.ids(), track, false));
  if (!rack) return Error{ErrorCode::Unknown, "audio-rack: addRack failed", {}};
  const doc::DeviceId c1 = rack->devices[0];
  const auto dry = run(doc::ops::addToChain(*store.current(), store.ids(), c1, "module:utility", doc::DeviceKind::AudioEffect));
  const auto sat = run(doc::ops::addChainWithDevice(*store.current(), store.ids(), c1, "module:saturator", doc::DeviceKind::AudioEffect));
  const auto wide = run(doc::ops::addChainWithDevice(*store.current(), store.ids(), c1, "module:utility", doc::DeviceKind::AudioEffect));
  if (!dry || !sat || !wide) return Error{ErrorCode::Unknown, "audio-rack: chains failed", {}};
  const doc::DeviceId s = sat->devices[0], w = wide->devices[0];
  const doc::DeviceId c2 = doc::findDevice(*store.current(), s).device->group, c3 = doc::findDevice(*store.current(), w).device->group;
  const doc::DeviceId drive = memberNamed(*store.current(), s, "Drive"), width = memberNamed(*store.current(), w, "Width"),
                      type = memberNamed(*store.current(), s, "Type");
  const std::string vp(doc::kValueParam);
  bool ok = run(doc::ops::renameChain(*store.current(), c1, "Dry")) && run(doc::ops::renameChain(*store.current(), c2, "Crunch")) &&
            run(doc::ops::renameChain(*store.current(), c3, "Wide")) &&
            run(doc::ops::setChainZone(*store.current(), store.ids(), c1, doc::ZoneKind::Select, {0, 0, 32, 64})) &&
            run(doc::ops::setChainZone(*store.current(), store.ids(), c2, doc::ZoneKind::Select, {32, 64, 64, 96})) &&
            run(doc::ops::setChainZone(*store.current(), store.ids(), c3, doc::ZoneKind::Select, {64, 96, 127, 127})) &&
            run(doc::ops::setChainMixer(*store.current(), c2, -3.0f, 0.0f)) &&
            run(doc::ops::setChainMixer(*store.current(), c3, -6.0f, 0.3f)) &&
            run(doc::ops::mapMacro(*store.current(), store.ids(), c1, 0, drive, vp, 40.0)) &&
            run(doc::ops::mapMacro(*store.current(), store.ids(), c1, 1, width, vp, 70.0)) &&
            run(doc::ops::setDeviceParamValue(*store.current(), *ctx.deviceLibrary(), type, vp, 6.0));
  if (!ok) return Error{ErrorCode::Unknown, "audio-rack: rack edits failed", {}};
  const std::optional<doc::RackInfo> info = doc::rackOf(*store.current(), c1);
  if (!info || !info->selector.valid()) return Error{ErrorCode::Unknown, "audio-rack: no selector", {}};
  ok = run(doc::ops::mapMacro(*store.current(), store.ids(), c1, 2, info->selector, vp, 52.0)) &&
       run(doc::ops::renameMacro(*store.current(), c1, 0, "Drive")) && run(doc::ops::renameMacro(*store.current(), c1, 1, "Width")) &&
       run(doc::ops::renameMacro(*store.current(), c1, 2, "Blend"));
  if (!ok) return Error{ErrorCode::Unknown, "audio-rack: macro edits failed", {}};
  ctx.setSelectedTrack(track);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "audio-rack: the Device view is not installed", {}};
  v->showChain(c1, c2);
  v->showZones(c1, doc::ZoneKind::Select);
  const doc::DeviceId one[1] = {c1};
  v->select(one);
  shell.tree().layoutNow();
  return Ok{};
}

// T4.18: device automation. Track "Keys": Basic Synth -> Utility -> Saturator (GU.11: modules; the Saturator where Auto
// Filter was), with a held chord. Lanes under Keys: the Saturator's drive (a sweep −6 -> +18 dB over bars 2-4, back to
// 0 dB), the Utility's gain (a ramp; overridden by hand: grey) and its width (curved) — their value blocks' controls. The Device view below shows the automated
// controls with their LEDs and the envelopes' values at the playhead (bar 3). (GU.9: the rack in it is gone.)
Status setupDeviceAutomation(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-automation: no device library", {}};
  ArrangementView* a = arrangementOf(shell);
  if (a == nullptr) return Error{ErrorCode::NotFound, "device-automation: arrangement view not installed", {}};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) { return runEdit(store, e); };
  const auto t = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Keys"));
  if (!t) return Error{ErrorCode::Unknown, "device-automation: createTrack failed", {}};
  const doc::TrackId track = t->tracks[0];
  const auto util = run(doc::ops::addDevice(*store.current(), store.ids(), track, "module:utility", doc::DeviceKind::AudioEffect));
  if (!util) return Error{ErrorCode::Unknown, "device-automation: addDevice failed", {}};
  const auto af = run(doc::ops::addDevice(*store.current(), store.ids(), track, "module:saturator", doc::DeviceKind::AudioEffect));
  if (!af) return Error{ErrorCode::Unknown, "device-automation: addDevice failed", {}};
  const doc::DeviceId drive = memberNamed(*store.current(), af->devices[0], "Drive");
  const doc::DeviceId gain = memberNamed(*store.current(), util->devices[0], "Gain");
  const doc::DeviceId width = memberNamed(*store.current(), util->devices[0], "Width");
  const std::string vp(doc::kValueParam);
  const Ticks bar = kTicksPerWhole;
  doc::Batch b{"scene", {}};
  {
    doc::NoteClip c;
    c.id = store.ids().clip();
    c.name = "Chords";
    c.start = 0;
    c.length = 4 * bar;
    std::vector<doc::Note> notes;
    for (const int32_t d : {0, 4, 7}) {
      doc::Note n;
      n.id = store.ids().note();
      n.start = 0;
      n.length = 4 * bar;
      n.pitch = tuning::PitchId::fromDegree(d, 0);
      n.velocity = 100.0f;
      notes.push_back(n);
    }
    auto seq = doc::NoteSeq::fromNotes(std::move(notes));
    if (!seq.ok()) return std::move(seq).error();
    c.notes = std::move(seq).value();
    b.commands.push_back(doc::AddClip{track, std::make_shared<const doc::NoteClip>(std::move(c)), nullptr});
  }
  // GU.1: a device param's lane holds knob positions: its points given in units go through the param's curve.
  auto position = [&](const doc::ParamRef& ref, const std::string& name, double v) {
    if (ref.kind != doc::ParamKind::DeviceParam) return v;
    const doc::Device* d = doc::findDevice(*store.current(), doc::DeviceId{ref.owner}).device;
    if (d == nullptr) return v;
    const graph::Patch* callee = ctx.deviceLibrary()->patchOf(*d);
    const graph::ParamDecl* decl = callee != nullptr ? doc::findAnyParamDecl(*callee, name) : nullptr;
    return decl != nullptr ? dsp::curvePosition(doc::controlCurve(*d, name, *decl), v, 0.0) : v;
  };
  auto lane = [&](doc::ParamRef ref, std::string name, std::vector<std::pair<double, float>> pts, float curve) {
    auto l = std::make_shared<doc::AutomationLane>();
    l->id = store.ids().lane();
    l->param = ref;
    l->paramName = std::move(name);
    for (const auto& [bars, v] : pts)
      l->points.push_back({static_cast<Ticks>(bars * static_cast<double>(bar)), static_cast<float>(position(ref, l->paramName, v)),
                           curve, store.ids().breakpoint()});
    b.commands.push_back(doc::AddAutomationLane{track, l});
  };
  lane(doc::ParamRef::device(drive, vp), vp, {{0.0, 0.5f}, {1.0, 0.5f}, {3.0, 7.94f}, {4.0, 1.0f}}, 0.0f);
  lane(doc::ParamRef::device(gain, vp), vp, {{0.5, -6.0f}, {3.5, 3.5f}}, 0.0f);
  lane(doc::ParamRef::device(width, vp), vp, {{0.0, 1.0f}, {3.0, 3.0f}, {4.0, 0.5f}}, -0.4f);
  if (!store.dispatch(b).ok()) return Error{ErrorCode::Unknown, "device-automation: lanes failed", {}};
  ctx.overrideAutomation(doc::ParamRef::device(gain, vp));
  ctx.transport().locate(2 * bar);
  ctx.setSelectedTrack(track);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(330.0f);
  shell.tree().layoutNow();
  a->automation().showLanes(track, true);
  const float sc = shell.tree().scale();
  a->setTimeView(0.0, static_cast<double>(a->lanesViewport().w) / sc / (4.5 * static_cast<double>(bar)));
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-automation: the Device view is not installed", {}};
  v->refreshAutomation();
  shell.tree().layoutNow();
  v->revealDevice(util->devices[0]);
  shell.tree().layoutNow();
  return Ok{};
}

// TX.knob-curves: knobs switched off their declared curves. Track "Keys": a Saturator (its Drive on Linear: the knob sits
// near its start; its Dry/Wet on Exponential) -> Utility (its Gain on Logarithmic, in dB); GU.11: the modules' value
// blocks, their curves on the modules' panels.
Status setupDeviceKnobCurves(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-knob-curves: no device library", {}};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) { return runEdit(store, e); };
  const auto t = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Keys"));
  if (!t) return Error{ErrorCode::Unknown, "device-knob-curves: createTrack failed", {}};
  const doc::TrackId track = t->tracks[0];
  const auto af = run(doc::ops::addDevice(*store.current(), store.ids(), track, "module:saturator", doc::DeviceKind::AudioEffect));
  const auto ut = run(doc::ops::addDevice(*store.current(), store.ids(), track, "module:utility", doc::DeviceKind::AudioEffect));
  if (!af || !ut) return Error{ErrorCode::Unknown, "device-knob-curves: addDevice failed", {}};
  const doc::DeviceId drive = memberNamed(*store.current(), af->devices[0], "Drive"),
                      mix = memberNamed(*store.current(), af->devices[0], "Dry/Wet"),
                      gain = memberNamed(*store.current(), ut->devices[0], "Gain");
  // GU.1: the curves first (a curve edit keeps the knob position), then the values typed in units on them.
  const doc::DeviceLibrary& lib = *ctx.deviceLibrary();
  const std::string vp(doc::kValueParam);
  using P = dsp::CurvePreset;
  const bool ok = run(doc::ops::setParamCurvePreset(*store.current(), lib, drive, vp, P::Linear)) &&
                  run(doc::ops::setParamCurvePreset(*store.current(), lib, mix, vp, P::Exponential)) &&
                  run(doc::ops::setParamCurvePreset(*store.current(), lib, gain, vp, P::Logarithmic)) &&
                  run(doc::ops::setDeviceParamValue(*store.current(), lib, drive, vp, 2.0)) &&
                  run(doc::ops::setDeviceParamValue(*store.current(), lib, gain, vp, -6.0));
  if (!ok) return Error{ErrorCode::Unknown, "device-knob-curves: curves failed", {}};
  ctx.setSelectedTrack(track);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(330.0f);
  shell.tree().layoutNow();
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-knob-curves: the Device view is not installed", {}};
  v->rebuild();
  shell.tree().layoutNow();
  return Ok{};
}

// GU.9 v2: instrument-rack (the T4.8 scene rebuilt from blocks): Split = an Instrument Rack [Low: Basic Synth (key zone
// up to 300 Hz, fading out to 400) | High: Operator (from 300 Hz, fading in to 400) | Soft: the Wavetable Synth
// (velocity up to 48, fading to 64; -4.5 dB, pan -0.4)] -> Saturator, per-voice chains, macros Bright / Level, two
// variations; the key zones shown. GU.11: every chain's instrument is a module (its group); Ctrl+G groups the track's
// instrument group into the first chain.
Status setupInstrumentRack(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "instrument-rack: no device library", {}};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) { return runEdit(store, e); };
  const auto t = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Split"));
  if (!t) return Error{ErrorCode::Unknown, "instrument-rack: createTrack failed", {}};
  const doc::TrackId track = t->tracks[0];
  const doc::DeviceId inst = pathHead(*store.current(), track);
  const auto rack = run(doc::ops::groupDevices(*store.current(), store.ids(), std::vector<doc::DeviceId>{inst}));
  if (!rack) return Error{ErrorCode::Unknown, "instrument-rack: group failed", {}};
  const doc::DeviceId lo = rack->devices[0];
  const auto opc = run(doc::ops::addChainWithDevice(*store.current(), store.ids(), lo, "module:operator", doc::DeviceKind::Instrument));
  const auto soc = run(doc::ops::addChainWithDevice(*store.current(), store.ids(), lo, "module:wavetable_synth", doc::DeviceKind::Instrument));
  if (!opc || !soc ||
      !run(doc::ops::addDevice(*store.current(), store.ids(), track, "module:saturator", doc::DeviceKind::AudioEffect)))
    return Error{ErrorCode::Unknown, "instrument-rack: chains failed", {}};
  const doc::DeviceId op = opc->devices[0];
  const doc::DeviceId hi = doc::findDevice(*store.current(), op).device->group;
  const doc::DeviceId soft = doc::findDevice(*store.current(), soc->devices[0]).device->group;
  const std::string vp(doc::kValueParam);
  // (the macros map controls of the chains' instruments: the default instrument's and Operator's first Amplitude)
  const doc::DeviceId bright = memberNamed(*store.current(), inst, "Amplitude"), level = memberNamed(*store.current(), op, "Amplitude");
  const bool ok =
      run(doc::ops::renameChain(*store.current(), lo, "Low")) && run(doc::ops::renameChain(*store.current(), hi, "High")) &&
      run(doc::ops::renameChain(*store.current(), soft, "Soft")) &&
      run(doc::ops::setChainZone(*store.current(), store.ids(), lo, doc::ZoneKind::Key, {1, 1, 300, 400})) &&
      run(doc::ops::setChainZone(*store.current(), store.ids(), hi, doc::ZoneKind::Key, {300, 400, 100000, 100000})) &&
      run(doc::ops::setChainZone(*store.current(), store.ids(), soft, doc::ZoneKind::Velocity, {0, 0, 48, 64})) &&
      run(doc::ops::setChainMixer(*store.current(), soft, -4.5f, -0.4f)) &&
      run(doc::ops::setVoiceChains(*store.current(), lo, true)) &&
      (!bright.valid() || run(doc::ops::mapMacro(*store.current(), store.ids(), lo, 0, bright, vp, 80.0))) &&
      (!level.valid() || run(doc::ops::mapMacro(*store.current(), store.ids(), lo, 1, level, vp, 60.0))) &&
      run(doc::ops::renameMacro(*store.current(), lo, 0, "Bright")) &&
      run(doc::ops::storeVariation(*store.current(), lo)) &&
      run(doc::ops::setMacroValue(*store.current(), lo, 0, 20.0)) &&
      run(doc::ops::storeVariation(*store.current(), lo));
  if (!ok) return Error{ErrorCode::Unknown, "instrument-rack: rack edits failed", {}};
  ctx.setSelectedTrack(track);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "instrument-rack: the Device view is not installed", {}};
  v->showChain(lo, lo);
  v->showZones(lo, doc::ZoneKind::Key);
  shell.tree().layoutNow();
  return Ok{};
}

// One entry per scene. T3.8+ append theirs (arrangement-basic, pianoroll-31edo, mos-chart-default, …).
// TMM.4: Phase accumulator faces in the project patch view (project_patch::buildMetaFacesProject), the whole project
// fitted: three bands start with a face, one with Operator's plain block. The Lead and Bell faces show still voices
// (synthetic: what TMM.2's voice taps will feed), each dot with the trail of the phases read during one frame.
Status setupProjectMetaFaces(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-meta-faces: no device library", {}};
  if (Status s = project_patch::buildMetaFacesProject(ctx.store()); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-meta-faces: the view is not installed", {}};
  const doc::Track* lead = namedTrack(*ctx.doc(), "Lead");
  const doc::Track* bell = namedTrack(*ctx.doc(), "Bell");
  if (lead == nullptr || bell == nullptr || lead->devices.empty() || bell->devices.empty())
    return Error{ErrorCode::NotFound, "project-meta-faces: no Lead / Bell face", {}};
  static SyntheticVoices leadVoices, bellVoices;
  leadVoices.setVoices({{0, 7.0, 0.04, 0.0, kSceneNoteInc[0]}, {1, 5.0, 0.31, 0.0, kSceneNoteInc[1]}, {2, 9.0, 0.57, 0.0, kSceneNoteInc[2]}, {3, 6.0, 0.83, 0.0, kSceneNoteInc[3]}});
  leadVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  bellVoices.setVoices({{0, 4.0, 0.12, 0.0, kSceneNoteInc[0]}, {1, 11.0, 0.46, 0.0, kSceneNoteInc[1]}, {2, 3.0, 0.71, 0.0, kSceneNoteInc[2]}});
  bellVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  for (SyntheticVoices* sv : {&leadVoices, &bellVoices}) {
    sv->advance(1.0 / 60.0);
    sv->freeze();
  }
  const doc::DeviceId leadDev = lead->devices[0]->id, bellDev = bell->devices[0]->id;
  v->faceVoices = [leadDev, bellDev](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    if (!path.empty()) return nullptr;
    return d == leadDev ? &leadVoices : (d == bellDev ? &bellVoices : nullptr);
  };
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->fitAll(false);
  shell.tree().layoutNow();
  return Ok{};
}

// TMM.7: Wavetable faces in the project patch view (project_patch::buildMetaWavetableProject): "Wave" (a chain
// Wavetable → Saturator) and "Pair" opened in place — a Phase accumulator wired into a Wavetable — with still synthetic
// voices on both faces (a three-note chord: dots on the Wavetable's highlighted frame, the accumulator's orbit).
Status setupProjectMetaWavetable(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-meta-wavetable: no device library", {}};
  doc::DeviceId pair;
  if (Status s = project_patch::buildMetaWavetableProject(ctx.store(), &pair); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-meta-wavetable: the view is not installed", {}};
  const doc::Track* wave = namedTrack(*ctx.doc(), "Wave");
  const doc::Track* two = namedTrack(*ctx.doc(), "Pair");
  if (wave == nullptr || two == nullptr || wave->devices.empty())
    return Error{ErrorCode::NotFound, "project-meta-wavetable: no Wave / Pair track", {}};
  static SyntheticVoices waveVoices, accVoices, wtVoices;
  waveVoices.setVoices({{0, 6.0, 0.12, 0.35}, {1, 7.5, 0.44, 0.35}, {2, 9.0, 0.71, 0.35}, {3, 12.0, 0.9, 0.35}});
  accVoices.setVoices({{0, 4.0, 0.08, 0.0, kSceneNoteInc[0]}, {1, 5.0, 0.4, 0.0, kSceneNoteInc[1]}, {2, 6.0, 0.66, 0.0, kSceneNoteInc[2]}});
  accVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  wtVoices.setVoices({{0, 4.0, 0.08, 0.7}, {1, 5.0, 0.4, 0.7}, {2, 6.0, 0.66, 0.7}});
  for (SyntheticVoices* sv : {&waveVoices, &accVoices, &wtVoices}) {
    sv->advance(1.0 / 60.0);
    sv->freeze();
  }
  const doc::DeviceId waveDev = wave->devices[0]->id;
  // GU.11: Pair is the Wavetable Synth: its oscillator Wavetable's engine shows the chord.
  const doc::DeviceId pairOsc = memberNamed(*ctx.doc(), memberNamed(*ctx.doc(), pair, "Wavetable"), "Wavetable");
  (void)accVoices;
  v->faceVoices = [waveDev, pairOsc](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    if (!path.empty()) return nullptr;
    if (d == waveDev) return &waveVoices;
    if (d == pairOsc) return &wtVoices;
    return nullptr;
  };
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->setDeviceExpanded(pair, true);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(two->id, false);
  shell.tree().layoutNow();
  return Ok{};
}

// TMM.10: meta modules as chain devices (project_patch::buildMetaChainProject). The Device view of "Meta Lead": the
// Phase accumulator's and the Wavetable's faces ARE their panels (scaled to the view's height), then Auto Filter's
// panel; still synthetic voices on both faces (a four-note chord).
Status setupDeviceMetaChain(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-meta-chain: no device library", {}};
  project_patch::MetaChainIds ids;
  if (Status s = project_patch::buildMetaChainProject(ctx.store(), &ids); !s) return s;
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-meta-chain: the Device view is not installed", {}};
  static SyntheticVoices accVoices, wtVoices;
  accVoices.setVoices({{0, 4.0, 0.06, 0.0, kSceneNoteInc[0]}, {1, 5.0, 0.31, 0.0, kSceneNoteInc[1]}, {2, 6.0, 0.58, 0.0, kSceneNoteInc[2]}, {3, 7.5, 0.82, 0.0, kSceneNoteInc[3]}});
  accVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  wtVoices.setVoices({{0, 4.0, 0.06, 0.35}, {1, 5.0, 0.31, 0.35}, {2, 6.0, 0.58, 0.35}, {3, 7.5, 0.82, 0.35}});
  for (SyntheticVoices* sv : {&accVoices, &wtVoices}) {
    sv->advance(1.0 / 60.0);
    sv->freeze();
  }
  const doc::DeviceId acc = ids.acc, wt = ids.wt;
  v->faceVoices = [acc, wt](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    if (!path.empty()) return nullptr;
    return d == acc ? &accVoices : (d == wt ? &wtVoices : nullptr);
  };
  ctx.setFaceVoices(v->faceVoices);  // GU.13: a leaf's panel reads them through the context, as the app's
  ctx.setSelectedTrack(ids.lead);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  v->rebuild();
  const doc::DeviceId one[1] = {wt};
  v->select(one);
  shell.tree().layoutNow();
  return Ok{};
}

// TMM.10: the same project in the project patch view, the Meta Lead band focused: Phase accumulator → Wavetable as
// faces wired per voice, then Auto Filter; the same still voices.
Status setupProjectMetaChain(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-meta-chain: no device library", {}};
  project_patch::MetaChainIds ids;
  if (Status s = project_patch::buildMetaChainProject(ctx.store(), &ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-meta-chain: the view is not installed", {}};
  static SyntheticVoices accVoices, wtVoices;
  accVoices.setVoices({{0, 4.0, 0.06, 0.0, kSceneNoteInc[0]}, {1, 5.0, 0.31, 0.0, kSceneNoteInc[1]}, {2, 6.0, 0.58, 0.0, kSceneNoteInc[2]}, {3, 7.5, 0.82, 0.0, kSceneNoteInc[3]}});
  accVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  wtVoices.setVoices({{0, 4.0, 0.06, 0.35}, {1, 5.0, 0.31, 0.35}, {2, 6.0, 0.58, 0.35}, {3, 7.5, 0.82, 0.35}});
  for (SyntheticVoices* sv : {&accVoices, &wtVoices}) {
    sv->advance(1.0 / 60.0);
    sv->freeze();
  }
  const doc::DeviceId acc = ids.acc, wt = ids.wt;
  v->faceVoices = [acc, wt](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    if (!path.empty()) return nullptr;
    return d == acc ? &accVoices : (d == wt ? &wtVoices : nullptr);
  };
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.lead, false);
  shell.tree().layoutNow();
  return Ok{};
}

// TX.patching (SPEC §15 2026-11-01 "free patching in the circuit"): the meta chain project with two FREE devices on the
// Lead band (out of its series, on the row under the chain): a Phase accumulator at 2 Hz whose output adds to the
// Wavetable's position (a wire onto a face's control), and a Utility whose audio input takes the Lead accumulator's main
// output (any output into any input). The Lead band framed.
Status setupProjectPatchFree(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-patch-free: no device library", {}};
  project_patch::MetaChainIds ids;
  if (Status s = project_patch::buildMetaChainProject(ctx.store(), &ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-patch-free: the view is not installed", {}};
  const size_t before = doc::findTrackPtr(*ctx.doc(), ids.lead)->devices.size();
  if (!v->addFreeDevice(ids.lead, std::string(doc::kModuleItemPrefix) + "phase_acc") || !v->addFreeDevice(ids.lead, "module:utility"))
    return Error{ErrorCode::Unknown, "project-patch-free: adding the free devices failed", {}};
  const doc::Track* lead = doc::findTrackPtr(*ctx.doc(), ids.lead);
  if (lead == nullptr || lead->devices.size() < before + 2) return Error{ErrorCode::NotFound, "project-patch-free: no Lead chain", {}};
  // GU.4: the module's group (the first block pasted), its Frequency value block, and the Utility's group (GU.11: a
  // module too: the group of the last blocks pasted).
  const doc::DeviceId lfo = lead->devices[before]->id, util = topNamed(*ctx.doc(), ids.lead, "Utility");
  doc::DeviceId lfoFreq;
  for (const doc::DevicePtr& d : lead->devices)
    if (d->group == lfo && d->name == doc::kPaFrequency) lfoFreq = d->id;
  auto run = [&](const doc::ops::EditResult& e) -> Status {
    if (!e) return e.error();
    return ctx.store().dispatch(e->command);
  };
  if (Status s = run(doc::ops::setDeviceParamValue(*ctx.doc(), *ctx.deviceLibrary(), lfoFreq, std::string(doc::kValueParam), 2.0)); !s) return s;
  if (Status s = run(doc::ops::connectWire(*ctx.doc(), ctx.store().ids(), lfo, "", ids.wt, "position")); !s) return s;
  if (Status s = run(doc::ops::connectWire(*ctx.doc(), ctx.store().ids(), ids.acc, "", util, "x")); !s) return s;
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.lead, false);
  shell.tree().layoutNow();
  return Ok{};
}

// TX.node-menu (SPEC §15 2026-11-02 "cables and the node menu"): the node menu open over the TX.patching scene (Tab at a
// point of the Lead band), "filter" typed: Auto Filter highlighted, its preview (kind chip, counts, description, ports,
// parameters in two columns), the other matches grouped below it.
Status setupProjectNodeMenu(Shell& shell) {
  if (Status s = setupProjectPatchFree(shell); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-node-menu: the view is not installed", {}};
  circuit::clearRecentNodes();
  const ui::IRect r = v->canvas().rect();
  circuit::NodeMenu* m = v->openSpawnMenu({static_cast<float>(r.x) + static_cast<float>(r.w) * 0.22f, static_cast<float>(r.y) + static_cast<float>(r.h) * 0.18f});
  if (m == nullptr) return Error{ErrorCode::NotFound, "project-node-menu: no overlay layer", {}};
  m->setQuery("filter");
  m->finishOpening();  // TX.node-menu-fix1: shown as it is after its open animation
  shell.tree().layoutNow();
  return Ok{};
}

// TX.node-menu-fix1: a new connection being drawn out in the project patch view (the TX.patching scene): a press on the
// free Utility's output and a drag onto an input pin of another block — the real cable (tube, plugs, the source's
// colour) from the port to the pin it snapped to, highlighted (its halo, a ring on the pin). Cables at rest (no motion:
// deterministic).
Status setupProjectCableDrag(Shell& shell) {
  if (Status s = setupProjectPatchFree(shell); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-cable-drag: the view is not installed", {}};
  project_patch::ProjectCanvas& c = v->canvas();
  c.setCablesAnimated(false);
  shell.tree().layoutNow();
  const project_patch::ProjectPatch& pp = v->patch();
  uint32_t src = project_patch::kNone;
  for (uint32_t i = 0; i < pp.nodes.size(); ++i)
    if (pp.nodes[i].kind == project_patch::NodeKind::Device && pp.nodes[i].offPath && pp.nodes[i].title == "Utility" && !pp.nodes[i].outputs.empty()) src = i;
  if (src == project_patch::kNone) return Error{ErrorCode::NotFound, "project-cable-drag: no free Utility", {}};
  const project_patch::Node& sn = pp.nodes[src];
  (void)sn;
  const ui::Vec2 from = c.toDevice(c.portWorld(src, 0, false));  // TX.orbit-ports: where its output port is drawn
  auto send = [&](ui::PointerType t, ui::Vec2 p) {
    ui::PointerEvent e;
    e.type = t;
    e.button = ui::PointerButton::Left;
    e.pos = p;
    shell.tree().dispatchPointer(e);
  };
  // The target: the first visible input pin of another block that a release would connect (tried along the drag).
  std::vector<ui::Vec2> pins;
  const ui::IRect cr = c.rect();
  for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
    const project_patch::Node& n = pp.nodes[i];
    if (i == src || n.kind != project_patch::NodeKind::Device) continue;
    for (size_t k = 0; k < n.inputs.size(); ++k) {
      const ui::Vec2 p = c.toDevice(c.portWorld(i, static_cast<uint32_t>(k), true));
      if (!n.inputs[k].anchored && cr.contains(p) && std::hypot(p.x - from.x, p.y - from.y) >= 120.0f) pins.push_back(p);
    }
  }
  send(ui::PointerType::Move, from);
  send(ui::PointerType::Down, from);
  if (!c.looseCable())
    return Error{ErrorCode::Unknown, "project-cable-drag: no cable on the pointer (drag " + std::to_string(static_cast<int>(c.drag())) + ")", {}};
  bool snapped = false;
  for (const ui::Vec2 to : pins) {
    for (int k = 1; k <= 6; ++k)
      send(ui::PointerType::Move, {from.x + (to.x - from.x) * static_cast<float>(k) / 6.0f, from.y + (to.y - from.y) * static_cast<float>(k) / 6.0f});
    if ((snapped = c.looseCableSnapped())) break;
  }
  if (!snapped) return Error{ErrorCode::Unknown, "project-cable-drag: no input the cable snaps to", {}};
  shell.tree().layoutNow();
  return Ok{};
}

// TX.panel-faces (bug report 2026-11-01_12-15-15: "the utility in the circuit should just look like the utility ui in
// the device list below"): closed stock devices drawn as their Device-view panels in the project patch view — "Keys"
// plays the default synth (its generic panel) into an Auto Filter (frequency 600 Hz, resonance 0.4) and a Utility
// (width 140 %, Mute off, Bass Mono on: its conditional controls shown), "Mod" holds an LFO whose output is wired onto
// the Utility's gain (a param pin on the panel's edge). The Keys band framed.
Status setupProjectPatchPanels(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-patch-panels: no device library", {}};
  doc::DocumentStore& st = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) -> Status {
    if (!e) return e.error();
    return st.dispatch(e->command);
  };
  auto keysE = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, {}, "Keys", doc::Colour{0xF6A23B});
  if (!keysE) return keysE.error();
  if (Status s = st.dispatch(keysE->command); !s) return s;
  const doc::TrackId keys = keysE->tracks.at(0);
  auto modE = doc::ops::createTrack(*st.current(), st.ids(), doc::TrackKind::Midi, keys, "Mod", doc::Colour{0xB38BEA});
  if (!modE) return modE.error();
  if (Status s = st.dispatch(modE->command); !s) return s;
  const doc::TrackId mod = modE->tracks.at(0);
  auto add = [&](doc::TrackId t, const char* patch) -> doc::DeviceId {
    auto e = doc::ops::addDevice(*st.current(), st.ids(), t, patch, doc::DeviceKind::AudioEffect);
    if (!e || e->devices.empty() || !st.dispatch(e->command)) return {};
    return e->devices[0];
  };
  const doc::DeviceId filter = add(keys, "module:saturator"), util = add(keys, "module:utility");
  if (const doc::Track* m = doc::findTrackPtr(*st.current(), mod); m != nullptr && !m->devices.empty()) {
    std::vector<doc::DeviceId> inst;  // (IG.1-2: its instrument group with it; Keys keeps its group — the new track's look)
    for (const doc::DevicePtr& d : m->devices) inst.push_back(d->id);
    if (Status s = run(doc::ops::deleteDevices(*st.current(), inst)); !s) return s;
  }
  const doc::DeviceId lfo = add(mod, "module:lfo");
  if (!filter.valid() || !util.valid() || !lfo.valid()) return Error{ErrorCode::Unknown, "project-patch-panels: adding the devices failed", {}};
  const std::string vp(doc::kValueParam);
  const doc::DeviceLibrary& lib = *ctx.deviceLibrary();
  if (Status s = run(doc::ops::setDeviceParamValue(*st.current(), lib, memberNamed(*st.current(), filter, "Drive"), vp, 2.0)); !s) return s;
  if (Status s = run(doc::ops::setDeviceParamValue(*st.current(), lib, memberNamed(*st.current(), filter, "Dry/Wet"), vp, 0.4)); !s) return s;
  if (Status s = run(doc::ops::setDeviceParamValue(*st.current(), lib, memberNamed(*st.current(), util, "Width"), vp, 1.4)); !s) return s;
  if (Status s = run(doc::ops::setDeviceParamValue(*st.current(), lib, memberNamed(*st.current(), util, "Mono"), vp, 1.0)); !s) return s;
  if (Status s = run(doc::ops::connectWire(*st.current(), st.ids(), lfo, "y", memberNamed(*st.current(), util, "Gain"), vp)); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-patch-panels: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->focusBand(keys, false);
  shell.tree().layoutNow();
  return Ok{};
}

// TMM.10-fix2 (SPEC §15 2026-11-01 "the note source is a Hz piano roll"): the same project in the project patch view
// with wires from the note sources' outputs: Meta Lead's velocity into its Wavetable's position (per voice: the run's
// stage), its Hz into the Auto Filter's frequency (the global filter: through the wire's reducer, highest), its gate
// into the filter's drive; Meta Bass's velocity across tracks into the Lead filter's resonance. The Lead band framed.
Status setupProjectPatchNotes(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-patch-notes: no device library", {}};
  project_patch::MetaChainIds ids;
  if (Status s = project_patch::buildMetaChainProject(ctx.store(), &ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-patch-notes: the view is not installed", {}};
  const doc::Track* lead = doc::findTrackPtr(*ctx.doc(), ids.lead);
  if (lead == nullptr || lead->devices.size() < 3) return Error{ErrorCode::NotFound, "project-patch-notes: no Lead chain", {}};
  const doc::DeviceId filter = lead->devices[2]->id;
  struct W {
    doc::TrackId track;
    const char* port;
    doc::DeviceId to;
    const char* param;
  };
  for (const W& w : {W{ids.lead, "velocity", ids.wt, "position"}, W{ids.lead, "hz", filter, "freq"},
                     W{ids.lead, "gate", filter, "drive"}, W{ids.bass, "velocity", filter, "res"}}) {
    auto e = doc::ops::connectNoteWire(*ctx.doc(), ctx.store().ids(), w.track, w.port, w.to, w.param);
    if (!e) return std::move(e).error();
    if (Status s = ctx.store().dispatch(e->command); !s) return s;
  }
  const doc::WireId hz = doc::wiresOf(*ctx.doc()).at(1).id;
  if (Status s = ctx.store().dispatch(doc::ops::setWireReducer(*ctx.doc(), hz, doc::WireReducer::Highest)->command); !s) return s;
  static SyntheticVoices accVoices, wtVoices;
  accVoices.setVoices({{0, 4.0, 0.06, 0.0, kSceneNoteInc[0]}, {1, 5.0, 0.31, 0.0, kSceneNoteInc[1]}, {2, 6.0, 0.58, 0.0, kSceneNoteInc[2]}, {3, 7.5, 0.82, 0.0, kSceneNoteInc[3]}});
  accVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  wtVoices.setVoices({{0, 4.0, 0.06, 0.35}, {1, 5.0, 0.31, 0.35}, {2, 6.0, 0.58, 0.35}, {3, 7.5, 0.82, 0.35}});
  for (SyntheticVoices* sv : {&accVoices, &wtVoices}) {
    sv->advance(1.0 / 60.0);
    sv->freeze();
  }
  const doc::DeviceId acc = ids.acc, wt = ids.wt;
  v->faceVoices = [acc, wt](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    if (!path.empty()) return nullptr;
    return d == acc ? &accVoices : (d == wt ? &wtVoices : nullptr);
  };
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.lead, false);  // near enough for the outputs' names; Bass's wire arrives from below
  shell.tree().layoutNow();
  return Ok{};
}

// TX.resize (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"): blocks scaled with S-drag — the Meta Lead
// band of project_patch::buildMetaChainProject with its Wavetable face at 1.5× and its Auto Filter panel at 1.25×, Meta
// Bass's Wavetable at 0.6×; the Lead's note velocity wired onto the scaled Wavetable's position and the scaled filter's
// frequency (the cables end on the scaled controls); the same still voices as project-meta-chain. The Lead band framed.
Status setupProjectPatchResize(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-patch-resize: no device library", {}};
  project_patch::MetaChainIds ids;
  if (Status s = project_patch::buildMetaChainProject(ctx.store(), &ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-patch-resize: the view is not installed", {}};
  const doc::Track* lead = doc::findTrackPtr(*ctx.doc(), ids.lead);
  if (lead == nullptr || lead->devices.size() < 3) return Error{ErrorCode::NotFound, "project-patch-resize: no Lead chain", {}};
  const doc::DeviceId filter = lead->devices[2]->id;
  for (const auto& [to, param] : {std::pair{ids.wt, "position"}, std::pair{filter, "freq"}}) {
    auto e = doc::ops::connectNoteWire(*ctx.doc(), ctx.store().ids(), ids.lead, "velocity", to, param);
    if (!e) return std::move(e).error();
    if (Status s = ctx.store().dispatch(e->command); !s) return s;
  }
  static SyntheticVoices accVoices, wtVoices;
  accVoices.setVoices({{0, 4.0, 0.06, 0.0, kSceneNoteInc[0]}, {1, 5.0, 0.31, 0.0, kSceneNoteInc[1]}, {2, 6.0, 0.58, 0.0, kSceneNoteInc[2]}, {3, 7.5, 0.82, 0.0, kSceneNoteInc[3]}});
  accVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  wtVoices.setVoices({{0, 4.0, 0.06, 0.35}, {1, 5.0, 0.31, 0.35}, {2, 6.0, 0.58, 0.35}, {3, 7.5, 0.82, 0.35}});
  for (SyntheticVoices* sv : {&accVoices, &wtVoices}) {
    sv->advance(1.0 / 60.0);
    sv->freeze();
  }
  const doc::DeviceId acc = ids.acc, wt = ids.wt;
  v->faceVoices = [acc, wt](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    if (!path.empty()) return nullptr;
    return d == acc ? &accVoices : (d == wt ? &wtVoices : nullptr);
  };
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  (void)v->setBlockScale(ids.wt, 1.5f);
  (void)v->setBlockScale(filter, 1.25f);
  (void)v->setBlockScale(ids.bassWt, 0.6f);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.lead, false);
  shell.tree().layoutNow();
  return Ok{};
}

// TX.block-drag (the user 2026-11-03): the project-patch-resize scene with Meta Bass's Phase accumulator shrunk to 0.05×
// (a miniature, its cables' plugs as small) and the Lead's Auto Filter panel being dragged (pressed on its title bar
// and moved through the window, still held): the panel itself is drawn at the pointer over the Wavetable it passes, its
// cables plugged into its ports there. Cables at rest (deterministic).
Status setupProjectBlockDrag(Shell& shell) {
  if (Status s = setupProjectPatchResize(shell); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-block-drag: the view is not installed", {}};
  ViewContext& ctx = shell.context();
  const doc::Track* lead = nullptr;
  const doc::Track* bass = nullptr;
  doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (t.name == "Meta Lead") lead = &t;
    if (t.name == "Meta Bass") bass = &t;
  });
  if (lead == nullptr || bass == nullptr || lead->devices.size() < 3 || bass->devices.empty())
    return Error{ErrorCode::NotFound, "project-block-drag: no Meta Lead / Meta Bass chains", {}};
  const doc::DeviceId filter = topNamed(*ctx.doc(), lead->id, "Saturator");  // GU.11: where Auto Filter was
  (void)v->setBlockScale(bass->devices[0]->id, doc::kMinPatchScale);
  project_patch::ProjectCanvas& c = v->canvas();
  c.setCablesAnimated(false);
  shell.tree().layoutNow();
  const project_patch::ProjectPatch& pp = v->patch();
  const uint32_t n = pp.nodeOf(filter);
  if (n == project_patch::kNone) return Error{ErrorCode::NotFound, "project-block-drag: no Auto Filter block", {}};
  const ui::Vec2 grip = c.toDevice({pp.nodes[n].x + pp.nodes[n].w * 0.5f, pp.nodes[n].y + 6.0f});
  auto send = [&](ui::PointerType t, ui::Vec2 at) {
    ui::PointerEvent e;
    e.type = t;
    if (t != ui::PointerType::Move) e.button = ui::PointerButton::Left;
    e.clickCount = 1;
    e.pos = at;
    shell.tree().dispatchPointer(e);
  };
  send(ui::PointerType::Move, grip);
  send(ui::PointerType::Down, grip);
  for (int k = 1; k <= 8; ++k)
    send(ui::PointerType::Move, {grip.x - 520.0f * static_cast<float>(k) / 8.0f, grip.y + 150.0f * static_cast<float>(k) / 8.0f});
  if (c.drag() != project_patch::ProjectCanvas::Drag::MoveDevice || c.draggedNodes().empty())
    return Error{ErrorCode::Unknown, "project-block-drag: the block is not being dragged", {}};
  shell.tree().layoutNow();
  return Ok{};
}

// TX.groups (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"): project_patch::buildGroupProject with Keys'
// Phase accumulator → Wavetable → Phase accumulator grouped by Ctrl+G's edit (basic synth → group: the capsule opened,
// its In → … → Out wires, the free LFO outside it still wired onto the Wavetable's position across the boundary) and
// Bass's Saturator → Utility grouped and closed (its block's miniature). The whole project fitted.
Status buildGroupScene(Shell& shell, project_patch::GroupProjectIds& ids, project_patch::ProjectPatchView*& v) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "groups: no device library", {}};
  if (Status s = project_patch::buildGroupProject(ctx.store(), &ids); !s) return s;
  v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "groups: the view is not installed", {}};
  const doc::DeviceId keys[3] = {ids.pa1, ids.wt, ids.pa2}, bass[2] = {ids.sat, ids.util};
  if (!v->groupDevices(keys) || !v->groupDevices(bass)) return Error{ErrorCode::Unknown, "groups: grouping failed", {}};
  return Ok{};
}

Status setupProjectPatchGroup(Shell& shell) {
  project_patch::GroupProjectIds ids;
  project_patch::ProjectPatchView* v = nullptr;
  if (Status s = buildGroupScene(shell, ids, v); !s) return s;
  const doc::Track* bass = doc::findTrackPtr(*shell.context().doc(), ids.bass);
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  for (const doc::DevicePtr& d : bass->devices)
    if (doc::isGroupDevice(*d)) v->setGroupOpen(d->id, false);
  v->selectGroup({});
  v->fitAll(false);
  shell.tree().layoutNow();
  return Ok{};
}

// GU.2: groups in groups (project_patch::buildNestedGroupProject): Keys' instrument group drawn as an instrument (its
// accent, "Instrument · 16 voices") holding the opened "Osc" capsule and the closed "Level" block (its miniature); Bass's
// plain "FX" group on the path holding the opened "Tone" capsule. The whole project fitted.
Status setupProjectPatchNested(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "nested groups: no device library", {}};
  project_patch::NestedProjectIds ids;
  if (Status s = project_patch::buildNestedGroupProject(ctx.store(), &ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "nested groups: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->setGroupOpen(ids.level, false);
  v->selectGroup({});
  v->fitAll(false);
  shell.tree().layoutNow();
  return Ok{};
}

// GU.3: a group drawn as its panel (project_patch::buildPanelProject): Keys' instrument group "Synth" closed — the block
// is its 480 × 220 panel of every generic element kind (the Curve tab shown), the whole project fitted.
Status setupProjectPatchGroupPanel(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "group panel: no device library", {}};
  project_patch::PanelProjectIds ids;
  if (Status s = project_patch::buildPanelProject(ctx.store(), &ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "group panel: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->setGroupOpen(ids.group, false);
  v->selectGroup({});
  v->fitAll(false);
  shell.tree().layoutNow();
  return Ok{};
}

// GU.3: the same project in the Device view — the instrument group's frame: its inspector (the polyphony settings) and
// its panel, fitted to the frame's height.
Status setupDeviceGroupPanelElements(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "group panel: no device library", {}};
  project_patch::PanelProjectIds ids;
  if (Status s = project_patch::buildPanelProject(ctx.store(), &ids); !s) return s;
  ctx.setSelectedTrack(ids.keys);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  shell.tree().layoutNow();
  return Ok{};
}

// TX.groups: the same project's Keys in the Device view — the default synth, then the group, whose panel is the circuit
// of its contents (the embedded, scoped canvas: In, the three modules, Out, the wires between them).
Status setupDeviceGroupPanel(Shell& shell) {
  project_patch::GroupProjectIds ids;
  project_patch::ProjectPatchView* v = nullptr;
  if (Status s = buildGroupScene(shell, ids, v); !s) return s;
  ViewContext& ctx = shell.context();
  ctx.setSelectedTrack(ids.keys);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  shell.tree().layoutNow();
  return Ok{};
}

// TX.free-rack: "Lead" in the Device view (the synth → Saturator → Auto Filter → Utility) with its free devices — three
// LFOs, each wired onto a knob of the chain, and a Phase accumulator feeding the Auto Filter's sidechain — in the rack
// over the chain, expanded (`open`) or collapsed; the cables of the signal path and of every wire.
Status setupDeviceFreeRack(Shell& shell, bool open) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-free-rack: no device library", {}};
  project_patch::FreeRackIds ids;
  if (Status s = project_patch::buildFreeRackProject(ctx.store(), 3, &ids); !s) return s;
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-free-rack: the Device view is not installed", {}};
  ctx.setSelectedTrack(ids.lead);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  v->setFreeRackOpen(open);
  shell.tree().layoutNow();
  shell.tree().layoutNow();
  if (CableLayer* l = findCableLayer(shell)) (void)l->refresh();
  return Ok{};
}

// TX.all-free: the free-rack project with its Lead track's main path broken — the cable into its last block (the
// Utility) deleted. The Device view lists what is still on the path (to the break: nothing reaches the mixer); the
// Utility, off the path now, is in the free rack.
Status setupDevicePathBroken(Shell& shell) {
  if (Status s = setupDeviceFreeRack(shell, true); !s) return s;
  ViewContext& ctx = shell.context();
  const doc::TrackId lead = ctx.selectedTrack();
  const doc::Track* t = doc::findTrackPtr(*ctx.doc(), lead);
  if (t == nullptr) return Error{ErrorCode::NotFound, "device-path-broken: no Lead track", {}};
  const doc::MainPath path = doc::mainPath(*ctx.doc(), *t);
  if (!path.complete || path.links.size() < 3) return Error{ErrorCode::NotFound, "device-path-broken: no path to break", {}};
  auto e = doc::ops::deleteWires(*ctx.doc(), std::vector<doc::WireId>{path.links[path.links.size() - 2]});
  if (!e) return std::move(e).error();
  if (Status s = ctx.store().dispatch(e->command); !s) return s;
  shell.tree().layoutNow();
  if (CableLayer* l = findCableLayer(shell)) (void)l->refresh();
  return Ok{};
}

// TMM.11: still voices for the Envelope faces — six voices in every stage (rising, at the peak, decaying, sustained,
// released early, near the end of the release), each with the levels its tap sampled during the last frame.
class EnvelopeSceneVoices final : public VoiceTapReader {
public:
  struct V {
    uint32_t slot;
    double from, to;  // the level sampled over the last frame, oldest to newest
    bool released;
  };
  explicit EnvelopeSceneVoices(std::vector<V> v) : v_(std::move(v)) {}
  uint32_t read(std::span<FaceVoice> out) override {
    uint32_t n = 0;
    for (const V& v : v_) {
      if (n >= out.size()) break;
      FaceVoice& f = out[n++];
      f.slot = v.slot;
      f.sampleCount = 12;
      for (uint32_t k = 0; k < f.sampleCount; ++k)
        f.samples[k] = static_cast<float>(meta::encodeEnvelopeTap(v.from + (v.to - v.from) * k / 11.0, v.released));
      f.phase = f.samples[f.sampleCount - 1];
    }
    return n;
  }
  uint64_t serial() const noexcept override { return 1; }
  bool live() const noexcept override { return false; }

private:
  std::vector<V> v_;
};

// TMM.11: the Envelope (project_patch::buildMetaEnvelopeProject). The Device view of "Pluck": Phase accumulator →
// Wavetable → Envelope, each face its panel, the Envelope's dots in every stage.
Status setupDeviceMetaEnvelope(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-meta-envelope: no device library", {}};
  project_patch::MetaEnvelopeIds ids;
  if (Status s = project_patch::buildMetaEnvelopeProject(ctx.store(), &ids); !s) return s;
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-meta-envelope: the Device view is not installed", {}};
  static SyntheticVoices accVoices, wtVoices;
  static EnvelopeSceneVoices envVoices({{0, 0.30, 0.52, false}, {1, 0.96, 0.995, false}, {2, 0.62, 0.55, false},
                                        {3, 0.35, 0.35, false}, {4, 0.33, 0.21, true}, {5, 0.05, 0.03, true}});
  accVoices.setVoices({{0, 4.0, 0.06, 0.0, kSceneNoteInc[0]}, {1, 5.0, 0.31, 0.0, kSceneNoteInc[1]}, {2, 6.0, 0.58, 0.0, kSceneNoteInc[2]}, {3, 7.5, 0.82, 0.0, kSceneNoteInc[3]}, {4, 3.0, 0.2, 0.0, kSceneNoteInc[4]}, {5, 9.0, 0.7, 0.0, kSceneNoteInc[5]}});
  accVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  wtVoices.setVoices({{0, 4.0, 0.06, 0.35}, {1, 5.0, 0.31, 0.35}, {2, 6.0, 0.58, 0.35}, {3, 7.5, 0.82, 0.35},
                      {4, 3.0, 0.2, 0.35}, {5, 9.0, 0.7, 0.35}});
  for (SyntheticVoices* sv : {&accVoices, &wtVoices}) {
    sv->advance(1.0 / 60.0);
    sv->freeze();
  }
  const doc::DeviceId acc = ids.acc, wt = ids.wt, env = ids.env;
  v->faceVoices = [acc, wt, env](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    if (!path.empty()) return nullptr;
    return d == acc ? &accVoices : (d == wt ? &wtVoices : (d == env ? static_cast<VoiceTapReader*>(&envVoices) : nullptr));
  };
  ctx.setFaceVoices(v->faceVoices);  // GU.13: a leaf's panel reads them through the context, as the app's
  ctx.setSelectedTrack(ids.pluck);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  v->rebuild();
  const doc::DeviceId one[1] = {env};
  v->select(one);
  shell.tree().layoutNow();
  return Ok{};
}

// TMM.11: the same project in the project patch view, "Shape" opened in place: acc1 → wt1 → env1 (the Envelope as a
// VCA, its done wired to the instrument's), the Envelope's dots in every stage.
Status setupProjectMetaEnvelope(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-meta-envelope: no device library", {}};
  project_patch::MetaEnvelopeIds ids;
  if (Status s = project_patch::buildMetaEnvelopeProject(ctx.store(), &ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-meta-envelope: the view is not installed", {}};
  static SyntheticVoices accVoices, wtVoices;
  static EnvelopeSceneVoices envVoices({{0, 0.20, 0.31, false}, {1, 0.86, 0.80, false}, {2, 0.70, 0.70, false},
                                        {3, 0.55, 0.47, true}});
  accVoices.setVoices({{0, 4.0, 0.08, 0.0, kSceneNoteInc[0]}, {1, 5.0, 0.4, 0.0, kSceneNoteInc[1]}, {2, 6.0, 0.66, 0.0, kSceneNoteInc[2]}, {3, 7.0, 0.9, 0.0, kSceneNoteInc[3]}});
  accVoices.setRate(kSceneRate);  // TMM.13-fix2: real note frequencies (their colours, not 0 Hz red)
  wtVoices.setVoices({{0, 4.0, 0.08, 0.7}, {1, 5.0, 0.4, 0.7}, {2, 6.0, 0.66, 0.7}, {3, 7.0, 0.9, 0.7}});
  for (SyntheticVoices* sv : {&accVoices, &wtVoices}) {
    sv->advance(1.0 / 60.0);
    sv->freeze();
  }
  const doc::DeviceId inst = ids.shapeInst;
  v->faceVoices = [inst](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    if (d != inst) return nullptr;
    if (path == "acc1") return &accVoices;
    if (path == "wt1") return &wtVoices;
    if (path == "env1") return &envVoices;
    return nullptr;
  };
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->setDeviceExpanded(inst, true);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.shape, false);
  shell.tree().layoutNow();
  return Ok{};
}

// TMM.14 / GU.4: the Device view of "Keys": the default synth, then the VCA module's panel (its group frame).
Status setupDeviceMetaVca(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-meta-vca: no device library", {}};
  project_patch::MetaVcaIds ids;
  if (Status s = project_patch::buildMetaVcaProject(ctx.store(), &ids); !s) return s;
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-meta-vca: the Device view is not installed", {}};
  const doc::DeviceId vca = ids.vca;
  ctx.setSelectedTrack(ids.keys);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  v->rebuild();
  const doc::DeviceId one[1] = {vca};
  v->select(one);
  shell.tree().layoutNow();
  return Ok{};
}

// TX.spectrum: the spectrum project's analysis (project_patch::buildSpectrumProject), as the playback link's taps would
// carry it: EQ Eight's (the test signal in; out through its three bells, its exact curve) with the measured response,
// and the Spectrum device's (the EQ's output), analysed now and held (deterministic).
Status adoptSpectrumFeeds(ViewContext& ctx, const project_patch::MetaSpectrumIds& ids) {
  struct Bell {
    double f, db, q;
  };
  static constexpr Bell kBells[] = {{250.0, -4.0, 1.2}, {1000.0, 6.0, 2.0}, {6000.0, -5.0, 0.7071}};
  // GU.10: EQ Eight's display math is retired; the bells are the analog prototypes the old exact curve sampled
  // (H(s) = (s² + s·A/q + 1) / (s² + s/(A·q) + 1), s = j·f/f0, A = 10^(dB/40)).
  auto eqMag = [](double hz) {
    std::complex<double> h = 1.0;
    for (const Bell& b : kBells) {
      const std::complex<double> s(0.0, hz / b.f);
      const double A = std::pow(10.0, b.db / 40.0);
      h *= (s * s + s * (A / b.q) + 1.0) / (s * s + s / (A * b.q) + 1.0);
    }
    return std::abs(h);
  };
  devices::AnalysedFeed eq = devices::makeAnalysedFeed(eqMag, true), sp = devices::makeAnalysedFeed(eqMag, false);
  std::shared_ptr<TapRing> eqTap(std::move(eq.tap)), spTap(std::move(sp.tap));
  const doc::DeviceId eqId = ids.eq, spId = ids.spectrum;
  ctx.setAnalysisSource(
      [eqId, spId, eqTap, spTap](doc::DeviceId d) -> std::shared_ptr<TapRing> {
        return d == eqId ? eqTap : d == spId ? spTap : nullptr;
      },
      48000.0);
  if (ctx.analysisHub() == nullptr) return Error{ErrorCode::Unknown, "spectrum scene: no analysis hub", {}};
  ctx.analysisHub()->adopt(eqId, eqTap, std::shared_ptr<devices::SpectrumFeed>(std::move(eq.feed)));
  ctx.analysisHub()->adopt(spId, spTap, std::shared_ptr<devices::SpectrumFeed>(std::move(sp.feed)));
  return Ok{};
}

// The Device view of "Mix" (an audio track): EQ Eight over its output spectrum with the measured response (violet,
// on the exact curve), the Spectrum module's face.
Status setupDeviceMetaSpectrum(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-meta-spectrum: no device library", {}};
  project_patch::MetaSpectrumIds ids;
  if (Status s = project_patch::buildSpectrumProject(ctx.store(), &ids); !s) return s;
  if (Status s = adoptSpectrumFeeds(ctx, ids); !s) return s;
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-meta-spectrum: the Device view is not installed", {}};
  ctx.setSelectedTrack(ids.keys);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  v->rebuild();
  shell.tree().layoutNow();
  const doc::DeviceId one[1] = {ids.spectrum};
  v->select(one);
  shell.tree().layoutNow();
  return Ok{};
}

// The same project in the project patch view: the band of "Mix", EQ Eight's panel and the Spectrum face as blocks.
Status setupProjectMetaSpectrum(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-meta-spectrum: no device library", {}};
  project_patch::MetaSpectrumIds ids;
  if (Status s = project_patch::buildSpectrumProject(ctx.store(), &ids); !s) return s;
  if (Status s = adoptSpectrumFeeds(ctx, ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-meta-spectrum: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.keys, false);
  shell.tree().layoutNow();
  return Ok{};
}

constexpr double kScenePi = 3.14159265358979323846;

// ---- TX.scope ---------------------------------------------------------------------------------------------------------

// An audio track "Scopes" with three Oscilloscopes, each showing another timebase: PITCH locked to the note (a bright
// 220 Hz wave whose tap carries the voice's pitch), FIXED with an edge trigger in two lanes (L 110 Hz bright, R a
// 165 Hz sine), BEAT over one bar (a groove: a decaying 55 Hz kick on each beat, a noise tick on the off-beats).
struct ScopeIds {
  doc::TrackId track;
  doc::DeviceId pitch, fixed, beat;
};

Status buildScopeProject(doc::DocumentStore& store, ScopeIds& ids) {
  const auto t = runEdit(store, doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Audio, {}, "Scopes",
                                                      doc::Colour{0x3FA9F5}));
  if (!t || t->tracks.empty()) return Error{ErrorCode::Unknown, "scope scene: no track", {}};
  ids.track = t->tracks[0];
  auto add = [&](std::vector<std::pair<std::string, double>> opts) -> doc::DeviceId {
    const auto e = runEdit(store, doc::ops::addMetaDevice(*store.current(), store.ids(), ids.track, "oscilloscope"));
    if (!e || e->devices.empty()) return {};
    std::vector<std::pair<std::string, meta::DataValue>> f;
    for (auto& [k, v] : opts) f.emplace_back(k, meta::DataValue::num(v));
    (void)runEdit(store, doc::ops::setMetaData(*store.current(), e->devices[0], "", "oscilloscope",
                                               std::make_shared<const meta::DataValue>(meta::DataValue::record(std::move(f)))));
    return e->devices[0];
  };
  ids.pitch = add({{"lock", 1.0}, {"cyc", 2.0}});                        // Pitch, Tracking, 2 cycles (the note's period)
  ids.fixed = add({{"tb", 0.0}, {"ch", 4.0}, {"win", 12.0}});            // Fixed 12 ms, rising trigger, L / R lanes
  ids.beat = add({{"tb", 2.0}, {"glow", 0.6}});                          // Beat, 1 bar
  if (!ids.pitch.valid() || !ids.fixed.valid() || !ids.beat.valid()) return Error{ErrorCode::Unknown, "scope scene: no Oscilloscope", {}};
  return Ok{};
}

// A 4-channel tap of 3 s of `gen(i, beat, hz, l, r)` at 48 kHz through a manual feed that keeps its scope history (a
// consumer for the scene's life), then held (deterministic).
struct ScopeSource {
  std::shared_ptr<TapRing> tap;
  std::shared_ptr<devices::SpectrumFeed> feed;
};
template <class Gen>
ScopeSource makeScopeSource(Gen gen) {
  ScopeSource s;
  s.tap = std::make_shared<TapRing>(4, 32768, 4096);
  s.feed = std::make_shared<devices::SpectrumFeed>(*s.tap, devices::AnalysisHub::config(48000.0), devices::SpectrumFeed::Manual{});
  s.feed->acquire(devices::kWantScope);
  constexpr uint32_t kFrames = 144000, kChunk = 4096;
  std::vector<float> ch[4];
  for (auto& c : ch) c.resize(kChunk);
  for (uint32_t i = 0; i < kFrames; i += kChunk) {
    const uint32_t n = std::min(kChunk, kFrames - i);
    for (uint32_t k = 0; k < n; ++k) gen(i + k, ch[0][k], ch[1][k], ch[2][k], ch[3][k]);
    const float* p[4] = {ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data()};
    s.tap->write(p, 4, n);
    s.feed->step(1.0 + static_cast<double>(i) / 48000.0);
  }
  s.feed->hold();
  return s;
}

double scopeBright(double ph) {
  double v = 0.0;
  for (int k = 1; k <= 8; ++k) v += std::sin(2.0 * kScenePi * k * ph + 0.7 * k * k) / k;
  return 0.45 * v;
}

Status adoptScopeFeeds(ViewContext& ctx, const ScopeIds& ids) {
  constexpr double kSr = 48000.0, kBeat = 2.0 / kSr;  // 120 bpm
  ScopeSource pitch = makeScopeSource([](uint32_t i, float& b, float& hz, float& l, float& r) {
    b = static_cast<float>(i * kBeat);
    hz = 220.0f;
    l = r = static_cast<float>(0.6 * scopeBright(220.0 * i / kSr));
  });
  ScopeSource fixed = makeScopeSource([](uint32_t i, float& b, float& hz, float& l, float& r) {
    b = static_cast<float>(i * kBeat);
    hz = 0.0f;
    l = static_cast<float>(0.5 * scopeBright(110.0 * i / kSr));
    r = static_cast<float>(0.35 * std::sin(2.0 * kScenePi * 165.0 * i / kSr));
  });
  ScopeSource beat = makeScopeSource([seed = uint32_t{0x2468ACEu}](uint32_t i, float& b, float& hz, float& l, float& r) mutable {
    const double beatPos = i * kBeat;
    b = static_cast<float>(beatPos);
    hz = 0.0f;
    const double inBeat = beatPos - std::floor(beatPos), sec = inBeat * 0.5;  // seconds since the beat
    const double kick = std::exp(-sec / 0.07) * std::sin(2.0 * kScenePi * 55.0 * sec);
    seed = seed * 1664525u + 1013904223u;
    const double noise = (static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
    const double off = inBeat >= 0.5 ? std::exp(-(sec - 0.25) / 0.012) * noise * 0.35 : 0.0;
    l = static_cast<float>(0.8 * kick + off);
    r = static_cast<float>(0.8 * kick - 0.6 * off);
  });
  const doc::DeviceId p = ids.pitch, f = ids.fixed, bt = ids.beat;
  ctx.setAnalysisSource(
      [p, f, bt, pt = pitch.tap, ft = fixed.tap, btt = beat.tap](doc::DeviceId d) -> std::shared_ptr<TapRing> {
        return d == p ? pt : d == f ? ft : d == bt ? btt : nullptr;
      },
      kSr);
  if (ctx.analysisHub() == nullptr) return Error{ErrorCode::Unknown, "scope scene: no analysis hub", {}};
  ctx.analysisHub()->adopt(p, pitch.tap, pitch.feed);
  ctx.analysisHub()->adopt(f, fixed.tap, fixed.feed);
  ctx.analysisHub()->adopt(bt, beat.tap, beat.feed);
  return Ok{};
}

// The Device view of "Scopes": the three Oscilloscope faces.
Status setupDeviceMetaScope(Shell& shell) {
  ViewContext& ctx = shell.context();
  ScopeIds ids;
  if (Status s = buildScopeProject(ctx.store(), ids); !s) return s;
  if (Status s = adoptScopeFeeds(ctx, ids); !s) return s;
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-meta-scope: the Device view is not installed", {}};
  ctx.setSelectedTrack(ids.track);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  v->rebuild();
  shell.tree().layoutNow();
  return Ok{};
}

// ---- TX.cables ---------------------------------------------------------------------------------------------------

// The user's meta patch (bug 2026-11-02_18-21-54, --project tests/views/fixtures/meta-cables): Notes → Phase accumulator →
// Wavetable → Envelope → Oscilloscope → Mixer, and a free VCA (Envelope.out → VCA.in, VCA.out → the accumulator's
// frequency) — every connection a hanging cable at its rest shape. `zoom` > 0: that zoom on the band (0: the band
// framed); `glow`: the track playing (a fixed meter level) and the VCA sweeping the frequency (fixed sweeps), live.
Status setupProjectCablesAt(Shell& shell, double zoom, bool glow) {
  ViewContext& ctx = shell.context();
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-cables: the view is not installed", {}};
  const doc::Track* track = nullptr;
  doc::forEachTrack(*ctx.doc(), [&](const doc::Track& t, const doc::Track*) {
    if (track == nullptr && t.kind == doc::TrackKind::Midi) track = &t;
  });
  if (track == nullptr) return Error{ErrorCode::NotFound, "project-cables: no MIDI track (needs the meta-cables fixture)", {}};
  const doc::TrackId id = track->id;
  if (glow) {
    static FixedMeterSource meters;
    static FixedModulationSource sweeps;
    meters = FixedMeterSource{};
    sweeps = FixedModulationSource();
    ui::MeterLevels l;
    l.rmsDb[0] = l.rmsDb[1] = -14.0f;
    l.peakDb[0] = -6.0f;
    l.peakDb[1] = -7.5f;
    meters.set(id, l);
    meters.setLive(true);
    for (const doc::Wire& w : doc::wiresOf(*ctx.doc())) sweeps.set(w.to, w.param, ModSweep{true, 0.35f, 0.2f});
    sweeps.setLive(true);
    v->setMeterSource(&meters);
    ctx.setModulationSource(&sweeps);
  }
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(id, false);
  shell.tree().layoutNow();
  if (zoom > 0.0) {
    const project_patch::ProjectPatch& pp = v->patch();
    const uint32_t b = pp.bandOf(id);
    if (b == project_patch::kNone) return Error{ErrorCode::NotFound, "project-cables: no band", {}};
    const ui::IRect r = v->canvas().rect();
    const float us = shell.tree().scale();
    circuit::CircuitCanvas::View view;
    view.zoom = zoom;
    view.x = 0.5 * (pp.bands[b].x0 + pp.bands[b].x1) - 0.5 * r.w / (zoom * us);
    view.y = pp.bands[b].y + 0.5 * pp.bands[b].h - 0.5 * r.h / (zoom * us);
    v->canvas().setView(view);
    shell.tree().layoutNow();
  }
  if (glow) v->canvas().wakeCables();
  return Ok{};
}
Status setupProjectCables(Shell& shell) { return setupProjectCablesAt(shell, 0.0, false); }

// TX.orbit-ports (SPEC §15 2026-11-03 "orbiting ports, a round VCA, Delete on a cable"): the same patch closer, framed on
// the VCA and its neighbours — every block's main input (a hollow ring) and output (a disc with a bright core and a
// glow) toward its cables. GU.4: the VCA is its module (a group drawn as its panel: the round knob, the curve editor);
// `panel`: framed close on it (zoom 2: the panel's elements).
Status setupProjectOrbitPorts(Shell& shell, bool panel) {
  if (Status s = setupProjectCablesAt(shell, 0.0, false); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  project_patch::ProjectCanvas& c = v->canvas();
  c.setCablesAnimated(false);
  const project_patch::ProjectPatch& pp = v->patch();
  const doc::Project& doc = *shell.context().doc();
  uint32_t vca = project_patch::kNone;
  for (uint32_t i = 0; i < pp.nodes.size(); ++i)
    if (pp.nodes[i].kind == project_patch::NodeKind::Device)
      if (const doc::Device* d = doc::findDevice(doc, pp.nodes[i].device).device; d != nullptr && doc::isGroupDevice(*d) && d->name == "VCA")
        vca = i;
  if (vca == project_patch::kNone) return Error{ErrorCode::NotFound, "project-orbit-ports: no VCA (needs the meta-cables fixture)", {}};
  const project_patch::Node& n = pp.nodes[vca];
  const ui::IRect r = c.rect();
  const float us = shell.tree().scale();
  circuit::CircuitCanvas::View view;
  view.zoom = panel ? 2.0 : 1.25;
  view.x = n.x + 0.5 * n.w - 0.5 * r.w / (view.zoom * us);
  view.y = n.y + 0.5 * n.h - 0.5 * r.h / (view.zoom * us) + (panel ? 50.0 : 0.0);
  c.setView(view);
  shell.tree().layoutNow();
  // GU.5b: framed close, the dial's range and curve popup open (a click on its chevron).
  if (panel && vca < v->scene().sceneNode.size() && v->scene().sceneNode[vca] != project_patch::kNone)
    if (Face* f = c.panelOn(v->scene().sceneNode[vca])) {
      FacePointer e;
      e.kind = FacePointer::Kind::Down;
      e.local = {60.0f, 85.0f};
      e.pxPerUnit = static_cast<float>(view.zoom) * us;
      e.uiScale = us;
      (void)f->onPointer(e);
      e.kind = FacePointer::Kind::Up;
      (void)f->onPointer(e);
      shell.tree().layoutNow();
    }
  return Ok{};
}
Status setupProjectCablesFar(Shell& shell) { return setupProjectCablesAt(shell, 0.2, false); }
Status setupProjectCablesGlow(Shell& shell) { return setupProjectCablesAt(shell, 0.0, true); }

// The same project in the project patch view: the band of "Scopes", its three faces as blocks.
Status setupProjectMetaScope(Shell& shell) {
  ViewContext& ctx = shell.context();
  ScopeIds ids;
  if (Status s = buildScopeProject(ctx.store(), ids); !s) return s;
  if (Status s = adoptScopeFeeds(ctx, ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-meta-scope: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.track, false);
  shell.tree().layoutNow();
  return Ok{};
}

// ---- TX.spectrogram ---------------------------------------------------------------------------------------------------

// An audio track "Spectra" with three Spectrograms over 3 s: a bright melody with hat ticks (Magma, log from 0 Hz), a
// minor-seventh pad over a pitch-dropping sub kick (Inferno, the keyboard on the left), and a 16th-note arpeggio with a
// kick on each beat in the Loop playhead over one bar (Pastel, coloured by frequency).
struct GramIds {
  doc::TrackId track;
  doc::DeviceId melody, pad, loop;
  meta::DataPtr dMelody, dPad, dLoop;
};

Status buildGramProject(doc::DocumentStore& store, GramIds& ids) {
  const auto t = runEdit(store, doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Audio, {}, "Spectra",
                                                      doc::Colour{0xB86BFF}));
  if (!t || t->tracks.empty()) return Error{ErrorCode::Unknown, "spectrogram scene: no track", {}};
  ids.track = t->tracks[0];
  auto add = [&](std::vector<std::pair<std::string, double>> opts, meta::DataPtr& data) -> doc::DeviceId {
    const auto e = runEdit(store, doc::ops::addMetaDevice(*store.current(), store.ids(), ids.track, "spectrogram"));
    if (!e || e->devices.empty()) return {};
    std::vector<std::pair<std::string, meta::DataValue>> f;
    for (auto& [k, v] : opts) f.emplace_back(k, meta::DataValue::num(v));
    data = std::make_shared<const meta::DataValue>(meta::DataValue::record(std::move(f)));
    (void)runEdit(store, doc::ops::setMetaData(*store.current(), e->devices[0], "", "spectrogram", data));
    return e->devices[0];
  };
  ids.melody = add({{"span", 3.0}}, ids.dMelody);
  ids.pad = add({{"span", 3.0}, {"map", 1.0}, {"keys", 1.0}}, ids.dPad);
  ids.loop = add({{"span", 2.0}, {"ph", 1.0}, {"map", 7.0}, {"src", 1.0}}, ids.dLoop);
  if (!ids.melody.valid() || !ids.pad.valid() || !ids.loop.valid())
    return Error{ErrorCode::Unknown, "spectrogram scene: no Spectrogram", {}};
  return Ok{};
}

// 3.5 s of `gen(i, beat, l, r)` at 48 kHz into a 4-channel tap through a manual feed that computes the columns the face
// will ask for (its request for a plot `plotPx` wide, made before it first paints), then held (deterministic).
template <class Gen>
ScopeSource makeGramSource(const meta::DataPtr& data, float plotPx, Gen gen) {
  ScopeSource s;
  s.tap = std::make_shared<TapRing>(4, 32768, 4096);
  s.feed = std::make_shared<devices::SpectrumFeed>(*s.tap, devices::AnalysisHub::config(48000.0), devices::SpectrumFeed::Manual{});
  s.feed->setSpectrogram(s.feed.get(), gramRequest(data.get(), plotPx, 48000.0));
  s.feed->acquire(devices::kWantSpectrogram);
  constexpr uint32_t kFrames = 168000, kChunk = 4096;
  std::vector<float> ch[4];
  for (auto& c : ch) c.resize(kChunk);
  for (uint32_t i = 0; i < kFrames; i += kChunk) {
    const uint32_t n = std::min(kChunk, kFrames - i);
    for (uint32_t k = 0; k < n; ++k) {
      ch[1][k] = 0.0f;
      gen(i + k, ch[0][k], ch[2][k], ch[3][k]);
    }
    const float* p[4] = {ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data()};
    s.tap->write(p, 4, n);
    s.feed->step(1.0 + static_cast<double>(i) / 48000.0);
  }
  s.feed->hold();
  return s;
}

double gramPartials(double hz, double t, int n, double tilt) {
  double v = 0.0;
  for (int k = 1; k <= n; ++k) v += std::sin(2.0 * kScenePi * hz * k * t + 0.9 * k) * std::pow(static_cast<double>(k), -tilt);
  return v;
}

Status adoptGramFeeds(ViewContext& ctx, const GramIds& ids, float plotPx) {
  constexpr double kSr = 48000.0, kBeat = 2.0 / kSr;  // 120 bpm
  // The melody: an A-minor pentatonic phrase of eighths, six harmonics, vibrato on the long last note; hat ticks.
  ScopeSource melody = makeGramSource(ids.dMelody, plotPx, [seed = uint32_t{0x1357BDFu}, ph = 0.0](uint32_t i, float& b, float& l, float& r) mutable {
    static const int kPhrase[] = {0, 3, 5, 7, 10, 7, 5, 12, 10, 7, 5, 3, 0, 0};
    const double t = i / kSr;
    b = static_cast<float>(i * kBeat);
    const int step = std::min(13, static_cast<int>(t / 0.25));
    double semis = kPhrase[step];
    if (step == 13) semis += 0.35 * std::sin(2.0 * kScenePi * 5.5 * (t - 3.25));
    const double hz = 220.0 * std::pow(2.0, semis / 12.0);
    ph += hz / kSr;
    const double inNote = t - 0.25 * step;
    const double env = std::min(1.0, inNote / 0.01) * (step == 13 ? 1.0 : std::exp(-inNote / 0.35));
    double v = 0.0;
    for (int k = 1; k <= 6; ++k) v += std::sin(2.0 * kScenePi * k * ph) / k;
    v *= 0.3 * env;
    seed = seed * 1664525u + 1013904223u;
    const double noise = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
    const double tick = std::fmod(t + 0.125, 0.25);
    v += 0.12 * noise * std::exp(-tick / 0.008);
    l = r = static_cast<float>(v);
  });
  // The pad: A minor seventh, eight harmonics each, a slow swell; a sub kick (110 → 45 Hz) every half second.
  ScopeSource pad = makeGramSource(ids.dPad, plotPx, [](uint32_t i, float& b, float& l, float& r) {
    const double t = i / kSr;
    b = static_cast<float>(i * kBeat);
    const double roots[] = {220.0, 261.626, 329.628, 391.995};
    double v = 0.0;
    for (const double f : roots) v += gramPartials(f, t, 8, 1.2);
    v *= 0.05 * (0.6 + 0.4 * std::sin(2.0 * kScenePi * 0.4 * t));
    const double kt = std::fmod(t, 0.5);
    const double kphase = 45.0 * kt + 65.0 * 0.04 * (1.0 - std::exp(-kt / 0.04));
    v += 0.7 * std::exp(-kt / 0.18) * std::sin(2.0 * kScenePi * kphase);
    l = static_cast<float>(v);
    r = static_cast<float>(v * 0.9);
  });
  // The loop: a 16th arpeggio (A2 C3 E3 G3 A3 G3 E3 C3), a kick on each beat.
  ScopeSource loop = makeGramSource(ids.dLoop, plotPx, [ph = 0.0](uint32_t i, float& b, float& l, float& r) mutable {
    static const int kArp[] = {0, 3, 7, 10, 12, 10, 7, 3};
    const double beatPos = i * kBeat;
    b = static_cast<float>(beatPos);
    const int sixteenth = static_cast<int>(beatPos * 4.0);
    const double hz = 110.0 * std::pow(2.0, kArp[sixteenth % 8] / 12.0);
    ph += hz / kSr;
    const double inStep = (beatPos * 4.0 - sixteenth) * 0.125;
    double v = 0.0;
    for (int k = 1; k <= 7; ++k) v += std::sin(2.0 * kScenePi * k * ph) / (k * k * 0.6 + 0.4);
    v *= 0.35 * std::exp(-inStep / 0.09);
    const double kt = (beatPos - std::floor(beatPos)) * 0.5;
    v += 0.6 * std::exp(-kt / 0.12) * std::sin(2.0 * kScenePi * (50.0 * kt + 60.0 * 0.03 * (1.0 - std::exp(-kt / 0.03))));
    l = r = static_cast<float>(v);
  });
  const doc::DeviceId a = ids.melody, p = ids.pad, c = ids.loop;
  ctx.setAnalysisSource(
      [a, p, c, at = melody.tap, pt = pad.tap, ct = loop.tap](doc::DeviceId d) -> std::shared_ptr<TapRing> {
        return d == a ? at : d == p ? pt : d == c ? ct : nullptr;
      },
      kSr);
  if (ctx.analysisHub() == nullptr) return Error{ErrorCode::Unknown, "spectrogram scene: no analysis hub", {}};
  ctx.analysisHub()->adopt(a, melody.tap, melody.feed);
  ctx.analysisHub()->adopt(p, pad.tap, pad.feed);
  ctx.analysisHub()->adopt(c, loop.tap, loop.feed);
  return Ok{};
}

// The Device view of "Spectra": the three Spectrogram faces (the panel draws them at ≈ 1.48: the plot ≈ 421 device px).
Status setupDeviceMetaSpectrogram(Shell& shell) {
  ViewContext& ctx = shell.context();
  GramIds ids;
  if (Status s = buildGramProject(ctx.store(), ids); !s) return s;
  if (Status s = adoptGramFeeds(ctx, ids, 421.0f); !s) return s;
  DeviceChainView* v = findDeviceChain(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "device-meta-spectrogram: the Device view is not installed", {}};
  ctx.setSelectedTrack(ids.track);
  ctx.setDetailVisible(true);
  ctx.setDetailMode(DetailMode::Device);
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) body->setPosition(300.0f);
  shell.tree().layoutNow();
  v->rebuild();
  shell.tree().layoutNow();
  return Ok{};
}

// The same project in the project patch view: the band of "Spectra", its three faces as blocks (plots ≈ 254 px).
Status setupProjectMetaSpectrogram(Shell& shell) {
  ViewContext& ctx = shell.context();
  GramIds ids;
  if (Status s = buildGramProject(ctx.store(), ids); !s) return s;
  if (Status s = adoptGramFeeds(ctx, ids, 254.0f); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-meta-spectrogram: the view is not installed", {}};
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.track, false);
  shell.tree().layoutNow();
  return Ok{};
}

// The same project in the project patch view, "Trem" opened in place: acc1 → wt1 → vca1 (Bend 0 … 2), four voices
// whose knob positions a modulation spreads.
Status setupProjectMetaVca(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "project-meta-vca: no device library", {}};
  project_patch::MetaVcaIds ids;
  if (Status s = project_patch::buildMetaVcaProject(ctx.store(), &ids); !s) return s;
  project_patch::ProjectPatchView* v = findProjectPatch(shell);
  if (v == nullptr) return Error{ErrorCode::NotFound, "project-meta-vca: the view is not installed", {}};
  static SyntheticVoices wtVoices;
  wtVoices.setVoices({{0, 4.0, 0.08, 0.0}, {1, 5.0, 0.4, 0.0}, {2, 6.0, 0.66, 0.0}, {3, 7.0, 0.9, 0.0}});
  wtVoices.advance(1.0 / 60.0);
  wtVoices.freeze();
  const doc::DeviceId inst = ids.tremInst;
  v->faceVoices = [inst](doc::DeviceId d, std::string_view path) -> VoiceTapReader* {
    return d == inst && path == "wt1" ? &wtVoices : nullptr;
  };
  shell.tree().layoutNow();
  v->setShown(true, false);
  closeModules(*v);
  shell.tree().layoutNow();
  v->setDeviceExpanded(inst, true);
  shell.tree().layoutNow();
  v->rebindFaces();
  v->focusBand(ids.trem, false);
  shell.tree().layoutNow();
  return Ok{};
}

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
    {"mos-circle-lit", 440, 560, setupMosCircleLit},
    {"mos-wheel-popover", 1600, 900, setupMosWheelPopover},
    {"tracks-mixer", 1280, 800, setupTracksMixer},
    {"mixer-view", 1280, 800, setupMixerView},
    {"automation-lanes", 1280, 800, setupAutomationLanes},
    {"automation-mode-over-clips", 1280, 800, setupAutomationMode},
    {"audio-prefs", 1280, 800, setupAudioPrefs},
    {"audio-prefs-wasapi", 1280, 800, setupAudioPrefsWasapi},
    {"library-prefs", 1280, 800, setupLibraryPrefs},
    {"midi-prefs", 1280, 800, setupMidiPrefs},
    {"warp-prefs", 1280, 800, setupWarpPrefs},
    {"record-prefs", 1280, 800, setupRecordPrefs},
    {"export-audio", 1280, 800, setupExportAudio},
    {"menu-bar", 1280, 800, setupMenuBar},
    {"tunings-panel", 1280, 800, setupTuningsPanel},
    {"pianoroll-ratios", 1280, 800, setupPianoRollRatios},
    {"pianoroll-note-menu", 1280, 800, setupPianoRollNoteMenu},
    {"pianoroll-ji-morph", 1280, 800, setupPianoRollJiMorph},
    {"pianoroll-ji-undertone", 1280, 800, setupPianoRollJiUndertone},
    {"pianoroll-ji-tuning", 1280, 800, setupPianoRollJiTuning},
    {"pianoroll-curve-notes", 1280, 800, setupPianoRollCurveNotes},
    {"pianoroll-ji-entered", 1280, 800, setupPianoRollJiEntered},
    {"pianoroll-tune-to-ji", 1280, 800, setupPianoRollTuneToJi},
    {"pianoroll-tune-to-ji-31edo", 1280, 800, setupPianoRollTuneToJi31},
    {"pianoroll-fundamental-lock", 1280, 800, setupPianoRollFundamentalLock},
    {"pianoroll-bends", 1280, 800, setupPianoRollBends},
    {"pianoroll-vibrato", 1280, 800, setupPianoRollVibrato},
    {"pianoroll-ride", 1280, 800, setupPianoRollRide},
    {"circuit-symbols", 1280, 800, setupCircuitSymbols},
    {"circuit", 1280, 800, nullptr, setupCircuit},
    {"circuit-svf", 1280, 800, nullptr, setupCircuitSvf},
    {"circuit-svf-core", 1280, 800, nullptr, setupCircuitSvfCore},
    {"circuit-editor", 1280, 800, nullptr, setupCircuitEditor},
    {"circuit-editor-error", 1280, 800, nullptr, setupCircuitEditorError},
    {"circuit-editor-probe", 1280, 800, nullptr, setupCircuitEditorProbe},  // T4.5: --project stdlib/envelopes/adsr.pqp
    {"circuit-editor-cost", 1280, 800, nullptr, setupCircuitEditorCost},    // T4.5: --project stdlib/filters/svf.pqp
    {"device-chain", 1280, 800, setupDeviceChain},
    {"device-wiring", 1280, 800, setupDeviceWiring},  // T4.7
    {"tracks-frozen", 1280, 800, setupTracksFrozen},  // FZ.3: --project tests/views/fixtures/mixer
    {"device-frozen", 2200, 800, setupDeviceFrozen},  // FZ.3 (wide: the instrument group, Saturator and Utility in view)
    {"eq-circuit-roundtrip", 1280, 800, setupEqCircuitRoundTrip},  // EQ.4b
    {"project-patch-overview", 1280, 800, setupProjectPatchOverview},  // T4.13
    {"project-patch-band", 1280, 800, setupProjectPatchBand},
    {"project-mixer", 1280, 800, setupProjectMixer},  // TX.mixer-look
    {"project-patch-tuning", 1280, 800, setupProjectPatchTuning},  // BR.tuning-block
    {"project-patch-tuning-wheel", 1280, 800, setupProjectPatchTuningWheel},  // BR.tuning-outside
    {"project-patch-zoom", 1280, 800, setupProjectPatchZoom},     // T5.x-circuit-chain
    {"project-circuit-device", 1280, 800, setupProjectCircuitDevice},  // T5.x-one-circuit
    {"project-patch-lanes", 1280, 800, setupProjectPatchLanes},        // T5.x-one-circuit: --project tests/views/fixtures/audio
    {"project-patch-lane-edit", 1280, 800, setupProjectPatchLaneEdit},  // TX.lane-edit: a range and automation in the windows
    {"project-patch-auto-wire", 1280, 800, setupProjectPatchAutoWire},  // TL.3: the automation wire
    {"project-meta-faces", 1280, 800, setupProjectMetaFaces},          // TMM.4
    {"project-meta-wavetable", 1280, 800, setupProjectMetaWavetable},  // TMM.7
    {"device-meta-chain", 1280, 800, setupDeviceMetaChain},            // TMM.10
    {"project-meta-chain", 1280, 800, setupProjectMetaChain},          // TMM.10
    {"project-patch-notes", 1280, 800, setupProjectPatchNotes},        // TMM.10-fix2
    {"project-patch-free", 1280, 800, setupProjectPatchFree},          // TX.patching
    {"project-node-menu", 1280, 800, setupProjectNodeMenu},          // TX.node-menu
    {"project-cable-drag", 1280, 800, setupProjectCableDrag},        // TX.node-menu-fix1
    {"project-patch-panels", 1600, 900, setupProjectPatchPanels},      // TX.panel-faces
    {"project-patch-resize", 1600, 900, setupProjectPatchResize},      // TX.resize
    {"project-block-drag", 1600, 900, setupProjectBlockDrag},          // TX.block-drag
    {"project-patch-group", 1600, 900, setupProjectPatchGroup},        // TX.groups
    {"project-patch-nested", 1600, 900, setupProjectPatchNested},      // GU.2
    {"project-patch-group-panel", 1600, 900, setupProjectPatchGroupPanel},          // GU.3
    {"device-group-panel-elements", 1600, 900, setupDeviceGroupPanelElements},      // GU.3
    {"device-group-panel", 1600, 900, setupDeviceGroupPanel},          // TX.groups
    {"device-free-rack", 1600, 900, [](Shell& s) { return setupDeviceFreeRack(s, true); }},              // TX.free-rack
    {"device-free-rack-collapsed", 1600, 900, [](Shell& s) { return setupDeviceFreeRack(s, false); }},   // TX.free-rack
    {"device-path-broken", 1600, 900, setupDevicePathBroken},  // TX.all-free
    {"device-meta-envelope", 1280, 800, setupDeviceMetaEnvelope},      // TMM.11
    {"project-meta-envelope", 1280, 800, setupProjectMetaEnvelope},    // TMM.11
    {"device-meta-vca", 1280, 800, setupDeviceMetaVca},                // TMM.14
    {"project-meta-vca", 1280, 800, setupProjectMetaVca},              // TMM.14
    {"device-meta-spectrum", 1280, 800, setupDeviceMetaSpectrum},      // TX.spectrum
    {"project-meta-spectrum", 1280, 800, setupProjectMetaSpectrum},    // TX.spectrum
    {"device-meta-scope", 1280, 800, setupDeviceMetaScope},            // TX.scope
    {"project-meta-scope", 1280, 800, setupProjectMetaScope},          // TX.scope
    {"project-cables", 1600, 900, setupProjectCables},                 // TX.cables: --project tests/views/fixtures/meta-cables
    {"project-cables-far", 1600, 900, setupProjectCablesFar},          // TX.cables: the same --project, zoom 0.2
    {"project-cables-glow", 1600, 900, setupProjectCablesGlow},        // TX.cables: the same --project, playing
    {"project-orbit-ports", 1600, 900, [](Shell& s) { return setupProjectOrbitPorts(s, false); }},  // TX.orbit-ports: the same --project
    {"project-vca-panel", 1600, 900, [](Shell& s) { return setupProjectOrbitPorts(s, true); }},  // GU.4: the VCA module's panel, near
    {"device-meta-spectrogram", 1280, 800, setupDeviceMetaSpectrogram},    // TX.spectrogram
    {"project-meta-spectrogram", 1280, 800, setupProjectMetaSpectrogram},  // TX.spectrogram
    {"audio-rack", 1280, 800, setupAudioRack},              // T4.8
    {"instrument-rack", 1280, 800, setupInstrumentRack},    // T4.8
    {"device-automation", 1600, 900, setupDeviceAutomation},  // T4.18 (wider: the filter and the rack side by side)
    {"device-knob-curves", 1600, 900, setupDeviceKnobCurves},  // TX.knob-curves
    {"circuit-inactive-hidden", 1280, 800, nullptr, setupCircuitInactiveHidden},  // T4.15: --project stdlib/envelopes/msenv.pqp
    {"circuit-inactive-shown", 1280, 800, nullptr, setupCircuitInactiveShown},
    {"circuit-tooltip", 1280, 800, nullptr, setupCircuitTooltip},
    {"arrangement-hover", 1280, 800, setupArrangementHover},  // T4.16b
    {"pianoroll-hover", 1280, 800, setupPianoRollHover},      // T4.16b: --project tests/views/fixtures/pianoroll-12edo
    {"arrangement-audio", 1280, 800, setupArrangementAudio},  // T5.6: --project tests/views/fixtures/audio
    {"clip-view-audio", 1280, 800, setupClipViewAudio},        // T5.6: the same --project
    {"arrangement-takes", 1280, 800, [](Shell& s) { return setupArrangementTakes(s, false); }},  // T6.10: the same --project
    {"arrangement-comp", 1280, 800, [](Shell& s) { return setupArrangementTakes(s, true); }},    // T6.10
    {"audio-clip-warp-beats", 1280, 800, [](Shell& s) { return setupAudioClipWarp(s, false); }},   // T6.8: the same --project
    {"audio-clip-warp-markers", 1280, 800, [](Shell& s) { return setupAudioClipWarp(s, true); }},  // T6.8
    {"audio-clip-warp-short", 1280, 800, [](Shell& s) { return setupAudioClipWarp(s, true, 240.0f); }},  // AC.fix
    {"browser-panel", 1280, 800, setupBrowserPanel},           // T5.10: the same --project (its samples as the User Library)
    {"browser-search", 1280, 800, setupBrowserSearch},         // T5.10
    {"browser-hotswap", 1280, 800, setupBrowserHotSwap},       // T5.11: the same --project
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

bool isPatchScene(std::string_view scene) noexcept {
  const SceneEntry* e = findScene(scene);
  return e && e->setupPatch;
}

Result<std::unique_ptr<Shell>> buildViewScene(std::string_view scene, ui::UiTree& tree, ViewContext& ctx,
                                              std::string_view patchFile) {
  const SceneEntry* e = findScene(scene);
  if (!e) return Error{ErrorCode::NotFound, "unknown view scene '" + std::string(scene) + "'", {}};
  auto shell = Shell::create(tree, ctx);
  if (!shell) return std::move(shell).error();
  if (e->setup)
    if (Status s = e->setup(**shell); !s) return std::move(s).error();
  if (e->setupPatch)
    if (Status s = e->setupPatch(**shell, patchFile); !s) return std::move(s).error();
  tree.layoutNow();
  return shell;
}

Result<ui::Image> renderViewScene(std::string_view scene, std::string_view projectDir, uint32_t width, uint32_t height,
                                  ui::UiScale scale, const ui::GpuOptions& options, const doc::DeviceLibrary* devices) {
  if (!isViewScene(scene)) return Error{ErrorCode::NotFound, "unknown view scene '" + std::string(scene) + "'", {}};
  if (width == 0 || height == 0) return Error{ErrorCode::InvalidArgument, "renderViewScene: zero size", {}};
  FakeTransport transport;
  ViewContext ctx(transport);
  ctx.setDeviceLibrary(devices);
  const bool patchScene = isPatchScene(scene);  // T4.2: projectDir names a .pqp file
  if (!projectDir.empty() && !patchScene)
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
  auto shell = buildViewScene(scene, tree, ctx, patchScene ? projectDir : std::string_view{});
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

std::shared_ptr<const library::Index> browserDemoIndex(std::string_view samplesDir, uint32_t extraFiles) {
  auto ix = std::make_shared<library::Index>();
  std::string root(samplesDir);
  for (char& c : root)
    if (c == '/') c = '\\';
  ix->places.push_back({root, library::PlaceKind::UserLibrary, "User Library"});
  ix->places.push_back({"C:\\pq_demo\\Field Recordings", library::PlaceKind::Folder, "Field Recordings"});
  ix->placeOnline = {1, 1};
  auto sample = [](std::string name, double seconds, uint32_t rate, uint16_t ch, float bpm = 0) {
    library::EntryRecord r;
    r.name = std::move(name);
    r.kind = library::ItemKind::Sample;
    r.category = library::Category::Samples;
    r.rate = rate;
    r.channels = ch;
    r.frames = static_cast<uint64_t>(seconds * rate);
    r.bits = 24;
    r.bpm = bpm;
    r.size = r.frames * ch * 3;
    return r;
  };
  auto real = [&](const char* name) {
    library::EntryRecord r = sample(name, 1.0, 48000, 2);
    const std::string path = std::string(samplesDir) + "/" + name;
    if (auto info = media::probe(std::filesystem::path(std::u8string(path.begin(), path.end())))) {
      r.frames = info->frames;
      r.rate = info->sampleRate;
      r.channels = static_cast<uint16_t>(info->channels);
      r.bits = static_cast<uint8_t>(info->bitsPerSample);
      r.bpm = static_cast<float>(info->bpm);
    }
    return r;
  };
  // Place 0: the root's subfolders first (depth-first, name order), each with its files right after it.
  const uint32_t r0 = ix->addDir(library::Index::kNone, "", 0, 0);
  ix->addEntry(r0, real("beat.wav"));
  ix->addEntry(r0, real("vox.wav"));
  const uint32_t drums = ix->addDir(r0, "Drums", 0, 1);
  ix->addEntry(drums, sample("Hat Open.aif", 0.42, 44100, 1));
  ix->addEntry(drums, sample("Kick 01.wav", 0.31, 44100, 1));
  ix->addEntry(drums, sample("Snare Tight.wav", 0.27, 48000, 2));
  if (extraFiles > 0) {
    const uint32_t gen = ix->addDir(r0, "Generated", 0, 1);
    char buf[32];
    for (uint32_t i = 0; i < extraFiles; ++i) {
      std::snprintf(buf, sizeof buf, "gen %06u.wav", i);
      ix->addEntry(gen, sample(buf, 0.5 + (i % 97) * 0.1, i % 3 == 0 ? 44100 : 48000, 2, (i % 5 == 0) ? 120.0f : 0.0f));
    }
  }
  const uint32_t loops = ix->addDir(r0, "Loops", 0, 1);
  ix->addEntry(loops, sample("Funk Break 98bpm.wav", 9.8, 44100, 2, 98.0f));
  ix->addEntry(loops, sample("Pad Swell.flac", 12.5, 48000, 2));
  const uint32_t presets = ix->addDir(r0, "Presets", 0, 1);
  const uint32_t op = ix->addDir(presets, "Operator", 0, 2);
  library::EntryRecord pr;
  pr.name = "Bright Pad.pqpreset";
  pr.kind = library::ItemKind::Preset;
  pr.category = library::Category::Instruments;
  ix->addEntry(op, pr);
  for (const char* n : {"Glass Bell.pqpreset", "Sub Pluck.pqpreset", "Warm Keys.pqpreset"}) {  // T5.11: hot-swap
    pr.name = n;
    ix->addEntry(op, pr);
  }
  // Place 1.
  const uint32_t r1 = ix->addDir(library::Index::kNone, "", 1, 0);
  ix->addEntry(r1, sample("Rain on Roof.wav", 64.0, 96000, 2));
  ix->addEntry(r1, sample("Vocal Chop.wav", 1.6, 48000, 1, 120.0f));
  return ix;
}

}  // namespace pq::views
