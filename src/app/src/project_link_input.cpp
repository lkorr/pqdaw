// T6.4: live input, the link's side (pq/app/project_link.h "Live input").
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/document/input_map.h"
#include "pq/document/lower.h"
#include "pq/document/pitch_resolve.h"

namespace pq::app {

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

void ProjectLink::publishLiveRoutes(const doc::Project& p, bool force) {  // UI thread
  engine::LiveRouteDesc desc;
  std::vector<uint64_t> key;
  std::unordered_map<uint32_t, LiveCacheEntry> next;
  auto visit = [&](const doc::TrackPath& path, const doc::Track& t) {
    const doc::Mixer& m = t.mixer;
    const bool listens = m.monitor == doc::MonitorMode::In || (m.monitor == doc::MonitorMode::Auto && m.arm);
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
    // The tables: cached on the definition the track's notes resolve in, its node table and its note input.
    const doc::EffectiveTuning eff = doc::effectiveTuningRef(p, path);
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
      auto map = doc::buildInputPitchMap(p, t.id, t.noteInput, std::move(play));
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
      // The computer keyboard: degrees 0 … 16 above each equave offset (a 12-TET key map counts semitones from key 60).
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
    desc.tracks.push_back(std::move(td));
    key.insert(key.end(), {t.id.value, slot, sources, m.midiIn.channel, reinterpret_cast<uintptr_t>(entry.def),
                           reinterpret_cast<uintptr_t>(entry.table), static_cast<uint64_t>(t.noteInput.mode),
                           static_cast<uint64_t>(t.noteInput.keyMap), signals.size()});
    for (const doc::TuningId s : signals) key.push_back(s.value);
    next.emplace(t.id.value, std::move(entry));
  };
  eachMidi(p.tracks, doc::TrackPath{}, visit);
  for (uint32_t i = 0; i < engine::kLiveSources; ++i) {
    desc.bendSemitones[i] = liveBend_[i];
    key.push_back(static_cast<uint64_t>(std::lround(liveBend_[i] * 1000.0f)));
  }
  liveCache_ = std::move(next);
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

}  // namespace pq::app
