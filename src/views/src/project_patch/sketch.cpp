// Projects for the project patch view's scenes, tests and benchmarks (T4.13): the user's sketch (a group of a synth
// track and a pad track with the group's own processing, a return fed by sends, a mastering chain, modulation across
// tracks) and a wide project of many tracks. Built through the edit operations, like a user would.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "pq/document/edit_ops.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/wavetable.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

namespace {

struct Runner {
  doc::DocumentStore& store;
  Status status = Ok{};
  std::optional<doc::ops::Edit> run(const doc::ops::EditResult& e) {
    if (!status) return std::nullopt;
    if (!e) {
      status = e.error();
      return std::nullopt;
    }
    if (Status s = store.dispatch(e->command); !s) {
      status = std::move(s);
      return std::nullopt;
    }
    return *e;
  }
  doc::TrackId track(doc::TrackKind kind, doc::TrackId after, const char* name, uint32_t colour) {
    auto e = run(doc::ops::createTrack(*store.current(), store.ids(), kind, after, name, doc::Colour{colour}));
    return e && !e->tracks.empty() ? e->tracks[0] : doc::TrackId{};
  }
  // GU.1: a device param set by its value in units (the knob position that gives it, through the store's library).
  void param(doc::DeviceId d, std::string name, double value) {
    if (!status) return;
    const doc::DeviceLibrary* lib = store.deviceLibrary();
    if (lib == nullptr) {
      status = Error{ErrorCode::InvalidArgument, "project builder: the store has no device library (DocumentStore::setDeviceLibrary)", {}};
      return;
    }
    (void)run(doc::ops::setDeviceParamValue(*store.current(), *lib, d, std::move(name), value));
  }
  // IG.1-2 (makeLoneInstrument): the track's instrument out of its instrument group.
  void lone(doc::TrackId t) {
    if (status) status = makeLoneInstrument(store, t);
  }
  // `index`: a main-path slot (doc::ops::kOffPath: a block placed unwired).
  doc::DeviceId device(doc::TrackId t, const char* patch, doc::DeviceKind kind, uint32_t index = doc::kAppend) {
    auto e = run(doc::ops::addDevice(*store.current(), store.ids(), t, patch, kind, index));
    return e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  }
  void clip(doc::TrackId t, Ticks start, Ticks end, const char* name, std::vector<int> degrees) {
    auto c = run(doc::ops::insertEmptyClip(*store.current(), store.ids(), t, start, end, name));
    if (!c || c->clips.empty()) return;
    std::vector<doc::Note> notes;
    Ticks at = 0;
    for (const int d : degrees) {
      doc::Note n;
      n.start = at;
      n.length = kPpq;
      n.pitch = tuning::PitchId::fromDegree(d, 0);
      notes.push_back(n);
      at += kPpq;
    }
    (void)run(doc::ops::addNotes(*store.current(), store.ids(), c->clips[0], notes));
  }
};

}  // namespace

Status makeLoneInstrument(doc::DocumentStore& store, doc::TrackId track) {
  const doc::Track* tr = doc::findTrackPtr(*store.current(), track);
  if (tr == nullptr) return Error{ErrorCode::NotFound, "makeLoneInstrument: no such track", {}};
  doc::DeviceId g, inst;
  for (const doc::DevicePtr& d : tr->devices)
    if (doc::isGroupDevice(*d)) g = d->id;
  for (const doc::DevicePtr& d : tr->devices)
    if (g.valid() && d->group == g) inst = d->id;
  if (!g.valid() || !inst.valid()) return Ok{};
  doc::Batch b{"Lone Instrument", {}};
  b.commands.push_back(doc::SetDeviceGroup{inst, {}});
  b.commands.push_back(doc::RemoveDevice{g});  // (its wires with it)
  doc::Wire in;
  in.id = store.ids().wire();
  in.noteSource = track;
  in.to = inst;
  b.commands.push_back(doc::AddWire{in});
  doc::Wire out;
  out.id = store.ids().wire();
  out.from = inst;
  out.mixer = track;
  b.commands.push_back(doc::AddWire{out});
  return store.dispatch(std::move(b));
}

Status buildSketchProject(doc::DocumentStore& store) {
  Runner r{store};
  using K = doc::DeviceKind;
  const doc::TrackId keys = r.track(doc::TrackKind::Midi, {}, "Keys", 0xF6A23B);
  const doc::TrackId pad = r.track(doc::TrackKind::Midi, keys, "Pad", 0x8ED36C);
  const doc::TrackId mod = r.track(doc::TrackKind::Midi, pad, "Mod", 0xB38BEA);
  for (const doc::TrackId t : {keys, pad}) r.lone(t);  // (IG.1-2: the sketch's instruments head their tracks)
  if (!r.status) return r.status;
  // The keys play Operator through a Saturator; the pad the default synth through an EQ Eight.
  (void)r.device(keys, "operator", K::Instrument);
  const doc::DeviceId sat = r.device(keys, "saturator", K::AudioEffect);
  (void)r.device(pad, "eq_eight", K::AudioEffect);
  // The modulation track holds only an LFO (no instrument: an audio track that makes no sound).
  if (const doc::Track* m = doc::findTrackPtr(*store.current(), mod); m != nullptr && !m->devices.empty()) {
    std::vector<doc::DeviceId> inst;  // (IG.1-2: its instrument group with it)
    for (const doc::DevicePtr& d : m->devices) inst.push_back(d->id);
    (void)r.run(doc::ops::deleteDevices(*store.current(), inst));
  }
  const doc::DeviceId lfo = r.device(mod, "lfo_device", K::AudioEffect);
  // Keys and Pad in a group with its own processing.
  const doc::TrackId two[2] = {keys, pad};
  auto g = r.run(doc::ops::groupTracks(*store.current(), store.ids(), two, "Group"));
  if (!g || g->tracks.empty()) {
    if (r.status) return Error{ErrorCode::Unknown, "sketch: grouping failed", {}};
    return r.status;
  }
  const doc::TrackId grp = g->tracks[0];
  (void)r.run(doc::ops::setTracksColour(*store.current(), std::vector<doc::TrackId>{grp}, doc::Colour{0x3FA9F5}));
  (void)r.device(grp, "eq_eight", K::AudioEffect);
  const doc::DeviceId gutil = r.device(grp, "utility", K::AudioEffect);
  // A return fed by sends, and the mastering chain.
  const doc::TrackId ret = r.track(doc::TrackKind::Return, {}, "Space", 0x6FC7C7);
  (void)r.device(ret, "eq_eight", K::AudioEffect);
  const doc::Snapshot s = store.current();
  if (s && s->master) (void)r.device(s->master->id, "saturator", K::AudioEffect);
  if (!r.status) return r.status;
  if (Status st = store.dispatch(doc::SetSendLevel{keys, ret, -6.0f}); !st) return st;
  if (Status st = store.dispatch(doc::SetSendLevel{pad, ret, -12.0f}); !st) return st;
  if (Status st = store.dispatch(doc::SetTrackVolume{pad, -4.5f}); !st) return st;
  if (Status st = store.dispatch(doc::SetTrackPan{keys, -0.35f}); !st) return st;
  // Modulation anywhere: the LFO into the group's width and into the keys' saturator drive.
  (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), lfo, "lfo", gutil, "width"));
  (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), lfo, "lfo", sat, "drive"));
  // Clips the source nodes show.
  r.clip(keys, 0, 4 * kTicksPerWhole, "Riff", {0, 4, 7, 11});
  r.clip(keys, 6 * kTicksPerWhole, 10 * kTicksPerWhole, "Riff 2", {2, 5, 9});
  r.clip(pad, 2 * kTicksPerWhole, 12 * kTicksPerWhole, "Pad", {0, 7});
  return r.status;
}

Status buildWideProject(doc::DocumentStore& store, uint32_t tracks) {
  Runner r{store};
  using K = doc::DeviceKind;
  doc::TrackId after;
  std::vector<doc::TrackId> made;
  for (uint32_t i = 0; i < tracks; ++i) {
    const std::string name = "Track " + std::to_string(i + 1);
    after = r.track(doc::TrackKind::Midi, after, name.c_str(), 0x3FA9F5 + 0x101010u * (i % 8));
    if (!r.status) return r.status;
    made.push_back(after);
    (void)r.device(after, i % 2 == 0 ? "saturator" : "eq_eight", K::AudioEffect);
    if (i % 3 == 0) (void)r.device(after, "utility", K::AudioEffect);
    r.clip(after, static_cast<Ticks>(i % 4) * kTicksPerWhole, static_cast<Ticks>(i % 4 + 4) * kTicksPerWhole, "Clip", {0, 3, 7});
  }
  // Groups of eight.
  for (uint32_t g = 0; g + 8 <= made.size(); g += 8) {
    const std::vector<doc::TrackId> members(made.begin() + g, made.begin() + g + 8);
    auto e = r.run(doc::ops::groupTracks(*store.current(), store.ids(), members, "Group " + std::to_string(g / 8 + 1)));
    if (e && !e->tracks.empty()) (void)r.device(e->tracks[0], "utility", K::AudioEffect);
  }
  const doc::TrackId a = r.track(doc::TrackKind::Return, {}, "A", 0x6FC7C7);
  const doc::TrackId b = r.track(doc::TrackKind::Return, a, "B", 0xC76F9F);
  if (!r.status) return r.status;
  for (size_t i = 0; i < made.size(); i += 2) (void)store.dispatch(doc::SetSendLevel{made[i], i % 4 == 0 ? a : b, -10.0f});
  return r.status;
}

namespace {

// The instrument of MIDI track `t` replaced by a chain Phase accumulator (TMM.4), with param values.
doc::DeviceId phaseAccOn(Runner& r, doc::TrackId t, double freq, double phase, double amp) {
  auto e = r.run(doc::ops::addMetaDevice(*r.store.current(), r.store.ids(), t, "phase_acc", 0));
  if (!e || e->devices.empty()) return {};
  const doc::DeviceId d = e->devices[0];
  if (freq != 0.0) (void)r.run(doc::ops::setMetaParamValue(*r.store.current(), d, "", "phase_acc", "freq", freq));
  if (phase != 0.0) (void)r.run(doc::ops::setMetaParamValue(*r.store.current(), d, "", "phase_acc", "phase", phase));
  if (amp != 1.0) (void)r.run(doc::ops::setMetaParamValue(*r.store.current(), d, "", "phase_acc", "amp", amp));
  return d;
}

}  // namespace

Status buildMetaFacesProject(doc::DocumentStore& store) {
  Runner r{store};
  using K = doc::DeviceKind;
  const doc::TrackId lead = r.track(doc::TrackKind::Midi, {}, "Lead", 0xF6A23B);
  const doc::TrackId bell = r.track(doc::TrackKind::Midi, lead, "Bell", 0x3FA9F5);
  const doc::TrackId sub = r.track(doc::TrackKind::Midi, bell, "Sub", 0x8ED36C);
  const doc::TrackId keys = r.track(doc::TrackKind::Midi, sub, "Keys", 0xB38BEA);
  for (const doc::TrackId t : {lead, bell, sub, keys}) r.lone(t);
  if (!r.status) return r.status;
  (void)phaseAccOn(r, lead, 0.0, 0.0, 1.0);
  (void)r.device(lead, "saturator", K::AudioEffect);
  (void)phaseAccOn(r, bell, 220.0, 0.25, 2.0);
  (void)r.device(bell, "auto_filter", K::AudioEffect);
  (void)phaseAccOn(r, sub, 4.5, 0.5, 0.5);
  (void)r.device(sub, "eq_eight", K::AudioEffect);
  (void)r.device(keys, "operator", K::Instrument);
  (void)r.device(keys, "saturator", K::AudioEffect);
  r.clip(lead, 0, 4 * kTicksPerWhole, "Line", {0, 4, 7, 11});
  r.clip(bell, 2 * kTicksPerWhole, 6 * kTicksPerWhole, "Chime", {0, 7});
  r.clip(keys, 0, 8 * kTicksPerWhole, "Chords", {0, 3, 7});
  return r.status;
}

Status addPhaseAccFaces(doc::DocumentStore& store, uint32_t count) {
  Runner r{store};
  uint32_t made = 0;
  std::vector<doc::TrackId> midi;
  doc::forEachTrack(*store.current(), [&](const doc::Track& t, const doc::Track*) {
    if (t.kind == doc::TrackKind::Midi) midi.push_back(t.id);
  });
  for (const doc::TrackId t : midi) {
    if (made >= count || !r.status) break;
    if (phaseAccOn(r, t, 25.0 * made, 0.125 * (made % 8), 1.0 + (made % 4)).valid()) ++made;
  }
  if (r.status && made < count) return Error{ErrorCode::NotFound, "addPhaseAccFaces: fewer MIDI tracks than faces", {}};
  return r.status;
}

namespace {

doc::DeviceId wavetableOn(Runner& r, doc::TrackId t, const meta::DataPtr& data) {
  auto e = r.run(doc::ops::addMetaDevice(*r.store.current(), r.store.ids(), t, "wavetable", 0));
  if (!e || e->devices.empty()) return {};
  const doc::DeviceId d = e->devices[0];
  if (data) (void)r.run(doc::ops::setMetaData(*r.store.current(), d, "", "wavetable", data));
  return d;
}

// The Pair instrument: an envelope around a Phase accumulator wired into a Wavetable.
constexpr const char* kWaveSynth =
    "patch BasicSynth {\n"  // a fork keeps its root patch's name
    "  param attack  = 5ms   [0.5ms..5s]\n"
    "  param release = 250ms [1ms..10s]\n"
    "  param level   = 0.2   [0..1]\n"
    "  out   out  : audio voice mono\n"
    "  out   done : bool voice mono\n"
    "\n"
    "  gate = voice.gate\n"
    "  coef = 1 - exp(-5 / max((gate ? attack : release) * sr, 1))\n"
    "  env  = z1(env + ((gate ? 1.0 : 0.0) - env) * coef)\n"
    "  acc1 = meta_phase_acc(voice.hz)\n"  // TMM.10-fix1: the note plays because its frequency is wired in
    "  wt1  = meta_wavetable(acc1)\n"
    "  out  = wt1 * env * (0.25 + 0.75 * voice.velocity) * level\n"
    "  done = !gate && env < 0.00003\n"
    "}\n";

}  // namespace

meta::DataPtr demoWavetableData() {
  static const meta::DataPtr data = [] {
    auto cycle = [](dsp::BasicCycle c) {
      const AlignedBuffer<float> b = dsp::basicCycle(c);
      return std::vector<float>(b.span().begin(), b.span().end());
    };
    // A bright cycle: odd partials under two formant bumps, phases spread (a drawn-looking, non-basic shape).
    std::vector<float> vocal(dsp::kKeyframeCycle);
    for (uint32_t i = 0; i < dsp::kKeyframeCycle; ++i) {
      const double ph = static_cast<double>(i) / dsp::kKeyframeCycle;
      double v = 0.0;
      for (int h = 1; h <= 24; ++h) {
        const double f1 = std::exp(-0.5 * std::pow((h - 3.0) / 1.2, 2.0));
        const double f2 = 0.6 * std::exp(-0.5 * std::pow((h - 9.0) / 2.0, 2.0));
        const double a = (h % 2 == 1 ? 1.0 : 0.35) * (f1 + f2 + 0.08 / h);
        v += a * std::sin(2.0 * std::numbers::pi * h * ph + 0.7 * h);
      }
      vocal[i] = static_cast<float>(v);
    }
    float peak = 0.0f;
    for (const float v : vocal) peak = std::max(peak, std::fabs(v));
    for (float& v : vocal) v /= peak;
    meta::WavetableData d;
    d.keyframes.push_back({0.0f, cycle(dsp::BasicCycle::Saw)});
    d.keyframes.push_back({0.5f, cycle(dsp::BasicCycle::Square)});
    d.keyframes.push_back({1.0f, std::move(vocal)});
    d.gaps = {dsp::GapMode::Spectral, dsp::GapMode::Crossfade};
    return meta::wavetableData(d);
  }();
  return data;
}

Status buildMetaWavetableProject(doc::DocumentStore& store, doc::DeviceId* pair) {
  Runner r{store};
  using K = doc::DeviceKind;
  const doc::TrackId wave = r.track(doc::TrackKind::Midi, {}, "Wave", 0x3FA9F5);
  const doc::TrackId two = r.track(doc::TrackKind::Midi, wave, "Pair", 0xF6A23B);
  for (const doc::TrackId t : {wave, two}) r.lone(t);
  if (!r.status) return r.status;
  const doc::DeviceId wt = wavetableOn(r, wave, demoWavetableData());
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "warp", 0.3));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "position", 0.35));
  (void)r.device(wave, "saturator", K::AudioEffect);
  const doc::Track* t = doc::findTrackPtr(*store.current(), two);
  if (t == nullptr || t->devices.empty()) return Error{ErrorCode::NotFound, "wavetable project: Pair has no instrument", {}};
  const doc::DeviceId inst = t->devices[0]->id;
  (void)r.run(doc::ops::editDevicePatch(*store.current(), inst, kWaveSynth));
  (void)r.run(doc::ops::setMetaData(*store.current(), inst, "wt1", "wavetable", demoWavetableData()));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), inst, "wt1", "wavetable", "position", 0.7));
  r.clip(wave, 0, 4 * kTicksPerWhole, "Line", {0, 4, 7, 11});
  r.clip(two, 0, 4 * kTicksPerWhole, "Chord", {0, 3, 7});
  if (pair != nullptr) *pair = inst;
  return r.status;
}

Status buildMetaChainProject(doc::DocumentStore& store, MetaChainIds* ids) {
  Runner r{store};
  using K = doc::DeviceKind;
  const doc::TrackId lead = r.track(doc::TrackKind::Midi, {}, "Meta Lead", 0x3FA9F5);
  const doc::TrackId bass = r.track(doc::TrackKind::Midi, lead, "Meta Bass", 0x8ED36C);
  for (const doc::TrackId t : {lead, bass}) r.lone(t);
  if (!r.status) return r.status;
  // Lead: Phase accumulator → Wavetable → Auto Filter (a meta voice run, then the voices summed into the filter).
  const doc::DeviceId acc = phaseAccOn(r, lead, 0.0, 0.0, 1.0);
  auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), lead, "wavetable"));
  const doc::DeviceId wt = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  if (!wt.valid()) return Error{ErrorCode::Unknown, "meta chain project: no Wavetable", {}};
  (void)r.run(doc::ops::setMetaData(*store.current(), wt, "", "wavetable", demoWavetableData()));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "position", 0.35));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "warp", 0.3));
  (void)r.device(lead, "auto_filter", K::AudioEffect);
  // Bass: a detuned Phase accumulator (+1.5 Hz) → a saw Wavetable, nothing after it (the voices sum at the chain end).
  const doc::DeviceId bacc = phaseAccOn(r, bass, 1.5, 0.0, 1.0);
  e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), bass, "wavetable"));
  const doc::DeviceId bwt = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  if (bwt.valid()) {
    meta::WavetableData d;
    const AlignedBuffer<float> saw = dsp::basicCycle(dsp::BasicCycle::Saw);
    d.keyframes.push_back({0.0f, std::vector<float>(saw.span().begin(), saw.span().end())});
    (void)r.run(doc::ops::setMetaData(*store.current(), bwt, "", "wavetable", meta::wavetableData(d)));
  }
  r.clip(lead, 0, 4 * kTicksPerWhole, "Chord", {0, 4, 7, 11});
  r.clip(bass, 0, 4 * kTicksPerWhole, "Root", {0, 7});
  if (ids != nullptr) *ids = MetaChainIds{lead, acc, wt, bass, bacc, bwt};
  return r.status;
}

Status addWavetableFaces(doc::DocumentStore& store, uint32_t count, const meta::DataPtr& data) {
  Runner r{store};
  uint32_t made = 0;
  std::vector<doc::TrackId> midi;
  doc::forEachTrack(*store.current(), [&](const doc::Track& t, const doc::Track*) {
    if (t.kind == doc::TrackKind::Midi) midi.push_back(t.id);
  });
  for (const doc::TrackId t : midi) {
    if (made >= count || !r.status) break;
    if (wavetableOn(r, t, data).valid()) ++made;
  }
  if (r.status && made < count) return Error{ErrorCode::NotFound, "addWavetableFaces: fewer MIDI tracks than faces", {}};
  return r.status;
}

namespace {

// The Shape instrument: a Phase accumulator into a Wavetable through an Envelope used as a VCA, whose `done` ends the
// voice (TMM.11).
constexpr const char* kEnvelopeSynth =
    "patch BasicSynth {\n"  // a fork keeps its root patch's name
    "  param level = 0.2 [0..1]\n"
    "  out   out  : audio voice mono\n"
    "  out   done : bool voice mono\n"
    "\n"
    "  acc1 = meta_phase_acc(voice.hz)\n"  // TMM.10-fix1: the note wired into its frequency
    "  wt1  = meta_wavetable(acc1)\n"
    "  env1 = meta_envelope(wt1)\n"
    "  out  = env1 * (0.25 + 0.75 * voice.velocity) * level\n"
    "  done = env1.done\n"
    "}\n";

}  // namespace

Status buildMetaEnvelopeProject(doc::DocumentStore& store, MetaEnvelopeIds* ids) {
  Runner r{store};
  const doc::TrackId pluck = r.track(doc::TrackKind::Midi, {}, "Pluck", 0x3FA9F5);
  const doc::TrackId shape = r.track(doc::TrackKind::Midi, pluck, "Shape", 0xF6A23B);
  for (const doc::TrackId t : {pluck, shape}) r.lone(t);
  if (!r.status) return r.status;
  // Pluck: Phase accumulator → Wavetable → Envelope (a meta voice run whose release is the envelope's).
  const doc::DeviceId acc = phaseAccOn(r, pluck, 0.0, 0.0, 1.0);
  auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), pluck, "wavetable"));
  const doc::DeviceId wt = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), pluck, "envelope"));
  const doc::DeviceId env = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  if (!wt.valid() || !env.valid()) return Error{ErrorCode::Unknown, "envelope project: no Wavetable / Envelope", {}};
  (void)r.run(doc::ops::setMetaData(*store.current(), wt, "", "wavetable", demoWavetableData()));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "position", 0.35));
  const std::pair<const char*, double> pluckEnv[] = {{"attack", 0.012}, {"decay", 0.9}, {"sustain", 0.35}, {"release", 1.2},
                                                     {"acurve", 0.45},  {"dcurve", -0.25}, {"rcurve", 0.3}};
  for (const auto& [name, v] : pluckEnv) (void)r.run(doc::ops::setMetaParamValue(*store.current(), env, "", "envelope", name, v));
  // Shape: the default synth forked into acc1 → wt1 → env1 (nested faces), a slower envelope.
  const doc::Track* t = doc::findTrackPtr(*store.current(), shape);
  if (t == nullptr || t->devices.empty()) return Error{ErrorCode::NotFound, "envelope project: Shape has no instrument", {}};
  const doc::DeviceId inst = t->devices[0]->id;
  (void)r.run(doc::ops::editDevicePatch(*store.current(), inst, kEnvelopeSynth));
  (void)r.run(doc::ops::setMetaData(*store.current(), inst, "wt1", "wavetable", demoWavetableData()));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), inst, "wt1", "wavetable", "position", 0.7));
  const std::pair<const char*, double> shapeEnv[] = {
      {"attack", 0.4}, {"decay", 2.0}, {"sustain", 0.7}, {"release", 3.0}, {"acurve", -0.4}};
  for (const auto& [name, v] : shapeEnv) (void)r.run(doc::ops::setMetaParamValue(*store.current(), inst, "env1", "envelope", name, v));
  r.clip(pluck, 0, 4 * kTicksPerWhole, "Line", {0, 4, 7, 11});
  r.clip(shape, 0, 4 * kTicksPerWhole, "Pad", {0, 3, 7});
  if (ids != nullptr) *ids = MetaEnvelopeIds{pluck, acc, wt, env, shape, inst};
  return r.status;
}

Status buildMetaOperatorProject(doc::DocumentStore& store, MetaOperatorIds* ids) {
  Runner r{store};
  const doc::TrackId keys = r.track(doc::TrackKind::Midi, {}, "FM Keys", 0x3FA9F5);
  r.lone(keys);
  if (!r.status) return r.status;
  const doc::DeviceId op = r.device(keys, "meta_operator", doc::DeviceKind::Instrument);
  if (!r.status) return r.status;
  if (!op.valid()) return Error{ErrorCode::NotFound, "meta operator project: no Meta Operator", {}};
  // Exact ratios: A 1, B 2, C 3/2, D 7/4.
  const std::pair<const char*, double> ratios[] = {{"nB", 2}, {"nC", 3}, {"dC", 2}, {"nD", 7}, {"dD", 4}};
  for (const auto& [name, v] : ratios) r.param(op, name, v);
  // The modulators: shorter, quieter envelopes (their amount is the depth); B reads the demo table, D a saw.
  struct EnvSet {
    const char* path;
    std::vector<std::pair<const char*, double>> values;
  };
  const EnvSet envs[] = {
      {"envA", {{"attack", 0.004}, {"decay", 1.4}, {"sustain", 0.55}, {"release", 0.8}, {"dcurve", -0.2}}},
      {"envB", {{"attack", 0.002}, {"decay", 0.5}, {"sustain", 0.3}, {"release", 0.4}, {"amount", 0.35}}},
      {"envC", {{"attack", 0.03}, {"decay", 0.9}, {"sustain", 0.5}, {"release", 0.6}, {"acurve", 0.4}, {"amount", 0.25}}},
      {"envD", {{"attack", 0.001}, {"decay", 0.25}, {"sustain", 0.0}, {"release", 0.2}, {"dcurve", 0.35}, {"amount", 0.15}}},
  };
  for (const EnvSet& e : envs)
    for (const auto& [name, v] : e.values)
      (void)r.run(doc::ops::setMetaParamValue(*store.current(), op, e.path, "envelope", name, v));
  (void)r.run(doc::ops::setMetaData(*store.current(), op, "wtB", "wavetable", demoWavetableData()));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), op, "wtB", "wavetable", "position", 0.4));
  {
    meta::WavetableData d;
    const AlignedBuffer<float> saw = dsp::basicCycle(dsp::BasicCycle::Saw);
    d.keyframes.push_back({0.0f, std::vector<float>(saw.span().begin(), saw.span().end())});
    (void)r.run(doc::ops::setMetaData(*store.current(), op, "wtD", "wavetable", meta::wavetableData(d)));
  }
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), op, "accD", "phase_acc", "freq", 1.5));
  r.clip(keys, 0, 4 * kTicksPerWhole, "Chord", {0, 4, 7, 11});
  if (ids != nullptr) *ids = MetaOperatorIds{keys, op};
  return r.status;
}

namespace {

// The Trem instrument (TMM.14): an envelope around a Phase accumulator into a Wavetable, through a VCA whose value an
// LFO-like sine moves (the face shows the knob's dots), at a narrow range.
constexpr const char* kVcaSynth =
    "patch BasicSynth {\n"  // a fork keeps its root patch's name
    "  param attack  = 5ms   [0.5ms..5s]\n"
    "  param release = 250ms [1ms..10s]\n"
    "  param level   = 0.2   [0..1]\n"
    "  out   out  : audio voice mono\n"
    "  out   done : bool voice mono\n"
    "\n"
    "  gate = voice.gate\n"
    "  coef = 1 - exp(-5 / max((gate ? attack : release) * sr, 1))\n"
    "  env  = z1(env + ((gate ? 1.0 : 0.0) - env) * coef)\n"
    "  acc1 = meta_phase_acc(voice.hz)\n"
    "  wt1  = meta_wavetable(acc1)\n"
    "  vca1 = meta_vca(wt1 * env)\n"
    "  out  = vca1 * (0.25 + 0.75 * voice.velocity) * level\n"
    "  done = !gate && env < 0.00003\n"
    "}\n";

}  // namespace

Status buildMetaVcaProject(doc::DocumentStore& store, MetaVcaIds* ids) {
  Runner r{store};
  const doc::TrackId keys = r.track(doc::TrackKind::Midi, {}, "Keys", 0x3FA9F5);
  const doc::TrackId trem = r.track(doc::TrackKind::Midi, keys, "Trem", 0xF6A23B);
  for (const doc::TrackId t : {keys, trem}) r.lone(t);
  if (!r.status) return r.status;
  // Keys: the default synth, then a chain VCA at ×0.60 on the default bipolar-log curve.
  auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), keys, "vca"));
  const doc::DeviceId vca = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  if (!vca.valid()) return Error{ErrorCode::Unknown, "vca project: no VCA", {}};
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), vca, "", "vca", "value", 0.62));
  // Trem: the synth forked with vca1 on a Bend curve from 0 to 2 (bent toward exponential), at the three-quarter mark.
  const doc::Track* t = doc::findTrackPtr(*store.current(), trem);
  if (t == nullptr || t->devices.empty()) return Error{ErrorCode::NotFound, "vca project: Trem has no instrument", {}};
  const doc::DeviceId inst = t->devices[0]->id;
  (void)r.run(doc::ops::editDevicePatch(*store.current(), inst, kVcaSynth));
  (void)r.run(doc::ops::setMetaSetting(*store.current(), inst, "vca1", "vca", "curve", 1.0));
  const std::pair<const char*, double> tremVca[] = {{"min", 0.0}, {"max", 2.0}, {"bend", 0.5}, {"value", 0.75}};
  for (const auto& [name, v] : tremVca) (void)r.run(doc::ops::setMetaParamValue(*store.current(), inst, "vca1", "vca", name, v));
  r.clip(keys, 0, 4 * kTicksPerWhole, "Line", {0, 4, 7, 11});
  r.clip(trem, 0, 4 * kTicksPerWhole, "Pad", {0, 3, 7});
  if (ids != nullptr) *ids = MetaVcaIds{keys, vca, trem, inst};
  return r.status;
}

Status buildSpectrumProject(doc::DocumentStore& store, MetaSpectrumIds* ids) {
  Runner r{store};
  // An audio track (no instrument: the two displays side by side fit the Device view).
  const doc::TrackId keys = r.track(doc::TrackKind::Audio, {}, "Mix", 0x3FA9F5);
  if (!r.status) return r.status;
  // EQ Eight: a cut at 250 Hz, a boost at 1 kHz, a cut at 6 kHz (bells; the other bands at 0 dB).
  const doc::DeviceId eq = r.device(keys, "eq_eight", doc::DeviceKind::AudioEffect);
  if (!eq.valid()) return r.status ? Status{Error{ErrorCode::NotFound, "spectrum project: no EQ Eight", {}}} : r.status;
  const std::pair<const char*, double> bands[] = {{"freq3", 250.0},  {"gain3", 0.6309573}, {"q3", 1.2},      // −4 dB
                                                  {"freq4", 1000.0}, {"gain4", 1.9952623}, {"q4", 2.0},      // +6 dB
                                                  {"freq6", 6000.0}, {"gain6", 0.5623413}, {"q6", 0.7071}};  // −5 dB
  for (const auto& [name, v] : bands) r.param(eq, name, v);
  auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), keys, "spectrum"));
  const doc::DeviceId spec = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  if (!spec.valid()) return r.status ? Status{Error{ErrorCode::NotFound, "spectrum project: no Spectrum", {}}} : r.status;
  if (ids != nullptr) *ids = MetaSpectrumIds{keys, eq, spec};
  return r.status;
}

Status buildGroupProject(doc::DocumentStore& store, GroupProjectIds* ids) {
  Runner r{store};
  using K = doc::DeviceKind;
  const doc::TrackId keys = r.track(doc::TrackKind::Midi, {}, "Keys", 0xF6A23B);
  const doc::TrackId bass = r.track(doc::TrackKind::Midi, keys, "Bass", 0x8ED36C);
  for (const doc::TrackId t : {keys, bass}) r.lone(t);
  if (!r.status) return r.status;
  // Keys: the default synth → Phase accumulator (110 Hz, FM by the synth) → Wavetable → Phase accumulator (3 Hz), and a
  // free LFO (out of the series) wired onto the Wavetable's position.
  auto meta = [&](doc::TrackId t, const char* type) -> doc::DeviceId {
    auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), t, type));
    return e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  };
  const doc::DeviceId pa1 = meta(keys, "phase_acc");
  const doc::DeviceId wt = meta(keys, "wavetable");
  const doc::DeviceId pa2 = meta(keys, "phase_acc");
  if (!r.status) return r.status;
  if (!pa1.valid() || !wt.valid() || !pa2.valid()) return Error{ErrorCode::Unknown, "group project: meta devices", {}};
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), pa1, "", "phase_acc", "freq", 110.0));
  (void)r.run(doc::ops::setMetaData(*store.current(), wt, "", "wavetable", demoWavetableData()));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "position", 0.35));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), pa2, "", "phase_acc", "freq", 3.0));
  const doc::DeviceId lfo = r.device(keys, "lfo_device", K::AudioEffect, doc::ops::kOffPath);
  if (lfo.valid()) {
    r.param(lfo, "rate", 0.5);
    (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), lfo, "lfo", wt, "position"));
  }
  // Bass: the default synth → Saturator → Utility.
  const doc::DeviceId sat = r.device(bass, "saturator", K::AudioEffect);
  const doc::DeviceId util = r.device(bass, "utility", K::AudioEffect);
  r.param(sat, "drive", 4.0);
  r.param(util, "width", 1.6);
  r.clip(keys, 0, 4 * kTicksPerWhole, "Chord", {0, 4, 7});
  r.clip(bass, 0, 4 * kTicksPerWhole, "Root", {0, 7});
  if (ids != nullptr) *ids = GroupProjectIds{keys, pa1, wt, pa2, lfo, bass, sat, util};
  return r.status;
}

Status buildFreeRackProject(doc::DocumentStore& store, uint32_t lfos, FreeRackIds* ids) {
  Runner r{store};
  using K = doc::DeviceKind;
  FreeRackIds out;
  out.lead = r.track(doc::TrackKind::Midi, {}, "Lead", 0x6CB6F0);
  r.lone(out.lead);
  if (!r.status) return r.status;
  // The chain: the default synth → Saturator → Auto Filter → Utility.
  out.sat = r.device(out.lead, "saturator", K::AudioEffect);
  out.filter = r.device(out.lead, "auto_filter", K::AudioEffect);
  out.util = r.device(out.lead, "utility", K::AudioEffect);
  if (!r.status) return r.status;
  r.param(out.sat, "drive", 6.0);
  // The free devices: LFOs, each wired onto a knob of the chain (in turn through these), and a Phase accumulator whose
  // audio feeds the Auto Filter's sidechain input (a wire into an input), its frequency moved by the first LFO.
  static constexpr std::pair<int, const char*> kTargets[] = {{0, "drive"},  {1, "freq"},    {2, "width"},  {1, "res"},
                                                             {0, "dryWet"}, {2, "balance"}, {1, "drive"}, {0, "output"}};
  const doc::DeviceId chain[3] = {out.sat, out.filter, out.util};
  for (uint32_t i = 0; i < lfos; ++i) {
    const doc::DeviceId lfo = r.device(out.lead, "lfo_device", K::AudioEffect, doc::ops::kOffPath);
    if (!lfo.valid()) {
      if (r.status) return Error{ErrorCode::Unknown, "free rack project: LFO", {}};
      return r.status;
    }
    r.param(lfo, "rate", 0.25 * static_cast<double>(i + 1));
    const auto& t = kTargets[i % std::size(kTargets)];
    (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), lfo, "lfo", chain[t.first], t.second));
    out.lfos.push_back(lfo);
  }
  {
    auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), out.lead, "phase_acc", doc::ops::kOffPath));
    out.acc = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
    if (!out.acc.valid()) {
      if (r.status) return Error{ErrorCode::Unknown, "free rack project: Phase accumulator", {}};
      return r.status;
    }
    (void)r.run(doc::ops::setMetaParamValue(*store.current(), out.acc, "", "phase_acc", "freq", 55.0));
    (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), out.acc, "out", out.filter, "sc"));
    if (!out.lfos.empty()) (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), out.lfos.front(), "lfo", out.acc, "freq"));
  }
  r.clip(out.lead, 0, 4 * kTicksPerWhole, "Riff", {0, 3, 7});
  if (ids != nullptr) *ids = std::move(out);
  return r.status;
}

}  // namespace pq::views::project_patch
