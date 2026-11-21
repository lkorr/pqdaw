// AP.8a (docs/reviews/AP.md H1, H2, H4, M2, M3, M9; the user, 2026-11-21: "changing the tuning resets the arp, make
// sure it plays continuously in real time while retuning"): an Arpeggiator on a MIDI track as the user hears it — the
// playback link's engine played block by block (64 frames), the probe instrument's output = Σ (note gain · voice Hz)
// of the sounding voices (tests/support/blocks.h addProbeInstrument). Every config holds a chord {0, 2, 4} of a MOS
// (5L2s, generator 696) that is released after two steps with Hold on, so from step 2 the arp plays a LATCHED set; the
// tuning then moves (a generator drag over several blocks, a new definition, a clip / track tuning switch) and every
// sounding step must follow the tuning in real time at its pattern place on its grid: checked per sample against
// K · Σ gain · ratio · Hz(degree, the tuning sounding then), outside the 10 ms after each edit.
// 120 BPM, eighth steps (12000 samples), gate 100 %.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/document/note_devices.h"
#include "pq/document/pitch_resolve.h"
#include "pq/engine/arp.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr double kRate = 48000.0;
constexpr Ticks kQ = kPpq;
constexpr size_t kStep = 12000;  // an eighth at 120 BPM

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

double position(const char* name, double value) {
  const NoteParamSpec* s = findNoteDeviceType("arpeggiator")->param(name);
  REQUIRE(s != nullptr);
  return noteParamPosition(*s, value);
}

TuningDef mos(double generator, int32_t n = 7) {
  TuningDef d;
  tuning::MosParams m;
  m.n = n;
  m.period = 1200.0;
  m.generator = generator;
  d.system = m;
  return d;
}
TuningDef edo(int32_t n) {
  TuningDef d;
  d.system = tuning::Edo::make(n).value();
  return d;
}

// The static Hz of absolute degree `d` (equave · size + degree) of `def` (tuning `id`).
double hzIn(const TuningDef& def, TuningId id, int32_t d) {
  auto t = buildTuning(def);
  REQUIRE(t.ok());
  int32_t n = 1;
  if (const tuning::Edo* e = t->edo()) n = static_cast<int32_t>(e->n());
  else if (const tuning::Mos* m = t->mos()) n = m->size();
  const int32_t q = d >= 0 ? d / n : -((-d + n - 1) / n);
  const NotePitch p = resolvePitch(tuning::PitchId::fromDegree(d - q * n, q), *t, id);
  REQUIRE(p.hz.has_value());
  return *p.hz;
}

// What one step sounds: a source degree (of tuning `onB` ? B : A) times an exact ratio, at a note gain.
struct Part {
  int32_t deg = 0;
  double ratio = 1.0;
  double gain = 1.0;
  bool onB = false;
};
using Model = std::function<std::vector<Part>(size_t step)>;

struct Opts {
  std::vector<std::pair<const char*, double>> params;  // arp params (values)
  Ticks chordLen = kQ;   // two steps, then the keys are up (Hold latches)
  bool hold = true;
  bool clipOnB = false;  // the chord clip plays tuning B (31-EDO) on the track's A
};

struct Rig {
  DocumentStore store{playback_fixture::unwiredDefaultProject()};
  TrackId track;
  TuningId a, b;
  ClipId clip;
  DeviceId arp;
  std::unique_ptr<ProjectLink> link;
  ListenerId sub{};
  engine::Engine::EventProbe probe;
  uint32_t slot = 0;

  explicit Rig(const Opts& o) {
    IdSource& ids = store.ids();
    track = ids.track();
    a = store.current()->tunings->front()->id;
    b = ids.tuning();
    Batch bt{"arp retune", {}};
    bt.commands.push_back(SetTuningDef{a, mos(696.0)});
    TuningSlot s31{b, "31-EDO", Colour{0x22AA44}, {}};
    s31.def = edo(31);
    bt.commands.push_back(AddTuning{s31});
    bt.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Arp"), {}, kAppend});
    bt.commands.push_back(SetTrackTuning{track, TuningAssignment::mainTuning(a)});
    std::vector<Note> chord;
    for (const int32_t d : {0, 2, 4}) chord.push_back(playback_fixture::note(ids, 0, o.chordLen, d));
    ClipPtr c = playback_fixture::clip(ids, 0, 16 * kQ, std::move(chord), "Chord");
    clip = c->id;
    if (o.clipOnB) {
      NoteClip nc = *c;
      nc.tuning = TuningAssignment::mainTuning(b);
      c = std::make_shared<const NoteClip>(std::move(nc));
    }
    bt.commands.push_back(AddClip{track, c});
    REQUIRE(store.dispatch(bt).ok());
    playback_fixture::wireSeries(store);
    (void)test_blocks::addProbeInstrument(store, track);
    arp = edit(ops::addDevice(doc(), ids, track, noteDevicePatch("arpeggiator"), DeviceKind::AudioEffect)).devices.at(0);
    param("rateDen", 8.0);
    param("gate", 1.0);
    if (o.hold) param("hold", 1.0);
    for (const auto& [name, v] : o.params) param(name, v);
    link = makeLink(store.current());
    sub = store.subscribe([this](const StoreEvent& e) { link->onDocumentChange(e); });
    const std::optional<uint32_t> s = link->eventSlotOf(track);
    REQUIRE(s.has_value());
    slot = *s;
    probe.entries.reserve(1 << 16);
    link->engine().setEventProbe(slot, &probe);
    REQUIRE(link->engine().post(engine::TransportCommand{engine::TransportCommandKind::Play, false, 0, 0, 0}));
  }
  ~Rig() {
    if (link) link->engine().setEventProbe(slot, nullptr);
  }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
  void param(const char* name, double v) { edit(ops::setDeviceParam(doc(), arp, name, position(name, v))); }
  // Plays `samples` (64-frame blocks); `at(sample)` runs before the block starting there.
  void play(std::vector<float>& out, size_t samples, const std::function<void(size_t)>& at) {
    engine::Engine& e = link->engine();
    float l[64], rr[64];
    float* outs[2] = {l, rr};
    for (size_t s = out.size(), end = out.size() + samples; s < end; s += 64) {
      at(s);
      e.process(engine::AudioBlock{nullptr, 0, outs, 2, 64, 0, 0, 0});
      out.insert(out.end(), l, l + 64);
      if (((s / 64) & 63) == 0) link->collect();  // what the UI's playhead polling does (collectIdle)
    }
  }
  // The generated note-ons the slot handed its voices, by song sample.
  std::vector<SampleTime> onsets() const {
    std::vector<SampleTime> v;
    for (const auto& en : probe.entries)
      if (en.e.kind == engine::NoteEventKind::NoteOn && engine::isStageNoteId(en.e.noteId)) v.push_back(en.song);
    return v;
  }
  // Generated notes ended at a song sample that is not a multiple of `grid` (a cut mid-step: a restart / locate).
  size_t offGrid(SampleTime grid, SampleTime from = 0) const {
    size_t n = 0;
    for (const auto& en : probe.entries)
      if (en.song >= from && engine::isStageNoteId(en.e.noteId) &&
          (en.e.kind == engine::NoteEventKind::NoteOff || en.e.kind == engine::NoteEventKind::NoteOn) &&
          en.song % grid != 0)
        ++n;
    return n;
  }
};

// Up over the chord {0, 2, 4} (by Hz), Steps `steps`, the copy j = (k / 3) mod steps shifted by `shift(j)`.
Model upModel(int32_t steps, const std::function<Part(int32_t deg, int32_t copy)>& shift) {
  return [=](size_t k) {
    const int32_t chord[3] = {0, 2, 4};
    const auto j = static_cast<int32_t>((k / 3) % static_cast<size_t>(steps));
    return std::vector<Part>{shift(chord[k % 3], j)};
  };
}

// The per-sample check: out(i) = K · Σ gain · ratio · hz(part, i) in each step's settled part, outside [w, w + 480)
// of every edit sample w. `hz(part, i)` is the Hz of the part's degree in the tuning sounding at sample i.
void checkSteps(const std::vector<float>& out, const Model& model, size_t steps,
                const std::function<double(const Part&, size_t)>& hz, const std::vector<size_t>& edits,
                double tol = 2e-3) {
  auto want = [&](size_t k, size_t i) {
    double s = 0.0;
    for (const Part& p : model(k)) s += p.gain * p.ratio * hz(p, i);
    return s;
  };
  const double K = [&] {
    double s = 0.0;
    for (size_t i = 4000; i < kStep - 200; ++i) s += out[i];
    return s / static_cast<double>(kStep - 4200) / want(0, 4000);
  }();
  REQUIRE(K > 0.0);
  for (size_t k = 0; k < steps; ++k) {
    CAPTURE(k);
    size_t bad = 0, checked = 0;
    double worst = 0.0, wGot = 0.0, wWant = 0.0;
    size_t wAt = 0;
    for (size_t i = k * kStep + 2000; i < (k + 1) * kStep - 200; ++i) {
      bool settled = true;
      for (const size_t w : edits)
        if (i >= w && i < w + 480) settled = false;
      if (!settled) continue;
      ++checked;
      const double w = K * want(k, i);
      const double err = std::fabs(out[i] - w) / w;
      if (err > worst) { worst = err; wGot = out[i] / K; wWant = w / K; wAt = i; }
      if (err > tol) ++bad;
    }
    CAPTURE(worst);
    CAPTURE(wGot);
    CAPTURE(wWant);
    CAPTURE(wAt);
    CHECK(checked > 4000);
    CHECK(bad == 0);
  }
}

// A generator drag in the middle of steps 2, 3 and 4 (four writes 640 samples apart each, at block starts), with the
// arp in `o`; every step (latched from step 2) at its pattern place, following the drag per sample.
void dragCheck(const Opts& o, const Model& model) {
  Rig r(o);
  std::vector<std::pair<size_t, double>> writes;
  for (int k = 2; k <= 4; ++k)
    for (int i = 0; i < 4; ++i)
      writes.emplace_back((static_cast<size_t>(k) * kStep + 4096 + static_cast<size_t>(i) * 640) / 64 * 64,
                          696.0 + 1.5 * (3 * (k - 2) + i + 1));
  const app::LinkStats s0 = r.link->stats();
  std::vector<float> out;
  std::vector<double> genAt;
  double gen = 696.0;
  r.play(out, 7 * kStep, [&](size_t s) {
    for (const auto& [at, g] : writes)
      if (at == s) {
        gen = g;
        REQUIRE(r.store.dispatch(SetTuningParam{r.a, ParamKind::MosGenerator, gen}).ok());
        r.link->flush();
      }
    genAt.resize(s + 64, gen);
  });
  CHECK(r.link->stats().noteStagePublishes == s0.noteStagePublishes);
  CHECK(r.link->stats().compiles == s0.compiles);
  std::map<std::pair<int32_t, double>, double> cache;
  const TuningDef base = findTuning(r.doc(), r.a)->def;
  auto hz = [&](const Part& p, size_t i) {
    const auto key = std::make_pair(p.deg, genAt[i]);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    TuningDef d = base;
    std::get<tuning::MosParams>(d.system).generator = genAt[i];
    return cache[key] = hzIn(d, r.a, p.deg);
  };
  std::vector<size_t> edits;
  for (const auto& w : writes) edits.push_back(w.first);
  checkSteps(out, model, 7, hz, edits);
  // The steps' note-ons on the grid (one per step: no restart, no extra step) and nothing cut mid-step.
  const std::vector<SampleTime> on = r.onsets();
  size_t perStep = model(0).size();
  CHECK(on.size() == 7 * perStep + perStep);  // steps 0 … 6, and step 7 at the end of the last block
  CHECK(r.offGrid(static_cast<SampleTime>(kStep)) == 0);
}

}  // namespace

TEST_CASE("arp retune (AP.8a H1): every unit's steps follow a generator drag on a Hold-latched chord, no restart") {
  SUBCASE("Degrees, Steps 2, Distance 2") {
    dragCheck({{{"steps", 2.0}, {"distance", 2.0}}}, upModel(2, [](int32_t d, int32_t j) { return Part{d + 2 * j}; }));
  }
  SUBCASE("Ratio 3/2, Steps 2") {
    dragCheck({{{"unit", 1.0}, {"steps", 2.0}, {"ratioP", 3.0}, {"ratioQ", 2.0}}},
              upModel(2, [](int32_t d, int32_t j) { return Part{d, std::pow(1.5, j)}; }));
  }
  SUBCASE("Harmonics, Steps 3 (Base 8: 8, 9, 10 over the note)") {
    dragCheck({{{"unit", 2.0}, {"steps", 3.0}}},
              upModel(3, [](int32_t d, int32_t j) { return Part{d, (8.0 + j) / 8.0}; }));
  }
  SUBCASE("Layers 1 (x 2/1 at 0.7) on Degrees, Steps 2") {
    Opts o{{{"steps", 2.0}, {"layers", 1.0}}};
    const Model m = [](size_t k) {
      const int32_t chord[3] = {0, 2, 4};
      const int32_t d = chord[k % 3] + static_cast<int32_t>((k / 3) % 2);
      return std::vector<Part>{Part{d, 1.0, 1.0}, Part{d, 2.0, 0.7}};
    };
    dragCheck(o, m);
  }
  SUBCASE("Harmonic Walk, Range 2 (Up over the merged walks)") {
    // The walk's elements: each held note's harmonics 8, 9, 10 over it (Base 8), sorted by their static Hz.
    const TuningDef def = mos(696.0);
    std::vector<std::pair<double, Part>> el;
    for (const int32_t d : {0, 2, 4})
      for (int m = 0; m <= 2; ++m) {
        const Part p{d, (8.0 + m) / 8.0};
        el.emplace_back(hzIn(def, TuningId{1}, d) * p.ratio, p);
      }
    std::sort(el.begin(), el.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    const Model m = [el](size_t k) { return std::vector<Part>{el[k % el.size()].second}; };
    dragCheck({{{"style", static_cast<double>(engine::ArpStyle::HarmonicWalk)}, {"walkRange", 2.0}}}, m);
  }
}

namespace {

// A tuning edit at a block start inside step 3 (`change`), the Hz before it from `before`, after it from `after`; the
// step it lands in is not checked (its 5 ms retune glide), every other step must play its pattern place.
void changeCheck(const Opts& o, const Model& model, const std::function<void(Rig&)>& change,
                 const std::function<double(Rig&, const Part&)>& before,
                 const std::function<double(Rig&, const Part&)>& after) {
  Rig r(o);
  std::map<std::pair<int32_t, bool>, double> hb;
  for (size_t k = 0; k < 7; ++k)
    for (const Part& p : model(k)) hb[{p.deg, p.onB}] = before(r, p);
  const size_t at = (3 * kStep + 4096) / 64 * 64;
  const app::LinkStats s0 = r.link->stats();
  std::vector<float> out;
  r.play(out, 7 * kStep, [&](size_t s) {
    if (s != at) return;
    change(r);
    r.link->flush();
  });
  CHECK(r.link->stats().noteStagePublishes == s0.noteStagePublishes);  // a tuning edit never republishes the chain
  std::map<std::pair<int32_t, bool>, double> ha;
  for (size_t k = 0; k < 7; ++k)
    for (const Part& p : model(k)) ha[{p.deg, p.onB}] = after(r, p);
  auto hz = [&](const Part& p, size_t i) { return i < at ? hb.at({p.deg, p.onB}) : ha.at({p.deg, p.onB}); };
  // the step of the change: only before it and from 10 ms after it
  checkSteps(out, model, 7, hz, {at});
  CHECK(r.offGrid(static_cast<SampleTime>(kStep)) == 0);
}

double hzA(Rig& r, const Part& p) { return hzIn(findTuning(r.doc(), r.a)->def, r.a, p.deg); }
double hzB(Rig& r, const Part& p) { return hzIn(findTuning(r.doc(), r.b)->def, r.b, p.deg); }

}  // namespace

TEST_CASE("arp retune (AP.8a H2): a new definition (MOS 7 -> 5) re-resolves a Hold-latched set, every unit") {
  auto change = [](Rig& r) { REQUIRE(r.store.dispatch(SetTuningDef{r.a, mos(720.0, 5)}).ok()); };
  const TuningDef before = mos(696.0);
  auto hzBefore = [before](Rig& r, const Part& p) { return hzIn(before, r.a, p.deg); };
  SUBCASE("Degrees, Steps 2") {
    changeCheck({{{"steps", 2.0}}}, upModel(2, [](int32_t d, int32_t j) { return Part{d + j}; }), change, hzBefore, hzA);
  }
  SUBCASE("Ratio 3/2, Steps 2") {
    changeCheck({{{"unit", 1.0}, {"steps", 2.0}, {"ratioP", 3.0}, {"ratioQ", 2.0}}},
                upModel(2, [](int32_t d, int32_t j) { return Part{d, std::pow(1.5, j)}; }), change, hzBefore, hzA);
  }
  SUBCASE("Harmonics, Steps 2") {
    changeCheck({{{"unit", 2.0}, {"steps", 2.0}}},
                upModel(2, [](int32_t d, int32_t j) { return Part{d, (8.0 + j) / 8.0}; }), change, hzBefore, hzA);
  }
}

TEST_CASE("arp retune (AP.8a H4): a clip with its own tuning steps in ITS degrees (31-EDO clip on a 5L2s track)") {
  // Distance 1 degree: copy 1 is the chord one 31-EDO step up (not a 5L2s step), and still a Degree note of the clip.
  Opts o{{{"steps", 2.0}}};
  o.clipOnB = true;
  o.hold = false;
  o.chordLen = 16 * kQ;
  const Model m = upModel(2, [](int32_t d, int32_t j) { return Part{d + j, 1.0, 1.0, true}; });
  // and an edit of the clip's own tuning (31 -> 22-EDO) re-resolves the steps in it, at their places
  changeCheck(o, m, [](Rig& r) { REQUIRE(r.store.dispatch(SetTuningDef{r.b, edo(22)}).ok()); },
              [](Rig& r, const Part& p) { return hzIn(edo(31), r.b, p.deg); }, hzB);
}

TEST_CASE("arp retune (AP.8a): a clip / track tuning switch while the arp plays keeps the pattern and follows it") {
  const Model m = upModel(2, [](int32_t d, int32_t j) { return Part{d, std::pow(1.5, j)}; });
  Opts o{{{"unit", 1.0}, {"steps", 2.0}, {"ratioP", 3.0}, {"ratioQ", 2.0}}};
  o.hold = false;
  o.chordLen = 16 * kQ;  // held: the timeline re-resolves the chord (kNoteEventRetune), the arp keeps its place
  SUBCASE("the clip switches to 31-EDO") {
    changeCheck(o, m,
                [](Rig& r) {
                  REQUIRE(r.store.dispatch(SetClipTuning{{r.track, r.clip}, TuningAssignment::mainTuning(r.b)}).ok());
                },
                hzA, hzB);
  }
  SUBCASE("the track switches to 31-EDO") {
    changeCheck(o, m,
                [](Rig& r) { REQUIRE(r.store.dispatch(SetTrackTuning{r.track, TuningAssignment::mainTuning(r.b)}).ok()); },
                hzA, hzB);
  }
  SUBCASE("Hold-latched: the track switches to 31-EDO (the latched set follows the track's tuning)") {
    Opts h = o;
    h.hold = true;
    h.chordLen = kQ;
    changeCheck(h, m,
                [](Rig& r) { REQUIRE(r.store.dispatch(SetTrackTuning{r.track, TuningAssignment::mainTuning(r.b)}).ok()); },
                hzA, hzB);
  }
}

TEST_CASE("arp retune (AP.8a M2): a note device inserted before / removed before a running arp keeps its pattern") {
  const Model m = upModel(2, [](int32_t d, int32_t j) { return Part{d + j}; });
  Opts o{{{"steps", 2.0}}};
  o.hold = false;
  o.chordLen = 16 * kQ;
  Rig r(o);
  const size_t ins = (2 * kStep + 4096) / 64 * 64, del = (4 * kStep + 4096) / 64 * 64;
  DeviceId first{};
  const app::LinkStats s0 = r.link->stats();
  std::vector<float> out;
  r.play(out, 7 * kStep, [&](size_t s) {
    if (s == ins) {  // a second arpeggiator at the head of the note run (slot 0 of the main path: before this one)
      first = r.edit(ops::addDevice(r.doc(), r.store.ids(), r.track, noteDevicePatch("arpeggiator"), DeviceKind::AudioEffect, 0))
                  .devices.at(0);
      const MainPath path = mainPath(r.doc(), *findTrackPtr(r.doc(), r.track));
      REQUIRE(path.blocks.size() == 3);
      CHECK(path.blocks[0]->id == first);
      CHECK(path.blocks[1]->id == r.arp);
      r.link->flush();
    } else if (s == del) {
      const DeviceId one[] = {first};  // the Device view's Delete: the path through it stays connected
      r.edit(ops::dissolveDevices(r.doc(), r.store.ids(), &playback_fixture::unitsLibrary(), one));
      r.link->flush();
    }
  });
  CHECK(r.link->stats().noteStagePublishes == s0.noteStagePublishes + 2);
  auto hz = [&](const Part& p, size_t) { return hzA(r, p); };
  checkSteps(out, m, 7, hz, {ins, del});
  CHECK(r.offGrid(static_cast<SampleTime>(kStep)) == 0);
}

TEST_CASE("arp retune (AP.8a M3): a tempo edit while playing is not a locate (no cut, the steps continue on the grid)") {
  Opts o{{{"steps", 2.0}}};
  o.hold = true;
  Rig r(o);
  const size_t at = (3 * kStep + 4096) / 64 * 64;
  std::vector<float> out;
  r.play(out, 8 * kStep, [&](size_t s) {
    if (s != at) return;
    REQUIRE(r.store.dispatch(SetTempoMap{{{0, 100.0, TempoCurve::Step}}}).ok());
    r.link->flush();
  });
  // After the edit the song sample of tick t is t · (60 / 100) / kPpq · sr: an eighth is 14400 samples.
  const SampleTime eighth100 = 14400;
  const auto& en = r.probe.entries;
  size_t cut = 0, onsAfter = 0, onsBefore = 0;
  int64_t swapTime = static_cast<int64_t>(at);
  for (const auto& x : en) {
    if (!engine::isStageNoteId(x.e.noteId)) continue;
    const bool after = x.time >= swapTime;
    const SampleTime grid = after ? eighth100 : static_cast<SampleTime>(kStep);
    const SampleTime miss = std::abs(x.song - (x.song + grid / 2) / grid * grid);
    // note-ons exactly on the grid; a sounding note's end re-timed by the tempo ratio (the swap keeps the TICK, so
    // the playhead moves by under a tick): within a few samples of it — a cut (a locate) is thousands off
    if (x.e.kind == engine::NoteEventKind::NoteOn && miss != 0) ++cut;
    if (x.e.kind == engine::NoteEventKind::NoteOff && miss > 16) ++cut;
    if (x.e.kind == engine::NoteEventKind::NoteOn) (after ? onsAfter : onsBefore)++;
  }
  CHECK(cut == 0);
  CHECK(onsBefore == 4);  // steps 0 … 3
  // the playhead was at tick 3.34 eighths; at 100 BPM the rest of the 8 · 12000 samples reach tick ≈ 7.0 eighths
  CHECK(onsAfter >= 3);
  // the pattern place continues: step 4 (copy 1, degree 0 + 1) is the first after the edit
  std::vector<float> hzOn;
  for (const auto& x : en)
    if (engine::isStageNoteId(x.e.noteId) && x.e.kind == engine::NoteEventKind::NoteOn) hzOn.push_back(x.e.hz);
  REQUIRE(hzOn.size() >= 6);
  const int32_t order[6] = {0, 2, 4, 1, 3, 5};
  for (size_t k = 0; k < 6; ++k) {
    CAPTURE(k);
    CHECK(hzOn[k] == doctest::Approx(hzIn(mos(696.0), r.a, order[k])).epsilon(1e-5));
  }
}

TEST_CASE("arp retune (AP.8a M9): a project reset never adopts the previous project's arp (its latched set ends)") {
  Opts o{{{"steps", 2.0}}};
  Rig r(o);
  const size_t at = (3 * kStep + 4096) / 64 * 64;
  std::vector<float> out;
  r.play(out, 6 * kStep, [&](size_t s) {
    if (s != at) return;
    // The same project again (same ids, same chain structure): a New / Open of an equal project.
    Snapshot again = std::make_shared<const Project>(*r.store.current());
    const uint64_t pub0 = r.link->stats().noteStagePublishes;
    r.store.reset(again);
    r.link->flush();
    CHECK(r.link->stats().noteStagePublishes == pub0 + 1);  // a fresh chain, never the old one kept
  });
  // Before the reset the latched chord plays; after it the new project's arp holds nothing (its clip's chord started
  // before the playhead: not chased by a reset): silence from 10 ms after the reset.
  double before = 0.0, after = 0.0;
  for (size_t i = 2 * kStep + 2000; i < 3 * kStep; ++i) before = std::max(before, std::fabs(static_cast<double>(out[i])));
  for (size_t i = at + 2 * kStep; i < out.size(); ++i) after = std::max(after, std::fabs(static_cast<double>(out[i])));
  CHECK(before > 0.0);
  CHECK(after == 0.0);
}

TEST_CASE("arp retune (AP.8a L8): a deleted wire frees its bus value index (1024 cables over a session, not in all)") {
  Opts o{{}};
  Rig r(o);
  // 1024 wires (ids not in the project: cables since deleted) take every index
  for (uint32_t i = 0; i < engine::kMaxBusValues; ++i) REQUIRE(r.link->busValueIndex(WireId{100000 + i}) != engine::kNoBusValue);
  CHECK(r.link->busValueIndex(WireId{300000}) == engine::kNoBusValue);  // full
  // any structural edit publishes the note chains, which frees the indices of wires that are gone
  r.edit(ops::setDeviceSetting(r.doc(), r.arp, "seed", 5.0));
  r.link->flush();
  const uint32_t again = r.link->busValueIndex(WireId{300000});
  CHECK(again != engine::kNoBusValue);
  CHECK(again < engine::kMaxBusValues);
  CHECK(r.link->busValueIndex(WireId{300000}) == again);  // stable while it lives
}
