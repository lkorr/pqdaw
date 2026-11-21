// What faces bind to: a document instance, synthetic voices (TMM.4). See pq/views/meta/face_targets.h.

#include "pq/views/meta/face_targets.h"

#include "pq/views/live_modulation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/meta/registry.h"
#include "pq/document/meta.h"
#include "pq/dsp/curve.h"
#include "pq/tuning/tuning.h"
#include "pq/meta/sampler.h"
#include "pq/views/piano_roll/row_model.h"
#include "pq/views/sample_analysis.h"
#include "pq/views/view_context.h"
#include "pq/views/waveforms.h"

namespace pq::views {

const meta::Type* metaTypeAt(const ViewContext& ctx, doc::DeviceId device, std::string_view path) {
  const doc::Device* d = doc::findDevice(*ctx.doc(), device).device;
  if (d == nullptr) return nullptr;
  if (path.empty()) return meta::findType(doc::metaDeviceType(*d));
  // GU.11: a native engine is the only meta instance (path ""); a stored nested state names its type.
  if (const doc::MetaInstance* inst = d->metaAt(path)) return meta::findType(inst->type);
  return nullptr;
}

DocTarget::~DocTarget() {
  if (gesture_) (void)ctx_.store().commitGesture();
}

const doc::Device* DocTarget::dev() const {
  const doc::Snapshot& now = ctx_.doc();
  if (now != seen_) {
    seen_ = now;
    seenDevice_ = now ? doc::findDevice(*now, device_).device : nullptr;
  }
  return seenDevice_;
}

double DocTarget::value(size_t param) const {
  if (param >= type_.params.size()) return 0.0;
  const doc::Device* d = dev();
  if (d == nullptr) return type_.params[param].def;
  for (const auto& [k, pos] : shown_)  // TL.6: an automated param at the playhead (a position: through its curve)
    if (k == param) return dsp::evalCurve(doc::metaParamCurve(*d, path_, type_, param), pos);
  return doc::metaParamValue(*d, path_, type_, param);
}

bool DocTarget::showPosition(size_t param, std::optional<double> position) {
  for (auto it = shown_.begin(); it != shown_.end(); ++it)
    if (it->first == param) {
      if (position && it->second == *position) return false;
      if (position) it->second = *position;
      else shown_.erase(it);
      return true;
    }
  if (!position || param >= type_.params.size()) return false;
  shown_.emplace_back(param, *position);
  return true;
}

std::optional<double> DocTarget::ghostPosition(size_t param) const {
  if (!path_.empty() || param >= type_.params.size()) return std::nullopt;
  const doc::Device* d = dev();
  if (d == nullptr) return std::nullopt;
  const LiveModulation& lm = ctx_.liveModulation();
  const std::string_view name = type_.params[param].name;
  if (!lm.now(device_, name)) return std::nullopt;
  const double pos = faceValueToTravel(*this, type_, param, value(param));
  return lm.ghostPosition(device_, name, pos, doc::metaParamCurve(*d, path_, type_, param));
}

bool DocTarget::modulated(size_t param) const {
  return path_.empty() && param < type_.params.size() && ctx_.liveModulation().modulated(device_, type_.params[param].name);
}

uint64_t DocTarget::modulationSerial() { return ctx_.liveModulation().refresh(); }
bool DocTarget::modulationLive() const { return ctx_.liveModulation().live(); }

bool DocTarget::editable() const noexcept { return dev() != nullptr; }

void DocTarget::begin(const char* label) {
  if (gesture_) return;
  gesture_ = ctx_.store().beginGesture(label).ok();
}

void DocTarget::set(size_t param, double value) {
  if (param >= type_.params.size()) return;
  auto e = doc::ops::setMetaParamValue(*ctx_.doc(), device_, path_, type_.id, std::string(type_.params[param].name), value);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return;
  }
  if (Status s = ctx_.store().dispatch(e->command); !s) ctx_.setStatus(s.error().message);
}

void DocTarget::end() {
  if (!gesture_) return;
  gesture_ = false;
  (void)ctx_.store().commitGesture();
}

void DocTarget::cancel() {
  if (!gesture_) return;
  gesture_ = false;
  (void)ctx_.store().cancelGesture();
}

void DocTarget::reset(size_t param) {
  if (param >= type_.params.size()) return;
  auto e = doc::ops::setMetaParam(*ctx_.doc(), device_, path_, type_.id, std::string(type_.params[param].name), std::nullopt);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return;
  }
  if (Status s = ctx_.store().dispatch(e->command); !s) ctx_.setStatus(s.error().message);
}

double DocTarget::setting(size_t i) const {
  if (i >= type_.settings.size()) return 0.0;
  const doc::Device* d = dev();
  if (d == nullptr) return type_.settings[i].def;
  const std::vector<double> s = doc::metaSettings(*d, path_, type_);
  return i < s.size() ? s[i] : type_.settings[i].def;
}

void DocTarget::setSetting(size_t i, double value) {
  if (i >= type_.settings.size()) return;
  auto e = doc::ops::setMetaSetting(*ctx_.doc(), device_, path_, type_.id, std::string(type_.settings[i].name), value);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return;
  }
  if (Status s = ctx_.store().dispatch(e->command); !s) ctx_.setStatus(s.error().message);
}

meta::DataPtr DocTarget::data() const {
  const doc::Device* d = dev();
  const doc::MetaInstance* inst = d != nullptr ? d->metaAt(path_) : nullptr;
  if (inst != nullptr && inst->data) return inst->data;
  return MetaTarget::data();  // the type's default (one pointer per type)
}

void DocTarget::setData(meta::DataPtr data) {
  auto e = doc::ops::setMetaData(*ctx_.doc(), device_, path_, type_.id, std::move(data));
  if (!e) {
    ctx_.setStatus(e.error().message);
    return;
  }
  if (Status s = ctx_.store().dispatch(e->command); !s) ctx_.setStatus(s.error().message);
}

// TX.spectrogram: the effective tuning of the device's track, its degrees resolved over every equave that reaches the
// range (a few hundred at most: a 53-EDO over ten octaves).
void DocTarget::pitchGuides(float loHz, float hiHz, std::vector<PitchGuide>& out) const {
  out.clear();
  const doc::Project& p = *ctx_.doc();
  const doc::DeviceLoc loc = doc::findDevice(p, device_);
  if (loc.track == nullptr || !(hiHz > loHz)) return;
  const std::optional<doc::TrackPath> path = doc::findTrack(p, loc.track->id);
  if (!path) return;
  const doc::TuningDef* def = doc::effectiveTuning(p, *path);
  if (def == nullptr) return;
  auto t = doc::buildTuning(*def);
  if (!t) return;
  const tuning::Tuning& tu = *t;
  int32_t size = 0;
  if (const tuning::Edo* e = tu.edo()) size = static_cast<int32_t>(e->n());
  else if (const tuning::Mos* m = tu.mos()) size = m->size();
  else if (const tuning::Scale* s = tu.scale()) size = s->size();
  if (size <= 0 || size > 1200) return;
  constexpr size_t kMaxGuides = 2048;
  for (int32_t eq = -16; eq <= 16 && out.size() < kMaxGuides; ++eq)
    for (int32_t d = 0; d < size && out.size() < kMaxGuides; ++d) {
      const std::optional<double> hz = tuning::resolveHzDouble(tuning::PitchId::fromDegree(d, eq), tu, {});
      if (!hz || *hz < loHz || *hz > hiHz) continue;
      out.push_back({static_cast<float>(*hz), d, eq});
    }
  std::sort(out.begin(), out.end(), [](const PitchGuide& a, const PitchGuide& b) { return a.hz < b.hz; });
}

// SM.11: a degree's name as the piano roll's keys print it (pq/views/piano_roll/row_model.h RowLabels::Auto): C-style
// names on a 12-compatible octave EDO (the reference's octave is 3, as Live counts), else the row label and the equave
// (octave EDOs counted as Live's octaves, other equaves from 0).
std::string DocTarget::pitchName(const PitchGuide& g) const {
  const doc::Project& p = *ctx_.doc();
  const doc::DeviceLoc loc = doc::findDevice(p, device_);
  if (loc.track == nullptr) return {};
  const std::optional<doc::TrackPath> path = doc::findTrack(p, loc.track->id);
  const doc::TuningDef* def = path ? doc::effectiveTuning(p, *path) : nullptr;
  if (def == nullptr) return {};
  auto t = doc::buildTuning(*def);
  if (!t) return {};
  const tuning::Tuning& tu = *t;
  int32_t size = 0;
  bool octave = false;
  if (const tuning::Edo* e = tu.edo()) {
    size = static_cast<int32_t>(e->n());
    octave = std::fabs(e->equaveCents() - 1200.0) < 1e-6;
  } else if (const tuning::Mos* m = tu.mos()) {
    size = m->size();
  } else if (const tuning::Scale* s = tu.scale()) {
    size = s->size();
  }
  if (size <= 0) return {};
  const roll::RowLabels mode = roll::resolveRowLabels(roll::RowLabels::Auto, tu);
  const int64_t d = static_cast<int64_t>(g.equave) * size + g.degree;
  const int64_t eq = octave ? g.equave + 3 : g.equave;
  char buf[48];
  if (mode == roll::RowLabels::Name && octave && size % 12 == 0 && g.degree % (size / 12) == 0) {
    static constexpr const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    std::snprintf(buf, sizeof buf, "%s%lld", kNames[g.degree / (size / 12)], static_cast<long long>(eq));
    return buf;
  }
  char lbuf[24];
  const size_t n = roll::formatRowLabel(mode == roll::RowLabels::Name ? roll::RowLabels::Degree : mode, tu, d, lbuf);
  std::snprintf(buf, sizeof buf, "%.*s %lld", static_cast<int>(n), lbuf, static_cast<long long>(eq));
  return buf;
}

// SM.12: the slicing through the document (one step, or the running gesture's). Transient needs the sample's analysis:
// while it is not there yet the request is made and the notice says so.
bool DocTarget::sliceSample(int by, double amount) {
  doc::ops::SliceSpec spec;
  spec.by = static_cast<doc::ops::SliceBy>(std::clamp(by, 0, 3));
  spec.sensitivity = std::clamp(amount, 0.0, 1.0);
  spec.beats = amount;
  spec.count = static_cast<uint32_t>(std::clamp(std::lround(amount), 1L, static_cast<long>(doc::ops::kMaxSlices)));
  const doc::Snapshot& p = ctx_.doc();
  std::vector<double> onsets;
  std::vector<float> strengths;
  if (spec.by == doc::ops::SliceBy::Transient) {
    const doc::Device* d = dev();
    const doc::MetaInstance* m = d != nullptr ? d->metaAt(path_) : nullptr;
    const uint32_t sample = m != nullptr ? meta::samplerSample(m->data.get()) : 0;
    const SampleAnalysisResult* a = sample != 0 ? ctx_.sampleAnalysis().get(doc::SampleId{sample}) : nullptr;
    if (a == nullptr || a->transients == nullptr) {
      ctx_.setStatus(sample != 0 ? "The sample is still being analysed: slice again in a moment" : "No sample to slice");
      return false;
    }
    onsets = a->transients->onsets;
    strengths = a->transients->strengths;
  }
  auto e = doc::ops::sliceSampler(*p, device_, spec, onsets, strengths);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return false;
  }
  if (Status s = ctx_.store().dispatch(e->command); !s) {
    ctx_.setStatus(s.error().message);
    return false;
  }
  return true;
}

bool DocTarget::sliceToNotes() {
  auto e = doc::ops::sliceToNotes(*ctx_.doc(), ctx_.store().ids(), device_, ctx_.startMarker());
  if (!e) {
    ctx_.setStatus(e.error().message);
    return false;
  }
  if (Status s = ctx_.store().dispatch(e->command); !s) {
    ctx_.setStatus(s.error().message);
    return false;
  }
  return true;
}

const dsp::Curve* DocTarget::curve(size_t param) const {  // GU.1
  if (!path_.empty() || param >= type_.params.size()) return nullptr;
  const doc::Device* d = dev();
  return d != nullptr ? d->curve(type_.params[param].name) : nullptr;
}

TonePort* DocTarget::tonePort() const { return ctx_.tonePort(); }

const media::WaveformOverview* DocTarget::sampleOverview(uint32_t sample) const {
  return sample != 0 ? ctx_.waveforms().overview(doc::SampleId{sample}) : nullptr;
}
uint64_t DocTarget::overviewRevision() const { return ctx_.waveforms().revision(); }

uint64_t DocTarget::guidesKey() const { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ctx_.doc().get())); }

void DocTarget::notice(std::string_view message) { ctx_.setStatus(std::string(message)); }

// ---- synthetic voices -------------------------------------------------------------------------------------------

void SyntheticVoices::setVoices(std::vector<Voice> v) {
  voices_ = std::move(v);
  pending_.assign(voices_.size(), {});
  for (size_t i = 0; i < voices_.size(); ++i) pending_[i].push_back(static_cast<float>(voices_[i].phase));
  ++serial_;
}

void SyntheticVoices::advance(double seconds) {
  if (voices_.empty() || seconds <= 0.0) return;
  constexpr double kRate = 48000.0, kBlock = 64.0, kPerBlock = 8.0;
  const double step = kBlock / kPerBlock / kRate;  // seconds between two sampled phases
  const auto samples = static_cast<uint32_t>(std::max(1.0, std::round(seconds / step)));
  for (size_t i = 0; i < voices_.size(); ++i) {
    Voice& v = voices_[i];
    for (uint32_t k = 0; k < samples; ++k) {
      v.phase += v.hz * step;
      v.phase -= std::floor(v.phase);
      pending_[i].push_back(static_cast<float>(v.phase));
    }
    if (pending_[i].size() > FaceVoice::kMaxSamples)
      pending_[i].erase(pending_[i].begin(), pending_[i].end() - FaceVoice::kMaxSamples);
  }
  ++serial_;
}

void SyntheticVoices::freeze() {
  for (Voice& v : voices_) v.hz = 0.0;
  ++serial_;
}

bool SyntheticVoices::live() const noexcept {
  return std::any_of(voices_.begin(), voices_.end(), [](const Voice& v) { return v.hz != 0.0; });
}

uint32_t SyntheticVoices::read(std::span<FaceVoice> out) {
  const auto n = static_cast<uint32_t>(std::min(out.size(), voices_.size()));
  for (uint32_t i = 0; i < n; ++i) {
    FaceVoice& f = out[i];
    f.slot = voices_[i].slot;
    f.phase = static_cast<float>(voices_[i].phase);
    f.position = static_cast<float>(voices_[i].position);
    f.increment = static_cast<float>(voices_[i].increment);
    f.rate = static_cast<float>(rate_);
    f.sampleCount = static_cast<uint32_t>(std::min<size_t>(pending_[i].size(), FaceVoice::kMaxSamples));
    std::copy_n(pending_[i].end() - f.sampleCount, f.sampleCount, f.samples.begin());
    pending_[i].clear();
  }
  return n;
}

}  // namespace pq::views
