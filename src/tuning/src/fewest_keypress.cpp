#include "pq/tuning/fewest_keypress.h"

#include <algorithm>
#include <cmath>

#include "pq/tuning/ji.h"
#include "pq/tuning/js_number.h"

namespace pq::tuning {

namespace {

// A JS array index: canonical decimal (no sign, no leading zeros except "0") below 2^32 − 1.
std::optional<uint32_t> arrayIndex(const std::string& key) {
  if (key.empty() || key.size() > 10) return std::nullopt;
  if (key.size() > 1 && key[0] == '0') return std::nullopt;
  uint64_t v = 0;
  for (char c : key) {
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + static_cast<uint64_t>(c - '0');
  }
  if (v >= 0xFFFFFFFFull) return std::nullopt;
  return static_cast<uint32_t>(v);
}

}  // namespace

std::vector<KeyRatio> trainerDefaultKeymap() {
  // data/keyMappings.js: DEFAULT_PRIME_MAPPINGS then DEFAULT_RECIPROCAL_MAPPINGS.
  const struct {
    const char* key;
    int64_t n, d;
  } table[] = {{"1", 2, 1},   {"2", 3, 2},   {"3", 5, 4},   {"4", 7, 4},   {"5", 11, 8},  {"6", 13, 8},  {"7", 17, 16},
               {"8", 19, 16}, {"9", 23, 16}, {"0", 29, 16}, {"q", 1, 2},   {"w", 2, 3},   {"e", 4, 5},   {"r", 4, 7},
               {"t", 8, 11},  {"y", 8, 13},  {"u", 16, 17}, {"i", 16, 19}, {"o", 16, 23}, {"p", 16, 29}};
  std::vector<KeyRatio> out;
  out.reserve(std::size(table));
  for (const auto& t : table) out.push_back({t.key, Rational(t.n, t.d)});
  return out;
}

std::vector<KeyRatio> jsObjectKeyOrder(std::span<const KeyRatio> insertions) {
  std::vector<KeyRatio> unique;  // first-insertion order, last value
  for (const KeyRatio& k : insertions) {
    auto it = std::find_if(unique.begin(), unique.end(), [&](const KeyRatio& u) { return u.key == k.key; });
    if (it == unique.end())
      unique.push_back(k);
    else
      it->ratio = k.ratio;
  }
  std::vector<std::pair<uint32_t, KeyRatio>> indexed;
  std::vector<KeyRatio> out;
  for (KeyRatio& k : unique) {
    if (const auto idx = arrayIndex(k.key))
      indexed.emplace_back(*idx, std::move(k));
    else
      out.push_back(std::move(k));
  }
  std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<KeyRatio> ordered;
  ordered.reserve(indexed.size() + out.size());
  for (auto& [i, k] : indexed) ordered.push_back(std::move(k));
  for (KeyRatio& k : out) ordered.push_back(std::move(k));
  return ordered;
}

int64_t ratioToEdoSteps(const Rational& r, uint32_t edo) noexcept {  // RT-safe
  // JS: Math.round(prEdo * Math.log2(num / denom))
  const double q = static_cast<double>(r.num()) / static_cast<double>(r.den());
  return static_cast<int64_t>(js::round(static_cast<double>(edo) * std::log2(q)));
}

Result<FewestKeypressMap> FewestKeypressMap::build(std::span<const KeyRatio> keys, uint32_t edo, uint64_t primeLimit) {
  if (edo == 0 || edo > kMaxEdo)
    return Error{ErrorCode::InvalidArgument, "fewest-keypress map needs 1 <= edo <= 65536", {}};
  FewestKeypressMap m;
  m.edo_ = edo;
  m.maxRange_ = static_cast<int32_t>(edo) * kRangeEquaves;

  // Unique step moves, filtered by prime limit (JS: for (const key in allMappings) …).
  for (size_t i = 0; i < keys.size(); ++i) {
    const Rational& r = keys[i].ratio;
    if (!r.isPositive())
      return Error{ErrorCode::InvalidArgument, "key '" + keys[i].key + "' has a non-positive ratio", {}};
    if (largestPrimeFactor(static_cast<uint64_t>(r.num())) > primeLimit) continue;
    if (largestPrimeFactor(static_cast<uint64_t>(r.den())) > primeLimit) continue;
    const int64_t s = ratioToEdoSteps(r, edo);
    if (s == 0) continue;
    if (std::any_of(m.moves_.begin(), m.moves_.end(), [&](const Move& mv) { return mv.steps == s; })) continue;
    m.moves_.push_back({s, r, static_cast<uint32_t>(i)});
  }

  const int32_t R = m.maxRange_;
  m.entries_.assign(static_cast<size_t>(2 * R + 1), Entry{});
  m.entries_[static_cast<size_t>(R)] = Entry{Rational{1}, 0, 0, -1};

  // BFS (JS: queue of [stepOffset, num, denom, presses], queue.shift()). Each step is enqueued at most once per
  // improvement, so the queue holds at most 2R + 1 + (rare re-improvements) items.
  struct Item {
    int32_t step;
    int32_t presses;
    Rational ratio;
  };
  std::vector<Item> queue;
  queue.reserve(static_cast<size_t>(2 * R + 2));
  queue.push_back({0, 0, Rational{1}});
  for (size_t head = 0; head < queue.size(); ++head) {
    const Item cur = queue[head];
    if (cur.presses >= kMaxPresses) continue;
    for (size_t mi = 0; mi < m.moves_.size(); ++mi) {
      const Move& mv = m.moves_[mi];
      const int64_t newStep = cur.step + mv.steps;
      if (newStep < -R || newStep > R) continue;
      const int32_t newPresses = cur.presses + 1;
      Entry& e = m.entries_[static_cast<size_t>(newStep + R)];
      if (e.presses < 0 || newPresses < e.presses) {
        // The JS multiplies before this test; the product only matters when it is stored, so computing it here
        // gives the same result with one checked multiply per new entry instead of one per (state, move).
        const std::optional<Rational> prod = mul(cur.ratio, mv.ratio);
        if (!prod) continue;  // deviation: the JS would continue with an inexact double product
        e = Entry{*prod, newPresses, cur.step, static_cast<int32_t>(mi)};
        queue.push_back({static_cast<int32_t>(newStep), newPresses, *prod});
      }
    }
  }
  return m;
}

std::string FewestKeypressMap::ratioLabel(int64_t step) const {
  const Entry* e = find(step);
  if (!e) return {};
  return std::to_string(e->ratio.num()) + "/" + std::to_string(e->ratio.den());
}

std::string FewestKeypressMap::cursorLabel(int64_t step) const {
  std::string r = ratioLabel(step);
  if (!r.empty()) return r;
  return (step >= 0 ? "+" : "") + std::to_string(step) + "\\" + std::to_string(edo_);
}

std::vector<uint32_t> FewestKeypressMap::keySequence(int64_t step) const {
  std::vector<uint32_t> seq;
  const Entry* e = find(step);
  if (!e) return seq;
  seq.reserve(static_cast<size_t>(e->presses));
  // Bounded: every hop goes to an entry with one press fewer.
  while (e->move >= 0) {
    seq.push_back(moves_[static_cast<size_t>(e->move)].keyIndex);
    e = find(e->fromStep);
  }
  std::reverse(seq.begin(), seq.end());
  return seq;
}

size_t FewestKeypressMap::size() const noexcept {
  return static_cast<size_t>(
      std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.presses >= 0; }));
}

}  // namespace pq::tuning
