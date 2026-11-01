// What faces bind to: a document instance, synthetic voices (TMM.4). See pq/views/meta/face_targets.h.

#include "pq/views/meta/face_targets.h"

#include <algorithm>
#include <cmath>

#include "pq/document/edit_ops.h"
#include "pq/document/meta.h"
#include "pq/views/view_context.h"

namespace pq::views {

DocTarget::~DocTarget() {
  if (gesture_) (void)ctx_.store().commitGesture();
}

double DocTarget::value(size_t param) const {
  if (param >= type_.params.size()) return 0.0;
  const doc::Device* d = doc::findDevice(*ctx_.doc(), device_).device;
  return d != nullptr ? doc::metaParamValue(*d, path_, type_, param) : type_.params[param].def;
}

bool DocTarget::editable() const noexcept { return doc::findDevice(*ctx_.doc(), device_).device != nullptr; }

void DocTarget::begin(const char* label) {
  if (gesture_) return;
  gesture_ = ctx_.store().beginGesture(label).ok();
}

void DocTarget::set(size_t param, double value) {
  if (param >= type_.params.size()) return;
  auto e = doc::ops::setMetaParam(*ctx_.doc(), device_, path_, type_.id, std::string(type_.params[param].name), value);
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
  const doc::Device* d = doc::findDevice(*ctx_.doc(), device_).device;
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
  const doc::Device* d = doc::findDevice(*ctx_.doc(), device_).device;
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
    f.sampleCount = static_cast<uint32_t>(std::min<size_t>(pending_[i].size(), FaceVoice::kMaxSamples));
    std::copy_n(pending_[i].end() - f.sampleCount, f.sampleCount, f.samples.begin());
    pending_[i].clear();
  }
  return n;
}

}  // namespace pq::views
