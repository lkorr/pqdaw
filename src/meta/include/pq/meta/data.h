#pragma once

// A meta module's state DATA (TMM.3, docs/plans/MM.md "Architecture"): what a module keeps besides its params and
// settings — a wavetable's keyframes and gap modes, an envelope's curve shapes, a drawing. One generic, immutable tree
// of values, so the document can compare, hash, copy and save every module's data without knowing its type, and the
// type interprets and validates it (Type::validateData).
//
// Data never reaches a module's expansion (Type::body never sees it): an edit of it is the document's MetaData change
// class and never recompiles (the link hands it to an off-thread builder, TMM.5). Whatever changes the circuit is a
// setting.
//
// The project file (the document's serializer owns JSON, pq/document/serialize.h) writes a Number as a JSON number, a
// String as a string, Floats as {"$f32": "<base64 of float32 little-endian>"}, a List as an array and a Record as an
// object (its field names are identifiers, so "$f32" is never one). Not RT code.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pq::meta {

struct DataValue {
  enum class Kind : uint8_t { Null, Number, String, Floats, List, Record };
  Kind kind = Kind::Null;
  double number = 0.0;                                   // Number (finite)
  std::string text;                                      // String (UTF-8)
  std::vector<float> floats;                             // Floats (a cycle of samples, a curve; finite)
  std::vector<DataValue> items;                          // List
  std::vector<std::pair<std::string, DataValue>> fields; // Record: sorted by name, unique identifiers

  static DataValue num(double v);
  static DataValue str(std::string s);
  static DataValue floatArray(std::vector<float> v);
  static DataValue list(std::vector<DataValue> v);
  // A record from fields in any order (sorted here; a repeated name keeps the last).
  static DataValue record(std::vector<std::pair<std::string, DataValue>> f);

  const DataValue* field(std::string_view name) const noexcept;  // Record: the field, or nullptr

  // Deep, exact equality (floats and numbers bit for bit, so a NaN never makes a value unequal to itself).
  friend bool operator==(const DataValue& a, const DataValue& b) noexcept;
};
using DataPtr = std::shared_ptr<const DataValue>;

// 64-bit FNV-1a over the whole tree (bit patterns of numbers and floats): the same for equal values in every process
// (table caches key on it, TMM.5).
uint64_t dataHash(const DataValue& v) noexcept;
// Deep equality of two possibly null pointers (null equals null only).
bool dataEqual(const DataPtr& a, const DataPtr& b) noexcept;
// Structural problems (non-finite numbers / floats, unsorted or non-identifier record names), "" when there are none.
std::string dataProblem(const DataValue& v);

// Base64 (RFC 4648, padded) of float32 little-endian samples and back (the project file's Floats). decode: false on a
// malformed string (wrong length, characters, or padding).
std::string encodeFloats(const std::vector<float>& v);
bool decodeFloats(std::string_view b64, std::vector<float>& out);

}  // namespace pq::meta
