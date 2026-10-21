#pragma once

// The mos-explorer's data tables (mos-data.js): temperament presets with their generator ranges, MOS family names
// and established mode names. The tables are generated from the JS by tools/fixtures/gen_mos_data_cpp.mjs and are
// plain constant data (no dynamic initialisation).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "pq/tuning/mos.h"

namespace pq::tuning {

struct Temperament {
  std::string_view name;
  std::span<const MosKey> mos;                 // MOS families it is tagged with
  double genLow = 0.0;                         // generator range in cents (genRange)
  double genHigh = 0.0;
  double period = 1200.0;                      // cents; 1200 when the source gives none
  bool hasPeriod = false;                      // the source states a period
  std::span<const std::string_view> commas;    // "81/80", …
  std::span<const std::string_view> commaNames;
  std::string_view wikiUrl;                    // empty if none
  std::string_view description;                // HTML fragment, as in the source
  bool disabled = false;
};

std::span<const Temperament> temperaments() noexcept;

// TEMP_COLORS: "#rrggbb" or "#rrggbbaa" for a temperament name (the table also names a few presets that are
// commented out in the source), or nullopt.
std::optional<std::string_view> temperamentColor(std::string_view name) noexcept;
struct NamedColor {
  std::string_view name;
  std::string_view color;
};
std::span<const NamedColor> temperamentColors() noexcept;

// MOS_NAMES / MULTI_PERIOD_MOS_NAMES / MODE_NAMES lookups.
std::optional<std::string_view> mosName(MosKey k) noexcept;
std::optional<std::string_view> multiPeriodMosName(MosKey k) noexcept;
std::span<const std::string_view> modeNames(MosKey k) noexcept;  // brightest first; empty if none

struct MosNameEntry {
  MosKey key;
  std::string_view name;
};
struct ModeNameEntry {
  MosKey key;
  std::span<const std::string_view> names;
};
std::span<const MosNameEntry> mosNameTable() noexcept;
std::span<const MosNameEntry> multiPeriodMosNameTable() noexcept;
std::span<const ModeNameEntry> modeNameTable() noexcept;

// mosDisplayName: the family name for a single-period scale, the multi-period name for a sub-octave period, else none.
std::optional<std::string_view> mosDisplayName(MosKey k, int32_t periodsPerEquave, double period) noexcept;

// getModeName: the established name of mode `modeRotation` of `key` at this generator, or nullopt.
std::optional<std::string_view> modeName(MosKey key, double genCents, int32_t modeRotation, double period,
                                         const RegionHint* sel = nullptr);

// getTemperamentsForMOS: presets matching a MOS. With a region, a preset matches when its generator range (or its
// mirror period − range) overlaps the region; without one, when its MOS tags include the key, a descendant or an
// ancestor. A preset can match twice (direct and mirror). Only presets whose period is within 0.01 cents of `period`.
struct TemperamentMatch {
  int32_t index = 0;          // into temperaments()
  double low = 0.0;           // effective generator range
  double high = 0.0;
};
struct CentsRange {
  double low = 0.0;
  double high = 0.0;
};
std::vector<TemperamentMatch> temperamentsForMos(MosKey key, double period, std::optional<CentsRange> region);
// getTemperamentsAtCursor: those whose effective range contains `cents`.
std::vector<TemperamentMatch> temperamentsAtCursor(MosKey key, double cents, double period, CentsRange region);

}  // namespace pq::tuning
