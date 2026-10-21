#include "text_common.h"

#include <charconv>

namespace pq::graph::detail {

const OperatorAtoms& OperatorAtoms::get() {
  static const OperatorAtoms ops = [] {
    OperatorAtoms o{};
    for (size_t i = 0; i < std::size(kBinaryOps); ++i) o.binary[i] = atomId(kBinaryOps[i].atom);
    o.neg = atomId("neg");
    o.notOp = atomId("not");
    o.select = atomId("select");
    o.channel = atomId("channel");
    o.pack = atomId("pack");
    return o;
  }();
  return ops;
}

namespace {

// For an atom port in the repeated part of a variadic signature: the index within the repetition, else -1.
int repeatIndex(const Patch& p, NodeIndex n, uint32_t port, bool input) {
  const Node& nd = p.node(n);
  if (nd.kind != NodeKind::Atom) return -1;
  const AtomSignature& sig = atomSignature(nd.atom);
  if (input ? !sig.has(atom_flags::kVariadicInputs) : !sig.has(atom_flags::kVariadicOutputs)) return -1;
  const uint32_t fixed = input ? sig.fixedInputs() : sig.fixedOutputs();
  return port >= fixed ? static_cast<int>(port - fixed) : -1;
}

}  // namespace

std::string portTextName(const Patch& p, NodeIndex n, uint32_t port, bool input) {
  std::string name(input ? p.inputName(n, port) : p.outputName(n, port));
  const int k = repeatIndex(p, n, port, input);
  if (k >= 0) name += std::to_string(k);
  return name;
}

std::optional<uint32_t> findPortByText(const Patch& p, NodeIndex n, std::string_view name, bool input) {
  const Node& nd = p.node(n);
  const uint32_t count = input ? nd.numInputs : nd.numOutputs;
  for (uint32_t i = 0; i < count; ++i) {
    const std::string_view base = input ? p.inputName(n, i) : p.outputName(n, i);
    const int k = repeatIndex(p, n, i, input);
    if (k < 0) {
      if (base == name) return i;
      continue;
    }
    // Repeated port: `<base><k>`.
    if (name.size() <= base.size() || name.substr(0, base.size()) != base) continue;
    const std::string_view digits = name.substr(base.size());
    int value = -1;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec == std::errc{} && ptr == digits.data() + digits.size() && value == k && (digits.size() == 1 || digits[0] != '0'))
      return i;
  }
  return std::nullopt;
}

uint64_t NameTable::hash(std::string_view s) noexcept {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h ^ (h >> 29);
}

void NameTable::reset(size_t expected) {
  size_t cap = 16;
  while (cap < expected * 2 + 2) cap *= 2;
  slots_.assign(cap, Slot{});
  size_ = 0;
}

const uint32_t* NameTable::find(std::string_view key) const noexcept {
  if (slots_.empty()) return nullptr;
  const size_t mask = slots_.size() - 1;
  for (size_t i = static_cast<size_t>(hash(key)) & mask;; i = (i + 1) & mask) {
    const Slot& s = slots_[i];
    if (s.value == kNone) return nullptr;
    if (s.key == key) return &s.value;
  }
}

bool NameTable::insert(std::string_view key, uint32_t value) {
  if (slots_.empty() || (size_ + 1) * 2 > slots_.size()) grow();
  const size_t mask = slots_.size() - 1;
  for (size_t i = static_cast<size_t>(hash(key)) & mask;; i = (i + 1) & mask) {
    Slot& s = slots_[i];
    if (s.value == kNone) {
      s.key = key;
      s.value = value;
      ++size_;
      return true;
    }
    if (s.key == key) return false;
  }
}

void NameTable::grow() {
  std::vector<Slot> old = std::move(slots_);
  slots_.assign(old.empty() ? 16 : old.size() * 2, Slot{});
  size_ = 0;
  for (const Slot& s : old)
    if (s.value != kNone) insert(s.key, s.value);
}

}  // namespace pq::graph::detail
