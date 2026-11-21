#include "pq/engine/note_stage_offline.h"

#include <algorithm>
#include <utility>

namespace pq::engine {

uint32_t timelineSegments(std::span<const TransportRun> runs, uint32_t frames, ClockSegment* segs) noexcept {  // RT-safe
  uint32_t n = 0, at = 0;
  for (const TransportRun& r : runs) {
    if (r.kind != TransportRun::Song || r.len == 0 || r.frame < at || r.frame + r.len > frames) continue;
    if (r.frame > at) segs[n++] = {at, r.frame - at, false, 0};
    segs[n++] = {r.frame, r.len, true, r.pos};
    at = r.frame + r.len;
  }
  if (at < frames) segs[n++] = {at, frames - at, false, 0};
  return n;
}

namespace {

// The song sample of block frame `f` (−1: in no song run).
SampleTime songAt(std::span<const TransportRun> runs, uint32_t f) noexcept {
  for (const TransportRun& r : runs)
    if (r.kind == TransportRun::Song && f >= r.frame && f < r.frame + r.len) return r.pos + (f - r.frame);
  return -1;
}

void append(std::vector<TimedNoteEvent>& to, const EventList& l, std::span<const TransportRun> runs) {
  for (uint32_t i = 0; i < l.count; ++i) to.push_back({songAt(runs, l.events[i].frame), l.events[i]});
}

}  // namespace

Result<NoteStageRender> renderNoteStages(std::unique_ptr<SequenceImage> sequence, std::unique_ptr<TempoImage> tempo,
                                         NoteStageChain& chain, std::span<const float> params, Ticks from,
                                         SampleTime frames, const NoteStageRenderConfig& config) {
  TimelineConfig tc = config.timeline;
  tc.sampleRate = config.sampleRate;
  tc.maxBlockFrames = std::max(config.blockFrames, 1u);
  tc.maxTracks = 1;
  tc.clipSlots = 1;
  tc.maxClipVoices = 1;
  tc.clipStreamBuffers = 1;
  tc.streams = nullptr;
  tc.sampleReclaimer = nullptr;
  Timeline tl(tc);
  if (Status s = tl.publishSequence(std::move(sequence)); !s) return s.error();
  if (tempo)
    if (Status s = tl.publishTempo(std::move(tempo)); !s) return s.error();
  TransportCommand play;
  play.kind = TransportCommandKind::Play;
  play.tick = from;
  tl.apply(play);
  NoteStageRender r;
  tl.setNoteLog(&r.notes);
  EventBuffer out(chain.input().capacity());
  ClockSegment segs[2 * Timeline::kMaxRuns + 1];
  int64_t time = 0;
  while (time < frames) {
    const auto n = static_cast<uint32_t>(std::min<SampleTime>(tc.maxBlockFrames, frames - time));
    // As Engine::onSlot: the chain's input is the slot's merged events (here the sequencer's alone).
    chain.input().clear();
    tl.process(time, n);
    const EventList seq = tl.events(0);
    for (uint32_t i = 0; i < seq.count; ++i) chain.input().push(seq.events[i]);
    const std::span<const TransportRun> runs = tl.runs();
    append(r.input, chain.input().list(), runs);
    // As Engine::runStages.
    NoteStageBlock b;
    b.time = time;
    b.frames = n;
    b.clock.tempo = &tl.tempoImage().tempo();
    b.clock.sampleRate = config.sampleRate;
    b.clock.bpm = tl.snapshot().bpm;
    b.clock.signatures = &tl.tempoImage().signatures();
    b.clock.beatsPerBar = tl.snapshot().beatsPerBar;
    b.clock.segments = segs;
    b.clock.numSegments = timelineSegments(runs, n, segs);
    b.params = params;
    out.clear();
    (void)chain.run(b, out);
    append(r.output, out.list(), runs);
    time += n;
  }
  tl.setNoteLog(nullptr);
  return r;
}

}  // namespace pq::engine
