// Live sample slots (SM.2): pq/dsp/sample_slot.h.
#include "pq/dsp/sample_slot.h"

#include "pq/dsp/sampler_stretch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pq::dsp {

namespace {

// Zeros a silent entry reads (every tap of a length-0 read lies in [−3, 2]).
const float* silentZeros() noexcept {
  static const float z[2 * 16] = {};
  return z + 16;
}

}  // namespace

SampleSlot::SampleSlot() {
  auto* e = new Entry;
  for (SampleLevelView& l : e->level) l = {{silentZeros(), silentZeros()}, 0};
  e->generation = 1;
  entry_.store(e, std::memory_order_release);
}

void SampleSlot::setStretch(std::unique_ptr<SamplerStretch> bank, EpochReclaimer& reclaimer) {
  std::lock_guard lock(writeMutex_);
  if (SamplerStretch* old = stretch_.exchange(bank.release(), std::memory_order_acq_rel)) reclaimer.retire(old);
}

SampleSlot::~SampleSlot() {
  delete stretch_.load(std::memory_order_acquire);  // SM.5
  // No image reads the slot any more (they hold it alive): every entry still on the chain is ours.
  const Entry* e = entry_.load(std::memory_order_acquire);
  while (e != nullptr) {
    const Entry* older = e->prev.load(std::memory_order_relaxed);
    deleter_(const_cast<Entry*>(e));
    e = older;
  }
}

Status SampleSlot::publish(SampleSource src, double engineRate, EpochReclaimer& reclaimer) {
  const SampleLevelView& l0 = src.level[0];
  const bool silent = l0.ch[0] == nullptr || l0.length <= 0;
  if (!silent) {
    if (!(src.fileRate > 0.0) || static_cast<double>(l0.length) > kSampleSlotMaxSeconds * src.fileRate)
      return Error{ErrorCode::InvalidArgument, "sample slot: longer than 10 minutes (or no file rate)", {}};
    if (src.levels > kMaxMipLevels) return Error{ErrorCode::InvalidArgument, "sample slot: too many mip levels", {}};
    for (uint32_t l = 1; l <= src.levels; ++l)
      if (src.level[l].ch[0] == nullptr || src.level[l].length <= 0)
        return Error{ErrorCode::InvalidArgument, "sample slot: a mip level without data", {}};
  }
  if (!(engineRate > 0.0)) return Error{ErrorCode::InvalidArgument, "sample slot: no engine rate", {}};
  auto* e = new Entry;
  if (silent) {
    for (SampleLevelView& l : e->level) l = {{silentZeros(), silentZeros()}, 0};
  } else {
    e->channels = std::clamp<uint32_t>(src.channels, 1u, 2u);
    e->levels = src.levels;
    for (uint32_t l = 0; l < kSampleSlotLevels; ++l) {
      SampleLevelView v = l <= src.levels ? src.level[l] : src.level[src.levels];
      if (e->channels == 1 || v.ch[1] == nullptr) v.ch[1] = v.ch[0];
      e->level[l] = v;
    }
    if (src.channels < 2) e->channels = 1;
    e->rateScale = static_cast<float>(src.fileRate / engineRate);
    e->fileRate = src.fileRate;
    // Slices: finite, in [0, length), ascending, unique.
    std::vector<float>& s = e->sliceStore;
    for (const float f : src.slices)
      if (std::isfinite(f) && f >= 0.0f && f < static_cast<float>(l0.length)) s.push_back(std::floor(f));
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    e->slices = s.empty() ? nullptr : s.data();
    e->sliceCount = static_cast<uint32_t>(s.size());
    // SM.4: warp markers (frame, beat) pairs: finite, sorted by frame, both strictly ascending (a pair that is not is
    // dropped).
    std::vector<std::pair<float, float>> pairs;
    for (size_t i = 0; i + 1 < src.markers.size(); i += 2)
      if (std::isfinite(src.markers[i]) && std::isfinite(src.markers[i + 1])) pairs.emplace_back(src.markers[i], src.markers[i + 1]);
    std::sort(pairs.begin(), pairs.end());
    std::vector<float>& m = e->markerStore;
    for (const auto& [f, b] : pairs)
      if (m.empty() || (f > m[m.size() - 2] && b > m.back())) {
        m.push_back(f);
        m.push_back(b);
      }
    e->markers = m.empty() ? nullptr : m.data();
    e->markerCount = static_cast<uint32_t>(m.size() / 2);
    e->owner = std::move(src.owner);
  }
  std::lock_guard lock(writeMutex_);
  const auto now = std::chrono::steady_clock::now();
  auto* cur = const_cast<Entry*>(entry_.load(std::memory_order_relaxed));
  e->prev.store(cur, std::memory_order_relaxed);
  e->generation = cur->generation + 1;
  cur->superseded = now;
  cur->lastRead = now;
  entry_.store(e, std::memory_order_release);
  retireOld(now, reclaimer);
  return Ok{};
}

void SampleSlot::sweep(EpochReclaimer& reclaimer) {
  std::lock_guard lock(writeMutex_);
  retireOld(std::chrono::steady_clock::now(), reclaimer);
}

uint32_t SampleSlot::entries() const {
  std::lock_guard lock(writeMutex_);
  uint32_t n = 0;
  for (const Entry* e = entry_.load(std::memory_order_relaxed); e != nullptr; e = e->prev.load(std::memory_order_relaxed)) ++n;
  return n;
}

void SampleSlot::retireOld(std::chrono::steady_clock::time_point now, EpochReclaimer& reclaimer) {
  const auto keep = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(keepSeconds_));
  // The oldest entry still in use (header "Publication"); the newest always is.
  auto* newest = const_cast<Entry*>(entry_.load(std::memory_order_relaxed));
  Entry* lastKept = newest;
  uint32_t i = 1;
  for (auto* e = const_cast<Entry*>(newest->prev.load(std::memory_order_relaxed)); e != nullptr;
       e = const_cast<Entry*>(e->prev.load(std::memory_order_relaxed)), ++i) {
    if (e->touched.exchange(0, std::memory_order_relaxed) != 0) e->lastRead = now;
    const bool used = now - e->superseded < keep || now - e->lastRead < keep;
    if (used && i < kSampleSlotMaxEntries) lastKept = e;
  }
  auto* x = const_cast<Entry*>(lastKept->prev.load(std::memory_order_relaxed));
  if (x == nullptr) return;
  lastKept->prev.store(nullptr, std::memory_order_release);  // unreachable for every kernel that searches from now on
  while (x != nullptr) {
    auto* older = const_cast<Entry*>(x->prev.load(std::memory_order_relaxed));
    reclaimer.retire(x, deleter_);
    x = older;
  }
}

// ---- SampleData ---------------------------------------------------------------------------------------------------

Result<std::shared_ptr<const SampleData>> SampleData::build(std::span<const float> left, std::span<const float> right,
                                                            uint32_t mipLevels) {
  constexpr std::size_t kMax = 2147483647u - 2u * kSampleDataGuard - 1u;
  if (left.empty() || left.size() > kMax || left.size() > static_cast<std::size_t>(kSampleSlotMaxSeconds * 768000.0))
    return Error{ErrorCode::InvalidArgument, "sample length must be in [1, 2^31 - 129] frames", {}};
  if (!right.empty() && right.size() != left.size())
    return Error{ErrorCode::InvalidArgument, "sample channels must have the same length", {}};
  if (mipLevels > kMaxMipLevels) return Error{ErrorCode::InvalidArgument, "too many mip levels", {}};
  auto d = std::make_shared<SampleData>();
  d->channels_ = right.empty() ? 1u : 2u;
  d->levels_ = mipLevels;
  const int64_t G = kSampleDataGuard;
  int64_t frames[kSampleSlotLevels]{};
  frames[0] = static_cast<int64_t>(left.size());
  std::size_t total = 0;
  for (uint32_t l = 0; l <= mipLevels; ++l) {
    if (l > 0) frames[l] = (frames[l - 1] + 1) / 2;
    total += static_cast<std::size_t>(frames[l] + 2 * G) * d->channels_;
  }
  d->store_.assign(total, 0.0f);
  float* at = d->store_.data();
  for (uint32_t l = 0; l <= mipLevels; ++l) {
    SampleLevelView& v = d->level_[l];
    v.length = static_cast<int32_t>(frames[l]);
    for (uint32_t c = 0; c < d->channels_; ++c) {
      float* data = at + G;
      if (l == 0) {
        const std::span<const float> src = c == 0 ? left : right;
        std::copy(src.begin(), src.end(), data);
      } else {
        // As media's SampleMips: level 0 is zero outside [0, frames); a level above holds real values in its guards.
        const SampleLevelView& from = d->level_[l - 1];
        const int64_t lo = l == 1 ? 0 : -G, hi = l == 1 ? frames[0] : frames[l - 1] + G;
        decimate2(from.ch[c], lo, hi, at, -G, frames[l] + 2 * G);
      }
      v.ch[c] = data;
      at += frames[l] + 2 * G;
    }
    if (d->channels_ == 1) v.ch[1] = v.ch[0];
  }
  return std::shared_ptr<const SampleData>(std::move(d));
}

SampleSource SampleData::source(const std::shared_ptr<const SampleData>& self, double fileRate,
                                std::vector<float> slices, std::vector<float> markers) {
  SampleSource s;
  s.owner = self;
  s.channels = self->channels_;
  s.levels = self->levels_;
  for (uint32_t l = 0; l <= self->levels_; ++l) s.level[l] = self->level_[l];
  s.fileRate = fileRate;
  s.slices = std::move(slices);
  s.markers = std::move(markers);
  return s;
}

}  // namespace pq::dsp
