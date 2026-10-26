#include "pq/engine/state_transfer.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace pq::engine {

namespace {

struct EntryKey {
  uint64_t key;
  uint16_t channel;
  graph::Poly arena;
  bool operator==(const EntryKey&) const = default;
};

struct EntryKeyHash {
  size_t operator()(const EntryKey& k) const noexcept {
    uint64_t h = k.key ^ (static_cast<uint64_t>(k.channel) << 48) ^ (static_cast<uint64_t>(k.arena) << 63);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return static_cast<size_t>(h);
  }
};

// Sorts by destination and merges ranges that are contiguous in both arenas.
void coalesce(std::vector<StateCopy>& v) {
  std::sort(v.begin(), v.end(), [](const StateCopy& a, const StateCopy& b) { return a.dst < b.dst; });
  size_t out = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    if (out > 0 && v[out - 1].src + v[out - 1].size == v[i].src && v[out - 1].dst + v[out - 1].size == v[i].dst) {
      v[out - 1].size += v[i].size;
      continue;
    }
    v[out++] = v[i];
  }
  v.resize(out);
}

}  // namespace

Result<TransferPlan> buildTransferPlan(const graph::StateLayout& from, const StateArena& fromArena,
                                       const graph::StateLayout& to, const StateArena& toArena) {
  auto bad = [](const std::string& m) { return Error{ErrorCode::InvalidArgument, "state transfer: " + m, {}}; };
  const bool voiceOk = fromArena.voice != nullptr && toArena.voice != nullptr && fromArena.voiceGroups > 0 &&
                       toArena.voiceGroups > 0;
  auto fits = [](const graph::StateEntry& e, const graph::StateLayout& L, const StateArena& A) {
    const uint64_t end = static_cast<uint64_t>(e.offset) + e.size;
    if (e.arena == graph::Poly::Global) return end <= L.globalBytes;
    return end <= L.voiceBytes && (A.voice == nullptr || end <= A.voiceStride);
  };
  std::unordered_map<EntryKey, uint32_t, EntryKeyHash> old;
  old.reserve(from.entries.size() * 2);
  for (uint32_t i = 0; i < from.entries.size(); ++i) {
    const graph::StateEntry& e = from.entries[i];
    if (!fits(e, from, fromArena)) return bad("old layout entry outside its arena");
    old.try_emplace(EntryKey{e.key, e.channel, e.arena}, i);
  }
  TransferPlan plan;
  plan.voiceGroups = voiceOk ? std::min(fromArena.voiceGroups, toArena.voiceGroups) : 0u;
  std::vector<uint8_t> used(from.entries.size(), 0);
  for (const graph::StateEntry& e : to.entries) {
    if (!fits(e, to, toArena)) return bad("new layout entry outside its arena");
    const auto it = old.find(EntryKey{e.key, e.channel, e.arena});
    if (it == old.end()) {
      ++plan.fresh;
      continue;
    }
    const graph::StateEntry& o = from.entries[it->second];
    if (!(o.tag == e.tag) || o.size != e.size || used[it->second]) {
      ++plan.mismatched;
      continue;
    }
    used[it->second] = 1;
    ++plan.matched;
    if (e.size == 0) continue;
    if (e.arena == graph::Poly::Global) {
      plan.global.push_back({o.offset, e.offset, e.size});
      plan.globalBytes += e.size;
    } else if (voiceOk) {
      plan.voice.push_back({o.offset, e.offset, e.size});
      plan.voiceBytes += e.size;
    }
  }
  for (uint8_t u : used) plan.dropped += u ? 0u : 1u;
  coalesce(plan.global);
  coalesce(plan.voice);
  return plan;
}

void executeTransfer(const TransferPlan& plan, const StateArena& from, const StateArena& to,
                     const uint32_t* groupMask, uint32_t maxGroups) noexcept {  // RT-safe
  if (from.global != nullptr && to.global != nullptr)
    for (const StateCopy& c : plan.global) std::memcpy(to.global + c.dst, from.global + c.src, c.size);
  if (plan.voice.empty() || from.voice == nullptr || to.voice == nullptr) return;
  const uint32_t groups = std::min({plan.voiceGroups, from.voiceGroups, to.voiceGroups, maxGroups});
  for (uint32_t g = 0; g < groups; ++g) {
    if (groupMask != nullptr && groupMask[g] == 0) continue;
    const std::byte* src = from.voice + static_cast<size_t>(g) * from.voiceStride;
    std::byte* dst = to.voice + static_cast<size_t>(g) * to.voiceStride;
    for (const StateCopy& c : plan.voice) std::memcpy(dst + c.dst, src + c.src, c.size);
  }
}

}  // namespace pq::engine
