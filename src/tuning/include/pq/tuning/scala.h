#pragma once

// Scala scale (.scl) and keyboard mapping (.kbm) files (SPEC §10: import to set tunings, export any tuning).
// Format references: https://www.huygens-fokker.org/scala/scl_format.html and the "Mappings" section of
// https://www.huygens-fokker.org/scala/help.htm#mappings.
//
// .scl reading rules (spec rules first, then the choices this parser makes where the spec is silent):
//   - Lines starting with '!' are comments, anywhere in the file. Lines end with LF, CRLF or a lone CR; a missing
//     final newline is fine; a UTF-8 byte-order mark is skipped.
//   - The first non-comment line is the description (kept verbatim, may be empty, any bytes: the archive uses
//     Latin-1, see descriptionUtf8()).
//   - The next non-comment line holds the note count: an integer ≥ 0 with optional surrounding spaces/tabs.
//   - Then `count` pitch lines. Leading spaces/tabs are ignored; the pitch value is parsed from the start of the
//     text and anything after a valid value and a space / tab (or a '!') is ignored ("100.0 cents", " 5/4   E\").
//     A value followed directly by anything else ("100,5", "1e3", "701.955c") or a ratio written with spaces
//     ("3 / 2") is a read error at that line (T2.6-fix4: they used to read as 100/1, 1/1, 701.955, 3/1).
//       * a value with a period is cents: [+|-]digits.[digits]  ("408.", "-5.0"; no leading period, as in Scala:
//         "floating point values may not start with a decimal point"),
//       * otherwise a ratio: digits[/digits]; a bare integer n is n/1; "10/20" is 1/2. Zero, a zero denominator and
//         negative ratios are read errors.
//   - Choices: blank (whitespace-only) lines after the description are skipped rather than rejected; lines after
//     the `count` pitches are ignored; fewer pitch lines than `count` is an error.
//   - Large ratios: numerators/denominators that do not fit int64 (the spec requires ≥ 2^31−1) are kept exactly as
//     their decimal digits (ScalaPitch::Kind::LargeRatio) with their size in cents, as Scala itself falls back to
//     floating point for them; writing reproduces the digits, so nothing is lost. See docs/QUESTIONS.md Q23.
//
// .kbm: seven header values (map size, first/last key to retune, middle key, reference key, reference frequency,
// formal-octave degree), then up to `map size` entries, each a scale degree (any integer) or 'x' (unmapped). Missing
// trailing entries are unmapped. An unmapped reference key is a read error (Scala treats it as one).
//
// Writers produce canonical files (comment header, description, count, one value per line, CRLF) that parse back
// equal: ratios as reduced n/d, cents in the shortest decimal form that reads back to the same double.
//
// Parsers and writers are non-RT (they allocate). mapKey is RT-safe.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/tuning/pitch_id.h"
#include "pq/tuning/rational.h"

namespace pq::tuning {

// One pitch line of a .scl file (degree 1 and up; degree 0 = 1/1 is implicit).
struct ScalaPitch {
  enum class Kind : uint8_t {
    Ratio,       // exact ratio (a pitch identity: SPEC §3.1)
    Cents,       // cents value as written
    LargeRatio,  // ratio whose numerator or denominator does not fit int64: kept as decimal digits
  };

  Kind kind = Kind::Cents;
  Rational ratio{1};       // Kind::Ratio
  double cents = 0.0;      // always set: the pitch in cents (exact for Cents, computed for the ratio kinds)
  std::string largeNum;    // Kind::LargeRatio: decimal digits, no sign, no leading zeros
  std::string largeDen;    // Kind::LargeRatio

  static ScalaPitch fromRatio(const Rational& r);  // precondition: r > 0
  static ScalaPitch fromCents(double c);           // precondition: finite

  friend bool operator==(const ScalaPitch& a, const ScalaPitch& b) noexcept;
};

struct ScalaScale {
  std::string description;          // one line, verbatim bytes
  std::vector<ScalaPitch> pitches;  // degrees 1..n; the last one is the formal octave (equave)

  friend bool operator==(const ScalaScale&, const ScalaScale&) = default;

  // The description as UTF-8: unchanged if it already is valid UTF-8, otherwise read as Latin-1 (the archive's
  // character set, per the format spec).
  std::string descriptionUtf8() const;
};

// Parses .scl text. Errors: Parse (with the 1-based line in Error::location), Overflow is never returned (large
// ratios become LargeRatio). `fileName` only labels errors.
Result<ScalaScale> parseScl(std::string_view text, std::string_view fileName = {});

// Writes canonical .scl text. `fileName` (optional) goes into the "! name" header comment.
// Errors: InvalidArgument if the description contains a line break or starts with '!' (it would read back as a
// comment), or a pitch is malformed (non-positive ratio, non-finite cents, LargeRatio digits not decimal).
Result<std::string> writeScl(const ScalaScale& scale, std::string_view fileName = {});

inline constexpr int32_t kUnmappedKey = INT32_MIN;  // a KeyboardMapping entry of 'x'

struct KeyboardMapping {
  int32_t mapSize = 0;            // 0: linear mapping (degree = key − middleKey)
  int32_t firstKey = 0;           // keys outside [firstKey, lastKey] are not retuned
  int32_t lastKey = 127;
  int32_t middleKey = 60;         // key that gets degree 0 (the first mapping entry)
  int32_t referenceKey = 69;      // key whose frequency is referenceHz
  double referenceHz = 440.0;
  int32_t formalOctaveDegree = 12;  // degrees added per repetition of the mapping pattern
  std::vector<int32_t> mapping;   // ≤ mapSize entries: degree, or kUnmappedKey; missing tail entries are unmapped

  friend bool operator==(const KeyboardMapping&, const KeyboardMapping&) = default;

  // The standard linear mapping Scala uses when no .kbm is loaded: map size 0, keys 0..127, middle 60, reference
  // key 69 at 440 Hz.
  static KeyboardMapping linear(int32_t middleKey = 60, int32_t referenceKey = 69, double referenceHz = 440.0);
};

// Parses .kbm text. Errors: Parse (bad or missing header value, bad entry, unmapped reference key).
Result<KeyboardMapping> parseKbm(std::string_view text, std::string_view fileName = {});

// Writes canonical .kbm text (a comment before each header value, CRLF). Errors: InvalidArgument for a mapping that would
// not read back (negative map size, more entries than mapSize, first > last, non-positive/non-finite frequency,
// unmapped reference key).
Result<std::string> writeKbm(const KeyboardMapping& kbm);

// The checks writeKbm makes, without writing: Ok if the mapping would read back. Errors: InvalidArgument.
Status validateKbm(const KeyboardMapping& kbm);

// One ratio pitch in Scala notation as the whole text: digits[/digits], no sign, no spaces. As parseScl reads a pitch
// line: a numerator and denominator that both fit int64 give Kind::Ratio (reduced), larger ones Kind::LargeRatio
// (digits kept without leading zeros, cents computed). Used by project files (T3.5). Errors: Parse.
Result<ScalaPitch> parseScalaRatio(std::string_view text);

// Absolute scale degree of `key` (degree 0 at middleKey, octave repetition by formalOctaveDegree), or nullopt if the
// key is outside [firstKey, lastKey] or unmapped. Computed in int64, so any int32 inputs are safe. RT-safe.
std::optional<int64_t> mapKeyToDegree(const KeyboardMapping& kbm, int32_t key) noexcept;

// The same as a Degree pitch identity in a scale of `scaleSize` degrees per equave: degree in [0, scaleSize) plus
// the equave index (floor division). nullopt if unmapped, scaleSize < 1 or the equave index does not fit int32.
// RT-safe.
std::optional<PitchId> mapKey(const KeyboardMapping& kbm, int32_t key, int32_t scaleSize) noexcept;

}  // namespace pq::tuning
