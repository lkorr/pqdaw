// Live note input → note identities (T6.1): pq/document/input_map.h.
#include "pq/document/input_map.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "pq/tuning/scala.h"

namespace pq::doc {

namespace {
// Degree identity of absolute degree d in a tuning of `size` degrees per equave (floor division).
std::optional<tuning::PitchId> degreeId(int64_t d, int64_t size) noexcept {
  if (size < 1) return std::nullopt;
  int64_t q = d / size, r = d % size;
  if (r < 0) {
    r += size;
    --q;
  }
  if (q < std::numeric_limits<int32_t>::min() || q > std::numeric_limits<int32_t>::max()) return std::nullopt;
  return tuning::PitchId::fromDegree(static_cast<int32_t>(r), static_cast<int32_t>(q));
}
}  // namespace

tuning::PitchId freezeHz(double hz, double referenceHz) noexcept {
  return tuning::PitchId::fromCents(1200.0 * std::log2(hz / referenceHz));
}

tuning::PitchId freezeHz(double hz, const TuningDef& def) noexcept { return freezeHz(hz, def.referenceHz); }

InputPitch InputPitchMap::resolved(const tuning::PitchId& id) const {
  // The drawn note's resolution (resolveTrackPitches → ClipResolver for an identity naming no note).
  const NotePitch np = resolvePitch(id, *tuning_, signal_);
  InputPitch out;
  out.id = id;
  if (!np.hz) return out;
  out.hz = *np.hz;
  out.follow = np.follow;
  out.signal = np.signal;
  out.degree = np.degree;
  out.equave = np.equave;
  if (np.keyDegree) out.noteDegree = static_cast<float>(*np.keyDegree);
  out.mapped = true;
  return out;
}

InputPitch InputPitchMap::hzPitch(double hz) const {
  if (!(hz > 0.0) || !std::isfinite(hz)) return {};
  return resolved(freezeHz(hz, referenceHz_));
}

InputPitch InputPitchMap::keysPitch(const tuning::PitchId& degree) const {
  const InputPitch k = resolved(degree);
  if (input_.mode == NoteInputMode::Keys || !k.mapped) return k;
  return hzPitch(k.hz);  // Hz mode: that degree's Hz, frozen
}

std::optional<tuning::PitchId> InputPitchMap::nearestDegree(double hz) const {
  const InputPitch c = resolved(freezeHz(hz, referenceHz_));
  if (!c.mapped || std::isnan(c.noteDegree)) return std::nullopt;
  return degreeId(static_cast<int64_t>(std::llround(static_cast<double>(c.noteDegree))), size_);
}

InputPitch InputPitchMap::twelveTet(int64_t key, double bendCents) const {
  const double hz = twelveTetHz(static_cast<double>(key));
  if (input_.mode == NoteInputMode::Hz && input_.keyMap == InputKeyMap::TwelveTetHz)
    return hzPitch(hz * std::exp2(bendCents / 1200.0));
  const std::optional<tuning::PitchId> d = nearestDegree(hz);
  if (!d) return {};
  if (input_.mode == NoteInputMode::Keys) return resolved(*d);
  const InputPitch k = resolved(*d);
  return k.mapped ? hzPitch(k.hz * std::exp2(bendCents / 1200.0)) : k;
}

InputPitch InputPitchMap::atNoteOn(uint8_t key, double bendCents) const {
  const InputPitch& k = at(key);
  if (input_.mode == NoteInputMode::Keys || !k.mapped || bendCents == 0.0 || !std::isfinite(bendCents)) return k;
  // Hz mode: the key's (unbent) Hz with the bend frozen in. A 12-TET key map recomputes from the key (same Hz).
  return hzPitch(k.hz * std::exp2(bendCents / 1200.0));
}

InputPitch InputPitchMap::pitchForDegree(int64_t d) const {
  if (input_.keyMap == InputKeyMap::TwelveTetNearest || input_.keyMap == InputKeyMap::TwelveTetHz) {
    if (d < -(int64_t{1} << 40) || d > (int64_t{1} << 40)) return {};
    return twelveTet(60 + d, 0.0);
  }
  const std::optional<tuning::PitchId> id = degreeId(d, size_);
  return id ? keysPitch(*id) : InputPitch{};
}

Result<InputPitchMapPtr> buildInputPitchMap(const Project& p, TrackId track, const NoteInput& input) {
  const std::optional<TrackPath> path = findTrack(p, track);
  if (!path) return Error{ErrorCode::NotFound, "buildInputPitchMap: no such track", {}};
  if (trackAt(p, *path).kind != TrackKind::Midi) return Error{ErrorCode::InvalidArgument, "only MIDI tracks take note input", {}};
  const EffectiveTuning eff = effectiveTuningRef(p, *path);
  if (eff.def == nullptr) return Error{ErrorCode::NotFound, "buildInputPitchMap: the track's tuning was not found", {}};
  Result<tuning::Tuning> built = buildTuning(*eff.def);
  if (!built) return std::move(built).error();
  auto map = std::make_shared<InputPitchMap>();
  map->input_ = input;
  map->tuning_ = std::make_shared<const tuning::Tuning>(std::move(built.value()));
  map->referenceHz_ = eff.def->referenceHz;
  map->signal_ = eff.main.valid() && hasTuningSignal(*eff.def) ? eff.main : TuningId{};  // as resolveTrackPitches
  map->size_ = degreesPerEquave(*map->tuning_);
  const ScaleDef* scale = eff.def->scale();
  const bool kbm = input.keyMap == InputKeyMap::Tuning && scale != nullptr && scale->mapping.has_value();
  const tuning::KeyboardMapping linear = tuning::KeyboardMapping::linear(60);
  for (uint32_t k = 0; k < InputPitchMap::kKeys; ++k) {
    const auto key = static_cast<int32_t>(k);
    InputPitch& out = map->keys_[k];
    switch (input.keyMap) {
      case InputKeyMap::Tuning:
      case InputKeyMap::Consecutive: {
        // tuning::mapKey: the .kbm (an 'x' key is unmapped), else the linear mapping (degree = key − 60).
        const std::optional<tuning::PitchId> d =
            tuning::mapKey(kbm ? *scale->mapping : linear, key, static_cast<int32_t>(std::min<int64_t>(map->size_, INT32_MAX)));
        out = d ? map->keysPitch(*d) : InputPitch{};
        break;
      }
      case InputKeyMap::TwelveTetNearest:
      case InputKeyMap::TwelveTetHz:
        out = map->twelveTet(key, 0.0);
        break;
    }
  }
  return InputPitchMapPtr(std::move(map));
}

Result<InputPitchMapPtr> buildInputPitchMap(const Project& p, TrackId track) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr) return Error{ErrorCode::NotFound, "buildInputPitchMap: no such track", {}};
  return buildInputPitchMap(p, track, t->noteInput);
}

}  // namespace pq::doc
