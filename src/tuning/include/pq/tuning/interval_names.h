#pragma once

// Interval naming tables ported from the JI composition trainer (T2.5):
//   - the trainer's INTERVAL_NAMES table (data/intervalNames.js) and its lookup getIntervalName (js/utils.js),
//     ported exactly ("DOWN a " prefix for descending intervals included);
//   - the interval gallery in "interval names.txt" (a copy of the Xenharmonic wiki "Gallery of just intervals"): per
//     ratio its colour-notation abbreviation and name, FJS name and common names. (The text's cents column is not
//     copied: cents(ratio()) is exact, and six listed values are typos; the generator validates the ratios with it.)
// Both tables are generated constant data (tools/fixtures/gen_ji_names_cpp.mjs → src/interval_names_tables.cpp).
//
// FJS caveat: the source text lost the FJS superscripts/subscripts when it was copied from the wiki, so an FJS name is
// stored as flat text, e.g. "M35" for M3^5 (5/4) and "m35" for m3_5 (6/5). The prime accidentals are the digits after
// the degree number; whether they are otonal or utonal is not recoverable from the text.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "pq/tuning/rational.h"

namespace pq::tuning {

// One INTERVAL_NAMES entry ("num/den" → name), in the JS object's property order.
struct TrainerIntervalName {
  int64_t num;
  int64_t den;
  std::string_view name;
};

std::span<const TrainerIntervalName> trainerIntervalNames() noexcept;

// Port of getIntervalName(num, denom): the name of the key "num/denom" exactly as written (so an unreduced 6/4 has
// no name, as in the JS); for num/denom < 1 the name of the reciprocal key (else of the key itself) prefixed with
// "DOWN a ". Empty string if there is no name.
std::string trainerIntervalName(int64_t num, int64_t denom);

// One gallery row. Strings are UTF-8 (the common names use ⟩ in monzos).
struct JiIntervalInfo {
  int64_t num;
  int64_t den;
  std::string_view colourAbbrev;  // e.g. "zg2"; alternatives joined with ", " (e.g. "w11, cw4")
  std::string_view colourName;    // e.g. "zogu 2nd"; empty for the few rows that list only the abbreviation
  std::string_view fjs;           // flat text, see the FJS caveat above; may be empty
  std::string_view commonNames;   // comma-separated; may be empty

  Rational ratio() const noexcept { return Rational::fromReduced(num, den); }
};

// All gallery rows, ascending by ratio (no duplicates).
std::span<const JiIntervalInfo> jiIntervalGallery() noexcept;

// The gallery row for exactly this ratio, or nullptr. Binary search on exact 128-bit cross products. RT-safe.
const JiIntervalInfo* findJiInterval(const Rational& r) noexcept;

}  // namespace pq::tuning
