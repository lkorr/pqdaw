// Meta-module edit builders (TMM.3): pq/document/edit_ops.h, "Meta modules".
#include "edit_common.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"
#include "pq/meta/sampler.h"
#include "pq/document/midi_key.h"
#include "pq/dsp/warp_voice.h"

#include <algorithm>
#include <cmath>

namespace pq::doc::ops {

using namespace detail;

namespace {

// The device and, for a nested path without state, the AddMetaInstance of its default state (into `e`).
Result<const Device*> prepare(const Project& p, Edit& e, DeviceId device, const std::string& path, std::string_view type) {
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  e.devices.push_back(device);
  e.tracks.push_back(l.track->id);
  if (!path.empty() && l.device->metaAt(path) == nullptr) {
    const meta::Type* t = meta::findType(type);
    if (t == nullptr) return invalid("unknown meta module type '" + std::string(type) + "'");
    batchOf(e).commands.push_back(AddMetaInstance{device, path, defaultMetaInstance(*t)});
  }
  return l.device;
}

}  // namespace

EditResult setMetaParam(const Project& p, DeviceId device, std::string path, std::string_view type, std::string param,
                        std::optional<double> value) {
  Edit e = makeEdit("Meta Module Parameter");
  Result<const Device*> d = prepare(p, e, device, path, type);
  if (!d) return std::move(d).error();
  batchOf(e).commands.push_back(SetMetaParam{device, std::move(path), std::move(param), value});
  return e;
}

EditResult setMetaParamValue(const Project& p, DeviceId device, std::string path, std::string_view type, std::string param,
                             double value) {  // GU.1
  const DeviceLoc l = findDevice(p, device);
  if (!l.track) return notFound("device not found");
  const meta::Type* t = meta::findType(path.empty() ? metaDeviceType(*l.device) : type);
  if (t == nullptr) return notFound("unknown meta module type");
  const int32_t i = t->paramIndex(param);
  if (i < 0) return invalid("'" + param + "' is not a param of " + std::string(t->id));
  const auto k = static_cast<size_t>(i);
  const double now = metaParamPosition(*l.device, path, *t, k);
  const double pos = dsp::curvePosition(metaParamCurve(*l.device, path, *t, k), value, now);
  return setMetaParam(p, device, std::move(path), type, std::move(param), pos);
}

EditResult setMetaSetting(const Project& p, DeviceId device, std::string path, std::string_view type, std::string setting,
                          std::optional<double> value) {
  Edit e = makeEdit("Meta Module Setting");
  Result<const Device*> d = prepare(p, e, device, path, type);
  if (!d) return std::move(d).error();
  batchOf(e).commands.push_back(SetMetaSetting{device, std::move(path), std::move(setting), value});
  return e;
}

EditResult setMetaData(const Project& p, DeviceId device, std::string path, std::string_view type, meta::DataPtr data) {
  Edit e = makeEdit("Edit Meta Module Data");
  Result<const Device*> d = prepare(p, e, device, path, type);
  if (!d) return std::move(d).error();
  if (path.empty() && (*d)->metaAt("") == nullptr) {  // a chain device's "" entry (always made with it; restored here)
    const meta::Type* t = meta::findType(metaDeviceType(**d));
    if (t == nullptr) return invalid("not a meta device");
    batchOf(e).commands.push_back(AddMetaInstance{device, "", defaultMetaInstance(*t)});
  }
  batchOf(e).commands.push_back(SetMetaData{device, std::move(path), std::move(data)});
  return e;
}

// SM.3: a Sampler's sample (pq/document/edit_ops.h).
EditResult setSamplerSample(const Project& p, DeviceId sampler, SampleId sample, std::optional<double> rootHz,
                            std::vector<float> slices) {
  const DeviceLoc l = findDevice(p, sampler);
  if (!l.track) return notFound("device not found");
  auto isEngine = [](const Device& d) { return isMetaDevice(d) && metaDeviceType(d) == "sampler"; };
  DeviceId engine;
  if (isEngine(*l.device)) {
    engine = sampler;
  } else if (isGroupDevice(*l.device)) {
    forEachDeviceIn(l.track->devices, [&](const Device& d) {
      if (!engine.valid() && d.group == sampler && isEngine(d)) engine = d.id;
    });
  }
  if (!engine.valid()) return invalid("not a Sampler");
  if (sample.valid() && findSample(p, sample) == nullptr) return notFound("sample not found");
  for (const float f : slices)
    if (!(f >= 0.0f) || !std::isfinite(f)) return invalid("a slice must be a frame ≥ 0");
  std::sort(slices.begin(), slices.end());
  slices.erase(std::unique(slices.begin(), slices.end()), slices.end());
  const double hz = rootHz && *rootHz > 0.0 && std::isfinite(*rootHz) ? *rootHz : meta::kSamplerDefaultRoot;
  Result<Edit> data = setMetaData(p, engine, "", "sampler", meta::samplerData(sample.valid() ? sample.value : 0, std::move(slices)));
  if (!data) return std::move(data).error();
  Result<Edit> root = setMetaParamValue(p, engine, "", "sampler", "root", hz);
  if (!root) return std::move(root).error();
  Edit e = makeEdit("Set Sampler Sample");
  e.devices.push_back(sampler);
  e.tracks.push_back(l.track->id);
  for (Edit* x : {&*data, &*root})
    for (Command& c : batchOf(*x).commands) batchOf(e).commands.push_back(std::move(c));
  return e;
}


// ---- SM.12: slicing, Slice to Notes ----------------------------------------------------------------------------------

namespace {

// The Sampler engine of `sampler` (the group or the engine itself) and its track; invalid: not a Sampler.
struct SamplerAt {
  DeviceId engine;
  const Track* track = nullptr;
  const Device* device = nullptr;
};
SamplerAt samplerAt(const Project& p, DeviceId sampler) {
  SamplerAt out;
  const DeviceLoc l = findDevice(p, sampler);
  if (!l.track) return out;
  auto isEngine = [](const Device& d) { return isMetaDevice(d) && metaDeviceType(d) == "sampler"; };
  if (isEngine(*l.device)) {
    out.engine = sampler;
    out.device = l.device;
  } else if (isGroupDevice(*l.device)) {
    forEachDeviceIn(l.track->devices, [&](const Device& d) {
      if (!out.engine.valid() && d.group == sampler && isEngine(d)) {
        out.engine = d.id;
        out.device = &d;
      }
    });
  }
  out.track = l.track;
  return out;
}

}  // namespace

DeviceId samplerEngineOf(const Project& p, DeviceId sampler) { return samplerAt(p, sampler).engine; }

double sliceThreshold(double sensitivity) noexcept {
  return kSliceBaseThreshold * std::exp2(3.0 * (1.0 - std::clamp(sensitivity, 0.0, 1.0)));
}

std::vector<float> sliceStarts(const SliceSpec& spec, int64_t frames, double rate, std::span<const double> onsets,
                               std::span<const float> strengths, std::span<const float> markers, double sampleBpm) {
  std::vector<float> out;
  if (frames <= 0 || !(rate > 0.0)) return out;
  out.push_back(0.0f);  // slice 1 always starts the sample
  const auto add = [&](double f) {
    if (f >= 1.0 && f < static_cast<double>(frames)) out.push_back(static_cast<float>(std::round(f)));
  };
  switch (spec.by) {
    case SliceBy::Manual: return {};
    case SliceBy::Transient: {
      const double tau = sliceThreshold(spec.sensitivity);
      for (size_t i = 0; i < onsets.size(); ++i)
        if (i >= strengths.size() || static_cast<double>(strengths[i]) >= tau) add(onsets[i]);
      break;
    }
    case SliceBy::Beat: {  // every `beats` content beats, through the markers (none: sampleBpm)
      dsp::WarpSource src;
      src.fileRate = rate;
      src.markers = markers.size() >= 2 ? markers.data() : nullptr;
      src.markerCount = static_cast<uint32_t>(markers.size() / 2);
      const double fpb = 60.0 * rate / std::max(1.0, sampleBpm);
      const double step = std::max(spec.beats, 1.0 / 64.0);
      const double b0 = dsp::warpBeatOfFrame(src, 0.0, fpb), b1 = dsp::warpBeatOfFrame(src, static_cast<double>(frames), fpb);
      for (double q = std::floor(b0 / step + 1.0) * step; q < b1 && out.size() < kMaxSlices; q += step)
        add(dsp::warpFrameOfBeat(src, q, fpb));
      break;
    }
    case SliceBy::Region: {
      const uint32_t n = std::clamp<uint32_t>(spec.count, 1, kMaxSlices);
      for (uint32_t i = 1; i < n; ++i) add(static_cast<double>(frames) * i / n);
      break;
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  if (out.size() > kMaxSlices) out.resize(kMaxSlices);
  return out;
}

EditResult sliceSampler(const Project& p, DeviceId sampler, const SliceSpec& spec, std::span<const double> onsets,
                        std::span<const float> strengths) {
  const SamplerAt s = samplerAt(p, sampler);
  if (!s.engine.valid()) return invalid("not a Sampler");
  const MetaInstance* inst = s.device->metaAt("");
  const meta::DataPtr data = inst != nullptr ? inst->data : nullptr;
  const uint32_t sid = meta::samplerSample(data.get());
  const SampleRef* ref = sid != 0 ? findSample(p, SampleId{sid}) : nullptr;
  meta::DataPtr d = meta::samplerWith(data.get(), "sliceBy", meta::DataValue::num(static_cast<double>(spec.by)));
  d = meta::samplerWith(d.get(), "sensitivity", meta::DataValue::num(std::clamp(spec.sensitivity, 0.0, 1.0)));
  d = meta::samplerWith(d.get(), "sliceBeats", meta::DataValue::num(spec.beats));
  d = meta::samplerWith(d.get(), "sliceCount", meta::DataValue::num(static_cast<double>(spec.count)));
  if (spec.by != SliceBy::Manual) {
    if (ref == nullptr) return invalid("the Sampler has no sample to slice");
    const meta::Type& t = meta::samplerType();
    const double bpm = metaParamValue(*s.device, "", t, meta::kSmpSampleBpm);
    d = meta::samplerWith(d.get(), "slices",
                          meta::DataValue::floatArray(sliceStarts(spec, ref->frames, ref->rate, onsets, strengths,
                                                                  meta::samplerMarkers(data.get()), bpm)));
  }
  Result<Edit> data2 = setMetaData(p, s.engine, "", "sampler", std::move(d));
  if (!data2) return std::move(data2).error();
  Edit e = makeEdit("Slice Sample");
  e.devices.push_back(sampler);
  e.tracks.push_back(s.track->id);
  for (Command& c : batchOf(*data2).commands) batchOf(e).commands.push_back(std::move(c));
  return e;
}

EditResult sliceToNotes(const Project& p, IdSource& ids, DeviceId sampler, Ticks at) {
  const SamplerAt s = samplerAt(p, sampler);
  if (!s.engine.valid()) return invalid("not a Sampler");
  if (s.track->kind != TrackKind::Midi) return invalid("Slice to Notes needs the Sampler on a MIDI track");
  const MetaInstance* inst = s.device->metaAt("");
  const meta::DataPtr data = inst != nullptr ? inst->data : nullptr;
  const uint32_t sid = meta::samplerSample(data.get());
  const SampleRef* ref = sid != 0 ? findSample(p, SampleId{sid}) : nullptr;
  if (ref == nullptr) return invalid("the Sampler has no sample");
  std::vector<float> slices = meta::samplerSlices(data.get());
  std::erase_if(slices, [&](float f) { return !(f >= 0.0f) || f >= static_cast<float>(ref->frames); });
  std::sort(slices.begin(), slices.end());
  if (slices.empty()) return invalid("the Sampler has no slices");
  const meta::Type& t = meta::samplerType();
  const DeviceValue* warpSetting = s.device->setting("warp");
  const auto warp = static_cast<meta::SamplerWarp>(
      std::lround(warpSetting != nullptr ? warpSetting->value : t.settings[meta::kSmpWarp].def));
  // Where slice frame f starts, in ticks after `at`: content beats (warp on: through the markers / sampleBpm), else the
  // seconds since the first slice through the tempo map from `at`.
  dsp::WarpSource src;
  const std::vector<float> markers = meta::samplerMarkers(data.get());
  src.fileRate = ref->rate;
  src.markers = markers.size() >= 2 ? markers.data() : nullptr;
  src.markerCount = static_cast<uint32_t>(markers.size() / 2);
  const double fpb = 60.0 * ref->rate / std::max(1.0, metaParamValue(*s.device, "", t, meta::kSmpSampleBpm));
  const TempoMap& tm = p.timeline->tempoMap;
  const double atSec = tm.ticksToSeconds(at);
  const double f0 = slices.front(), q0 = dsp::warpBeatOfFrame(src, f0, fpb);
  const auto tickOf = [&](double f) -> Ticks {
    if (warp != meta::SamplerWarp::Off) return static_cast<Ticks>(std::llround((dsp::warpBeatOfFrame(src, f, fpb) - q0) * kPpq));
    return static_cast<Ticks>(std::llround(tm.secondsToTicks(atSec + (f - f0) / ref->rate))) - at;
  };
  const std::optional<TrackPath> path = findTrack(p, s.track->id);
  const DegreeKeyMap keys(path ? effectiveTuning(p, *path) : nullptr);
  CapturedClip clip;
  clip.track = s.track->id;
  clip.start = at;
  clip.name = "Slices";
  for (size_t i = 0; i < slices.size(); ++i) {
    const std::optional<int64_t> deg = keys.degreeOf(kSliceKey0 + static_cast<int64_t>(i));
    if (!deg) return invalid("the track's tuning maps no degree to key " + std::to_string(kSliceKey0 + static_cast<int64_t>(i)));
    const Ticks a = tickOf(slices[i]);
    const Ticks b = tickOf(i + 1 < slices.size() ? slices[i + 1] : static_cast<double>(ref->frames));
    Note n;
    n.start = a;
    n.length = std::max<Ticks>(1, b - a);
    n.pitch = tuning::PitchId::fromDegree(static_cast<int32_t>(*deg), 0);
    n.id = ids.note();
    clip.notes.push_back(n);
  }
  clip.length = std::max<Ticks>(kPpq, clip.notes.back().start + clip.notes.back().length);
  Result<Edit> cap = captureClip(p, ids, std::span<const CapturedClip>(&clip, 1));
  if (!cap) return std::move(cap).error();
  Edit e = makeEdit("Slice to Notes");
  e.clips = cap->clips;
  e.tracks.push_back(s.track->id);
  for (Command& c : batchOf(*cap).commands) batchOf(e).commands.push_back(std::move(c));
  return e;
}

}  // namespace pq::doc::ops
