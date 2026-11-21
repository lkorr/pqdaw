// AP.7: the offline note-stage render (pq/app/note_stage_render.h).
#include "pq/app/note_stage_render.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <unordered_map>

#include "pq/document/pitch_resolve.h"
#include "pq/engine/project_program.h"
#include "pq/engine/sequence.h"
#include "pq/engine/timeline.h"
#include "project_link_ji.h"

namespace pq::app {

engine::ArpTables arpTablesOf(const doc::LoweredNoteChain& c) {
  auto one = [](const doc::NoteDegreeTable& t) { return engine::ArpDegreeTable{t.cents, t.equaveCents, t.referenceHz}; };
  engine::ArpTables r(one(c.trackTable));
  for (const doc::NoteDegreeTable& n : c.nodes) r.nodes.push_back(one(n));
  return r;
}

std::unique_ptr<engine::ArpStage> makeArpStage(const doc::LoweredNoteStage& st, const engine::ArpTables& table,
                                               const std::function<uint32_t(const std::string& key)>& slotOf,
                                               const std::function<uint32_t(doc::WireId)>& busOf,
                                               std::shared_ptr<engine::ArpTableMailbox> box,
                                               std::shared_ptr<NoteStageStatus> status) {
  if (st.type != "arpeggiator") return nullptr;  // (the one note device type: doc::noteDeviceTypes)
  engine::ArpParams fixed;
  engine::ArpLaneSteps steps;  // AP.5
  for (size_t k = 0; k < st.values.size() && k < static_cast<size_t>(engine::ArpParamId::Count); ++k) {
    const auto id = static_cast<engine::ArpParamId>(k);
    if (!engine::setArpLaneStep(steps, id, static_cast<float>(st.values[k])))
      engine::setArpParam(fixed, id, static_cast<float>(st.values[k]));
  }
  std::vector<engine::ArpStage::Mapping> map;
  for (const doc::LoweredNoteStage::Slot& r : st.runtime) {
    const uint32_t ps = slotOf ? slotOf(r.key) : kNoSlot;
    if (ps == kNoSlot || r.param >= static_cast<uint32_t>(engine::ArpParamId::Count)) continue;
    map.push_back({static_cast<engine::ArpParamId>(r.param), ps, r.scale, r.offset});
    if (busOf)
      for (const doc::WireId w : r.mods)  // AP.2b: each cable into it adds its bus value
        if (const uint32_t v = busOf(w); v != engine::kNoBusValue)
          map.push_back({static_cast<engine::ArpParamId>(r.param), v, 1.0f, 0.0f, true});
  }
  auto stage = std::make_unique<engine::ArpStage>(fixed, std::move(map), table, steps, std::move(box), std::move(status));
  stage->key = st.device.value;  // AP.2b: a republished chain's stage adopts the running one's state
  return stage;
}

namespace {

using engine::ArpExact;

// a then b (b applied to what a gave).
ArpExact compose(const ArpExact& a, const ArpExact& b) noexcept {
  using K = ArpExact::Kind;
  if (a.kind == K::Same) return b;
  if (b.kind == K::Same) return a;
  ArpExact x;
  if (a.kind == K::Degrees && b.kind == K::Degrees) {
    x.kind = K::Degrees;
    x.degrees = a.degrees + b.degrees;
    if (x.degrees == 0) x.kind = K::Same;
    return x;
  }
  if (a.kind == K::Ratio && b.kind == K::Ratio) {
    if (const std::optional<tuning::Rational> m = tuning::mul(tuning::Rational(a.num, a.den), tuning::Rational(b.num, b.den))) {
      x.kind = m->num() == m->den() ? K::Same : K::Ratio;
      x.num = m->num();
      x.den = m->den();
      return x;
    }
  }
  x.kind = K::Inexact;
  return x;
}

double rampShape(engine::RampShape shape, int8_t bend, double u) noexcept {  // note_events.h "Expression"
  u = std::clamp(u, 0.0, 1.0);
  switch (shape) {
    case engine::RampShape::Exponential: {
      const double k = bend / 16.0;
      if (std::fabs(k) < 1e-9) return u;
      return (1.0 - std::exp(-k * u)) / (1.0 - std::exp(-k));
    }
    case engine::RampShape::SCurve: return u * u * (3.0 - 2.0 * u);
    case engine::RampShape::Overshoot: {
      const double a = 4.0 * std::abs(static_cast<int>(bend)) / 127.0, v = u - 1.0;
      return 1.0 + (a + 1.0) * v * v * v + a * v * v;
    }
    default: return u;
  }
}

// A generated note's events as the run gave them.
struct RawPitch {
  SampleTime at = 0;
  double hz = 0.0;
  uint32_t len = 0;
  engine::RampShape shape = engine::RampShape::Linear;
  int8_t bend = 0;
};
struct Collected {
  SampleTime on = 0, off = -1;
  float hz0 = 0.0f, velocity = 0.0f, gain = 1.0f;
  std::vector<RawPitch> pitches;
};

// A note's pitch as points (song sample, cents relative to its identity's Hz), following the voice's ramps.
struct CurveBuilder {
  std::vector<std::pair<SampleTime, double>> pts;
  SampleTime rampAt = 0;
  uint32_t rampLen = 0;
  double c0 = 0.0, c1 = 0.0;
  engine::RampShape shape = engine::RampShape::Linear;
  int8_t bend = 0;
  double refHz = 1.0;
  double centsAt(SampleTime t) const noexcept {
    if (rampLen == 0 || t >= rampAt + rampLen) return c1;
    if (t <= rampAt) return c0;
    return c0 + (c1 - c0) * rampShape(shape, bend, static_cast<double>(t - rampAt) / rampLen);
  }
  void start(SampleTime t, double hz) {
    c0 = c1 = 1200.0 * std::log2(hz / refHz);
    pts.emplace_back(t, c1);
  }
  // A Pitch event at `t` (a later one replaces a running ramp: note_events.h).
  void pitch(const RawPitch& e) {
    const SampleTime t = e.at;
    const double now = centsAt(t);
    while (!pts.empty() && pts.back().first > t) pts.pop_back();  // the interrupted ramp's rest
    if (pts.empty() || pts.back().first != t || pts.back().second != now) pts.emplace_back(t, now);
    const double target = 1200.0 * std::log2(e.hz / refHz);
    rampAt = t;
    rampLen = e.len;
    c0 = now;
    c1 = target;
    shape = e.shape;
    bend = e.bend;
    if (e.len == 0) {
      pts.emplace_back(t, target);
      return;
    }
    // Linear in cents: its end; a shaped glide sampled to 16 lines (the engine's ramp is the exact one).
    const int n = e.shape == engine::RampShape::Linear ? 1 : 16;
    for (int i = 1; i <= n; ++i) {
      const SampleTime at = t + static_cast<SampleTime>(std::llround(static_cast<double>(e.len) * i / n));
      pts.emplace_back(at, c0 + (c1 - c0) * rampShape(e.shape, e.bend, static_cast<double>(i) / n));
    }
  }
};

// The arrangement tick of song sample `s`: the grid tick when it plays at that sample, else the first tick there.
Ticks tickOfSample(const TempoMap& tm, SampleTime s, double rate) noexcept {
  const auto t = static_cast<Ticks>(std::llround(tm.samplesToTicks(s, rate)));
  if (tm.ticksToSamples(t, rate) == s) return t;
  return tm.firstTickAtOrAfterSample(s, rate);
}

struct SourceNote {
  size_t clip = 0;
  const doc::Note* note = nullptr;
  double hz = 0.0;  // its lowered (static) Hz
  float keyDegree = std::numeric_limits<float>::quiet_NaN();
  uint16_t node = 0;  // AP.8a: the tuning node it follows (its degree table; 0: the track's)
};

}  // namespace

Result<TrackNoteRender> renderTrackNoteStages(const doc::Project& p, doc::TrackId trackId,
                                              const NoteStageRenderOptions& o) {
  const std::optional<doc::TrackPath> path = doc::findTrack(p, trackId);
  const doc::Track* track = doc::findTrackPtr(p, trackId);
  if (!path || track == nullptr || track->kind != doc::TrackKind::Midi)
    return Error{ErrorCode::NotFound, "note stages: no such MIDI track", {}};
  TrackNoteRender out;
  std::vector<doc::LoweredNoteChain> chains = doc::lowerNoteChains(p);
  const auto ch = std::find_if(chains.begin(), chains.end(), [&](const doc::LoweredNoteChain& c) { return c.track == trackId; });
  if (ch == chains.end()) return out;
  // ---- the stages: the playback link's, their runtime slots read from the devices' knob positions ----
  std::vector<float> params;
  std::unordered_map<std::string, uint32_t> slotOfKey;
  for (const doc::LoweredNoteStage& st : ch->stages) {
    const doc::Device* d = doc::findDevice(p, st.device).device;
    const doc::NoteDeviceType* type = d != nullptr ? doc::noteDeviceTypeOf(*d) : nullptr;
    if (type == nullptr) continue;
    for (const doc::LoweredNoteStage::Slot& r : st.runtime) {
      const doc::NoteParamSpec& s = type->params[r.param];
      const doc::DeviceValue* v = d->param(s.name);  // the slot holds the knob position (lower.cpp noteDeviceParams)
      slotOfKey.emplace(r.key, static_cast<uint32_t>(params.size()));
      params.push_back(static_cast<float>(v != nullptr ? v->value : doc::noteParamPosition(s, s.def)));
    }
  }
  const engine::ArpTables table = arpTablesOf(*ch);
  engine::NoteStageChain chain;
  std::vector<std::vector<engine::ArpTraceNote>> traces(ch->stages.size());
  for (size_t i = 0; i < ch->stages.size(); ++i) {
    std::unique_ptr<engine::ArpStage> stage = makeArpStage(
        ch->stages[i], table, [&](const std::string& k) { const auto it = slotOfKey.find(k); return it == slotOfKey.end() ? kNoSlot : it->second; },
        {});
    if (!stage) continue;
    stage->core().setTrace(&traces[chain.size()]);
    out.devices.push_back(ch->stages[i].device);
    (void)chain.add(std::move(stage));
  }
  if (chain.size() == 0) return out;
  out.any = true;
  // ---- the notes: the playback lowering of the track's played clips ----
  const std::vector<doc::ClipSlot>& slots = doc::playedClips(*track);
  Result<doc::TrackPitches> pitches = doc::resolveTrackPitches(p, trackId);
  if (!pitches) return std::move(pitches).error();
  const std::vector<doc::TuningId> signals = doc::trackTuningSignals(p, *path);
  const bool tempoMoves = p.timeline->tempo.size() > 1;
  engine::TrackSequenceDesc desc;
  desc.id = trackId.value;
  std::unordered_map<uint32_t, SourceNote> sources;  // by document note id
  Ticks end = 0;
  std::vector<size_t> noteClips;  // index into slots of each ClipGenerated
  for (size_t ci = 0; ci < slots.size() && ci < pitches->clips.size(); ++ci) {
    if (!slots[ci].clip) continue;
    const doc::NoteClip& c = *slots[ci].clip;
    engine::ClipDesc cd = lowerClipDesc(p, c, pitches->clips[ci], signals, tempoMoves);
    std::unordered_map<uint32_t, const engine::NoteDesc*> byId;
    for (const engine::NoteDesc& nd : cd.notes) byId.emplace(nd.id, &nd);
    for (const doc::Note& n : c.notes)
      if (const auto it = byId.find(n.id.value); it != byId.end())
        sources[n.id.value] = {out.clips.size(), &n, it->second->hz, it->second->keyDegree, it->second->pitch.node};
    end = std::max(end, c.start + std::max<Ticks>(c.length, 1));
    out.clips.push_back({c.id, {}});
    noteClips.push_back(ci);
    desc.clips.push_back(std::move(cd));
  }
  engine::SequenceBuildOptions so;
  so.seed = o.chanceSeed;
  auto image = engine::SequenceImage::build(std::span<const engine::TrackSequenceDesc>(&desc, 1), so);
  if (!image) return std::move(image).error();
  auto tempo = engine::TempoImage::create(p.timeline->tempo, p.timeline->signatures);
  if (!tempo) return std::move(tempo).error();
  const TempoMap tm = (*tempo)->tempo();
  end = std::min(end, o.until);
  const SampleTime frames = std::max<SampleTime>(0, tm.ticksToSamples(end, o.sampleRate));
  engine::NoteStageRenderConfig rc;
  rc.sampleRate = o.sampleRate;
  rc.blockFrames = o.blockFrames;
  rc.timeline.maxEventsPerTrack = 256;  // ProjectLink's engine (kTimelineEventsPerTrack)
  Result<engine::NoteStageRender> run =
      engine::renderNoteStages(std::move(image.value()), std::move(tempo.value()), chain, params, 0, frames, rc);
  if (!run) return std::move(run).error();
  // ---- the generated notes: their source (through the chain) and identity ----
  std::unordered_map<uint32_t, uint32_t> docOf;  // sequencer event id → document note id
  for (const auto& [ev, id] : run->notes) docOf[ev] = id;
  std::vector<std::unordered_map<uint32_t, const engine::ArpTraceNote*>> traceOf(chain.size());
  for (size_t i = 0; i < chain.size(); ++i)
    for (const engine::ArpTraceNote& t : traces[i]) traceOf[i][t.id] = &t;
  // The document source and the exact rule of generated note `id` (its stage index is in the id).
  auto origin = [&](uint32_t id, uint32_t& doc, ArpExact& x) -> bool {
    x = {};
    for (int guard = 0; guard < 64; ++guard) {
      if (!engine::isStageNoteId(id)) {
        const auto it = docOf.find(id);
        if (it == docOf.end()) return false;
        doc = it->second;
        return true;
      }
      const uint32_t stage = (id >> 24) & 0x7Fu;
      if (stage >= traceOf.size()) return false;
      const auto it = traceOf[stage].find(id);
      if (it == traceOf[stage].end()) return false;
      x = compose(it->second->exact, x);
      id = it->second->source;
    }
    return false;
  };
  // AP.8a (H4): the degree table of a source's tuning node (the stage's ArpTables::of).
  auto tableOf = [&](uint16_t node) -> const doc::NoteDegreeTable& {
    return node >= 1 && node <= ch->nodes.size() ? ch->nodes[node - 1u] : ch->trackTable;
  };
  auto sizeOf = [](const doc::NoteDegreeTable& t) -> int64_t { return t.cents.empty() ? 12 : static_cast<int64_t>(t.cents.size()); };
  auto centsOf = [&](const doc::NoteDegreeTable& t, int64_t deg) {
    const int64_t N = sizeOf(t);
    const int64_t q = deg >= 0 ? deg / N : -((-deg + N - 1) / N);
    const double step = t.cents.empty() ? 100.0 * static_cast<double>(deg - q * N) : t.cents[static_cast<size_t>(deg - q * N)];
    return static_cast<double>(q) * (t.cents.empty() ? 1200.0 : t.equaveCents) + step;
  };
  std::unordered_map<uint32_t, Collected> live;
  struct Done {
    uint32_t doc;
    ArpExact x;
    Collected g;
  };
  std::vector<Done> done;
  std::unordered_map<uint32_t, std::pair<uint32_t, ArpExact>> originOf;
  for (const engine::TimedNoteEvent& te : run->output) {
    const engine::NoteEvent& e = te.e;
    if (!engine::isStageNoteId(e.noteId)) continue;  // a note the stages passed through
    switch (e.kind) {
      case engine::NoteEventKind::NoteOn: {
        uint32_t doc = 0;
        ArpExact x;
        if (!origin(e.noteId, doc, x) || sources.count(doc) == 0) break;
        Collected g;
        g.on = te.song;
        g.hz0 = e.hz;
        g.velocity = e.velocity;
        g.gain = e.gain;
        live[e.noteId] = std::move(g);
        originOf[e.noteId] = {doc, x};
        break;
      }
      case engine::NoteEventKind::Pitch: {
        const auto it = live.find(e.noteId);
        if (it == live.end() || !(e.hz > 0.0f)) break;
        it->second.pitches.push_back({te.song, static_cast<double>(e.hz), e.rampFrames,
                                      static_cast<engine::RampShape>(e.rampShape), static_cast<int8_t>(e.rampBend)});
        break;
      }
      case engine::NoteEventKind::NoteOff: {
        const auto it = live.find(e.noteId);
        if (it == live.end()) break;
        it->second.off = te.song;
        const auto& [doc, x] = originOf[e.noteId];
        done.push_back({doc, x, std::move(it->second)});
        live.erase(it);
        break;
      }
      case engine::NoteEventKind::AllNotesOff:
      case engine::NoteEventKind::AllSoundOff:
        for (auto& [id, g] : live) {
          g.off = te.song;
          const auto& [doc, x] = originOf[id];
          done.push_back({doc, x, std::move(g)});
        }
        live.clear();
        break;
      default: break;
    }
  }
  for (auto& [id, g] : live) {  // still sounding at the end: cut there
    g.off = frames;
    const auto& [doc, x] = originOf[id];
    done.push_back({doc, x, std::move(g)});
  }
  std::sort(done.begin(), done.end(), [](const Done& a, const Done& b) { return a.g.on < b.g.on; });
  // ---- to document notes ----
  for (Done& d : done) {
    const SourceNote& src = sources.at(d.doc);
    const doc::NoteClip& c = *slots[noteClips[src.clip]].clip;
    const Ticks clipEnd = c.start + std::max<Ticks>(c.length, 1);
    const Ticks t0 = tickOfSample(tm, d.g.on, o.sampleRate);
    if (t0 < c.start || t0 >= clipEnd) continue;  // not in its clip's span
    const Ticks content0 = t0 - c.start + c.contentStart;
    if (c.loop.enabled && c.loop.end > c.loop.start && content0 >= c.loop.end) continue;  // a later pass
    const Ticks t1 = std::min(std::max(tickOfSample(tm, d.g.off, o.sampleRate), t0 + 1), clipEnd);
    doc::GeneratedNote gn;
    gn.start = content0;
    gn.length = std::max<Ticks>(t1 - t0, 1);
    gn.source = src.note->id;
    gn.velocity = std::clamp(d.g.velocity * d.g.gain * 127.0f, 1.0f, 127.0f);
    const tuning::PitchId& sp = src.note->pitch;
    using K = ArpExact::Kind;
    double hz = 0.0;
    bool exact = true;
    switch (d.x.kind) {
      case K::Same:
        gn.pitch = sp;
        hz = src.hz;
        break;
      case K::Degrees: {
        const doc::NoteDegreeTable& tab = tableOf(src.node);
        const int64_t N = sizeOf(tab);
        if (sp.kind() == tuning::PitchKind::Degree) {
          const int64_t D = static_cast<int64_t>(sp.equave()) * N + sp.degree();
          const int64_t D1 = D + d.x.degrees;
          const int64_t q = D1 >= 0 ? D1 / N : -((-D1 + N - 1) / N);
          gn.pitch = tuning::PitchId::fromDegree(static_cast<int32_t>(D1 - q * N), static_cast<int32_t>(q));
          hz = src.hz * std::exp2((centsOf(tab, D1) - centsOf(tab, D)) / 1200.0);
        } else {  // the arp shifted from round(noteDegree): its Hz, as Cents
          const int64_t d0 = std::isfinite(src.keyDegree) ? std::llround(src.keyDegree) : 0;
          hz = src.hz * std::exp2((centsOf(tab, d0 + d.x.degrees) - centsOf(tab, d0)) / 1200.0);
          exact = false;
        }
        break;
      }
      case K::Ratio: {
        const tuning::Rational r(d.x.num, d.x.den);
        std::optional<tuning::PitchId> pid;
        if (sp.kind() == tuning::PitchKind::Ratio) {  // r times its ratio, to the same anchor (a member stays one)
          if (const std::optional<tuning::Rational> m = tuning::mul(sp.ratio(), r))
            pid = tuning::PitchId::tryFromRatio(*m, sp.anchorKind(), sp.anchorId());
        } else {  // r anchored to the source note itself
          pid = tuning::PitchId::tryFromRatio(r, tuning::AnchorKind::Note, src.note->id.value);
        }
        hz = src.hz * static_cast<double>(d.x.num) / static_cast<double>(d.x.den);
        if (pid) gn.pitch = *pid;
        else exact = false;
        break;
      }
      case K::Inexact:
        exact = false;
        hz = d.g.hz0;  // (its onset's Hz; an Into glide's target when it glides in)
        break;
    }
    if (!exact) {
      // An inexact identity: the Hz it settles on (an Into glide's target when it glides in at its onset).
      if (d.x.kind == K::Inexact && !d.g.pitches.empty() && d.g.pitches.front().at == d.g.on) hz = d.g.pitches.front().hz;
      double ref = 0.0;
      const doc::ClipPitches& cp = pitches->clips[noteClips[src.clip]];
      if (cp.tuning.def != nullptr)
        if (Result<tuning::Tuning> tt = doc::buildTuning(*cp.tuning.def); tt) ref = tt->referenceHz();
      if (!(ref > 0.0) || !(hz > 0.0)) continue;
      gn.pitch = tuning::PitchId::fromCents(1200.0 * std::log2(hz / ref));
      ++out.inexact;
    }
    gn.hz = hz;
    gn.exact = exact;
    // Its pitch curve: the onset's Hz, then each Pitch event, relative to `hz`.
    CurveBuilder g;
    g.refHz = hz;
    g.start(d.g.on, d.g.hz0);
    for (const RawPitch& e : d.g.pitches) g.pitch(e);
    bool bends = false;
    for (const auto& [at, cents] : g.pts) bends = bends || std::fabs(cents) > 1e-6;
    if (bends) {
      for (const auto& [at, cents] : g.pts) {
        const Ticks nt = std::clamp<Ticks>(tickOfSample(tm, at, o.sampleRate) - t0, 0, gn.length);
        doc::Breakpoint b;
        b.tick = nt;
        b.value = static_cast<float>(cents);
        if (!gn.curve.empty() && gn.curve.back().tick == nt && gn.curve.size() >= 2 &&
            gn.curve[gn.curve.size() - 2].tick == nt)
          gn.curve.back() = b;  // at most two points per tick (a jump)
        else
          gn.curve.push_back(b);
      }
    }
    out.clips[src.clip].notes.push_back(std::move(gn));
  }
  if (o.keepEvents) out.events = std::move(run.value());
  return out;
}

}  // namespace pq::app
