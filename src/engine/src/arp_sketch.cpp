// AP.10: the resolved cycle of an arpeggiator (pq/engine/arp_sketch.h).
#include "pq/engine/arp_sketch.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "pq/core/note_stage_status.h"

namespace pq::engine {

double arpSketchHzAt(const ArpSketch& s, const ArpSketchNote& n, double t) noexcept {
  double hz = n.hz;
  for (uint32_t i = 0; i < n.numBends; ++i) {
    const ArpSketchBend& b = s.bends[n.firstBend + i];
    if (t < b.at) break;
    const ArpSketchBend* next = i + 1 < n.numBends ? &s.bends[n.firstBend + i + 1] : nullptr;
    // The value where this event starts (the running one's at that time), then toward its target.
    const double from = hz;
    const double end = next != nullptr ? std::min<double>(t, next->at) : t;
    if (b.len <= 0.0f || end >= b.at + b.len) {
      hz = b.hz;
    } else {
      const double s0 = rampShapeAt(b.shape, b.bend, (end - b.at) / b.len);
      hz = from * std::exp2(std::log2(static_cast<double>(b.hz) / from) * s0);
    }
    if (next == nullptr || t < next->at) break;
  }
  return hz;
}

ArpSketch sketchArp(const ArpParams& params, const ArpLaneSteps& lanes, const ArpTables& tables,
                    std::span<const ArpSketchHeld> held, const ArpSketchConfig& cfg) {
  ArpSketch out;
  if (held.empty()) return out;
  const ArpParams p = sanitized(params);
  const double sr = cfg.sampleRate > 0.0 ? cfg.sampleRate : 4800.0;
  const double bpm = cfg.bpm > 0.0 ? cfg.bpm : 120.0;
  // One step (a Rate period) as the free clock plays it (synced: num / den of a whole note × m / n at the tempo).
  const double stepSec = p.rate.synced ? 4.0 * 60.0 / bpm * static_cast<double>(p.rate.num) * p.rate.tupletM /
                                             (static_cast<double>(p.rate.den) * p.rate.tupletN)
                                       : static_cast<double>(p.rate.ms) * 1e-3;
  const double stepFrames = std::max(stepSec * sr, 1.0);
  out.stepMs = stepSec * 1e3;
  const auto block = static_cast<uint32_t>(
      std::clamp<double>(std::floor(stepFrames * 0.5), 1.0, static_cast<double>(std::max(cfg.blockFrames, 1u))));

  ArpCore core(0, tables);
  core.setLaneSteps(lanes);
  auto status = std::make_shared<NoteStageStatus>();
  core.setStatus(status);
  std::vector<ArpTraceNote> trace;
  core.setTrace(&trace);

  const auto n = static_cast<uint32_t>(std::min<size_t>(held.size(), kArpMaxHeld));
  EventBuffer in(n + 4), evs(8192);
  for (uint32_t i = 0; i < n; ++i) {
    const ArpSketchHeld& h = held[i];
    NoteEvent e = noteOn(0, i + 1, h.hz, h.velocity);
    e.ratio = h.ratio;
    e.harmonic = h.harmonic;
    e.degree = h.degree;
    e.equave = h.equave;
    e.tuning = h.tuning;
    e.noteDegree = h.noteDegree;
    in.push(e);
  }
  StageClock clock;
  clock.sampleRate = sr;
  clock.bpm = bpm;
  clock.beatsPerBar = cfg.beatsPerBar;
  NoteStageBlock b;
  b.clock = clock;
  b.frames = block;

  struct Open {
    uint32_t id = 0;
    size_t note = 0;
  };
  std::vector<Open> open;                         // notes not ended yet, by id
  std::vector<std::vector<ArpSketchBend>> bends;  // per note
  size_t traced = 0;
  double end = -1.0;  // frames: the cycle's end (known after the first block)
  uint64_t seenSteps = status->steps.load(std::memory_order_relaxed);
  const EventList none{};
  for (int64_t t = 0;; t += block) {
    b.time = t;
    evs.clear();
    (void)core.process(p, b, t == 0 ? in.list() : none, evs);
    const uint64_t st = status->steps.load(std::memory_order_relaxed);
    const int32_t pos = st != seenSteps ? status->position.load(std::memory_order_relaxed) : -1;
    const int32_t cp = status->copy.load(std::memory_order_relaxed);
    seenSteps = st;
    if (end < 0.0) {
      double c = core.cycleSteps();
      if (!(c > 0.0)) c = 1.0;
      if (c > static_cast<double>(cfg.maxSteps)) {
        c = static_cast<double>(cfg.maxSteps);
        out.truncated = true;
      }
      out.cycle = c;
      end = c * stepFrames;
    }
    const EventList& l = evs.list();
    for (uint32_t k = 0; k < l.count; ++k) {
      const NoteEvent& e = l.events[k];
      const double at = (static_cast<double>(t) + e.frame) / stepFrames;
      auto find = [&](uint32_t id) -> ArpSketchNote* {
        for (const Open& o : open)
          if (o.id == id) return &out.notes[o.note];
        return nullptr;
      };
      switch (e.kind) {
        case NoteEventKind::NoteOn: {
          if (static_cast<double>(t) + e.frame >= end) break;  // past the cycle
          ArpSketchNote nn;
          nn.on = nn.off = static_cast<float>(at);
          nn.hz = nn.ownHz = e.hz;
          nn.ratio = e.ratio;
          nn.harmonic = e.harmonic;
          nn.position = pos;
          nn.copy = cp;
          while (traced < trace.size() && trace[traced].id != e.noteId) ++traced;
          if (traced < trace.size()) {
            nn.source = trace[traced].source >= 1 ? trace[traced].source - 1 : 0;
            nn.exact = trace[traced].exact;
          }
          open.push_back({e.noteId, out.notes.size()});
          out.notes.push_back(nn);
          bends.emplace_back();
          break;
        }
        case NoteEventKind::Pitch:
          if (ArpSketchNote* nn = find(e.noteId)) {
            const auto i = static_cast<size_t>(nn - out.notes.data());
            ArpSketchBend bd;
            bd.at = static_cast<float>(at);
            bd.len = static_cast<float>(static_cast<double>(e.rampFrames) / stepFrames);
            bd.hz = e.hz;
            bd.shape = e.rampShape < static_cast<uint8_t>(RampShape::Count) ? static_cast<RampShape>(e.rampShape)
                                                                              : RampShape::Linear;
            bd.bend = static_cast<int8_t>(e.rampBend);
            if (bends[i].empty() && bd.at == nn->on && bd.len > 0.0f) nn->ownHz = bd.hz;  // the Into glide's target
            bends[i].push_back(bd);
          }
          break;
        case NoteEventKind::NoteOff:
          for (size_t o = 0; o < open.size(); ++o)
            if (open[o].id == e.noteId) {
              out.notes[open[o].note].off = static_cast<float>(at);
              open.erase(open.begin() + static_cast<std::ptrdiff_t>(o));
              break;
            }
          break;
        case NoteEventKind::AllNotesOff:
        case NoteEventKind::AllSoundOff:
          for (const Open& o : open) out.notes[o.note].off = static_cast<float>(at);
          open.clear();
          break;
        default: break;
      }
    }
    // Done: past the cycle with every note of it ended (or 2 steps more at most: a gate up to 200 %, a tie).
    const double now = static_cast<double>(t + block);
    if (now >= end && (open.empty() || now >= end + 2.5 * stepFrames)) {
      for (const Open& o : open) out.notes[o.note].off = static_cast<float>(now / stepFrames);
      break;
    }
    if (core.idle() && t > 0) break;  // Repeats ended
  }
  for (size_t i = 0; i < out.notes.size(); ++i) {
    out.notes[i].firstBend = static_cast<uint32_t>(out.bends.size());
    out.notes[i].numBends = static_cast<uint32_t>(bends[i].size());
    out.bends.insert(out.bends.end(), bends[i].begin(), bends[i].end());
  }
  return out;
}

}  // namespace pq::engine
