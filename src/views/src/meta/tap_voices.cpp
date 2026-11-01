// The faces' voice reader over the engine's voice taps (TMM.5): pq/views/meta/tap_voices.h.
#include "pq/views/meta/tap_voices.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pq::views {

namespace {

double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

TapVoices::TapVoices(Resolve resolve, TableResolve table) : resolve_(std::move(resolve)), tableResolve_(std::move(table)) {
  follow();
  readGen_ = seenGen_ = table_.slot ? tableGeneration() : 0;  // the table there now is not news (a face reads it)
  lastTableChange_ = -1e300;
}

TapVoices::~TapVoices() = default;

void TapVoices::follow() const noexcept {
  std::shared_ptr<pq::VoiceTap> t;
  try {
    t = resolve_ ? resolve_() : nullptr;
  } catch (...) {  // the host's lookup failed to allocate: keep the tap we have
    return;
  }
  if (tableResolve_) {
    try {
      table_ = tableResolve_();
    } catch (...) {  // keep the slot we have
    }
  }
  if (t == tap_) return;
  reader_.reset();
  tap_ = std::move(t);
  ++tapSerial_;
  active_ = false;
  readWritten_ = 0;
  if (tap_) {
    reader_ = std::make_unique<pq::VoiceTapReader>(tap_);
    seen_.assign(reader_->voiceSlots(), 0);
  } else {
    seen_.clear();
  }
}

uint64_t TapVoices::written() const noexcept {
  if (!tap_) return 0;
  uint64_t n = 0;
  for (uint32_t g = 0; g < tap_->groups(); ++g) n += tap_->written(g);
  return n;
}

// The voices shown count as gone this long after the last record (the core reader's window, plus a margin).
constexpr double kStaleSeconds = 0.12;

uint64_t TapVoices::tableGeneration() const noexcept {
  // The slot's publish count, made distinct per slot object (a recompile may give the instance a new slot).
  const uint64_t g = table_.slot ? table_.slot->generation() ^ (reinterpret_cast<uintptr_t>(table_.slot.get()) * 0xD6E8FEB86659FD93ull)
                                 : 0;
  if (g != seenGen_) {
    seenGen_ = g;
    lastTableChange_ = nowSeconds();
  }
  return g;
}

uint64_t TapVoices::serial() const noexcept {
  follow();
  const bool stale = active_ && nowSeconds() - lastRecord_ > kStaleSeconds;
  return (tapSerial_ << 48) ^ written() ^ (stale ? 1ull << 47 : 0ull) ^ (tableGeneration() * 0x9E3779B97F4A7C15ull);
}

bool TapVoices::fresh() const noexcept {
  follow();
  return written() != readWritten_ || tableGeneration() != readGen_;
}

bool TapVoices::live() const noexcept {
  follow();
  const uint64_t g = tableGeneration();
  return active_ || written() != readWritten_ || g != readGen_ || nowSeconds() - lastTableChange_ < kTableLinger;
}

HeardTable TapVoices::heardTable() const {
  follow();
  HeardTable h;
  if (!table_.slot) return h;
  h.generation = tableGeneration();
  h.table = table_.slot->table();
  h.modulated = table_.modulated;
  return h;
}

uint32_t TapVoices::read(std::span<FaceVoice> out) {
  follow();
  readGen_ = tableGeneration();
  if (!reader_) return 0;
  const double now = nowSeconds();
  const uint64_t w = written();
  if (w != readWritten_) lastRecord_ = now;
  readWritten_ = w;
  (void)reader_->poll(now);
  uint32_t n = 0;
  hist_.resize(FaceVoice::kMaxSamples);
  for (uint32_t s = 0; s < reader_->voiceSlots() && n < out.size(); ++s) {
    const pq::VoiceTapReader::Voice& v = reader_->voice(s);
    if (!v.active) {
      seen_[s] = v.serial;
      continue;
    }
    FaceVoice& f = out[n++];
    f.slot = s;
    f.phase = v.value - std::floor(v.value);
    f.position = v.position;
    f.increment = v.increment;
    // The samples since this slot's last read: kVoiceTapSamples per record (at most one displayed frame's worth).
    const uint64_t records = seen_[s] != 0 && v.serial > seen_[s] ? v.serial - seen_[s] : 1;
    seen_[s] = v.serial;
    const auto want = static_cast<uint32_t>(std::min<uint64_t>(records * kVoiceTapSamples, FaceVoice::kMaxSamples));
    const uint32_t got = reader_->history(s, std::span<float>(hist_.data(), want));
    f.sampleCount = got;
    for (uint32_t i = 0; i < got; ++i) f.samples[i] = hist_[i] - std::floor(hist_[i]);
  }
  active_ = n != 0;
  return n;
}

}  // namespace pq::views
