#pragma once

// Value types of the patch IR: port types (kind / rate / poly / width), literal constants with their unit suffix,
// and parameter attributes. See docs/PATCH_LANGUAGE.md and SPEC §7.1.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/stable_id.h"

namespace pq::graph {

struct NodeTag;
// Survives edits (erase, reorder); used to key program state across hot swaps (SPEC §7.5). Distinct from the dense
// node index, which changes when nodes are erased.
using NodeId = StableId<NodeTag>;

// Dense indices into a Patch's flat arrays.
using NodeIndex = uint32_t;
using WireIndex = uint32_t;
using SolveIndex = uint32_t;
using AtomId = uint16_t;
inline constexpr uint32_t kNone = 0xFFFFFFFFu;

// Every field has an Infer value. In a declaration, a concrete value is an upper bound (PATCH_LANGUAGE §3:
// inference may lower rate and poly); Infer means "no constraint, decided by inference (T1.3)".
enum class Kind : uint8_t { Infer, Real, Bool };
enum class Rate : uint8_t { Infer, Const, Param, Audio, Event };  // ordered: Const < Param < Audio
enum class Poly : uint8_t { Infer, Global, Voice };

inline constexpr uint16_t kWidthInfer = 0;
inline constexpr uint16_t kWidthMono = 1;
inline constexpr uint16_t kWidthStereo = 2;

struct PortType {
  Kind kind = Kind::Infer;
  Rate rate = Rate::Infer;
  Poly poly = Poly::Infer;
  uint16_t width = kWidthInfer;  // channels; 0 = inferred

  friend constexpr bool operator==(const PortType&, const PortType&) noexcept = default;
};

constexpr PortType portType(Kind k, Rate r = Rate::Infer, Poly p = Poly::Infer, uint16_t w = kWidthInfer) noexcept {
  return PortType{k, r, p, w};
}

std::string_view toText(Kind) noexcept;
std::string_view toText(Rate) noexcept;
std::string_view toText(Poly) noexcept;
// "real audio voice stereo"-style text of a port type (Infer fields are omitted; "?" if everything is inferred).
std::string toText(const PortType&);

// Number literal suffixes (PATCH_LANGUAGE §1). Literals are stored as written (value + suffix), so printing
// reproduces them exactly and parse(print(p)) == p holds bit-for-bit; value() gives the normalised number.
enum class Unit : uint8_t { None, Hz, kHz, ms, s, smp, dB, cents, st, percent };

std::string_view unitSuffix(Unit) noexcept;  // "", "Hz", "kHz", "ms", "s", "smp", "dB", "c", "st", "%"
// Inverse of unitSuffix for the non-empty suffixes; false if `s` is not a unit suffix.
bool unitFromSuffix(std::string_view s, Unit& out) noexcept;

// A literal: a number with an optional unit, or a bool (`true`/`false`, literal 1/0, Unit::None).
struct Constant {
  Kind kind = Kind::Real;  // Real or Bool
  Unit unit = Unit::None;
  double literal = 0.0;    // as written, before unit normalisation

  static constexpr Constant real(double v, Unit u = Unit::None) noexcept { return {Kind::Real, u, v}; }
  static constexpr Constant boolean(bool b) noexcept { return {Kind::Bool, Unit::None, b ? 1.0 : 0.0}; }

  // Normalised value: Hz, seconds, samples, linear gain (dB), cents (c, st), fraction (%).
  double value() const noexcept;

  // Exact comparison of what was written (bitwise on the double, so 0.0 and -0.0 differ and NaN == NaN).
  friend bool operator==(const Constant&, const Constant&) noexcept;
};

// The value of an attribute argument: a number (with unit), a bool, a string ("Cutoff"), or a bare identifier
// (`@unit(Hz)`, `@curve(exp)`).
struct AttrValue {
  enum class Type : uint8_t { Number, Bool, String, Ident };
  Type type = Type::Number;
  Constant number;   // Number / Bool
  std::string text;  // String / Ident

  static AttrValue num(double v, Unit u = Unit::None) { return {Type::Number, Constant::real(v, u), {}}; }
  static AttrValue boolean(bool b) { return {Type::Bool, Constant::boolean(b), {}}; }
  static AttrValue string(std::string s) { return {Type::String, {}, std::move(s)}; }
  static AttrValue ident(std::string s) { return {Type::Ident, {}, std::move(s)}; }

  friend bool operator==(const AttrValue&, const AttrValue&) = default;
};

struct AttrArg {
  std::string name;  // empty for a positional argument
  AttrValue value;
  friend bool operator==(const AttrArg&, const AttrArg&) = default;
};

// `@name(args)`; `@hidden` and `@hidden()` are the same attribute (no args).
struct Attr {
  std::string name;
  std::vector<AttrArg> args;
  friend bool operator==(const Attr&, const Attr&) = default;
};

// `param name : kind = default [min..max] @attrs…` (PATCH_LANGUAGE §2). Default and range are constant literals:
// the parser (T1.2) folds constant expressions; params are runtime values and are never folded into code.
struct ParamDecl {
  Kind kind = Kind::Real;  // Real or Bool
  Constant defaultValue = Constant::real(0.0);
  bool hasRange = false;
  Constant rangeMin;
  Constant rangeMax;
  std::vector<Attr> attrs;

  friend bool operator==(const ParamDecl&, const ParamDecl&) = default;
};

// Visual placement of a node (`node x at (a, b) rot r flip`). Rotation is in degrees, a multiple of 90 in [0, 360).
struct NodeLayout {
  bool placed = false;  // false: no `node … at` entry
  bool flip = false;
  int16_t rotation = 0;
  float x = 0.0f;
  float y = 0.0f;
  friend bool operator==(const NodeLayout&, const NodeLayout&) = default;
};

struct Point2 {
  float x = 0.0f;
  float y = 0.0f;
  friend bool operator==(const Point2&, const Point2&) = default;
};

// `comment "text" at (x, y)` in a layout block.
struct LayoutComment {
  std::string text;
  Point2 at;
  friend bool operator==(const LayoutComment&, const LayoutComment&) = default;
};

// True for `[A-Za-z_][A-Za-z0-9_]*`.
bool isIdentifier(std::string_view s) noexcept;
// The language keywords listed in PATCH_LANGUAGE §1 (`patch import param in out export solve iterations layout true
// false`). Port-type and layout words (`real`, `audio`, `at`, …) only appear in fixed positions and are not reserved.
bool isKeyword(std::string_view s) noexcept;
// Names that would shadow the built-in namespaces `voice.*`, `transport.*` and the `sr` constant.
bool isReservedName(std::string_view s) noexcept;
// Keywords that cannot name a node, param, port or patch: every keyword except `in` and `out`, which the language's
// own example uses as names (`out out : audio voice mono` … `out = car * env`, PATCH_LANGUAGE §4) and which the
// parser can tell apart by position (docs/QUESTIONS.md Q3).
bool isKeywordNotName(std::string_view s) noexcept;

}  // namespace pq::graph
