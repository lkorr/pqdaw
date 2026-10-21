#include "pq/tuning/interval_names.h"

#include <algorithm>

namespace pq::tuning {

namespace {

std::string_view trainerLookup(int64_t num, int64_t den) noexcept {
  for (const TrainerIntervalName& t : trainerIntervalNames())  // 68 entries: a linear scan is fine for UI labels
    if (t.num == num && t.den == den) return t.name;
  return {};
}

}  // namespace

std::string trainerIntervalName(int64_t num, int64_t denom) {
  // JS: const ratio = num / denom; if (ratio < 1) { baseName = NAMES[`${denom}/${num}`] || NAMES[key] || '';
  //     if (baseName) return `DOWN a ${baseName}`; } return NAMES[key] || '';
  const double ratio = static_cast<double>(num) / static_cast<double>(denom);
  if (ratio < 1.0) {
    std::string_view base = trainerLookup(denom, num);
    if (base.empty()) base = trainerLookup(num, denom);
    if (!base.empty()) return "DOWN a " + std::string(base);
  }
  return std::string(trainerLookup(num, denom));
}

const JiIntervalInfo* findJiInterval(const Rational& r) noexcept {  // RT-safe
  const auto g = jiIntervalGallery();
  const auto it = std::lower_bound(g.begin(), g.end(), r,
                                   [](const JiIntervalInfo& e, const Rational& x) { return e.ratio() < x; });
  if (it == g.end() || it->ratio() != r) return nullptr;
  return &*it;
}

}  // namespace pq::tuning
