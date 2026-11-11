// T6.4: live input, the link's side (pq/app/project_link.h "Live input").
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "pq/app/midi_capture.h"
#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/input_map.h"
#include "pq/document/lower.h"
#include "pq/document/pitch_resolve.h"
#include "pq/views/computer_keyboard.h"

namespace pq::app {

// KB.1: the views' keyboard sends (equave, step) in the engine table's range.
static_assert(views::ComputerKeyboard::kSteps == engine::kKeyboardSteps);
static_assert(views::ComputerKeyboard::kMinEquave == engine::kKeyboardMinEquave &&
              views::ComputerKeyboard::kMaxEquave == engine::kKeyboardMaxEquave);

namespace {

// A key's pitch as the engine plays it: the NoteDesc fields lowerTrack gives the same drawn note (project_link.cpp),
// which Timeline::setIdentity turns into the note-on's identity fields.
engine::LivePitch livePitch(const doc::InputPitch& ip, const std::vector<doc::TuningId>& signals) {
  engine::LivePitch lp;
  if (!ip.mapped || !(ip.hz > 0.0) || !std::isfinite(static_cast<float>(ip.hz))) return lp;
  lp.id = ip.id;
  lp.hz = static_cast<float>(ip.hz);
  if (ip.follow != doc::PitchFollow::Static) {
    const uint32_t node = doc::tuningNodeIndex(signals, ip.signal);
    if (node != 0) {
      lp.tuning = static_cast<float>(node);
      if (ip.follow == doc::PitchFollow::Degree) {
        lp.degree = static_cast<float>(ip.degree);
        lp.equave = static_cast<float>(ip.equave);
      }
    }
  }
  lp.noteDegree = ip.noteDegree;
  return lp;
}

// Every active MIDI track with its path (as project_link.cpp's forEachMidi: deactivated groups hide their members).
template <class F>
void eachMidi(const std::vector<doc::TrackPtr>& list, doc::TrackPath path, F& f) {
  for (uint32_t i = 0; i < list.size(); ++i) {
    const doc::Track& t = *list[i];
    if (!t.active || path.depth >= doc::kMaxTrackDepth) continue;
    doc::TrackPath here = path;
    here.index[here.depth++] = i;
    if (t.kind == doc::TrackKind::Midi) f(here, t);
    if (t.kind == doc::TrackKind::Group) eachMidi(t.children, here, f);
  }
}

}  // namespace

void ProjectLink::setLivePorts(std::vector<std::string> names, std::vector<float> bendSemitones) {  // UI thread
  for (uint32_t i = 0; i < engine::kLivePorts; ++i) {
    livePortNames_[i] = i < names.size() ? std::move(names[i]) : std::string();
    liveBend_[i] = i < bendSemitones.size() ? bendSemitones[i] : 0.0f;
  }
  liveRouteKey_.clear();  // the port indices may mean other ports now
  publishLiveRoutes(*current_, true);
}

Ticks ProjectLink::liveClipAt() const noexcept {  // UI thread (KB.2): as playhead() reports the transport
  const bool taken = report_.commands >= commandsPosted_;
  const bool playing = taken ? report_.playing || report_.countingIn : optimistic_.playing;
  if (playing) return taken ? report_.tick : optimistic_.tick;
  return ctx_ != nullptr ? ctx_->startMarker() : 0;
}

void ProjectLink::followLiveClips() {  // UI thread (KB.2)
  if (!current_) return;
  const doc::ClipRef e = ctx_ != nullptr ? ctx_->liveEditedClip() : doc::ClipRef{};
  const Ticks at = liveClipAt();
  if (e.track == liveEdited_.track && e.clip == liveEdited_.clip && at >= liveFrom_ && at < liveTo_) return;
  publishLiveRoutes(*current_, false);
}

void ProjectLink::publishLiveRoutes(const doc::Project& p, bool force) {  // UI thread
  // KB.2: each track's live clip, from the clip view's clip and the playhead / start marker.
  const doc::ClipRef edited = ctx_ != nullptr ? ctx_->liveEditedClip() : doc::ClipRef{};
  const Ticks at = liveClipAt();
  Ticks spanFrom = std::numeric_limits<Ticks>::min(), spanTo = std::numeric_limits<Ticks>::max();
  engine::LiveRouteDesc desc;
  std::vector<uint64_t> key;
  std::unordered_map<uint32_t, LiveCacheEntry> next;
  auto visit = [&](const doc::TrackPath& path, const doc::Track& t) {
    const doc::Mixer& m = t.mixer;
    const bool listens = m.monitor == doc::MonitorMode::In || (m.monitor == doc::MonitorMode::Auto && armed(t));
    if (!listens || m.midiIn.kind == doc::MidiInputKind::None) return;
    uint32_t sources = 0;
    if (m.midiIn.kind == doc::MidiInputKind::All) {
      sources = 1u << engine::kLiveKeyboard;
      for (uint32_t i = 0; i < engine::kLivePorts; ++i)
        if (!livePortNames_[i].empty()) sources |= 1u << i;
    } else if (m.midiIn.port == doc::kComputerKeyboardPort) {
      sources = 1u << engine::kLiveKeyboard;
    } else {
      for (uint32_t i = 0; i < engine::kLivePorts; ++i)
        if (livePortNames_[i] == m.midiIn.port) sources |= 1u << i;
    }
    if (sources == 0) return;  // its port is not open
    const uint32_t slot = slotForTrack(t.id);
    if (slot == kNoSlot) return;
    // The tables: cached on the definition its live clip's notes resolve in (KB.2), its node table and its note input.
    const doc::LiveClip live = doc::liveClip(t, edited.track == t.id ? edited.clip : doc::ClipId{}, at);
    spanFrom = std::max(spanFrom, live.from);
    spanTo = std::min(spanTo, live.to);
    const doc::EffectiveTuning eff = doc::effectiveTuningRef(p, path, live.clip);
    if (eff.def == nullptr) return;
    EntryPtr table;
    if (eff.main.valid() && doc::hasTuningSignal(*eff.def)) {
      std::lock_guard lock(mutex_);
      if (const EntryPtr* e = tuningEntry(eff.main, *eff.def); e != nullptr && (*e)->built) table = *e;
    }
    std::vector<doc::TuningId> signals = doc::trackTuningSignals(p, path);
    LiveCacheEntry entry;
    auto it = liveCache_.find(t.id.value);
    if (it != liveCache_.end() && it->second.def == eff.def && it->second.table == table.get() &&
        it->second.input == t.noteInput && it->second.signals == signals) {
      entry = std::move(it->second);
    } else {
      std::shared_ptr<const tuning::Tuning> play;
      if (table) play = std::shared_ptr<const tuning::Tuning>(table, &*table->built);  // held by the entry
      auto map = doc::buildInputPitchMap(p, t.id, t.noteInput, std::move(play), live.clip);
      if (!map.ok()) return;
      const doc::InputPitchMap& im = **map;
      entry.def = eff.def;
      entry.table = table.get();
      entry.input = t.noteInput;
      entry.signals = signals;
      entry.from = current_;
      entry.held = table;
      entry.keys.reserve(doc::InputPitchMap::kKeys);
      for (uint32_t k = 0; k < doc::InputPitchMap::kKeys; ++k)
        entry.keys.push_back(livePitch(im.at(static_cast<uint8_t>(k)), signals));
      // The computer keyboard: degrees 0 … 35 above each equave offset (KB.1) (a 12-TET key map counts semitones from key 60).
      const bool twelve = t.noteInput.keyMap == doc::InputKeyMap::TwelveTetNearest ||
                          t.noteInput.keyMap == doc::InputKeyMap::TwelveTetHz;
      const int64_t size = twelve ? 12 : std::max<int64_t>(im.degreesPerEquave(), 1);
      entry.keyboard.reserve(engine::kKeyboardKeys);
      for (uint32_t i = 0; i < engine::kKeyboardKeys; ++i) {
        const auto k = static_cast<uint8_t>(i);
        const int64_t d = int64_t{engine::keyboardEquave(k)} * size + engine::keyboardStep(k);
        entry.keyboard.push_back(livePitch(im.pitchForDegree(d), signals));
      }
    }
    engine::LiveTrackDesc td;
    td.slot = slot;
    td.hzMode = t.noteInput.mode == doc::NoteInputMode::Hz;
    td.sources = sources;
    td.channel = m.midiIn.channel;
    td.keys = entry.keys;
    td.keyboard = entry.keyboard;
    // T6.9: Capture MIDI's ring, allocated the first time the track listens (here, off the audio thread).
    CaptureState& cs = capture_[t.id.value];
    if (!cs.ring) cs.ring = std::make_shared<engine::CaptureRing>(engine::kCaptureNotes);
    td.capture = cs.ring;
    desc.tracks.push_back(std::move(td));
    key.insert(key.end(), {t.id.value, slot, sources, m.midiIn.channel, reinterpret_cast<uintptr_t>(entry.def),
                           reinterpret_cast<uintptr_t>(entry.table), static_cast<uint64_t>(t.noteInput.mode),
                           static_cast<uint64_t>(t.noteInput.keyMap), signals.size(),
                           reinterpret_cast<uintptr_t>(cs.ring.get())});
    for (const doc::TuningId s : signals) key.push_back(s.value);
    next.emplace(t.id.value, std::move(entry));
  };
  eachMidi(p.tracks, doc::TrackPath{}, visit);
  // T6.9: a deleted track's ring goes (the engine lets go of it once the image without it is current).
  std::erase_if(capture_, [&](const auto& kv) { return doc::findTrackPtr(p, doc::TrackId{kv.first}) == nullptr; });
  for (uint32_t i = 0; i < engine::kLiveSources; ++i) {
    desc.bendSemitones[i] = liveBend_[i];
    key.push_back(static_cast<uint64_t>(std::lround(liveBend_[i] * 1000.0f)));
  }
  liveCache_ = std::move(next);
  liveEdited_ = edited;
  liveFrom_ = spanFrom;
  liveTo_ = spanTo;
  if (!force && key == liveRouteKey_ && liveEngine_ == engine_.get()) return;
  auto img = engine::LiveRouteImage::build(desc);
  if (!img.ok()) {
    setError("live input: " + img.error().message);
    return;
  }
  engine_->liveInput().publish(std::move(img.value()));
  engine_->liveInput().collect(engine_->audioStopped());
  liveRouteKey_ = std::move(key);
  liveEngine_ = engine_.get();
  liveTracks_ = static_cast<uint32_t>(desc.tracks.size());
  ++livePublishes_;
}

void ProjectLink::setAutoArm(uint32_t trackId) {  // UI thread (KB.1)
  if (trackId == autoArm_) return;
  autoArm_ = trackId;
  if (current_) publishLiveRoutes(*current_, false);  // the route key changes when the listening set does
}

bool ProjectLink::armed(const doc::Track& t) const noexcept {
  return t.mixer.arm || (autoArm_ != 0 && t.id.value == autoArm_ && t.kind == doc::TrackKind::Midi);
}

void ProjectLink::liveKey(int32_t equave, int32_t step, float velocity, bool on) {  // UI thread
  if (step < 0 || step >= engine::kKeyboardSteps) return;
  equave = std::clamp(equave, engine::kKeyboardMinEquave, engine::kKeyboardMaxEquave);
  engine::RawMidi m;
  m.stamp = static_cast<int64_t>(engine::qpcNow100ns());
  m.port = static_cast<uint8_t>(engine::kLiveKeyboard);
  m.status = on ? 0x90 : 0x80;
  m.d1 = engine::keyboardKey(equave, step);
  m.d2 = static_cast<uint8_t>(std::clamp(std::lround(velocity), on ? 1l : 0l, 127l));
  (void)livePorts_->push(engine::kLiveKeyboard, m);  // a full ring releases the keyboard's notes (no stuck note)
  engine_->wake();  // a sleeping engine runs the next block (and restarts a suspended stream)
}

// ---- T6.9: Capture MIDI ------------------------------------------------------------------------------------------

const engine::CaptureRing* ProjectLink::captureRing(doc::TrackId id) const {  // UI thread
  const auto it = capture_.find(id.value);
  return it == capture_.end() ? nullptr : it->second.ring.get();
}

void ProjectLink::panic() {  // UI thread (M6 review L16)
  if (engine_) engine_->panic();
}

bool ProjectLink::captureMidi(Ticks insertMarker) {  // UI thread
  doc::DocumentStore* store = ctx_ != nullptr ? &ctx_->store() : recordStore_;
  if (store == nullptr) return false;
  const views::PlayheadSnapshot ph = playhead();  // first: it may commit a recording session (a new snapshot)
  const bool playing = ph.playing && !ph.countingIn;
  const doc::Snapshot snap = store->current();
  const doc::Project& p = *snap;
  // The listening tracks (those the routes have a table for), each from its last capture on.
  struct Read {
    uint32_t id;
    uint64_t written;
    std::vector<engine::CapturedNote> notes;
  };
  std::vector<Read> reads;
  for (const auto& [id, entry] : liveCache_) {
    const auto it = capture_.find(id);
    const doc::Track* t = doc::findTrackPtr(p, doc::TrackId{id});
    if (it == capture_.end() || !it->second.ring || t == nullptr) continue;
    Read r{id, it->second.ring->written(), {}};
    it->second.ring->read(it->second.mark, r.notes);
    if (r.notes.empty()) continue;
    reads.push_back(std::move(r));
  }
  std::sort(reads.begin(), reads.end(), [](const Read& a, const Read& b) { return a.id < b.id; });  // a stable order
  std::vector<CaptureTrack> tracks;
  for (const Read& r : reads) tracks.push_back({doc::TrackId{r.id}, r.notes});
  CaptureTiming timing;
  timing.sampleRate = cfg_.sampleRate;
  timing.now = engine_->liveInput().engineNow();
  timing.noteBack = int64_t{engine_->liveInput().latency()} + engine_->recorder().outputLatency();
  timing.pdc = outputLatency();
  timing.playing = playing;
  timing.insertMarker = insertMarker;
  timing.loop = loopSent_ && loop_.enabled;
  timing.loopStart = loop_.start;
  timing.loopEnd = loop_.end;
  const CapturePlan plan = planCapture(p, tracks, timing);
  if (plan.clips.empty()) return false;
  auto e = doc::ops::captureClip(p, store->ids(), plan.clips, plan.bpm);
  if (!e.ok()) {
    setError("capture: " + e.error().message);
    return false;
  }
  if (Status st = store->dispatch(e->command); !st.ok()) {
    setError("capture: " + st.error().message);
    return false;
  }
  for (const Read& r : reads) capture_[r.id].mark = r.written;  // "since the last capture"
  return true;
}

}  // namespace pq::app
