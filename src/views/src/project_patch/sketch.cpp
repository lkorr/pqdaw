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
#include "pq/document/main_path.h"
#include "pq/document/modules.h"
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
  // GU.4: a built-in module's block set (local ids from 1, the group first: doc::modules.h) pasted into `t`, inserted on
  // its main path at `slot` (kOffPath: placed unwired). Returns the pasted ids, index k for local id k + 1.
  std::vector<doc::DeviceId> module(doc::TrackId t, const doc::BlockSet& set, uint32_t slot) {
    auto e = run(doc::ops::pasteBlocks(*store.current(), store.ids(), t, set));
    if (!e || e->devices.empty()) return {};
    if (slot != doc::ops::kOffPath) (void)run(doc::ops::insertOnPath(*store.current(), store.ids(), e->devices[0], slot));
    return e->devices;
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

// GU.4: a Phase Accumulator module (its group preset) with these values in units, at main-path slot `slot` of `t`
// (kOffPath: unwired). `roles` (optional) gets its blocks' ids. Returns the group.
doc::DeviceId phaseAccModule(Runner& r, doc::TrackId t, double freq, double phase, double amp, uint32_t slot,
                             doc::PhaseAccumulatorRoles* roles = nullptr) {
  doc::IdSource local;
  doc::PhaseAccumulatorOptions o;
  o.freq = dsp::curvePosition(doc::phaseAccumulatorCurve(doc::kPaFrequency), freq, 0.0);
  o.phase = dsp::curvePosition(doc::phaseAccumulatorCurve(doc::kPaPhase), phase, 0.0);
  o.amp = dsp::curvePosition(doc::phaseAccumulatorCurve(doc::kPaAmplitude), amp, 0.25);
  doc::PhaseAccumulatorRoles lr;
  const std::vector<doc::DeviceId> ids = r.module(t, doc::phaseAccumulatorBlocks(local, o, &lr), slot);
  if (ids.empty()) return {};
  if (roles != nullptr) {
    auto at = [&](doc::DeviceId l) { return l.valid() && l.value - 1 < ids.size() ? ids[l.value - 1] : doc::DeviceId{}; };
    *roles = doc::PhaseAccumulatorRoles{at(lr.group), at(lr.freq), at(lr.phase), at(lr.amp), at(lr.pitch), at(lr.ref),
                                        at(lr.sum), at(lr.ratio), at(lr.pitched), at(lr.phasor), at(lr.addPhase),
                                        at(lr.pm), at(lr.frac), at(lr.out)};
  }
  return ids[0];
}
// The head of MIDI track `t`'s path: a Phase Accumulator module (TMM.4's chain accumulator, GU.4's preset), in place of
// the track's lone stock instrument (as adding the meta module at the head replaced it).
doc::DeviceId phaseAccOn(Runner& r, doc::TrackId t, double freq, double phase, double amp) {
  if (const doc::Track* tr = doc::findTrackPtr(*r.store.current(), t)) {
    const doc::MainPath path = doc::mainPath(*r.store.current(), *tr);
    if (!path.blocks.empty() && path.blocks.front()->kind == doc::DeviceKind::Instrument && !doc::isMetaDevice(*path.blocks.front()) &&
        !doc::isGroupDevice(*path.blocks.front())) {
      const doc::DeviceId one[1] = {path.blocks.front()->id};
      (void)r.run(doc::ops::deleteDevices(*r.store.current(), one));
    }
  }
  return phaseAccModule(r, t, freq, phase, amp, 0);
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
    "  acc1 = phasor(voice.hz)\n"  // GU.4: the phase (the Phase Accumulator is a group preset, not a call)
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
  if (!r.status) return r.status;
  for (const doc::TrackId t : {lead, bass}) r.lone(t);
  // Lead: Wavetable → Auto Filter (a meta voice run, then the voices summed into the filter). GU.4: the Wavetable heads
  // the path (at the notes its phase input reads the note's phase, as the Phase accumulator before it gave it); a
  // Phase Accumulator module (a group: its panel) is free beside it, a slow ramp (0.25 Hz) into its warp.
  const doc::DeviceId lone = doc::mainPath(*store.current(), *doc::findTrackPtr(*store.current(), lead)).blocks.empty()
                                 ? doc::DeviceId{}
                                 : doc::mainPath(*store.current(), *doc::findTrackPtr(*store.current(), lead)).blocks.front()->id;
  auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), lead, "wavetable", 0));
  const doc::DeviceId wt = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  if (!wt.valid()) return Error{ErrorCode::Unknown, "meta chain project: no Wavetable", {}};
  if (lone.valid() && doc::findDevice(*store.current(), lone).device != nullptr) {  // (the lone instrument, if not replaced)
    const doc::DeviceId one[1] = {lone};
    (void)r.run(doc::ops::deleteDevices(*store.current(), one));
  }
  const doc::DeviceId acc = phaseAccModule(r, lead, 0.25, 0.0, 1.0, doc::ops::kOffPath);
  if (acc.valid()) (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), acc, "", wt, "warp"));
  (void)r.run(doc::ops::setMetaData(*store.current(), wt, "", "wavetable", demoWavetableData()));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "position", 0.35));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "warp", 0.3));
  (void)r.device(lead, "auto_filter", K::AudioEffect);
  // Bass: a saw Wavetable at the notes, nothing after it (the voices sum at the chain end). (GU.4: no accumulator.)
  const doc::DeviceId blone = doc::mainPath(*store.current(), *doc::findTrackPtr(*store.current(), bass)).blocks.empty()
                                  ? doc::DeviceId{}
                                  : doc::mainPath(*store.current(), *doc::findTrackPtr(*store.current(), bass)).blocks.front()->id;
  e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), bass, "wavetable", 0));
  const doc::DeviceId bwt = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
  if (blone.valid() && doc::findDevice(*store.current(), blone).device != nullptr) {
    const doc::DeviceId one[1] = {blone};
    (void)r.run(doc::ops::deleteDevices(*store.current(), one));
  }
  const doc::DeviceId bacc{};
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
    "  acc1 = phasor(voice.hz)\n"  // GU.4: the phase (the Phase Accumulator is a group preset, not a call)
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

namespace {

// The Trem instrument (TMM.14): an envelope around a phase into a Wavetable, its level `trem` (GU.4: the VCA is a group
// preset now, not a call: a plain multiply).
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
    "  param trem    = 0.75  [0..2]\n"
    "  acc1 = phasor(voice.hz)\n"
    "  wt1  = meta_wavetable(acc1)\n"
    "  vca1 = wt1 * env * trem\n"
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
  // Keys: the default synth, then a VCA module at ×0.60 on its law (the knob at 0.62) on the path.
  doc::IdSource local;
  doc::VcaOptions vo;
  vo.value = 0.62;
  const std::vector<doc::DeviceId> made = r.module(keys, doc::vcaBlocks(local, vo), doc::kAppend);
  const doc::DeviceId vca = made.empty() ? doc::DeviceId{} : made[0];
  if (!vca.valid()) return r.status ? Status(Error{ErrorCode::Unknown, "vca project: no VCA", {}}) : r.status;
  // Trem: the synth forked (its level a param).
  const doc::Track* t = doc::findTrackPtr(*store.current(), trem);
  if (t == nullptr || t->devices.empty()) return Error{ErrorCode::NotFound, "vca project: Trem has no instrument", {}};
  const doc::DeviceId inst = t->devices[0]->id;
  (void)r.run(doc::ops::editDevicePatch(*store.current(), inst, kVcaSynth));
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
  // (GU.4: Envelopes where the Phase accumulators were — a Phase Accumulator is a group preset now, and this project is
  // about grouping single blocks.)
  const doc::DeviceId pa1 = meta(keys, "envelope");
  const doc::DeviceId wt = meta(keys, "wavetable");
  const doc::DeviceId pa2 = meta(keys, "envelope");
  if (!r.status) return r.status;
  if (!pa1.valid() || !wt.valid() || !pa2.valid()) return Error{ErrorCode::Unknown, "group project: meta devices", {}};
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), pa1, "", "envelope", "sustain", 0.8));
  (void)r.run(doc::ops::setMetaData(*store.current(), wt, "", "wavetable", demoWavetableData()));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), wt, "", "wavetable", "position", 0.35));
  (void)r.run(doc::ops::setMetaParamValue(*store.current(), pa2, "", "envelope", "attack", 0.3));
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

Status buildNestedGroupProject(doc::DocumentStore& store, NestedProjectIds* ids) {
  Runner r{store};
  using K = doc::DeviceKind;
  NestedProjectIds o;
  o.keys = r.track(doc::TrackKind::Midi, {}, "Keys", 0xF6A23B);
  o.bass = r.track(doc::TrackKind::Midi, o.keys, "Bass", 0x8ED36C);
  if (!r.status) return r.status;
  auto headOf = [&](doc::TrackId t) {
    const doc::Track* tr = doc::findTrackPtr(*store.current(), t);
    const doc::MainPath path = doc::mainPath(*store.current(), *tr);
    return path.blocks.empty() ? doc::DeviceId{} : path.blocks.front()->id;
  };
  auto into = [&](doc::DeviceId d, doc::DeviceId g) {
    (void)r.run(doc::ops::setDeviceGroup(*store.current(), store.ids(), store.deviceLibrary(), d, g));
  };
  auto wire = [&](doc::DeviceId a, const char* port, doc::DeviceId b, const char* param) {
    (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), a, port, b, param));
  };
  auto group = [&](doc::TrackId t, const char* name, doc::DeviceId parent) {
    const doc::DeviceId g = r.device(t, "Group", K::AudioEffect, doc::ops::kOffPath);
    (void)r.run(doc::ops::renameDevice(*store.current(), g, name));
    if (parent.valid()) into(g, parent);
    return g;
  };
  auto meta = [&](doc::TrackId t, const char* type, doc::DeviceId g) -> doc::DeviceId {
    auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), t, type, doc::ops::kOffPath));
    const doc::DeviceId d = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
    if (d.valid() && g.valid()) into(d, g);
    return d;
  };
  // Keys: its instrument group without the Basic Synth.
  o.group = headOf(o.keys);
  {
    const doc::Track* tr = doc::findTrackPtr(*store.current(), o.keys);
    std::vector<doc::DeviceId> del;
    for (const doc::DevicePtr& d : tr->devices)
      if (d->group == o.group) del.push_back(d->id);
    (void)r.run(doc::ops::deleteDevices(*store.current(), del));
  }
  if (!r.status) return r.status;
  o.pa = meta(o.keys, "wavetable", o.group);  // (GU.4: a Wavetable reading the note's phase; was a Phase accumulator)
  o.osc = group(o.keys, "Osc", o.group);
  o.wt = meta(o.keys, "wavetable", o.osc);
  o.env = meta(o.keys, "envelope", o.group);
  o.level = group(o.keys, "Level", o.group);
  o.mul = r.device(o.keys, "atom:mul", K::AudioEffect, doc::ops::kOffPath);
  into(o.mul, o.level);
  o.value = r.device(o.keys, "Value", K::AudioEffect, doc::ops::kOffPath);
  into(o.value, o.level);
  if (!r.status) return r.status;
  (void)r.run(doc::ops::setMetaData(*store.current(), o.wt, "", "wavetable", demoWavetableData()));
  r.param(o.value, "value", 0.8);
  wire(o.group, "input", o.pa, "x");
  wire(o.pa, "", o.osc, "x");
  wire(o.osc, "input", o.wt, "");
  wire(o.wt, "", o.osc, "output");
  wire(o.osc, "y", o.env, "");
  wire(o.env, "", o.level, "x");
  wire(o.level, "input", o.mul, "a");
  wire(o.value, "y", o.mul, "b");
  wire(o.mul, "out", o.level, "output");
  wire(o.level, "y", o.group, "output");
  // Bass: Basic Synth (its instrument group) → FX [Saturator → Tone [Utility]] on the path.
  o.sat = r.device(o.bass, "saturator", K::AudioEffect);
  o.util = r.device(o.bass, "utility", K::AudioEffect);
  if (!r.status) return r.status;
  r.param(o.sat, "drive", 4.0);
  r.param(o.util, "width", 1.6);
  if (const doc::DeviceLibrary* lib = store.deviceLibrary()) {
    const doc::DeviceId series[2] = {o.sat, o.util};
    auto fx = r.run(doc::ops::groupInCircuit(store.current(), store.ids(), *lib, series));
    if (fx && !fx->devices.empty()) o.fx = fx->devices[0];
    (void)r.run(doc::ops::renameDevice(*store.current(), o.fx, "FX"));
    const doc::DeviceId inner[1] = {o.util};
    auto tone = r.run(doc::ops::groupInCircuit(store.current(), store.ids(), *lib, inner));
    if (tone && !tone->devices.empty()) o.tone = tone->devices[0];
    (void)r.run(doc::ops::renameDevice(*store.current(), o.tone, "Tone"));
  }
  r.clip(o.keys, 0, 4 * kTicksPerWhole, "Chord", {0, 4, 7});
  r.clip(o.bass, 0, 4 * kTicksPerWhole, "Root", {0, 7});
  if (ids != nullptr) *ids = o;
  return r.status;
}

// GU.3: a group with a panel of every generic element kind (buildPanelProject in project_patch_view.h).
Status buildPanelProject(doc::DocumentStore& store, PanelProjectIds* ids) {
  Runner r{store};
  using K = doc::DeviceKind;
  PanelProjectIds o;
  o.keys = r.track(doc::TrackKind::Midi, {}, "Keys", 0xF6A23B);
  if (!r.status) return r.status;
  {
    const doc::Track* tr = doc::findTrackPtr(*store.current(), o.keys);
    const doc::MainPath path = doc::mainPath(*store.current(), *tr);
    o.group = path.blocks.empty() ? doc::DeviceId{} : path.blocks.front()->id;
    std::vector<doc::DeviceId> del;
    for (const doc::DevicePtr& d : tr->devices)
      if (d->group == o.group) del.push_back(d->id);
    (void)r.run(doc::ops::deleteDevices(*store.current(), del));
  }
  if (!r.status || !o.group.valid()) return r.status ? Status(Error{ErrorCode::NotFound, "panel project: no instrument group", {}}) : r.status;
  (void)r.run(doc::ops::renameDevice(*store.current(), o.group, "Synth"));
  const doc::DeviceLibrary* lib = store.deviceLibrary();
  if (lib == nullptr) return Error{ErrorCode::InvalidArgument, "panel project: the store has no device library", {}};
  auto into = [&](doc::DeviceId d) { (void)r.run(doc::ops::setDeviceGroup(*store.current(), store.ids(), lib, d, o.group)); };
  auto value = [&](const char* name) {
    const doc::DeviceId v = r.device(o.keys, "Value", K::AudioEffect, doc::ops::kOffPath);
    (void)r.run(doc::ops::renameDevice(*store.current(), v, name));
    into(v);
    return v;
  };
  {  // (GU.4: a Wavetable reading the note's phase; was a Phase accumulator, a group preset now)
    auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), o.keys, "wavetable", doc::ops::kOffPath));
    o.pa = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
    into(o.pa);
  }
  o.mul = r.device(o.keys, "atom:mul", K::AudioEffect, doc::ops::kOffPath);
  into(o.mul);
  o.level = value("Level");
  o.cutoff = value("Cutoff");
  o.drive = value("Drive");
  o.mix = value("Mix");
  if (!r.status) return r.status;
  const std::string vp(doc::kValueParam);
  {
    dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Octave, 20.0, 20000.0);
    c.unit = dsp::CurveUnit::Hz;
    (void)r.run(doc::ops::setParamCurve(*store.current(), *lib, o.cutoff, vp, c));
    dsp::Curve d = dsp::curvePreset(dsp::CurvePreset::Logarithmic, 0.0, 1.0, dsp::kCurveBendK);
    (void)r.run(doc::ops::setParamCurve(*store.current(), *lib, o.drive, vp, d));
    dsp::Curve m = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, 1.0);
    m.unit = dsp::CurveUnit::Percent;
    (void)r.run(doc::ops::setParamCurve(*store.current(), *lib, o.mix, vp, m));
  }
  r.param(o.level, vp, 0.8);
  r.param(o.cutoff, vp, 1000.0);
  r.param(o.drive, vp, 0.35);
  r.param(o.mix, vp, 0.6);
  (void)r.run(doc::ops::setDeviceSetting(*store.current(), o.group, std::string(doc::kGroupLegato), 1.0));
  (void)r.run(doc::ops::setDeviceSetting(*store.current(), o.group, std::string(doc::kGroupSteal), 1.0));
  (void)r.run(doc::ops::setDeviceSetting(*store.current(), o.group, std::string(doc::kGroupVoices), 8.0));
  auto wire = [&](doc::DeviceId a, const char* port, doc::DeviceId b, const char* param) {
    (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), a, port, b, param));
  };
  wire(o.group, "input", o.pa, "x");
  wire(o.pa, "", o.mul, "a");
  wire(o.level, "y", o.mul, "b");
  wire(o.mul, "out", o.group, "output");
  if (!r.status) return r.status;
  // The panel: two sections — the oscillator's knobs, sliders, a number, a button and a mod source; a tabbed curve
  // editor / XY pad, the polyphony settings, a label and decorations.
  using doc::styleNumber;
  using doc::styleText;
  std::vector<doc::PanelElement> es;
  auto el = [&](const char* kind, float x, float y, float w, float h, std::vector<doc::Binding> b = {}) -> doc::PanelElement& {
    doc::PanelElement e;
    e.kind = kind;
    e.x = x, e.y = y, e.w = w, e.h = h;
    e.bindings = std::move(b);
    es.push_back(std::move(e));
    return es.back();
  };
  auto ctl = [&](doc::DeviceId d) { return doc::controlBinding(d, vp); };
  auto set = [&](std::string_view s) { return doc::settingBinding(o.group, std::string(s)); };
  {
    doc::PanelElement& a = el("shape", 4, 4, 232, 212);
    a.setField(styleText("stroke", "none"));
    a.setField(styleNumber("opacity", 0.35));
    a.setField(styleNumber("radius", 6));
    doc::PanelElement& b = el("shape", 240, 4, 236, 212);
    b.setField(styleText("stroke", "none"));
    b.setField(styleNumber("opacity", 0.35));
  }
  el("label", 12, 8, 60, 12).setField(styleText("text", "OSC"));
  es.back().setField(styleText("align", "left"));
  es.back().setField(styleNumber("size", 8));
  el("knob", 12, 22, 48, 60, {ctl(o.level)});
  {
    doc::PanelElement& k = el("knob", 66, 22, 48, 60, {ctl(o.cutoff)});
    k.setField(styleText("pointer", "pie"));
    k.setField(styleText("readout", "above"));
  }
  {
    doc::PanelElement& k = el("knob", 120, 22, 48, 60, {ctl(o.drive)});
    k.setField(styleText("pointer", "dot"));
    k.setField(styleNumber("track", 2));
    k.setField(styleText("colour", "signal"));
  }
  {
    doc::PanelElement& k = el("knob", 174, 30, 48, 48, {ctl(o.mix)});
    k.setField(styleText("pointer", "line"));
    k.setField(styleText("readout", "inside"));
    k.setField(styleText("label", "-"));
    k.setField(styleNumber("sweep", 300));
  }
  el("slider", 12, 88, 150, 26, {ctl(o.level)});
  {
    doc::PanelElement& s = el("slider", 186, 86, 24, 84, {ctl(o.drive)});
    s.setField(styleText("handle", "dot"));
    s.setField(styleNumber("track", 3));
  }
  el("number", 12, 122, 70, 26, {ctl(o.cutoff)});
  el("modsource", 92, 120, 24, 32, {doc::signalBinding(o.level, "y")}).setField(styleText("label", "LVL"));
  el("button", 12, 160, 70, 20, {ctl(o.mix)}).setField(styleText("text", "Hold"));
  {
    doc::PanelElement& l = el("shape", 124, 136, 52, 4);
    l.setField(styleText("shape", "line"));
    l.setField(styleText("stroke", "tick"));
  }
  {
    doc::PanelElement& t = el("tabs", 248, 8, 120, 16);
    t.setField(styleText("items", "Curve|XY"));
  }
  {
    doc::PanelElement& l = el("label", 380, 8, 88, 14);
    l.setField(styleText("text", "SYNTH"));
    l.setField(styleText("align", "right"));
    l.setField(styleText("colour", "value"));
  }
  el("curve", 248, 30, 220, 110, {ctl(o.cutoff)});
  el("xy", 248, 30, 110, 110, {ctl(o.drive), ctl(o.mix)});
  {
    doc::PanelElement& k = el("knob", 380, 46, 60, 76, {ctl(o.level)});
    k.setField(styleText("pointer", "pie"));
    k.setField(styleText("colour", "signal"));
  }
  el("toggle", 248, 150, 80, 16, {set(doc::kGroupMono)}).setField(styleText("text", "Mono"));
  {
    doc::PanelElement& t = el("toggle", 336, 148, 64, 20, {set(doc::kGroupLegato)});
    t.setField(styleText("look", "button"));
    t.setField(styleText("text", "Legato"));
  }
  el("menu", 248, 176, 90, 20, {set(doc::kGroupSteal)}).setField(styleText("label", "-"));
  el("number", 346, 176, 56, 20, {set(doc::kGroupVoices)}).setField(styleText("label", "-"));
  {
    doc::PanelElement& c = el("shape", 452, 194, 14, 14);
    c.setField(styleText("shape", "circle"));
    c.setField(styleText("fill", "value"));
    c.setField(styleText("stroke", "none"));
  }
  std::vector<uint32_t> made;
  (void)r.run(doc::ops::addPanelElements(*store.current(), o.group, std::move(es), &made));
  if (!r.status) return r.status;
  // The curve on tab 0, the XY pad and the big knob on tab 1.
  const doc::Panel& pnl = *doc::findDevice(*store.current(), o.group).device->panel;
  uint32_t tabs = 0;
  for (const doc::PanelElement& e : pnl.elements)
    if (e.kind == "tabs") tabs = e.id;
  for (const doc::PanelElement& e : pnl.elements) {
    const bool curve = e.kind == "curve", second = e.kind == "xy" || (e.kind == "knob" && e.x >= 370.0f);
    if (!curve && !second) continue;
    doc::PanelElement c = e;
    c.when = doc::Visibility{tabs, {curve ? 0.0 : 1.0}};
    (void)r.run(doc::ops::setPanelElement(*store.current(), o.group, std::move(c)));
  }
  {  // a size with its margin
    (void)r.run(doc::ops::setPanelSize(*store.current(), o.group, 480.0f, 220.0f));
  }
  r.clip(o.keys, 0, 4 * kTicksPerWhole, "Chord", {0, 4, 7});
  o.tabs = tabs;
  if (ids != nullptr) *ids = o;
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
  {  // (GU.4: a free Wavetable — one block — where a free Phase accumulator was: a group preset now)
    auto e = r.run(doc::ops::addMetaDevice(*store.current(), store.ids(), out.lead, "wavetable", doc::ops::kOffPath));
    out.acc = e && !e->devices.empty() ? e->devices[0] : doc::DeviceId{};
    if (!out.acc.valid()) {
      if (r.status) return Error{ErrorCode::Unknown, "free rack project: Wavetable", {}};
      return r.status;
    }
    (void)r.run(doc::ops::setMetaParamValue(*store.current(), out.acc, "", "wavetable", "position", 0.4));
    (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), out.acc, "out", out.filter, "sc"));
    if (!out.lfos.empty()) (void)r.run(doc::ops::connectWire(*store.current(), store.ids(), out.lfos.front(), "lfo", out.acc, "position"));
  }
  r.clip(out.lead, 0, 4 * kTicksPerWhole, "Riff", {0, 3, 7});
  if (ids != nullptr) *ids = std::move(out);
  return r.status;
}

}  // namespace pq::views::project_patch
