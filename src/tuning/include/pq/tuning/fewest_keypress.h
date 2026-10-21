#pragma once

// Fewest-keypress labelling (SPEC §6.2): for every EDO step offset within ±2 equaves, the ratio reachable with the
// fewest macro-key presses, where each key multiplies the composition by its interval and moves round(edo·log2 r)
// steps. Port of prBuildStepToRatioMap from the JI composition trainer (js/game-pianoroll.js), checked against the
// original under Node (tests/tuning/fixtures/ji_bfs.json). Ported behaviour, including its tie-breaking:
//   - moves are the keys in the given order, skipping a key whose numerator or denominator has a prime factor above
//     the prime limit, a key that moves 0 steps, and a key whose step count an earlier key already has;
//   - breadth-first from 0 steps / 1/1 with a FIFO queue, at most 6 presses, steps limited to [−2·edo, 2·edo];
//     the first ratio found for a step wins (a later one only replaces it with strictly fewer presses).
// Key order matters for ties. The trainer iterates a JS object, whose order is not insertion order when keys are
// digits: jsObjectKeyOrder reproduces it (the default keymap therefore starts with '0' = 29/16).
//
// Deviations: ratios are exact int64 (JS uses doubles, which round silently beyond 2^53; with the default keys the
// largest product, 29^6, is far below that). A move whose product would overflow int64 is skipped. Key ratios are
// Rationals, i.e. reduced (the JS checks the prime limit on the numbers as typed).
//
// Beyond the port, each entry records the step and key it was reached from, so keySequence() gives the actual keys
// to press (SPEC §6.2: "shows the shortest key sequence").
//
// Building allocates (UI / document path, not RT). find() is RT-safe.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/tuning/rational.h"

namespace pq::tuning {

// A macro key and the interval it multiplies the composition by.
struct KeyRatio {
  std::string key;
  Rational ratio;

  friend bool operator==(const KeyRatio&, const KeyRatio&) = default;
};

// The trainer's default keymap (data/keyMappings.js), in insertion order: '1'…'9','0' = 2/1 3/2 5/4 7/4 11/8 13/8
// 17/16 19/16 23/16 29/16, then 'q'…'p' = their reciprocals.
std::vector<KeyRatio> trainerDefaultKeymap();

// The property order of a JS object built by assigning `insertions` in order (as `{...prime, ...reciprocal}` does):
// array-index keys ("0", "17", …: canonical decimal < 2^32 − 1) first in ascending numeric order, then the other keys
// in first-insertion order; a repeated key keeps its first position and takes its last value.
std::vector<KeyRatio> jsObjectKeyOrder(std::span<const KeyRatio> insertions);

// round(edo · log2(r)) with JS Math.round (halves toward +∞), computed as in the JS (double divide, then log2).
int64_t ratioToEdoSteps(const Rational& r, uint32_t edo) noexcept;  // RT-safe

class FewestKeypressMap {
public:
  static constexpr int32_t kMaxPresses = 6;  // the JS search depth cap
  static constexpr int32_t kRangeEquaves = 2;  // steps limited to ±2·edo
  static constexpr uint32_t kMaxEdo = 65536;   // keeps the dense table (4·edo + 1 entries of 32 bytes) ≤ 8 MB

  struct Move {
    int64_t steps;
    Rational ratio;
    uint32_t keyIndex;  // index into the keys passed to build()
  };

  struct Entry {
    Rational ratio{1};
    int32_t presses = -1;   // −1: not reachable
    int32_t fromStep = 0;   // previous step on the found path (entry 0 points to itself)
    int32_t move = -1;      // index into moves() of the last press; −1 for step 0
  };

  // keys in iteration order (use jsObjectKeyOrder to match the trainer); edo ≥ 1; primeLimit as in the JS
  // (prLabelPrimeLimit). Errors: InvalidArgument for edo 0, edo > kMaxEdo or a non-positive key ratio.
  static Result<FewestKeypressMap> build(std::span<const KeyRatio> keys, uint32_t edo, uint64_t primeLimit);

  uint32_t edo() const noexcept { return edo_; }
  int32_t maxRange() const noexcept { return maxRange_; }
  std::span<const Move> moves() const noexcept { return moves_; }

  // The entry for a signed step offset, or nullptr if the step is out of range or unreachable. RT-safe.
  const Entry* find(int64_t step) const noexcept {
    if (step < -maxRange_ || step > maxRange_) return nullptr;
    const Entry& e = entries_[static_cast<size_t>(step + maxRange_)];
    return e.presses < 0 ? nullptr : &e;
  }

  // The JS result string for a step ("n/d"), or "" if the step has no entry.
  std::string ratioLabel(int64_t step) const;
  // The trainer's cursor label: the ratio, else "+k\edo" / "-k\edo".
  std::string cursorLabel(int64_t step) const;
  // Indices (into the keys passed to build) of the keys to press, in order; empty for step 0 or no entry.
  std::vector<uint32_t> keySequence(int64_t step) const;

  // Number of reachable steps (including 0).
  size_t size() const noexcept;

private:
  FewestKeypressMap() = default;

  uint32_t edo_ = 12;
  int32_t maxRange_ = 24;
  std::vector<Move> moves_;
  std::vector<Entry> entries_;  // index step + maxRange_
};

}  // namespace pq::tuning
