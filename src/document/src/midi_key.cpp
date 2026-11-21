// SM.1: a note's MIDI key (pq/document/midi_key.h describes it).

#include "pq/document/midi_key.h"

#include <algorithm>
#include <cmath>

#include "pq/document/model.h"
#include "pq/tuning/scala.h"

namespace pq::doc {

DegreeKeyMap::DegreeKeyMap(const TuningDef* def) {
  const ScaleDef* s = def ? def->scale() : nullptr;
  if (!s || !s->mapping) return;
  kbm_ = true;
  keyOf_.reserve(128);
  for (int32_t k = 0; k <= 127; ++k)
    if (const std::optional<int64_t> d = tuning::mapKeyToDegree(*s->mapping, k)) keyOf_.emplace_back(*d, static_cast<int16_t>(k));
  // Sorted by degree, the lowest key first; then one entry per degree.
  std::stable_sort(keyOf_.begin(), keyOf_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  keyOf_.erase(std::unique(keyOf_.begin(), keyOf_.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
               keyOf_.end());
}

int64_t DegreeKeyMap::key(int64_t absDegree) const noexcept {
  if (!kbm_) return 60 + absDegree;
  const auto it = std::lower_bound(keyOf_.begin(), keyOf_.end(), absDegree,
                                   [](const std::pair<int64_t, int16_t>& e, int64_t d) { return e.first < d; });
  return it != keyOf_.end() && it->first == absDegree ? it->second : int64_t{tuning::kNoMidiKey};
}

int16_t midiKeyOf(const tuning::PitchId& id, std::optional<double> keyDegree, double hz, const DegreeKeyMap& keys) noexcept {
  if (id.kind() == tuning::PitchKind::Degree) {
    if (!keyDegree || !(std::fabs(*keyDegree) < 1e9)) return tuning::kNoMidiKey;
    return tuning::midiKeyOrNone(keys.key(std::llround(*keyDegree)));
  }
  return tuning::nearestMidiKeyOrNone(hz);
}

}  // namespace pq::doc
