#include "pq/tuning/mos_data.h"

#include <algorithm>
#include <cmath>

#include "mos_data_tables.h"

namespace pq::tuning {

using namespace detail;

std::span<const Temperament> temperaments() noexcept { return {kTemperaments, kTemperamentCount}; }

std::span<const NamedColor> temperamentColors() noexcept { return {kTemperamentColors, kTemperamentColorCount}; }

std::optional<std::string_view> temperamentColor(std::string_view name) noexcept {
  for (const NamedColor& c : temperamentColors())
    if (c.name == name) return c.color;
  return std::nullopt;
}

std::span<const MosNameEntry> mosNameTable() noexcept { return {kMosNames, kMosNameCount}; }
std::span<const MosNameEntry> multiPeriodMosNameTable() noexcept { return {kMultiPeriodMosNames, kMultiPeriodMosNameCount}; }
std::span<const ModeNameEntry> modeNameTable() noexcept { return {kModeNames, kModeNameCount}; }

namespace {
std::optional<std::string_view> findName(std::span<const MosNameEntry> table, MosKey k) noexcept {
  for (const MosNameEntry& e : table)
    if (e.key == k) return e.name;
  return std::nullopt;
}
}  // namespace

std::optional<std::string_view> mosName(MosKey k) noexcept { return findName(mosNameTable(), k); }
std::optional<std::string_view> multiPeriodMosName(MosKey k) noexcept { return findName(multiPeriodMosNameTable(), k); }

std::span<const std::string_view> modeNames(MosKey k) noexcept {
  for (const ModeNameEntry& e : modeNameTable())
    if (e.key == k) return e.names;
  return {};
}

std::optional<std::string_view> mosDisplayName(MosKey k, int32_t periodsPerEquave, double period) noexcept {
  if (periodsPerEquave <= 1) return mosName(k);
  if (period < 1200.0) return multiPeriodMosName(k);
  return std::nullopt;
}

std::optional<std::string_view> modeName(MosKey key, double genCents, int32_t modeRotation, double period,
                                         const RegionHint* sel) {
  const std::span<const std::string_view> modes = modeNames(key);
  if (modes.empty()) return std::nullopt;
  const int32_t n = key.size();
  if (!(n > 1)) return std::nullopt;
  const std::vector<int32_t> chain = generatorChain(genCents, n, period, sel);
  const int32_t root = chain[static_cast<size_t>(((modeRotation % n) + n) % n)];
  const bool dark = !isBrightGenerator(genCents, n, period, sel);
  const int32_t idx = dark ? (n - 1 - root) : root;
  if (idx < static_cast<int32_t>(modes.size())) return modes[static_cast<size_t>(idx)];
  return std::nullopt;
}

std::vector<TemperamentMatch> temperamentsForMos(MosKey key, double period, std::optional<CentsRange> region) {
  std::vector<TemperamentMatch> out;
  const std::span<const Temperament> all = temperaments();
  for (size_t i = 0; i < all.size(); ++i) {
    const Temperament& t = all[i];
    if (t.disabled) continue;
    if (std::fabs(t.period - period) > 0.01) continue;
    const bool directMatch = std::any_of(t.mos.begin(), t.mos.end(),
                                         [&](MosKey m) { return m == key || isDescendantMos(key, m); });
    const bool ancestorMatch =
        !directMatch && std::any_of(t.mos.begin(), t.mos.end(), [&](MosKey m) { return isDescendantMos(m, key); });
    const double mirrorLow = t.period - t.genHigh, mirrorHigh = t.period - t.genLow;
    const bool directOverlap =
        !region ? (directMatch || ancestorMatch) : (t.genLow < region->high && t.genHigh > region->low);
    const bool mirrorOverlap = region && mirrorLow < region->high && mirrorHigh > region->low;
    if (directOverlap) out.push_back({static_cast<int32_t>(i), t.genLow, t.genHigh});
    if (mirrorOverlap) out.push_back({static_cast<int32_t>(i), mirrorLow, mirrorHigh});
  }
  return out;
}

std::vector<TemperamentMatch> temperamentsAtCursor(MosKey key, double cents, double period, CentsRange region) {
  std::vector<TemperamentMatch> all = temperamentsForMos(key, period, region);
  std::erase_if(all, [&](const TemperamentMatch& m) { return !(cents >= m.low && cents <= m.high); });
  return all;
}

}  // namespace pq::tuning
