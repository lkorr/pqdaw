// Validation helpers shared by command application (apply.cpp) and the project loader (serialize.cpp).

#include "checks.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace pq::doc::detail {

Error invalid(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m), {}}; }
Error notFound(std::string m) { return Error{ErrorCode::NotFound, std::move(m), {}}; }
Error unsupported(std::string m) { return Error{ErrorCode::Unsupported, std::move(m), {}}; }
Status ok() { return Ok{}; }

// ---- Validation helpers ------------------------------------------------------------------------------------------

Status checkTick(Ticks t, const char* what) {
  if (t < -kMaxTicks || t > kMaxTicks) return invalid(std::string(what) + " out of range");
  return ok();
}
Status checkLength(Ticks l, const char* what) {
  if (l < 1 || l > kMaxTicks) return invalid(std::string(what) + " must be at least one tick");
  return ok();
}
bool finiteIn(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

Status checkParamValue(ParamKind k, double v) {
  const ParamRange r = paramRange(k);
  if (!finiteIn(v, r.min, r.max)) return invalid("parameter value out of range");
  return ok();
}

Status checkTuningDef(const TuningDef& d) {
  Result<tuning::Tuning> t = buildTuning(d);
  if (!t) return std::move(t).error();
  return ok();
}

Status checkAssignment(const Project& p, const TuningAssignment& a) {
  switch (a.mode) {
    case TuningMode::Inherit: return ok();
    case TuningMode::Main:
      if (!findTuning(p, a.main)) return notFound("main tuning not found");
      return ok();
    case TuningMode::Private:
      if (!a.privateDef) return invalid("private tuning without a definition");
      return checkTuningDef(*a.privateDef);
  }
  return invalid("bad tuning mode");
}

Status checkSignature(TimeSignature sig) {
  if (sig.numerator < 1 || sig.numerator > 1024 || !noteValueIsExact(sig.denominator))
    return invalid("invalid time signature");
  return ok();
}

Status checkLoop(const ClipLoop& l) {
  PQ_DOC_TRY(checkTick(l.start, "loop start"));
  PQ_DOC_TRY(checkTick(l.end, "loop end"));
  if (l.end <= l.start) return invalid("loop end must be after loop start");
  return ok();
}

// Everything about a clip except note validity (NoteSeq contents are validated where notes enter: fromNotes and
// the note commands).
Status checkClip(const Project& p, const NoteClip& c) {
  if (!c.id.valid()) return invalid("clip has no id");
  PQ_DOC_TRY(checkTick(c.start, "clip start"));
  PQ_DOC_TRY(checkLength(c.length, "clip length"));
  PQ_DOC_TRY(checkTick(c.contentStart, "clip content start"));
  PQ_DOC_TRY(checkLoop(c.loop));
  PQ_DOC_TRY(checkSignature(c.signature));
  PQ_DOC_TRY(checkFundamental(c.fundamental));
  return checkAssignment(p, c.tuning);
}

Status checkAudioClip(const Project& p, const AudioClip& c) {
  if (!c.id.valid()) return invalid("clip has no id");
  PQ_DOC_TRY(checkTick(c.start, "clip start"));
  PQ_DOC_TRY(checkLength(c.length, "clip length"));
  if (!findSample(p, c.sample)) return notFound("unknown sample id " + std::to_string(c.sample.value));
  if (c.warp != AudioWarp::Off && c.warp != AudioWarp::RePitch) return invalid("unknown warp mode");
  if (!std::isfinite(c.contentStartFrame) || std::abs(c.contentStartFrame) > 9.0e15)
    return invalid("content start frame out of range");
  if (!finiteIn(c.seconds, 1e-9, 1.0e8)) return invalid("clip duration must be positive (seconds)");
  PQ_DOC_TRY(checkTick(c.contentStart, "clip content start"));
  PQ_DOC_TRY(checkLoop(c.loop));
  if (!finiteIn(c.sourceBpm, kMinBpm, kMaxBpm)) return invalid("source BPM out of range");
  if (!finiteIn(c.gainDb, kMinClipGainDb, kMaxClipGainDb)) return invalid("clip gain out of range");
  switch (c.transpose.unit) {
    case TransposeUnit::Cents:
      if (!finiteIn(c.transpose.cents, -kMaxTransposeCents, kMaxTransposeCents)) return invalid("transpose out of range");
      break;
    case TransposeUnit::Steps:
      if (c.transpose.steps < -kMaxTransposeSteps || c.transpose.steps > kMaxTransposeSteps)
        return invalid("transpose steps out of range");
      break;
    case TransposeUnit::Ratio:
      if (!c.transpose.ratio.isPositive() || std::abs(tuning::cents(c.transpose.ratio)) > kMaxTransposeCents + 1e-9)
        return invalid("transpose ratio out of range");
      break;
    default: return invalid("unknown transpose unit");
  }
  if (!finiteIn(c.detuneCents, -kMaxDetuneCents, kMaxDetuneCents)) return invalid("detune out of range");
  for (const AudioFade* f : {&c.fadeIn, &c.fadeOut}) {
    if (f->length < 0 || f->length > kMaxTicks) return invalid("fade length out of range");
    if (!finiteIn(f->curve, -1.0, 1.0)) return invalid("fade curve must be in [-1, 1]");
  }
  if (c.crossfadeIn < 0 || c.crossfadeIn > kMaxTicks) return invalid("crossfade length out of range");
  return checkAssignment(p, c.tuning);
}

Status checkSampleRef(const SampleRef& r) {
  if (!r.id.valid()) return invalid("sample has no id");
  if (r.path.empty()) return invalid("sample path is empty");
  if (r.frames < 1) return invalid("sample frames must be at least 1");
  if (!finiteIn(r.rate, 1e-3, 1.0e7)) return invalid("sample rate out of range");
  if (r.channels < 1 || r.channels > 1024) return invalid("sample channels out of range");
  return ok();
}

Status checkFundamental(const ClipFundamental& f) {
  if (f.lock && !f.lock->isPositive()) return invalid("a fundamental lock must be positive");
  for (size_t i = 0; i < f.steps.size(); ++i) {
    const HarmonicStep& h = f.steps[i];
    if (!h.note.valid()) return invalid("harmonic step without a note");
    if (i > 0 && !(f.steps[i - 1].note.value < h.note.value)) return invalid("harmonic steps must be sorted by note id");
    if (h.denominator < 1 || h.denominator > kMaxFundamentalDenominator)
      return invalid("harmonic step denominator out of range");
    if (!h.ratio.isPositive()) return invalid("harmonic step ratio must be positive");
  }
  return ok();
}

Status checkVolume(float db) { return checkParamValue(ParamKind::TrackVolume, db); }

Status checkPoints(ParamKind k, const std::vector<Breakpoint>& pts) {
  std::vector<uint32_t> ids;
  ids.reserve(pts.size());
  const ParamRange r = paramRange(k);
  for (const Breakpoint& b : pts) {
    if (!b.id.valid()) return invalid("breakpoint has no id");
    PQ_DOC_TRY(checkTick(b.tick, "breakpoint"));
    if (!finiteIn(b.value, r.min, r.max)) return invalid("breakpoint value out of the parameter's range");
    if (!finiteIn(b.curve, -1.0, 1.0)) return invalid("breakpoint curve must be in [-1, 1]");
    ids.push_back(b.id.value);
  }
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return invalid("duplicate breakpoint id");
  return ok();
}

bool pointLess(const Breakpoint& a, const Breakpoint& b) {
  return a.tick != b.tick ? a.tick < b.tick : a.id < b.id;
}

bool tuningParamApplies(const TuningDef& d, ParamKind k) {
  switch (k) {
    case ParamKind::TuningReference: return true;
    case ParamKind::TuningEquave: return d.edo() != nullptr;
    case ParamKind::MosGenerator:
    case ParamKind::MosPeriod:
    case ParamKind::MosStretch:
      return d.mos() != nullptr;
    default: return false;
  }
}

bool trackHasSends(TrackKind k) {
  return k == TrackKind::Midi || k == TrackKind::Audio || k == TrackKind::Group || k == TrackKind::Return;
}

// Whether `host` may carry a lane for `r` (see AddAutomationLane).
Status checkLaneParam(const Project& p, const Track& host, const ParamRef& r) {
  if (!isAutomatable(r.kind)) return invalid("parameter is not automatable");
  if (isTrackParam(r.kind)) {
    if (r.owner != host.id.value) return invalid("a track parameter can only be automated on its own track");
    if (r.kind == ParamKind::TrackMute && host.kind == TrackKind::Master) return invalid("the master has no mute");
    if (r.kind == ParamKind::TrackSend) {
      if (!trackHasSends(host.kind)) return invalid("this track has no sends");
      const Track* target = findTrackPtr(p, TrackId{r.target});
      if (!target || target->kind != TrackKind::Return || target->id == host.id)
        return invalid("send target must be another return track");
    } else if (r.target != 0) {
      return invalid("only sends have a target");
    }
    return ok();
  }
  const TuningSlot* slot = findTuning(p, TuningId{r.owner});
  if (!slot) return notFound("automated tuning not found");
  if (r.target != 0) return invalid("tuning parameters have no target");
  if (!tuningParamApplies(slot->def, r.kind)) return invalid("parameter does not apply to this tuning's kind");
  return ok();
}

Status checkDeviceValue(const std::string& name, const std::optional<double>& value) {
  // A patch identifier ([A-Za-z_][A-Za-z0-9_]*, PATCH_LANGUAGE §1): it is a JSON key in the project file.
  bool ident = !name.empty() && !(name[0] >= '0' && name[0] <= '9');
  for (const char c : name)
    ident = ident && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
  if (!ident) return invalid("'" + name + "' is not a device param or setting name");
  if (value && !std::isfinite(*value)) return invalid("device value '" + name + "' is not finite");
  return ok();
}

namespace {
Status checkValues(const std::vector<DeviceValue>& v, const char* what) {
  for (size_t i = 0; i < v.size(); ++i) {
    PQ_DOC_TRY(checkDeviceValue(v[i].name, v[i].value));
    if (i > 0 && !(v[i - 1].name < v[i].name))
      return invalid(std::string("device ") + what + " must be sorted by name, one per name");
  }
  return ok();
}
}  // namespace

Status checkChain(TrackKind kind, const std::vector<DevicePtr>& devices) {
  return checkList(listKindOf(kind), devices);
}

ListKind listKindOf(TrackKind kind) noexcept { return kind == TrackKind::Midi ? ListKind::MidiTrack : ListKind::OtherTrack; }

ListKind chainListKind(const Device& rack) noexcept {
  return rack.kind == DeviceKind::Instrument ? ListKind::InstrumentRackChain : ListKind::AudioRackChain;
}

// M4 review M9: iterative (an explicit list of device lists still to check), so racks nest as deep as memory allows. A
// rack's own fields are checked by checkRackOwn when it is met; its chains' lists are queued.
Status checkList(ListKind kind, const std::vector<DevicePtr>& devices) {
  std::vector<std::pair<ListKind, const std::vector<DevicePtr>*>> work{{kind, &devices}};
  while (!work.empty()) {
    const auto [k, listPtr] = work.back();
    work.pop_back();
    const std::vector<DevicePtr>& list = *listPtr;
    for (size_t i = 0; i < list.size(); ++i) {
      const DevicePtr& d = list[i];
      if (!d) return invalid("null device");
      if (!d->id.valid()) return invalid("device has no id");
      if (d->patch.empty()) return invalid("device has no patch");
      for (size_t j = 0; j < i; ++j)
        if (list[j]->id == d->id) return invalid("device id twice in a chain");
      if (d->kind == DeviceKind::Instrument) {
        if (k == ListKind::OtherTrack) return invalid("instruments go on MIDI tracks");
        if (k == ListKind::AudioRackChain) return invalid("an Audio Effect Rack's chains hold audio effects only");
        if (i != 0)
          return invalid(k == ListKind::MidiTrack ? "the instrument must be the first device of a MIDI track"
                                                  : "the instrument must be the first device of its chain");
      }
      PQ_DOC_TRY(checkValues(d->params, "params"));
      PQ_DOC_TRY(checkValues(d->settings, "settings"));
      if (d->rack) {
        PQ_DOC_TRY(checkRackOwn(*d));
        for (size_t c = d->rack->chains.size(); c-- > 0;) work.emplace_back(chainListKind(*d), &d->rack->chains[c]->devices);
      } else if (d->patch == kAudioEffectRack || d->patch == kInstrumentRack) {
        return invalid("'" + d->patch + "' is a rack and needs its chains");
      }
    }
  }
  return ok();
}

Status checkMapping(const MacroMapping& m) {
  if (!m.id.valid()) return invalid("mapping has no id");
  if (m.macro >= kMacroCount) return invalid("a rack has 16 macros");
  if (!m.device.valid()) return invalid("mapping has no target device");
  PQ_DOC_TRY(checkDeviceValue(m.param, std::nullopt));
  if (!finiteIn(m.min, 0.0, 1.0) || !finiteIn(m.max, 0.0, 1.0)) return invalid("mapping min / max must be in [0, 1]");
  if (!finiteIn(m.curve, -1.0, 1.0)) return invalid("mapping curve must be in [-1, 1]");
  return ok();
}

Status checkRack(const Device& d) {
  PQ_DOC_TRY(checkRackOwn(d));
  for (const ChainPtr& c : d.rack->chains) PQ_DOC_TRY(checkList(chainListKind(d), c->devices));
  return ok();
}

Status checkRackOwn(const Device& d) {
  const Rack& r = *d.rack;
  const bool instrument = d.kind == DeviceKind::Instrument;
  if (d.patch != (instrument ? kInstrumentRack : kAudioEffectRack)) return invalid("a rack's patch must name its kind");
  if (!d.settings.empty()) return invalid("racks have no settings");
  for (const DeviceValue& v : d.params) {
    if (!isRackParam(v.name)) return invalid("'" + v.name + "' is not a rack param (macro1 … macro16, select)");
    if (!finiteIn(v.value, 0.0, kMacroMax)) return invalid("rack param '" + v.name + "' must be in [0, 127]");
  }
  if (r.visibleMacros < 1 || r.visibleMacros > kMacroCount) return invalid("a rack shows 1 … 16 macros");
  if (static_cast<uint8_t>(r.keyUnit) > static_cast<uint8_t>(KeyZoneUnit::Degree)) return invalid("unknown key zone unit");
  if (r.voiceChains && !instrument) return invalid("only Instrument Rack chains run per voice");
  for (const MacroVariation& v : r.variations)
    for (const double x : v.values)
      if (!finiteIn(x, 0.0, kMacroMax)) return invalid("macro variation values must be in [0, 127]");
  for (size_t i = 0; i < r.chains.size(); ++i) {
    const ChainPtr& c = r.chains[i];
    if (!c) return invalid("null chain");
    if (!c->id.valid()) return invalid("chain has no id");
    for (size_t j = 0; j < i; ++j)
      if (r.chains[j]->id == c->id) return invalid("chain id twice in a rack");
    if (!finiteIn(c->volumeDb, kMinVolumeDb, kMaxVolumeDb)) return invalid("chain volume out of range");
    if (!finiteIn(c->pan, -1.0, 1.0)) return invalid("chain pan out of range");
    for (const ZoneKind k : {ZoneKind::Key, ZoneKind::Velocity, ZoneKind::Select})
      if (!zoneValid(c->zone(k), k, r.keyUnit)) return invalid("chain zone out of order or out of range");
    if (!instrument && (!zoneOff(c->key, ZoneKind::Key, r.keyUnit) || !zoneOff(c->velocity, ZoneKind::Velocity, r.keyUnit)))
      return invalid("key and velocity zones are Instrument Rack zones");
  }
  for (size_t i = 0; i < r.mappings.size(); ++i) {
    const MacroMapping& m = r.mappings[i];
    PQ_DOC_TRY(checkMapping(m));
    if (i > 0 && !(r.mappings[i - 1].id < m.id)) return invalid("mappings must be sorted by id, unique");
    for (size_t j = 0; j < i; ++j)
      if (r.mappings[j].device == m.device && r.mappings[j].param == m.param) return invalid("a param is mapped twice");
    if (m.device == d.id) {
      if (m.param != kSelectParam) return invalid("a rack's macros map its chain selector, not themselves");
    } else if (!deviceContains(d, m.device)) {
      return invalid("a macro maps only devices inside its rack");
    }
  }
  return ok();
}

Status checkWire(const Wire& w) {
  if (!w.id.valid()) return invalid("wire has no id");
  if (!w.from.valid() || !w.to.valid()) return invalid("wire end has no device");
  PQ_DOC_TRY(checkDeviceValue(w.port, std::nullopt));
  PQ_DOC_TRY(checkDeviceValue(w.param, std::nullopt));
  if (!finiteIn(w.amount, -1.0, 1.0)) return invalid("wire amount must be in [-1, 1]");
  if (static_cast<uint8_t>(w.reducer) > static_cast<uint8_t>(WireReducer::Loudest)) return invalid("unknown wire reducer");
  return ok();
}

// Rejects routing cycles: following each track's output (Default → parent group or master) must end at the
// master or at a track with no output, within one step per track.
Status checkRouting(const Project& p) {
  struct Hop {
    uint32_t id;
    uint32_t next;  // 0 = ends here
  };
  std::vector<Hop> hops;
  const uint32_t masterId = p.master ? p.master->id.value : 0;
  forEachTrack(p, [&](const Track& t, const Track* parent) {
    uint32_t next = 0;
    if (t.kind != TrackKind::Master) {
      switch (t.mixer.output.kind) {
        case OutputKind::Default: next = parent ? parent->id.value : masterId; break;
        case OutputKind::Master: next = masterId; break;
        case OutputKind::Track: next = t.mixer.output.track.value; break;
        case OutputKind::None: next = 0; break;
      }
    }
    hops.push_back({t.id.value, next});
  });
  std::sort(hops.begin(), hops.end(), [](const Hop& a, const Hop& b) { return a.id < b.id; });
  auto nextOf = [&](uint32_t id) -> uint32_t {
    auto it = std::lower_bound(hops.begin(), hops.end(), id, [](const Hop& h, uint32_t v) { return h.id < v; });
    return (it != hops.end() && it->id == id) ? it->next : 0;
  };
  for (const Hop& h : hops) {
    uint32_t at = h.next;
    for (size_t steps = 0; at != 0; ++steps) {
      if (at == h.id || steps > hops.size()) return invalid("routing would create a cycle");
      at = nextOf(at);
    }
  }
  return ok();
}

}  // namespace pq::doc::detail
