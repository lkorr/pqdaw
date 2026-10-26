#include "pq/views/view_scenes.h"

#include <algorithm>
#include <optional>
#include <string>

#include "pq/ui/draw_list.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scene_textures.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/circuit/circuit_viewer.h"
#include "pq/views/devices/device_view.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/fake_transport.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/piano_roll/clip_view.h"
#include "pq/views/mos_wheel/mos_chart.h"
#include "pq/views/mos_wheel/mos_circle.h"
#include "pq/views/mos_wheel/mos_wheel.h"
#include "pq/views/prefs/audio_prefs.h"
#include "pq/views/tunings/tunings_panel.h"
#include "pq/document/edit_ops.h"
#include "shell/shell_widgets.h"

namespace pq::views {

namespace {

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
    s.inputChannels = 0;
    s.bufferFrames = 192;
    port.apply(s);
    port.applied.clear();
    port.running.deviceName = s.outputName;
    port.running.inputLatencyMs = 0.0;
    port.running.outputLatencyMs = 8.0;
    port.running.state = AudioStatus::State::Suspended;
    port.load = 0.0;
  }
  shell.context().setAudioPrefs(&port);
  if (!openPreferences(shell.overlay(), port)) return Error{ErrorCode::NotFound, "audio-prefs: no overlay layer", {}};
  return Ok{};
}
Status setupAudioPrefs(Shell& shell) { return openAudioPrefs(shell, false); }
Status setupAudioPrefsWasapi(Shell& shell) { return openAudioPrefs(shell, true); }

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
Status setupCircuitSvf(Shell& shell, std::string_view file) { return openPatchScene(shell, file, "svf"); }
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

// T4.1: the Device view of a MIDI track: Basic Synth → Saturator (Digital Clip, drive +6 dB, selected) → Utility (off),
// generic panels. Needs the device library (renderViewScene's `devices`; pqdaw --screenshot loads the stock one).
Status setupDeviceChain(Shell& shell) {
  ViewContext& ctx = shell.context();
  if (ctx.deviceLibrary() == nullptr) return Error{ErrorCode::NotFound, "device-chain: no device library", {}};
  doc::DocumentStore& store = ctx.store();
  auto run = [&](const doc::ops::EditResult& e) -> std::optional<doc::ops::Edit> {
    if (!e.ok() || !store.dispatch(e->command).ok()) return std::nullopt;
    return *e;
  };
  const auto t = run(doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Midi, {}, "Keys"));
  if (!t) return Error{ErrorCode::Unknown, "device-chain: createTrack failed", {}};
  const doc::TrackId track = t->tracks[0];
  const auto sat = run(doc::ops::addDevice(*store.current(), store.ids(), track, "saturator", doc::DeviceKind::AudioEffect));
  const auto util = run(doc::ops::addDevice(*store.current(), store.ids(), track, "utility", doc::DeviceKind::AudioEffect));
  if (!sat || !util) return Error{ErrorCode::Unknown, "device-chain: addDevice failed", {}};
  const doc::DeviceId satId = sat->devices[0], utilId = util->devices[0];
  if (!run(doc::ops::setDeviceSetting(*store.current(), satId, "type", 6.0)) ||
      !run(doc::ops::setDeviceParam(*store.current(), satId, "drive", 1.9952623149688795)) ||  // +6 dB
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
    {"automation-lanes", 1280, 800, setupAutomationLanes},
    {"automation-mode-over-clips", 1280, 800, setupAutomationMode},
    {"audio-prefs", 1280, 800, setupAudioPrefs},
    {"audio-prefs-wasapi", 1280, 800, setupAudioPrefsWasapi},
    {"menu-bar", 1280, 800, setupMenuBar},
    {"tunings-panel", 1280, 800, setupTuningsPanel},
    {"pianoroll-ratios", 1280, 800, setupPianoRollRatios},
    {"pianoroll-note-menu", 1280, 800, setupPianoRollNoteMenu},
    {"circuit-symbols", 1280, 800, setupCircuitSymbols},
    {"circuit", 1280, 800, nullptr, setupCircuit},
    {"circuit-svf", 1280, 800, nullptr, setupCircuitSvf},
    {"circuit-svf-core", 1280, 800, nullptr, setupCircuitSvfCore},
    {"device-utility", 1280, 800, nullptr, devices::setupDeviceScene},       // T4.6: --project stdlib/devices/utility.pqp
    {"device-saturator", 1280, 800, nullptr, devices::setupDeviceScene},
    {"device-eq-eight", 1280, 800, nullptr, devices::setupDeviceScene},
    {"device-operator", 1280, 800, nullptr, devices::setupDeviceScene},      // T4.12: --project stdlib/devices/operator.pqp
    {"circuit-editor", 1280, 800, nullptr, setupCircuitEditor},
    {"circuit-editor-error", 1280, 800, nullptr, setupCircuitEditorError},
    {"device-chain", 1280, 800, setupDeviceChain},
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

}  // namespace pq::views
