// Voices (T1.8): pq/engine/voices.h says what the engine does. Per block:
//   1. allocate()   events in time order → lane decisions (logical state) + LaneOps {frame, lane, kind, event}
//   2. LaneOps bucketed per lane (stable counting sort); lanes that start this block get their state reset
//   3. renderGroup()  only for groups whose signals change: a vector fill of every lane's start-of-block values,
//                     then per lane with ops / glides / fades a scalar walk of its segments between op frames
//   4. image->process with the voice table (groups with no voice are skipped by the image)
//   5. mixAndMeasure()  Σ gain · out per voice `out` slot; per-lane peaks of the monitored outs
//   6. endVoices()    done slot / silence / timeout / end of a steal fade → lanes free from the next block
#include "pq/engine/voices.h"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "pq/dsp/atoms.h"
#include "pq/dsp/kernel.h"

namespace pq::engine {

namespace {

constexpr uint32_t kAudioSignals = 4;  // hz, cents, gate, trigger
constexpr uint32_t kParamSignals = 6;  // velocity, release, ratio, harmonic, unisonCount, index

uint32_t roundUp(uint32_t n, uint32_t a) noexcept { return (n + a - 1) / a * a; }

inline float horizontalSum(__m256 v) noexcept {  // RT-safe
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_movehdup_ps(s));
  return _mm_cvtss_f32(s);
}

inline bool isDelayAtom(dsp::Atom a) noexcept {
  switch (a) {
    case dsp::Atom::DelayLinear:
    case dsp::Atom::DelayCubic:
    case dsp::Atom::DelayAllpass:
    case dsp::Atom::DelayReadLinear:
    case dsp::Atom::DelayReadCubic:
    case dsp::Atom::DelayReadAllpass:
    case dsp::Atom::DelayWrite: return true;
    default: return false;
  }
}

const float kTrueMask = dsp::boolMask(true);

}  // namespace

uint32_t findVoiceSlot(const ImageInterface& io, const graph::Patch& patch, std::string_view name) {
  for (uint32_t s = 0; s < io.voiceOut.size(); ++s) {
    const graph::NodeIndex n = io.voiceOut[s].node;
    if (n < patch.nodeCount() && patch.nodeName(n) == name) return s;
  }
  return kNoSlot;
}

Status VoiceEngine::prepare(ProgramImage* image, const VoiceConfig& config) {
  auto bad = [](const char* m) { return Error{ErrorCode::InvalidArgument, std::string("voices: ") + m, {}}; };
  if (image == nullptr) return bad("no image");
  const ImageInterface& io = image->io();
  if (!io.hasVoiceStages) return bad("the patch has no voice stages");
  if (io.maxVoiceGroups == 0 || io.maxBlockFrames == 0) return bad("the image has no voice groups");
  const uint32_t lanes = io.maxVoiceGroups * 8;
  if (config.maxVoices == 0 || config.maxVoices > lanes) return bad("maxVoices must be in [1, maxVoiceGroups · 8]");
  if (config.doneSlot != kNoSlot && config.doneSlot >= io.voiceOut.size()) return bad("doneSlot out of range");
  if (!(config.referenceHz > 0.0) || config.maxEventsPerBlock == 0) return bad("invalid referenceHz / maxEventsPerBlock");

  image_ = image;
  cfg_ = config;
  groups_ = io.maxVoiceGroups;
  lanes_ = lanes;
  maxBlock_ = io.maxBlockFrames;
  stride_ = roundUp(maxBlock_, 16);
  groupStride_ = roundUp(maxBlock_ * 8, 16);
  sampleRate_ = io.sampleRate > 0.0 ? io.sampleRate : 48000.0;
  fadeFrames_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(cfg_.stealFadeMs * 0.001 * sampleRate_)));
  silenceFrames_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(cfg_.silenceMs * 0.001 * sampleRate_)));
  releaseTimeoutFrames_ = std::max<int64_t>(0, std::llround(cfg_.releaseTimeoutMs * 0.001 * sampleRate_));
  silenceLevel_ = static_cast<float>(std::pow(10.0, cfg_.silenceDb / 20.0));

  // Voice out slots: `out`s are mixed and monitored; the done slot is read; other exports are not requested.
  numOutSlots_ = static_cast<uint32_t>(io.voiceOut.size());
  mixSlots_.clear();
  std::vector<uint8_t> requested(numOutSlots_, 0);
  for (uint32_t s = 0; s < numOutSlots_; ++s)
    if (!io.voiceOut[s].isExport && s != cfg_.doneSlot) {
      mixSlots_.push_back(s);
      requested[s] = 1;
    }
  monitorSlots_.clear();
  if (cfg_.monitorMask == 0) {
    monitorSlots_ = mixSlots_;
  } else {
    for (uint32_t s = 0; s < numOutSlots_ && s < 32; ++s)
      if ((cfg_.monitorMask >> s) & 1u) {
        monitorSlots_.push_back(s);
        requested[s] = 1;
      }
    if (monitorSlots_.empty()) return bad("monitorMask names no voice output slot");
  }
  if (cfg_.doneSlot != kNoSlot) requested[cfg_.doneSlot] = 1;
  uint32_t numRequested = 0;
  std::vector<uint32_t> outIndex(numOutSlots_, 0);
  for (uint32_t s = 0; s < numOutSlots_; ++s)
    if (requested[s]) outIndex[s] = numRequested++;

  audioSig_ = AlignedBuffer<float>(static_cast<size_t>(groups_) * kAudioSignals * groupStride_);
  paramSig_ = AlignedBuffer<float>(static_cast<size_t>(groups_) * kParamSignals * 8);
  gainBuf_ = AlignedBuffer<float>(static_cast<size_t>(groups_) * groupStride_);
  outBuf_ = AlignedBuffer<float>(std::max<size_t>(1, static_cast<size_t>(groups_) * numRequested * groupStride_));
  mixBuf_ = AlignedBuffer<float>(std::max<size_t>(1, mixSlots_.size() * stride_));
  sigPtr_.assign(static_cast<size_t>(groups_) * kVoiceSignalCount, nullptr);
  outPtr_.assign(std::max<size_t>(1, static_cast<size_t>(groups_) * numOutSlots_), nullptr);
  gio_.assign(groups_, VoiceGroupIO{});
  for (uint32_t g = 0; g < groups_; ++g) {
    float* a = audioSig_.data() + static_cast<size_t>(g) * kAudioSignals * groupStride_;
    float* p = paramSig_.data() + static_cast<size_t>(g) * kParamSignals * 8;
    const float** sp = sigPtr_.data() + static_cast<size_t>(g) * kVoiceSignalCount;
    sp[static_cast<uint32_t>(VoiceSignal::Hz)] = a;
    sp[static_cast<uint32_t>(VoiceSignal::Cents)] = a + groupStride_;
    sp[static_cast<uint32_t>(VoiceSignal::Gate)] = a + 2 * groupStride_;
    sp[static_cast<uint32_t>(VoiceSignal::Trigger)] = a + 3 * groupStride_;
    sp[static_cast<uint32_t>(VoiceSignal::Velocity)] = p;
    sp[static_cast<uint32_t>(VoiceSignal::Release)] = p + 8;
    sp[static_cast<uint32_t>(VoiceSignal::Ratio)] = p + 16;
    sp[static_cast<uint32_t>(VoiceSignal::Harmonic)] = p + 24;
    sp[static_cast<uint32_t>(VoiceSignal::UnisonCount)] = p + 32;
    sp[static_cast<uint32_t>(VoiceSignal::Index)] = p + 40;
    for (uint32_t s = 0; s < numOutSlots_; ++s)
      if (requested[s])
        outPtr_[static_cast<size_t>(g) * numOutSlots_ + s] =
            outBuf_.data() + (static_cast<size_t>(g) * numRequested + outIndex[s]) * groupStride_;
    VoiceGroupIO& gi = gio_[g];
    gi.signals = sp;
    gi.out = numOutSlots_ ? outPtr_.data() + static_cast<size_t>(g) * numOutSlots_ : nullptr;
    gi.numOut = numOutSlots_;
  }
  table_ = VoiceTable{};
  table_.groups = gio_.data();

  // Lane reset: per voice state entry, the words that belong to one lane (word index ≡ lane mod 8 inside the
  // 32-byte-aligned entry), minus shared mutable words (noise counter, delay-line header). Template = StateInit.
  const graph::StateLayout& layout = image->stateLayout();
  const StateArena arena = image->stateArena();
  stateTemplate_ = AlignedBuffer<uint32_t>(std::max<size_t>(8, arena.voiceStride / 4));
  voiceState_ = arena.voice;  // fixed for the image's lifetime (allocated when it was built)
  voiceStateStride_ = arena.voiceStride;
  resetSpans_.clear();
  moveSpans_.clear();
  delays_.clear();
  for (const graph::StateEntry& e : layout.entries) {
    if (e.arena != graph::Poly::Voice || e.size == 0 || e.tag.lanes != 8) continue;
    if (e.offset + e.size > arena.voiceStride) return bad("state entry outside the voice block");
    // Which words are per lane (pq/dsp/atom_state.h): every field is an [8] array except the noise counter, the
    // delay-line header and the phasor's incScale/halfSr (shared by the group's lanes).
    uint32_t skip = 0, size = e.size;
    if (e.tag.kernel != graph::StateTag::kSolveGuess) {
      const auto atom = static_cast<dsp::Atom>(e.tag.kernel);
      dsp::initState(atom, reinterpret_cast<std::byte*>(stateTemplate_.data()) + e.offset, e.init);
      if (atom == dsp::Atom::Noise) continue;  // per-lane seeds are constant; the counter is shared
      if (isDelayAtom(atom)) skip = 32;        // capacity, mask, writePos, lanes, maxDelay: shared
      if (atom == dsp::Atom::Phasor || atom == dsp::Atom::SineHz) size = 32;  // phase[8] only
    }
    if (size <= skip) continue;
    auto addSpan = [](std::vector<ResetSpan>& v, ResetSpan sp) {
      if (!v.empty() && v.back().firstWord + v.back().numWords == sp.firstWord)
        v.back().numWords += sp.numWords;
      else
        v.push_back(sp);
    };
    addSpan(resetSpans_, {(e.offset + skip) / 4, (size - skip) / 4});
    if (skip == 0) {
      addSpan(moveSpans_, {e.offset / 4, size / 4});
    } else {  // a delay line: moving a voice re-aligns its ring to the destination group's write position
      const uint32_t* h = stateTemplate_.data() + e.offset / 4;
      if (h[3] != 8 || h[0] == 0 || (h[0] & (h[0] - 1)) != 0 || e.size < 64 + h[0] * 32)
        return bad("unexpected voice delay-line state layout");
      delays_.push_back({e.offset / 4, h[0]});
    }
  }

  // Per lane and group state.
  status_.assign(lanes_, LaneStatus::Free);
  noteId_.assign(lanes_, 0);
  startTime_.assign(lanes_, 0);
  releaseTime_.assign(lanes_, 0);
  fadeEnd_.assign(lanes_, 0);
  noteHz_.assign(lanes_, 0.0f);
  level_.assign(lanes_, 0.0f);
  silentFrames_.assign(lanes_, 0);
  hasPending_.assign(lanes_, 0);
  pending_.assign(lanes_, NoteEvent{});
  hz_.assign(lanes_, 0.0);
  cents_.assign(lanes_, kIdleCents);
  hzMul_.assign(lanes_, 1.0);
  centsStep_.assign(lanes_, 0.0);
  hzTarget_.assign(lanes_, 0.0f);
  centsTarget_.assign(lanes_, 0.0);
  rampLeft_.assign(lanes_, 0);
  gateMask_.assign(lanes_, 0.0f);
  laneGain_.assign(lanes_, 0.0f);
  fadeStep_.assign(lanes_, 0.0f);
  velocity_.assign(lanes_, 0.0f);
  release_.assign(lanes_, 0.0f);
  ratio_.assign(lanes_, 1.0f);
  harmonic_.assign(lanes_, 0.0f);
  unison_.assign(lanes_, 1.0f);
  index_.assign(lanes_, 0.0f);
  validFrames_.assign(groups_, 0);
  groupDirty_.assign(groups_, 0);
  carryDirty_.assign(groups_, 1);
  groupCount_.assign(groups_, 0);
  laneMask_.assign(groups_, 0);
  activeList_.assign(groups_, 0);
  mixOut_.assign(groups_, nullptr);
  mixGain_.assign(groups_, nullptr);
  const size_t opCap = 2 * static_cast<size_t>(cfg_.maxEventsPerBlock) + 3 * static_cast<size_t>(lanes_);
  ops_.assign(opCap, LaneOp{});
  sortedOps_.assign(opCap, LaneOp{});
  opBegin_.assign(lanes_ + 1, 0);
  numOps_ = 0;
  clock_ = 0;
  live_ = 0;
  lastLane_ = kNoVoice;
  numReady_ = 0;
  opsLastBlock_ = true;  // opBegin_ starts zeroed below anyway
  groupsRun_ = 0;
  stats_ = VoiceStats{};
  return Ok{};
}

uint32_t VoiceEngine::laneOf(uint32_t noteId) const noexcept {  // RT-safe
  for (uint32_t l = 0; l < lanes_; ++l)
    if (noteId_[l] == noteId && (status_[l] == LaneStatus::Held || status_[l] == LaneStatus::Released ||
                                 status_[l] == LaneStatus::Fading))
      return l;
  return kNoSlot;
}

uint32_t VoiceEngine::activeGroups() const noexcept {  // RT-safe
  uint32_t n = 0;
  for (uint32_t g = 0; g < groups_; ++g) n += laneMask_[g] != 0 ? 1u : 0u;
  return n;
}

void VoiceEngine::setStatus(uint32_t lane, LaneStatus s) noexcept {  // RT-safe
  const uint32_t g = lane >> 3, bit = 1u << (lane & 7u);
  const bool wasFree = status_[lane] == LaneStatus::Free, isFree = s == LaneStatus::Free;
  status_[lane] = s;
  if (wasFree && !isFree) {
    ++groupCount_[g];
    laneMask_[g] |= bit;
  } else if (!wasFree && isFree) {
    --groupCount_[g];
    laneMask_[g] &= ~bit;
  }
}

void VoiceEngine::addOp(uint32_t frame, uint32_t lane, OpKind kind, const NoteEvent& e) noexcept {  // RT-safe
  LaneOp& op = ops_[numOps_++];  // capacity checked per event in allocate()
  op.frame = frame;
  op.lane = static_cast<uint16_t>(lane);
  op.kind = kind;
  op.ev = e;
  groupDirty_[lane >> 3] = 1;
}

// Best fit: the fullest occupied group with a free lane, else the lowest empty group.
uint32_t VoiceEngine::findFreeLane() const noexcept {  // RT-safe
  uint32_t best = kNoSlot, bestCount = 0;
  for (uint32_t g = 0; g < groups_; ++g) {
    const uint32_t c = groupCount_[g];
    if (c > 0 && c < 8 && c > bestCount) {
      best = g;
      bestCount = c;
    }
  }
  if (best == kNoSlot)
    for (uint32_t g = 0; g < groups_; ++g)
      if (groupCount_[g] == 0) {
        best = g;
        break;
      }
  if (best == kNoSlot) return kNoSlot;
  const uint32_t freeBits = ~laneMask_[best] & 0xFFu;
  return best * 8 + static_cast<uint32_t>(std::countr_zero(freeBits));
}

uint32_t VoiceEngine::pickVictim() const noexcept {  // RT-safe
  bool anyReleased = false;
  for (uint32_t l = 0; l < lanes_ && !anyReleased; ++l) anyReleased = status_[l] == LaneStatus::Released;
  const LaneStatus want = anyReleased ? LaneStatus::Released : LaneStatus::Held;
  uint32_t best = kNoSlot;
  for (uint32_t l = 0; l < lanes_; ++l) {
    if (status_[l] != want) continue;
    if (best == kNoSlot) {
      best = l;
      continue;
    }
    if (cfg_.steal == StealPolicy::Quietest) {
      if (level_[l] < level_[best] || (level_[l] == level_[best] && startTime_[l] < startTime_[best])) best = l;
    } else if (startTime_[l] < startTime_[best]) {
      best = l;
    }
  }
  return best;
}

void VoiceEngine::onNoteOn(const NoteEvent& e, uint32_t frame) noexcept {  // RT-safe
  ++stats_.noteOns;
  if (!(e.hz > 0.0f) || !std::isfinite(e.hz)) {
    ++stats_.droppedNotes;
    return;
  }
  const int64_t t = clock_ + frame;
  if (cfg_.retriggerSameNote) {
    const uint32_t bits = std::bit_cast<uint32_t>(e.hz);
    for (uint32_t l = 0; l < lanes_; ++l) {
      if ((status_[l] != LaneStatus::Held && status_[l] != LaneStatus::Released) ||
          std::bit_cast<uint32_t>(noteHz_[l]) != bits)
        continue;
      addOp(frame, l, OpKind::Retrigger, e);
      setStatus(l, LaneStatus::Held);
      noteId_[l] = e.noteId;
      startTime_[l] = t;
      silentFrames_[l] = 0;
      lastLane_ = l;
      ++stats_.retriggers;
      return;
    }
  }
  uint32_t victim = kNoSlot;
  if (live_ >= cfg_.maxVoices) {
    if (cfg_.steal == StealPolicy::None) {
      ++stats_.droppedNotes;
      return;
    }
    victim = pickVictim();
    if (victim != kNoSlot) {
      addOp(frame, victim, OpKind::Fade, e);
      setStatus(victim, LaneStatus::Fading);
      fadeEnd_[victim] = t + fadeFrames_;
      --live_;
      ++stats_.steals;
    }
  }
  const uint32_t lane = findFreeLane();
  if (lane == kNoSlot) {
    // No free lane: the note waits in a fading lane (the victim, else the one whose fade ends first, preferring
    // lanes without a waiting note). A waiting note counts as live (it holds its place in the polyphony).
    uint32_t target = victim;
    for (int pass = 0; pass < 2 && target == kNoSlot; ++pass)
      for (uint32_t l = 0; l < lanes_; ++l)
        if (status_[l] == LaneStatus::Fading && (pass == 1 || !hasPending_[l]) &&
            (target == kNoSlot || fadeEnd_[l] < fadeEnd_[target]))
          target = l;
    if (target == kNoSlot) {
      ++stats_.droppedNotes;
      return;
    }
    if (hasPending_[target]) {  // replaced
      ++stats_.droppedNotes;
      --live_;
    }
    pending_[target] = e;
    hasPending_[target] = 1;
    ++live_;
    ++stats_.deferredStarts;
    return;
  }
  addOp(frame, lane, OpKind::Start, e);
  setStatus(lane, LaneStatus::Held);
  noteId_[lane] = e.noteId;
  noteHz_[lane] = e.hz;
  startTime_[lane] = t;
  silentFrames_[lane] = 0;
  level_[lane] = 0.0f;
  if (cfg_.resetOnNoteOn) resetLaneState(lane);  // before this block's image run
  lastLane_ = lane;
  ++live_;
}

void VoiceEngine::allocate(const EventList* events, uint32_t frames) noexcept {  // RT-safe
  // Deferred notes whose lane's fade ended last block start at frame 0.
  for (uint32_t l = 0; l < lanes_ && numReady_ > 0; ++l) {
    if (status_[l] != LaneStatus::Pending) continue;
    --numReady_;
    const NoteEvent e = pending_[l];
    hasPending_[l] = 0;
    addOp(0, l, OpKind::Start, e);
    setStatus(l, LaneStatus::Held);
    noteId_[l] = e.noteId;
    noteHz_[l] = e.hz;
    startTime_[l] = clock_;
    silentFrames_[l] = 0;
    level_[l] = 0.0f;
    if (cfg_.resetOnNoteOn) resetLaneState(l);
    lastLane_ = l;  // already counted in live_ while it waited
  }
  if (events == nullptr || events->events == nullptr) return;
  const uint32_t n = events->count;
  for (uint32_t i = 0; i < n; ++i) {
    const NoteEvent& e = events->events[i];
    const bool all = e.kind == NoteEventKind::AllNotesOff || e.kind == NoteEventKind::AllSoundOff;
    if (i >= cfg_.maxEventsPerBlock || numOps_ + (all ? lanes_ : 2u) > ops_.size()) {
      ++stats_.droppedEvents;
      continue;
    }
    uint32_t frame = e.frame;
    if (frame >= frames) {
      frame = frames - 1;
      ++stats_.lateEvents;
    }
    switch (e.kind) {
      case NoteEventKind::NoteOn: onNoteOn(e, frame); break;
      case NoteEventKind::NoteOff: {
        ++stats_.noteOffs;
        uint32_t lane = kNoSlot;
        for (uint32_t l = 0; l < lanes_ && lane == kNoSlot; ++l)
          if (status_[l] == LaneStatus::Held && noteId_[l] == e.noteId) lane = l;
        if (lane != kNoSlot) {
          addOp(frame, lane, OpKind::Release, e);
          setStatus(lane, LaneStatus::Released);
          releaseTime_[lane] = clock_ + frame;
          silentFrames_[lane] = 0;
          break;
        }
        bool found = false;
        for (uint32_t l = 0; l < lanes_ && !found; ++l)
          if (hasPending_[l] && pending_[l].noteId == e.noteId) {  // released before it could start
            hasPending_[l] = 0;
            --live_;
            ++stats_.droppedNotes;
            found = true;
          }
        if (!found) ++stats_.unmatchedEvents;
        break;
      }
      case NoteEventKind::Pitch: {
        ++stats_.pitchEvents;
        if (!(e.hz > 0.0f) || !std::isfinite(e.hz)) {
          ++stats_.unmatchedEvents;
          break;
        }
        uint32_t lane = kNoSlot;
        for (uint32_t l = 0; l < lanes_ && lane == kNoSlot; ++l)
          if ((status_[l] == LaneStatus::Held || status_[l] == LaneStatus::Released) && noteId_[l] == e.noteId) lane = l;
        if (lane != kNoSlot) {
          addOp(frame, lane, OpKind::Pitch, e);
          noteHz_[lane] = e.hz;
          break;
        }
        bool found = false;
        for (uint32_t l = 0; l < lanes_ && !found; ++l)
          if (hasPending_[l] && pending_[l].noteId == e.noteId) {
            pending_[l].hz = e.hz;
            found = true;
          }
        if (!found) ++stats_.unmatchedEvents;
        break;
      }
      case NoteEventKind::AllNotesOff:
        for (uint32_t l = 0; l < lanes_; ++l) {
          if (status_[l] != LaneStatus::Held) continue;
          addOp(frame, l, OpKind::Release, e);
          setStatus(l, LaneStatus::Released);
          releaseTime_[l] = clock_ + frame;
          silentFrames_[l] = 0;
        }
        break;
      case NoteEventKind::AllSoundOff:
        for (uint32_t l = 0; l < lanes_; ++l) {
          if (hasPending_[l]) {
            hasPending_[l] = 0;
            --live_;
          }
          if (status_[l] != LaneStatus::Held && status_[l] != LaneStatus::Released) continue;
          addOp(frame, l, OpKind::Fade, e);
          setStatus(l, LaneStatus::Fading);
          fadeEnd_[l] = clock_ + frame + fadeFrames_;
          --live_;
        }
        break;
    }
  }
}

void VoiceEngine::resetLaneState(uint32_t lane) noexcept {  // RT-safe
  if (voiceState_ == nullptr) return;
  uint32_t* block = reinterpret_cast<uint32_t*>(voiceState_ + static_cast<size_t>(lane >> 3) * voiceStateStride_);
  const uint32_t* tmpl = stateTemplate_.data();
  const uint32_t l = lane & 7u;
  for (const ResetSpan& sp : resetSpans_)
    for (uint32_t w = sp.firstWord + l; w < sp.firstWord + sp.numWords; w += 8) block[w] = tmpl[w];
}

// Writes lane `lane`'s hz, cents, gate and gain for frames [from, to), advancing glides and fades. A constant
// field equal (bit for bit) to the block's vector fill (`fill`: hz, cents, gate, gain) is already in place.
void VoiceEngine::renderSegment(uint32_t lane, uint32_t from, uint32_t to, const uint32_t* fill) noexcept {  // RT-safe
  if (from >= to) return;
  auto same = [&](float v, int k) { return std::bit_cast<uint32_t>(v) == fill[k]; };
  const uint32_t g = lane >> 3, l = lane & 7u;
  float* a = audioSig_.data() + static_cast<size_t>(g) * kAudioSignals * groupStride_;
  float* hz = a;
  float* cents = a + groupStride_;
  float* gate = a + 2 * groupStride_;
  float* gain = gainBuf_.data() + static_cast<size_t>(g) * groupStride_;
  if (rampLeft_[lane] == 0) {
    const float h = static_cast<float>(hz_[lane]), c = static_cast<float>(cents_[lane]);
    if (!same(h, 0))
      for (uint32_t f = from; f < to; ++f) hz[f * 8 + l] = h;
    if (!same(c, 1))
      for (uint32_t f = from; f < to; ++f) cents[f * 8 + l] = c;
  } else {
    double h = hz_[lane], c = cents_[lane];
    uint32_t left = rampLeft_[lane];
    for (uint32_t f = from; f < to; ++f) {
      hz[f * 8 + l] = static_cast<float>(h);
      cents[f * 8 + l] = static_cast<float>(c);
      if (left > 0) {
        if (--left == 0) {
          h = hzTarget_[lane];
          c = centsTarget_[lane];
        } else {
          h *= hzMul_[lane];
          c += centsStep_[lane];
        }
      }
    }
    hz_[lane] = h;
    cents_[lane] = c;
    rampLeft_[lane] = left;
  }
  const float gm = gateMask_[lane];
  if (!same(gm, 2))
    for (uint32_t f = from; f < to; ++f) gate[f * 8 + l] = gm;
  if (fadeStep_[lane] > 0.0f) {
    float v = laneGain_[lane];
    const float step = fadeStep_[lane];
    for (uint32_t f = from; f < to; ++f) {
      gain[f * 8 + l] = v;
      v = v > step ? v - step : 0.0f;
    }
    laneGain_[lane] = v;
    if (v == 0.0f) fadeStep_[lane] = 0.0f;
  } else if (const float v = laneGain_[lane]; !same(v, 3)) {
    for (uint32_t f = from; f < to; ++f) gain[f * 8 + l] = v;
  }
}

void VoiceEngine::applyOp(const LaneOp& op) noexcept {  // RT-safe
  const uint32_t lane = op.lane;
  const NoteEvent& e = op.ev;
  switch (op.kind) {
    case OpKind::Start:
    case OpKind::Retrigger: {
      hz_[lane] = e.hz;
      cents_[lane] = 1200.0 * std::log2(static_cast<double>(e.hz) / cfg_.referenceHz);
      rampLeft_[lane] = 0;
      gateMask_[lane] = kTrueMask;
      laneGain_[lane] = 1.0f;
      fadeStep_[lane] = 0.0f;
      velocity_[lane] = e.velocity;
      release_[lane] = 0.0f;
      ratio_[lane] = e.ratio;
      harmonic_[lane] = e.harmonic;
      unison_[lane] = static_cast<float>(e.unisonCount);
      index_[lane] = static_cast<float>(e.unisonIndex);
      float* trig = audioSig_.data() + (static_cast<size_t>(lane >> 3) * kAudioSignals + 3) * groupStride_;
      trig[op.frame * 8 + (lane & 7u)] = kTrueMask;
      break;
    }
    case OpKind::Release:
      gateMask_[lane] = 0.0f;
      release_[lane] = e.velocity;
      break;
    case OpKind::Pitch: {
      const double target = e.hz;
      const double tc = 1200.0 * std::log2(target / cfg_.referenceHz);
      if (e.rampFrames == 0 || !(hz_[lane] > 0.0)) {
        hz_[lane] = target;
        cents_[lane] = tc;
        rampLeft_[lane] = 0;
      } else {
        const double r = static_cast<double>(e.rampFrames);
        hzMul_[lane] = std::pow(target / hz_[lane], 1.0 / r);
        centsStep_[lane] = (tc - cents_[lane]) / r;
        hzTarget_[lane] = e.hz;
        centsTarget_[lane] = tc;
        rampLeft_[lane] = e.rampFrames;
      }
      break;
    }
    case OpKind::Fade:
      fadeStep_[lane] = laneGain_[lane] / static_cast<float>(fadeFrames_);
      if (!(fadeStep_[lane] > 0.0f)) fadeStep_[lane] = 0.0f;
      break;
  }
}

void VoiceEngine::renderGroup(uint32_t g, uint32_t frames) noexcept {  // RT-safe
  float* a = audioSig_.data() + static_cast<size_t>(g) * kAudioSignals * groupStride_;
  float* gain = gainBuf_.data() + static_cast<size_t>(g) * groupStride_;
  const uint32_t l0 = g * 8;
  // 1. Every lane's start-of-block values over the whole block (final for lanes without changes).
  alignas(32) float hv[8], cv[8], gv[8], av[8];
  for (uint32_t l = 0; l < 8; ++l) {
    hv[l] = static_cast<float>(hz_[l0 + l]);
    cv[l] = static_cast<float>(cents_[l0 + l]);
    gv[l] = gateMask_[l0 + l];
    av[l] = laneGain_[l0 + l];
  }
  const __m256 h = _mm256_load_ps(hv), c = _mm256_load_ps(cv), gt = _mm256_load_ps(gv), gn = _mm256_load_ps(av);
  const __m256 z = _mm256_setzero_ps();
  for (uint32_t f = 0; f < frames; ++f) {
    _mm256_store_ps(a + f * 8, h);
    _mm256_store_ps(a + groupStride_ + f * 8, c);
    _mm256_store_ps(a + 2 * groupStride_ + f * 8, gt);
    _mm256_store_ps(a + 3 * groupStride_ + f * 8, z);
    _mm256_store_ps(gain + f * 8, gn);
  }
  // 2. Lanes with ops, glides or fades: segments between their op frames.
  bool changing = false;
  for (uint32_t l = 0; l < 8; ++l) {
    const uint32_t lane = l0 + l;
    const uint32_t b = opBegin_[lane], e = opBegin_[lane + 1];
    if (b == e && rampLeft_[lane] == 0 && fadeStep_[lane] == 0.0f) continue;
    changing = true;
    // Before its first op a lane without a glide or fade holds its start-of-block values: already written.
    uint32_t cursor = b < e && rampLeft_[lane] == 0 && fadeStep_[lane] == 0.0f ? sortedOps_[b].frame : 0u;
    const uint32_t fill[4] = {std::bit_cast<uint32_t>(hv[l]), std::bit_cast<uint32_t>(cv[l]),
                              std::bit_cast<uint32_t>(gv[l]), std::bit_cast<uint32_t>(av[l])};
    for (uint32_t i = b; i < e; ++i) {
      const LaneOp& op = sortedOps_[i];
      renderSegment(lane, cursor, op.frame, fill);
      applyOp(op);
      cursor = op.frame;
    }
    renderSegment(lane, cursor, frames, fill);
  }
  // 3. Param-rate signals: the lanes' latest values.
  float* p = paramSig_.data() + static_cast<size_t>(g) * kParamSignals * 8;
  for (uint32_t l = 0; l < 8; ++l) {
    p[l] = velocity_[l0 + l];
    p[8 + l] = release_[l0 + l];
    p[16 + l] = ratio_[l0 + l];
    p[24 + l] = harmonic_[l0 + l];
    p[32 + l] = unison_[l0 + l];
    p[40 + l] = index_[l0 + l];
  }
  // Constant over time only if nothing changed in this block and nothing keeps changing.
  bool moving = false;
  for (uint32_t l = 0; l < 8; ++l) moving = moving || rampLeft_[l0 + l] != 0 || fadeStep_[l0 + l] != 0.0f;
  validFrames_[g] = changing ? 0u : frames;
  if (changing || moving) carryDirty_[g] = 1;
}

// Moves the voice in lane a to the free lane b (other group): its state words, then the engine's per-lane state.
// Noise state stays with the group (the voice continues with the destination group's noise stream).
void VoiceEngine::moveLane(uint32_t a, uint32_t b) noexcept {  // RT-safe
  const uint32_t la = a & 7u, lb = b & 7u;
  if (voiceState_ != nullptr) {
    const uint32_t* src = reinterpret_cast<const uint32_t*>(voiceState_ + static_cast<size_t>(a >> 3) * voiceStateStride_);
    uint32_t* dst = reinterpret_cast<uint32_t*>(voiceState_ + static_cast<size_t>(b >> 3) * voiceStateStride_);
    for (const ResetSpan& sp : moveSpans_)
      for (uint32_t w = sp.firstWord; w < sp.firstWord + sp.numWords; w += 8) dst[w + lb] = src[w + la];
    for (const DelaySpan& d : delays_) {  // DelayState: header (writePos = word 2), apY1[8], ring[capacity][8]
      const uint32_t* s = src + d.firstWord;
      uint32_t* t = dst + d.firstWord;
      t[8 + lb] = s[8 + la];
      const uint32_t mask = d.capacity - 1, shift = (t[2] - s[2]) & mask;
      for (uint32_t i = 0; i < d.capacity; ++i) t[16 + ((i + shift) & mask) * 8 + lb] = s[16 + i * 8 + la];
    }
  }
  noteId_[b] = noteId_[a];
  startTime_[b] = startTime_[a];
  releaseTime_[b] = releaseTime_[a];
  fadeEnd_[b] = fadeEnd_[a];
  noteHz_[b] = noteHz_[a];
  level_[b] = level_[a];
  silentFrames_[b] = silentFrames_[a];
  hasPending_[b] = hasPending_[a];
  pending_[b] = pending_[a];
  hz_[b] = hz_[a];
  cents_[b] = cents_[a];
  hzMul_[b] = hzMul_[a];
  centsStep_[b] = centsStep_[a];
  hzTarget_[b] = hzTarget_[a];
  centsTarget_[b] = centsTarget_[a];
  rampLeft_[b] = rampLeft_[a];
  gateMask_[b] = gateMask_[a];
  laneGain_[b] = laneGain_[a];
  fadeStep_[b] = fadeStep_[a];
  velocity_[b] = velocity_[a];
  release_[b] = release_[a];
  ratio_[b] = ratio_[a];
  harmonic_[b] = harmonic_[a];
  unison_[b] = unison_[a];
  index_[b] = index_[a];
  setStatus(b, status_[a]);
  setStatus(a, LaneStatus::Free);
  hasPending_[a] = 0;
  hz_[a] = 0.0;
  cents_[a] = kIdleCents;
  rampLeft_[a] = 0;
  gateMask_[a] = 0.0f;
  laneGain_[a] = 0.0f;
  fadeStep_[a] = 0.0f;
  level_[a] = 0.0f;
  if (lastLane_ == a) lastLane_ = b;
  carryDirty_[a >> 3] = 1;
  carryDirty_[b >> 3] = 1;
  ++stats_.voiceMoves;
}

// When the occupied groups exceed ceil(occupied lanes / 8), empties the least occupied group into free lanes of the
// fullest other groups (they always have room then). At most one group per block.
void VoiceEngine::compact() noexcept {  // RT-safe
  uint32_t occupied = 0, used = 0, src = kNoSlot;
  for (uint32_t g = 0; g < groups_; ++g) {
    if (laneMask_[g] == 0) continue;
    ++occupied;
    used += groupCount_[g];
    if (src == kNoSlot || groupCount_[g] <= groupCount_[src]) src = g;
  }
  if (occupied <= (used + 7) / 8) return;
  for (uint32_t m = laneMask_[src]; m != 0; m &= m - 1) {
    uint32_t dst = kNoSlot;
    for (uint32_t g = 0; g < groups_; ++g)
      if (g != src && laneMask_[g] != 0 && groupCount_[g] < 8 && (dst == kNoSlot || groupCount_[g] > groupCount_[dst])) dst = g;
    if (dst == kNoSlot) return;  // cannot happen (see above); stay consistent if it does
    moveLane(src * 8 + static_cast<uint32_t>(std::countr_zero(m)),
             dst * 8 + static_cast<uint32_t>(std::countr_zero(~laneMask_[dst] & 0xFFu)));
  }
}

void VoiceEngine::freeLane(uint32_t lane) noexcept {  // RT-safe
  const LaneStatus s = status_[lane];
  if (s == LaneStatus::Held || s == LaneStatus::Released) --live_;
  ++stats_.voicesEnded;
  if (s == LaneStatus::Fading && hasPending_[lane]) {
    setStatus(lane, LaneStatus::Pending);  // starts at frame 0 of the next block
    ++numReady_;
  } else {
    hasPending_[lane] = 0;
    setStatus(lane, LaneStatus::Free);
  }
  hz_[lane] = 0.0;
  cents_[lane] = kIdleCents;
  rampLeft_[lane] = 0;
  gateMask_[lane] = 0.0f;
  laneGain_[lane] = 0.0f;
  fadeStep_[lane] = 0.0f;
  level_[lane] = 0.0f;
  carryDirty_[lane >> 3] = 1;
}

void VoiceEngine::mixAndMeasure(uint32_t frames) noexcept {  // RT-safe
  uint32_t* act = activeList_.data();
  uint32_t na = 0;
  for (uint32_t g = 0; g < groups_; ++g)
    if (laneMask_[g] != 0) act[na++] = g;
  groupsRun_ = na;
  for (uint32_t i = 0; i < mixSlots_.size(); ++i) {
    float* dst = mixBuf_.data() + static_cast<size_t>(i) * stride_;
    const uint32_t slot = mixSlots_[i];
    if (na == 0) {
      std::memset(dst, 0, sizeof(float) * frames);
      continue;
    }
    for (uint32_t k = 0; k < na; ++k) {
      mixOut_[k] = outPtr_[static_cast<size_t>(act[k]) * numOutSlots_ + slot];
      mixGain_[k] = gainBuf_.data() + static_cast<size_t>(act[k]) * groupStride_;
    }
    uint32_t f = 0;
    for (; f + 8 <= frames; f += 8) {  // 8 frames: Σ over groups per frame, then one transposed horizontal sum
      __m256 acc[8];
      for (uint32_t j = 0; j < 8; ++j) acc[j] = _mm256_setzero_ps();
      for (uint32_t k = 0; k < na; ++k) {
        const float* o = mixOut_[k] + f * 8;
        const float* gn = mixGain_[k] + f * 8;
        for (uint32_t j = 0; j < 8; ++j)
          acc[j] = _mm256_fmadd_ps(_mm256_load_ps(o + j * 8), _mm256_load_ps(gn + j * 8), acc[j]);
      }
      const __m256 t0 = _mm256_hadd_ps(acc[0], acc[1]), t1 = _mm256_hadd_ps(acc[2], acc[3]);
      const __m256 t2 = _mm256_hadd_ps(acc[4], acc[5]), t3 = _mm256_hadd_ps(acc[6], acc[7]);
      const __m256 u0 = _mm256_hadd_ps(t0, t1), u1 = _mm256_hadd_ps(t2, t3);
      _mm256_storeu_ps(dst + f, _mm256_add_ps(_mm256_permute2f128_ps(u0, u1, 0x20), _mm256_permute2f128_ps(u0, u1, 0x31)));
    }
    for (; f < frames; ++f) {
      __m256 acc = _mm256_setzero_ps();
      for (uint32_t k = 0; k < na; ++k)
        acc = _mm256_fmadd_ps(_mm256_load_ps(mixOut_[k] + f * 8), _mm256_load_ps(mixGain_[k] + f * 8), acc);
      dst[f] = horizontalSum(acc);
    }
  }
  // Per-lane peaks of the monitored outputs: for silence detection (groups with a released voice) and quietest
  // stealing (every group).
  if (monitorSlots_.empty()) return;
  const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
  const bool all = cfg_.steal == StealPolicy::Quietest;
  for (uint32_t k = 0; k < na; ++k) {
    const uint32_t g = act[k];
    bool released = all;
    for (uint32_t l = 0; l < 8 && !released; ++l) released = status_[g * 8 + l] == LaneStatus::Released;
    if (!released) continue;
    __m256 peak = _mm256_setzero_ps();
    for (uint32_t slot : monitorSlots_) {
      const float* o = outPtr_[static_cast<size_t>(g) * numOutSlots_ + slot];
      for (uint32_t f = 0; f < frames; ++f) peak = _mm256_max_ps(peak, _mm256_and_ps(_mm256_load_ps(o + f * 8), absMask));
    }
    _mm256_storeu_ps(level_.data() + g * 8, peak);
  }
}

void VoiceEngine::endVoices(uint32_t frames) noexcept {  // RT-safe
  const int64_t end = clock_ + frames;
  const bool monitored = !monitorSlots_.empty();
  for (uint32_t g = 0; g < groups_; ++g) {
    if (laneMask_[g] == 0) continue;
    const float* done = cfg_.doneSlot != kNoSlot ? outPtr_[static_cast<size_t>(g) * numOutSlots_ + cfg_.doneSlot] : nullptr;
    for (uint32_t l = 0; l < 8; ++l) {
      const uint32_t lane = g * 8 + l;
      const LaneStatus s = status_[lane];
      if (s == LaneStatus::Free || s == LaneStatus::Pending) continue;
      bool finished = false;
      if (done != nullptr) {
        const float v = done[(frames - 1) * 8 + l];
        finished = std::bit_cast<uint32_t>(v) == 0xFFFFFFFFu || v > 0.5f;
      }
      if (s == LaneStatus::Fading) {
        finished = finished || fadeEnd_[lane] <= end;
      } else if (s == LaneStatus::Released && !finished) {
        if (monitored) {
          if (level_[lane] < silenceLevel_) {
            silentFrames_[lane] += frames;
            finished = silentFrames_[lane] >= silenceFrames_;
          } else {
            silentFrames_[lane] = 0;
          }
        } else if (done == nullptr) {
          finished = end - releaseTime_[lane] >= releaseTimeoutFrames_;
        }
      }
      if (finished) freeLane(lane);
    }
  }
}

void VoiceEngine::process(ProcessContext& ctx) noexcept {  // RT-safe
  const uint32_t frames = ctx.frames;
  if (image_ == nullptr || frames == 0) return;
  if (frames > maxBlock_) {  // the image refuses it (zeroed outputs, rejectedBlocks)
    ctx.voices = nullptr;
    image_->process(ctx);
    for (uint32_t i = 0; i < mixSlots_.size(); ++i) std::memset(mixBuf_.data() + static_cast<size_t>(i) * stride_, 0, sizeof(float) * maxBlock_);
    return;
  }
  numOps_ = 0;
  for (uint32_t g = 0; g < groups_; ++g) {
    groupDirty_[g] = carryDirty_[g];
    carryDirty_[g] = 0;
  }
  allocate(ctx.events, frames);
  // Bucket the ops per lane (stable: per-lane time order is kept). opBegin_ stays all zero while there are none.
  if (numOps_ > 0 || opsLastBlock_) {
    std::fill(opBegin_.begin(), opBegin_.end(), 0u);
    for (uint32_t i = 0; i < numOps_; ++i) ++opBegin_[ops_[i].lane + 1u];
    for (uint32_t l = 0; l < lanes_; ++l) opBegin_[l + 1] += opBegin_[l];
    for (uint32_t i = 0; i < numOps_; ++i) sortedOps_[opBegin_[ops_[i].lane]++] = ops_[i];
    for (uint32_t l = lanes_; l > 0; --l) opBegin_[l] = opBegin_[l - 1];
    opBegin_[0] = 0;
    opsLastBlock_ = numOps_ > 0;
  }
  uint32_t numGroups = 0;
  for (uint32_t g = 0; g < groups_; ++g) {
    if (laneMask_[g] == 0 && !groupDirty_[g]) {
      validFrames_[g] = 0;  // not run: its buffers are irrelevant until a voice starts (which makes it dirty)
    } else if (groupDirty_[g] || validFrames_[g] < frames) {
      renderGroup(g, frames);
    }
    gio_[g].laneMask = laneMask_[g];
    if (laneMask_[g] != 0) numGroups = g + 1;
  }
  table_.numGroups = numGroups;
  table_.lastGroup = lastLane_ != kNoVoice ? lastLane_ >> 3 : kNoVoice;
  table_.lastLane = lastLane_ != kNoVoice ? lastLane_ & 7u : 0u;
  ctx.voices = &table_;
  image_->process(ctx);
  mixAndMeasure(frames);
  endVoices(frames);
  if (cfg_.compact) compact();
  clock_ += frames;
  ++stats_.blocks;
}

}  // namespace pq::engine
