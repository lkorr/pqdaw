#pragma once

// Pieces shared by the parser and the printer (private to pq_graph): operator table, text names of ports, a small
// open-addressing name table.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lexer.h"
#include "pq/graph/patch.h"

namespace pq::graph::detail {

// Precedence levels of PATCH_LANGUAGE §2 (higher binds tighter).
enum Prec : uint8_t {
  kPrecTernary = 1,
  kPrecOr = 2,
  kPrecXor = 3,
  kPrecAnd = 4,
  kPrecCompare = 5,  // non-associative
  kPrecAdd = 6,
  kPrecMul = 7,
  kPrecUnary = 8,
  kPrecPostfix = 9,
  kPrecPrimary = 10,
};

struct BinaryOp {
  Tok tok;
  std::string_view text;
  std::string_view atom;
  uint8_t prec;
};

inline constexpr BinaryOp kBinaryOps[] = {
    {Tok::OrOr, "||", "or", kPrecOr},       {Tok::XorXor, "^^", "xor", kPrecXor},   {Tok::AndAnd, "&&", "and", kPrecAnd},
    {Tok::Lt, "<", "lt", kPrecCompare},     {Tok::Le, "<=", "le", kPrecCompare},    {Tok::Gt, ">", "gt", kPrecCompare},
    {Tok::Ge, ">=", "ge", kPrecCompare},    {Tok::Eq, "==", "eq", kPrecCompare},    {Tok::Ne, "!=", "ne", kPrecCompare},
    {Tok::Plus, "+", "add", kPrecAdd},      {Tok::Minus, "-", "sub", kPrecAdd},     {Tok::Star, "*", "mul", kPrecMul},
    {Tok::Slash, "/", "div", kPrecMul},     {Tok::Percent, "%", "mod", kPrecMul},
};

// Atom ids of the operators, looked up once.
struct OperatorAtoms {
  AtomId binary[std::size(kBinaryOps)];
  AtomId neg, notOp, select, channel, pack;
  static const OperatorAtoms& get();
  // Index into kBinaryOps for an operator atom, or -1.
  int binaryIndex(AtomId a) const noexcept {
    for (int i = 0; i < static_cast<int>(std::size(kBinaryOps)); ++i)
      if (binary[i] == a) return i;
    return -1;
  }
};

// The name a port has in text. Ports of an atom's repeated (variadic) spec all share the spec's name in the IR, so
// text numbers them: `in0`, `in1`, … (mux, pack) and `out0`, `out1`, … (demux). Everything else uses
// Patch::inputName/outputName.
std::string portTextName(const Patch& p, NodeIndex n, uint32_t port, bool input);
std::optional<uint32_t> findPortByText(const Patch& p, NodeIndex n, std::string_view name, bool input);

// Open-addressing hash table from a name to a uint32 (string_views must outlive the table).
class NameTable {
public:
  void reset(size_t expected);
  // nullptr if absent.
  const uint32_t* find(std::string_view key) const noexcept;
  // False if the key exists already (the table is unchanged).
  bool insert(std::string_view key, uint32_t value);

private:
  struct Slot {
    std::string_view key;
    uint32_t value = kNone;
  };
  static uint64_t hash(std::string_view s) noexcept;
  void grow();
  std::vector<Slot> slots_;
  size_t size_ = 0;
};

}  // namespace pq::graph::detail
