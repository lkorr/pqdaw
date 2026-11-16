#include "pq/views/live_modulation.h"

#include <algorithm>
#include <cmath>

#include "pq/document/racks.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/view_context.h"

namespace pq::views {

namespace {
// A macro's mapping (a Travel wire from a macro's value block) is the macro's knob, not modulation (as the rings).
bool macroMapping(const doc::Project& p, const doc::Wire& w) {
  if (!w.travels() || w.fromNoteSource()) return false;
  const doc::Device* src = doc::findDevice(p, w.from).device;
  return src != nullptr && doc::isValueDevice(*src) && src->setting(doc::kMacroSetting) != nullptr;
}
}  // namespace

void LiveModulation::syncTargets() const {
  const doc::Snapshot& s = ctx_.doc();
  const doc::WireList* wl = s ? &doc::wiresOf(*s) : nullptr;
  if (wl == wires_) return;
  wires_ = wl;
  held_ = s;
  targets_.clear();
  query_.clear();
  if (wl == nullptr) return;
  for (const doc::Wire& w : *wl) {
    if (w.param.empty() || w.toMixer() || macroMapping(*s, w)) continue;
    const uint8_t law = w.travels() ? 0 : w.law == doc::WireLaw::Units ? 2 : 1;
    size_t i = 0;
    if (const Target* t = find(w.to, w.param, &i); t != nullptr) {
      targets_[i].law = std::max(targets_[i].law, law);
      continue;
    }
    targets_.push_back({w.to, w.param, law});
    query_.push_back({w.to, w.param});
  }
}

const LiveModulation::Target* LiveModulation::find(doc::DeviceId device, std::string_view param, size_t* index) const {
  for (size_t i = 0; i < targets_.size(); ++i)
    if (targets_[i].device == device && targets_[i].param == param) {
      if (index != nullptr) *index = i;
      return &targets_[i];
    }
  return nullptr;
}

uint64_t LiveModulation::refresh() {
  ModulationSource* src = ctx_.modulationSource();
  syncTargets();
  if (valuesFor_ != wires_ || values_.size() != targets_.size()) {  // a new wire list: the new targets' (read below)
    valuesFor_ = wires_;
    values_.assign(targets_.size(), ModSweep{});
    lastRead_ = -1.0;
    ++serial_;
  }
  if (src == nullptr || targets_.empty()) {
    if (lit_) {
      lit_ = false;
      std::fill(values_.begin(), values_.end(), ModSweep{});
      ++serial_;
    }
    return serial_;
  }
  const double t = MetaFace::clockNow();
  if (lastRead_ >= 0.0 && t - lastRead_ < kFrameSeconds) return serial_;  // this frame's read
  if (!src->modulationLive() && !lit_) return serial_;  // idle: nothing to read
  lastRead_ = t;
  ++reads_;
  std::vector<ModSweep> fresh(targets_.size());
  src->readModulationNow(query_, fresh);
  bool changed = false, lit = false;
  for (size_t i = 0; i < fresh.size(); ++i) {
    changed = changed || fresh[i].known != values_[i].known || fresh[i].now != values_[i].now;
    lit = lit || (fresh[i].known && fresh[i].now != 0.0f);
  }
  lit_ = lit;
  if (changed) {
    values_ = std::move(fresh);
    ++serial_;
  }
  return serial_;
}

bool LiveModulation::live() const {
  const ModulationSource* src = ctx_.modulationSource();
  return src != nullptr && (lit_ || src->modulationLive()) && !targets_.empty();
}

bool LiveModulation::modulated(doc::DeviceId device, std::string_view param) const {
  syncTargets();
  return find(device, param) != nullptr;
}

std::optional<LiveModulation::Now> LiveModulation::now(doc::DeviceId device, std::string_view param) const {
  syncTargets();
  size_t i = 0;
  const Target* t = find(device, param, &i);
  if (t == nullptr || valuesFor_ != wires_ || i >= values_.size() || !values_[i].known || values_[i].now == 0.0f) return std::nullopt;
  return Now{values_[i].now, t->law};
}

std::optional<double> LiveModulation::ghostPosition(doc::DeviceId device, std::string_view param, double pos,
                                                    const dsp::Curve& curve, const devices::ParamInfo* info) const {
  const std::optional<Now> n = now(device, param);
  if (!n) return std::nullopt;
  const double d = static_cast<double>(n->delta);
  double g = pos;
  // The tap's domain (lower.h "Modulation wires"; DeviceChainView's rings convert the same way).
  if (n->law == 2 && info != nullptr) {  // migrated Units wires: the DSP units
    devices::ParamInfo pi = *info;
    pi.curve = curve;
    g = devices::toPosition(pi, devices::fromPosition(pi, pos) + d, pos);
  } else if (n->law >= 1 && doc::addsInUnits(curve)) {  // Add wires on a linear curve: the curve's units
    g = dsp::curvePosition(curve, dsp::curveOutput(curve, dsp::evalCurveUnits(curve, pos) + d), pos);
  } else {  // the knob's position
    g = pos + d;
  }
  if (!std::isfinite(g)) return std::nullopt;
  g = std::clamp(g, 0.0, 1.0);
  if (std::fabs(g - pos) < 1e-5) return std::nullopt;
  return g;
}

}  // namespace pq::views
