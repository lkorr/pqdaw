// Meta-module state data (TMM.3): pq/meta/data.h.
#include "pq/meta/data.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace pq::meta {

namespace {

bool identifier(std::string_view s) noexcept {
  if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
  for (const char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}

struct Fnv {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t n) noexcept {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  }
  template <class T>
  void operator()(const T& v) noexcept {
    bytes(&v, sizeof v);
  }
};

// Iterative walks (a deep List nest must not overflow the stack): pairs / nodes still to visit.
bool equalTrees(const DataValue& a, const DataValue& b) noexcept {
  std::vector<std::pair<const DataValue*, const DataValue*>> todo{{&a, &b}};
  while (!todo.empty()) {
    const auto [x, y] = todo.back();
    todo.pop_back();
    if (x == y) continue;
    if (x->kind != y->kind) return false;
    switch (x->kind) {
      case DataValue::Kind::Null: break;
      case DataValue::Kind::Number:
        if (std::bit_cast<uint64_t>(x->number) != std::bit_cast<uint64_t>(y->number)) return false;
        break;
      case DataValue::Kind::String:
        if (x->text != y->text) return false;
        break;
      case DataValue::Kind::Floats:
        if (x->floats.size() != y->floats.size() ||
            (!x->floats.empty() && std::memcmp(x->floats.data(), y->floats.data(), x->floats.size() * sizeof(float)) != 0))
          return false;
        break;
      case DataValue::Kind::List:
        if (x->items.size() != y->items.size()) return false;
        for (size_t i = 0; i < x->items.size(); ++i) todo.emplace_back(&x->items[i], &y->items[i]);
        break;
      case DataValue::Kind::Record:
        if (x->fields.size() != y->fields.size()) return false;
        for (size_t i = 0; i < x->fields.size(); ++i) {
          if (x->fields[i].first != y->fields[i].first) return false;
          todo.emplace_back(&x->fields[i].second, &y->fields[i].second);
        }
        break;
    }
  }
  return true;
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

DataValue DataValue::num(double v) {
  DataValue d;
  d.kind = Kind::Number;
  d.number = v;
  return d;
}
DataValue DataValue::str(std::string s) {
  DataValue d;
  d.kind = Kind::String;
  d.text = std::move(s);
  return d;
}
DataValue DataValue::floatArray(std::vector<float> v) {
  DataValue d;
  d.kind = Kind::Floats;
  d.floats = std::move(v);
  return d;
}
DataValue DataValue::list(std::vector<DataValue> v) {
  DataValue d;
  d.kind = Kind::List;
  d.items = std::move(v);
  return d;
}
DataValue DataValue::record(std::vector<std::pair<std::string, DataValue>> f) {
  DataValue d;
  d.kind = Kind::Record;
  std::stable_sort(f.begin(), f.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& x : f) {
    if (!d.fields.empty() && d.fields.back().first == x.first)
      d.fields.back().second = std::move(x.second);
    else
      d.fields.push_back(std::move(x));
  }
  return d;
}

const DataValue* DataValue::field(std::string_view name) const noexcept {
  if (kind != Kind::Record) return nullptr;
  const auto it = std::lower_bound(fields.begin(), fields.end(), name,
                                   [](const auto& f, std::string_view n) { return f.first < n; });
  return it != fields.end() && it->first == name ? &it->second : nullptr;
}

bool operator==(const DataValue& a, const DataValue& b) noexcept { return equalTrees(a, b); }

bool dataEqual(const DataPtr& a, const DataPtr& b) noexcept {
  if (a == b) return true;
  if (!a || !b) return false;
  return *a == *b;
}

uint64_t dataHash(const DataValue& v) noexcept {
  Fnv h;
  std::vector<const DataValue*> todo{&v};
  while (!todo.empty()) {
    const DataValue* x = todo.back();
    todo.pop_back();
    h(x->kind);
    switch (x->kind) {
      case DataValue::Kind::Null: break;
      case DataValue::Kind::Number: h(std::bit_cast<uint64_t>(x->number)); break;
      case DataValue::Kind::String:
        h(x->text.size());
        h.bytes(x->text.data(), x->text.size());
        break;
      case DataValue::Kind::Floats:
        h(x->floats.size());
        h.bytes(x->floats.data(), x->floats.size() * sizeof(float));
        break;
      case DataValue::Kind::List:
        h(x->items.size());
        for (size_t i = x->items.size(); i-- > 0;) todo.push_back(&x->items[i]);
        break;
      case DataValue::Kind::Record:
        h(x->fields.size());
        for (size_t i = x->fields.size(); i-- > 0;) {
          todo.push_back(&x->fields[i].second);
        }
        for (const auto& f : x->fields) {
          h(f.first.size());
          h.bytes(f.first.data(), f.first.size());
        }
        break;
    }
  }
  return h.h;
}

std::string dataProblem(const DataValue& v) {
  std::vector<const DataValue*> todo{&v};
  while (!todo.empty()) {
    const DataValue* x = todo.back();
    todo.pop_back();
    switch (x->kind) {
      case DataValue::Kind::Number:
        if (!std::isfinite(x->number)) return "a number is not finite";
        break;
      case DataValue::Kind::Floats:
        for (const float f : x->floats)
          if (!std::isfinite(f)) return "a sample is not finite";
        break;
      case DataValue::Kind::List:
        for (const DataValue& i : x->items) todo.push_back(&i);
        break;
      case DataValue::Kind::Record:
        for (size_t i = 0; i < x->fields.size(); ++i) {
          if (!identifier(x->fields[i].first)) return "'" + x->fields[i].first + "' is not a field name";
          if (i > 0 && !(x->fields[i - 1].first < x->fields[i].first)) return "record fields must be sorted, one per name";
          todo.push_back(&x->fields[i].second);
        }
        break;
      default: break;
    }
  }
  return {};
}

std::string encodeFloats(const std::vector<float>& v) {
  const size_t n = v.size() * 4;
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  auto byte = [&](size_t i) -> uint32_t {
    const uint32_t bits = std::bit_cast<uint32_t>(v[i / 4]);
    return (bits >> (8 * (i % 4))) & 0xFFu;  // little-endian
  };
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t b0 = byte(i), b1 = i + 1 < n ? byte(i + 1) : 0, b2 = i + 2 < n ? byte(i + 2) : 0;
    const uint32_t w = (b0 << 16) | (b1 << 8) | b2;
    out += kB64[(w >> 18) & 63];
    out += kB64[(w >> 12) & 63];
    out += i + 1 < n ? kB64[(w >> 6) & 63] : '=';
    out += i + 2 < n ? kB64[w & 63] : '=';
  }
  return out;
}

bool decodeFloats(std::string_view s, std::vector<float>& out) {
  out.clear();
  if (s.size() % 4 != 0) return false;
  std::vector<uint8_t> bytes;
  bytes.reserve(s.size() / 4 * 3);
  for (size_t i = 0; i < s.size(); i += 4) {
    const bool last = i + 4 == s.size();
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s[i + static_cast<size_t>(k)];
      if (c == '=' && last && k >= 2) {
        v[k] = 0;
        ++pad;
        continue;
      }
      if (pad > 0) return false;  // data after padding
      v[k] = b64Value(c);
      if (v[k] < 0) return false;
    }
    const uint32_t w = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                       (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
    bytes.push_back(static_cast<uint8_t>(w >> 16));
    if (pad < 2) bytes.push_back(static_cast<uint8_t>(w >> 8));
    if (pad < 1) bytes.push_back(static_cast<uint8_t>(w));
  }
  if (bytes.size() % 4 != 0) return false;
  out.resize(bytes.size() / 4);
  for (size_t i = 0; i < out.size(); ++i) {
    const uint32_t bits = static_cast<uint32_t>(bytes[4 * i]) | (static_cast<uint32_t>(bytes[4 * i + 1]) << 8) |
                          (static_cast<uint32_t>(bytes[4 * i + 2]) << 16) | (static_cast<uint32_t>(bytes[4 * i + 3]) << 24);
    out[i] = std::bit_cast<float>(bits);
  }
  return true;
}

}  // namespace pq::meta
