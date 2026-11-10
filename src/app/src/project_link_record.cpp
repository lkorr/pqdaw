// T6.7: recording, the link's side (pq/app/project_link.h "Recording").
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>

#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/input_map.h"
#include "pq/document/samples.h"

namespace pq::app {

namespace {

std::string pathUtf8(const std::filesystem::path& p) {
  const std::u8string u = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string envUtf8(const wchar_t* name) {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return pathUtf8(std::filesystem::path(std::wstring(buf, n)));
}

// This process's unsaved-recordings folder (one per run).
const std::string& sessionName() {
  static const std::string name = [] {
    char b[64];
    std::snprintf(b, sizeof b, "%lu-%lld", static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<long long>(std::time(nullptr)));
    return std::string(b);
  }();
  return name;
}

// A file-name-safe version of a track name.
std::string safeName(std::string_view s) {
  std::string out;
  for (char c : s) out += std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || (c >= 0 && c < 32) ? '_' : c;
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? std::string("Track") : out;
}

std::string inputLabel(const doc::AudioInput& in) {
  return in.channels <= 1 ? "In " + std::to_string(in.firstChannel + 1)
                          : "In " + std::to_string(in.firstChannel + 1) + "-" + std::to_string(in.firstChannel + in.channels);
}

std::string stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H-%M-%S", &tm);
  return b;
}

std::filesystem::path u8path(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// The song sample a note heard at engine time `e` sat on, through the session's passes (the pass containing it; before
// the first, the first's law extrapolated; between passes, the earlier one's), and the index of that pass.
std::pair<int64_t, size_t> songAt(const std::vector<RecordingPass>& passes, int64_t e) {
  size_t k = 0;
  for (size_t i = 0; i < passes.size(); ++i)
    if (passes[i].engineStart <= e) k = i;
  const RecordingPass& p = passes[k];
  return {p.songStart + (e - p.engineStart), k};
}

}  // namespace

std::string ProjectLink::recordDir() const {
  const std::string pdir = ctx_ != nullptr ? ctx_->projectDir() : cfg_.projectDir;
  if (!pdir.empty()) return pathUtf8(u8path(pdir) / "samples" / "recorded");
  return unsavedRecordDir();
}

std::string ProjectLink::unsavedRecordDir() const {
  if (!cfg_.unsavedDir.empty()) return cfg_.unsavedDir;
  std::string base = envUtf8(L"LOCALAPPDATA");
  if (base.empty()) base = pathUtf8(std::filesystem::temp_directory_path());
  return pathUtf8(u8path(base) / "pqdaw" / "Unsaved" / sessionName() / "samples" / "recorded");
}

uint64_t ProjectLink::discardUnsavedRecordings() {  // UI thread
  if (recording_ && recording_->active()) {  // the session belongs to the discarded project: no commit
    if (playhead().recording) postTransport({engine::TransportCommandKind::RecordOff, false, 0, 0, 0});
    const RecordingResult dropped = recording_->finish();
    (void)dropped;
    if (inputUseFn_) inputUseFn_();
  }
  std::filesystem::path target, root;
  if (!cfg_.unsavedDir.empty()) {
    target = u8path(cfg_.unsavedDir).lexically_normal();
    if (!target.has_filename()) target = target.parent_path();  // a trailing separator
    root = target.parent_path();
  } else {
    std::string base = envUtf8(L"LOCALAPPDATA");
    if (base.empty()) base = pathUtf8(std::filesystem::temp_directory_path());
    root = u8path(base) / "pqdaw" / "Unsaved";
    target = root / sessionName();
  }
  std::string why;
  const uint64_t n = removeUnsavedFolder(target, root, &why);
  if (!why.empty()) setError("unsaved recordings: " + why);
  return n;
}

uint32_t ProjectLink::audioInputChannels() { return std::max(availableInputs_, engine_->recorder().deviceInputs()); }

void ProjectLink::setRecordOffsetMs(double ms) {  // UI thread
  recordOffsetMs_ = std::isfinite(ms) ? ms : 0.0;
  if (engine_) engine_->recorder().setManualOffset(static_cast<int32_t>(std::lround(recordOffsetMs_ * cfg_.sampleRate / 1000.0)));
}

std::vector<engine::UnitInput> ProjectLink::unitInputs(const doc::LoweredProject& lp) const {  // compile path
  const graph::ProjectPartition& part = compiler_->partition();
  std::vector<engine::UnitInput> in(part.units.size());
  for (const doc::AudioInputBinding& b : lp.audioInputs)
    for (uint32_t i = 0; i < lp.trackIds.size() && i < part.unitOf.size(); ++i) {
      if (lp.trackIds[i] != b.track) continue;
      const uint32_t u = part.unitOf[i];
      if (u < in.size() && in[u].channels == 0) in[u] = {b.input.firstChannel, b.input.channels};
    }
  return in;
}

void ProjectLink::syncRecordInputs(const doc::Project& p, bool force) {  // UI thread
  // The monitor gates of the audio tracks with an input, and the channels the armed ones record.
  auto img = std::make_shared<engine::MonitorImage>();
  std::vector<uint64_t> key;
  std::vector<uint16_t> channels;
  uint32_t used = 0;  // T6.7b
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (t.kind != doc::TrackKind::Audio || t.mixer.audioIn.none()) return;
    used = std::max<uint32_t>(used, uint32_t{t.mixer.audioIn.firstChannel} + t.mixer.audioIn.channels);
    const uint32_t param = paramSlot(std::to_string(t.id.value) + ":monitor");
    if (param == kNoSlot) return;
    engine::MonitorEntry e;
    e.param = param;
    e.mode = static_cast<engine::MonitorEntry::Mode>(t.mixer.monitor);
    e.armed = t.mixer.arm;
    e.clipSlot = slotForTrack(t.id);
    img->entries.push_back(e);
    key.insert(key.end(), {param, static_cast<uint64_t>(e.mode), e.armed ? 1u : 0u, e.clipSlot});
    if (t.mixer.arm && t.active)
      for (uint32_t c = 0; c < t.mixer.audioIn.channels; ++c)
        channels.push_back(static_cast<uint16_t>(t.mixer.audioIn.firstChannel + c));
  });
  if (force || key != monitorKey_) {
    engine_->recorder().setMonitors(img->entries.empty() ? nullptr : std::move(img));
    monitorKey_ = std::move(key);
    engine_->wake();  // a sleeping engine takes the gates (an armed input is heard at once)
  }
  std::sort(channels.begin(), channels.end());
  channels.erase(std::unique(channels.begin(), channels.end()), channels.end());
  recording_->prepareChannels(std::move(channels));
  engine_->recorder().collect(engine_->audioStopped());
  if (used != inputsUsed_) {  // T6.7b: the device manager opens what the tracks use
    inputsUsed_ = used;
    if (inputUseFn_) inputUseFn_();
  }
  if (force || !punchSent_ || *punchSent_ != p.punch) {
    engine::TransportCommand c{engine::TransportCommandKind::SetPunch, false, 0, 0, 0};
    c.punchEnabled = p.punch.enabled;
    c.punchIn = p.punch.in;
    c.punchOut = p.punch.out;
    postTransport(c);
    punchSent_ = p.punch;
  }
}

void ProjectLink::record(Ticks from) {  // UI thread
  const views::PlayheadSnapshot ph = playhead();
  if (recording_->active() || ph.recording) {  // Record again: recording ends, playback goes on (the commit follows)
    postTransport({engine::TransportCommandKind::RecordOff, false, 0, 0, 0});
    optimistic_ = ph;
    optimistic_.recording = false;
    return;
  }
  // The session: every armed track — an audio track with an input records a file, a MIDI track its live notes.
  std::vector<RecordingTrack> tracks;
  const std::filesystem::path dir = u8path(recordDir());
  const std::string when = stamp();
  doc::forEachTrack(*current_, [&](const doc::Track& t, const doc::Track*) {
    if (!t.mixer.arm || !t.active) return;
    RecordingTrack rt;
    rt.track = t.id;
    if (t.kind == doc::TrackKind::Audio && !t.mixer.audioIn.none()) {
      rt.audio = true;
      rt.input = t.mixer.audioIn;
      const std::string base = safeName(t.name) + " " + when + " " + inputLabel(t.mixer.audioIn);
      std::filesystem::path path = dir / u8path(base + ".wav");
      std::error_code ec;
      for (int n = 2; std::filesystem::exists(path, ec); ++n) path = dir / u8path(base + " (" + std::to_string(n) + ").wav");
      rt.path = path;
      tracks.push_back(std::move(rt));
    } else if (t.kind == doc::TrackKind::Midi) {
      rt.slot = slotForTrack(t.id);
      if (rt.slot != kNoSlot) tracks.push_back(std::move(rt));
    }
  });
  if (!tracks.empty())
    if (Status st = recording_->begin(std::move(tracks), cfg_.sampleRate); !st.ok()) {
      setError("recording: " + st.error().message);
      if (ctx_ != nullptr) ctx_->setStatus("Recording failed: " + st.error().message);
    }
  engine::TransportCommand c{engine::TransportCommandKind::Record, false, from, 0, 0};
  if (ph.playing && !ph.countingIn) {
    c.here = true;  // recording starts where the playhead is
    optimistic_ = ph;
  } else {
    prefetchAt(from);
    engageDisk(true);
    c.countInBars = current_ ? current_->record.countInBars : 0;
    optimistic_ = {true, from, 0};
    optimistic_.countingIn = c.countInBars > 0;
  }
  optimistic_.recording = true;
  postTransport(c);
  recordCommand_ = commandsPosted_;
  recordOffSent_ = false;
}

void ProjectLink::pollRecording() {  // UI thread
  if (!recording_ || !recording_->active()) return;
  recording_->pollNotes();
  if (report_.commands < recordCommand_) return;  // the Record command is not taken yet
  if (!report_.recording) {
    finishRecording();
    return;
  }
  // Punch-out without a loop: recording ends once the playhead passed it (plus the record latency: the frames that
  // were played before it arrive that much later).
  const doc::Project& p = *current_;
  if (p.punch.enabled && !(loopSent_ && loop_.enabled) && !recordOffSent_) {
    const int64_t off = engine_->recorder().latencyOffset(outputLatency());
    if (report_.songSample >= p.timeline->tempoMap.ticksToSamples(p.punch.out, cfg_.sampleRate) + off) {
      postTransport({engine::TransportCommandKind::RecordOff, false, 0, 0, 0});
      recordOffSent_ = true;
    }
  }
}

void ProjectLink::finishRecording() {  // UI thread
  RecordingResult r = recording_->finish();
  if (inputUseFn_) inputUseFn_();  // T6.7b: an input reopen held back during the session may go now
  for (const std::string& e : r.errors) setError("recording: " + e);
  doc::DocumentStore* store = ctx_ != nullptr ? &ctx_->store() : recordStore_;
  if (store == nullptr || r.passes.empty()) return;  // nothing to commit to (the files stay on disk)
  const doc::Project& p = *store->current();
  const TempoMap& tm = p.timeline->tempoMap;
  const double sr = r.sampleRate;
  const uint32_t pdc = outputLatency();
  const int64_t off = engine_->recorder().latencyOffset(pdc);
  const int64_t noteBack = int64_t{engine_->liveInput().latency()} + engine_->recorder().outputLatency();
  const std::string pdir = ctx_ != nullptr ? ctx_->projectDir() : cfg_.projectDir;
  const size_t np = r.passes.size();
  // The tick of song sample `s`: the nearest to its exact time, which plays on `s` itself whenever a tick does (ticks are
  // finer than samples at any sane tempo), else the first after it; and how far that tick's sample is past s.
  auto tickAt = [&](int64_t s) {
    Ticks t = static_cast<Ticks>(std::llround(tm.samplesToTicks(s, sr)));
    if (tm.ticksToSamples(t, sr) != s) t = std::max<Ticks>(0, tm.firstTickAtOrAfterSample(s, sr));
    return std::pair<Ticks, int64_t>{t, tm.ticksToSamples(t, sr) - s};
  };
  doc::ops::RecordingCommit commit;
  for (const RecordingTrack& rt : r.tracks) {
    const doc::Track* t = doc::findTrackPtr(p, rt.track);
    if (t == nullptr) continue;
    doc::ops::RecordedTrack out;
    out.track = rt.track;
    if (rt.audio) {
      std::error_code ec;
      if (r.fileFrames <= 0) {
        std::filesystem::remove(rt.path, ec);
        continue;
      }
      const uint32_t ch = std::max<uint32_t>(1, std::min<uint32_t>(rt.input.channels, 2));
      const int64_t frames = r.fileFrames;
      doc::SampleProbeFn probe = [&](std::string_view) -> Result<doc::SampleProbe> {
        return doc::SampleProbe{frames, sr, ch};
      };
      auto ref = doc::makeSampleRef(store->ids(), pathUtf8(rt.path), pdir, probe);
      if (!ref.ok()) {
        setError("recording: " + ref.error().message);
        continue;
      }
      for (size_t k = 0; k < np; ++k) {
        const RecordingPass& ps = r.passes[k];
        if (ps.audio1 <= ps.audio0) continue;
        const RecordingPass* prev = k > 0 ? &r.passes[k - 1] : nullptr;
        const RecordingPass* next = k + 1 < np ? &r.passes[k + 1] : nullptr;
        // A loop wrap with the file running on: the boundary stays on the loop point (see the file comment).
        const bool inContig = prev != nullptr && prev->audio1 == prev->songEnd && ps.audio0 == ps.songStart &&
                              prev->file0 + (prev->audio1 - prev->audio0) == ps.file0;
        const bool outContig = next != nullptr && ps.audio1 == ps.songEnd && next->audio0 == next->songStart &&
                               ps.file0 + (ps.audio1 - ps.audio0) == next->file0;
        int64_t ts = inContig ? ps.audio0 : ps.audio0 - off;
        int64_t cf = inContig ? ps.file0 + off : ps.file0;
        int64_t te = outContig ? ps.audio1 : ps.audio1 - off;
        if (cf < 0) ts -= cf, cf = 0;
        if (ts < 0) cf -= ts, ts = 0;
        const auto [tick, past] = tickAt(ts);
        ts += past;
        cf += past;
        te = std::min(te, ts + (frames - cf));
        if (te <= ts) continue;
        doc::AudioClip a;
        a.name = ref.value().displayName;
        a.colour = t->colour;
        a.start = tick;
        a.sample = ref.value().id;
        a.warp = doc::AudioWarp::Off;
        a.contentStartFrame = static_cast<double>(cf);
        a.seconds = static_cast<double>(te - ts) / sr;
        out.passes.push_back(doc::ClipSlot{doc::ClipId{}, nullptr, std::make_shared<const doc::AudioClip>(std::move(a))});
      }
      if (out.passes.empty()) continue;
      commit.samples.push_back(std::move(ref.value()));
    } else {
      // Notes: per pass, the On / Off records of the track's slot paired by (port, channel, key).
      std::vector<std::vector<doc::Note>> notes(np);
      struct Held {
        size_t pass, index;
      };
      std::map<std::tuple<uint8_t, uint8_t, uint8_t>, Held> held;
      std::optional<doc::InputPitchMapPtr> map;  // Hz mode with a note-on bend: the map's atNoteOn
      std::vector<Ticks> passStart(np), passEnd(np);
      std::vector<uint8_t> inside(np, 1);  // the pass overlaps the punch region
      for (size_t k = 0; k < np; ++k) {
        int64_t s0 = r.passes[k].songStart, s1 = r.passes[k].songEnd;
        if (p.punch.enabled) {
          s0 = std::max(s0, tm.ticksToSamples(p.punch.in, sr));
          s1 = std::min(s1, tm.ticksToSamples(p.punch.out, sr));
        }
        inside[k] = s1 > s0 ? 1 : 0;
        passStart[k] = tickAt(s0).first;
        passEnd[k] = std::max(passStart[k] + 1, tickAt(s1).first);
      }
      const Ticks tolerance = kPpq / 8;  // a note this early snaps onto its pass start (a 32nd)
      const Ticks q = p.record.recordQuantize;
      for (const engine::LiveNoteRecord& n : r.notes) {
        if (n.slot != rt.slot) continue;
        const auto [song, k] = songAt(r.passes, n.engineTime - noteBack);
        const auto tick = static_cast<Ticks>(std::llround(tm.samplesToTicks(song - pdc, sr)));
        const auto id = std::make_tuple(n.raw.port, static_cast<uint8_t>(n.raw.status & 15u), n.raw.d1);
        if (n.kind == engine::LiveNoteRecord::Kind::On) {
          if (!n.pitch.mapped() || inside[k] == 0) continue;
          Ticks start = tick;
          if (start < passStart[k]) {
            if (passStart[k] - start > tolerance) continue;
            start = passStart[k];
          }
          if (start >= passEnd[k]) continue;
          if (q > 0) start = std::clamp<Ticks>(static_cast<Ticks>(std::llround(static_cast<double>(start) / q)) * q,
                                              passStart[k], passEnd[k] - 1);
          tuning::PitchId pitch = n.pitch.id;
          if (t->noteInput.mode == doc::NoteInputMode::Hz && n.bendCents != 0.0f && n.raw.port != engine::kLiveKeyboard) {
            if (!map) {
              auto m = doc::buildInputPitchMap(p, t->id);
              map = m.ok() ? m.value() : nullptr;
            }
            if (*map) pitch = (*map)->atNoteOn(n.raw.d1, n.bendCents).id;
          }
          doc::Note note;
          note.start = start - passStart[k];
          note.length = passEnd[k] - start;  // until its note-off (held at the end: to the pass end)
          note.pitch = pitch;
          note.velocity = std::clamp(n.velocity, 1.0f, 127.0f);
          held[id] = {k, notes[k].size()};
          notes[k].push_back(note);
        } else if (auto it = held.find(id); it != held.end()) {
          doc::Note& note = notes[it->second.pass][it->second.index];
          const size_t hk = it->second.pass;
          const Ticks end = k == hk ? std::min(tick, passEnd[hk]) : passEnd[hk];
          note.length = std::max<Ticks>(1, end - (passStart[hk] + note.start));
          note.releaseVelocity = std::clamp(n.velocity, 0.0f, 127.0f);
          held.erase(it);
        }
      }
      for (size_t k = 0; k < np; ++k) {
        if (notes[k].empty()) continue;  // a pass without a note makes no take
        doc::NoteClip c;
        c.name = t->name;
        c.colour = t->colour;
        c.start = passStart[k];
        c.length = passEnd[k] - passStart[k];
        for (doc::Note& n : notes[k]) n.id = store->ids().note();
        auto seq = doc::NoteSeq::fromNotes(std::move(notes[k]));
        if (!seq.ok()) {
          setError("recording: " + seq.error().message);
          continue;
        }
        c.notes = std::move(seq.value());
        out.passes.push_back(doc::ClipSlot{doc::ClipId{}, std::make_shared<const doc::NoteClip>(std::move(c)), nullptr});
      }
      out.overdub = p.record.overdub;
      if (out.passes.empty()) continue;
    }
    commit.tracks.push_back(std::move(out));
  }
  if (commit.tracks.empty()) return;
  doc::ops::EditResult e = doc::ops::commitRecording(p, store->ids(), commit);
  if (!e.ok()) {
    setError("recording: " + e.error().message);
    if (ctx_ != nullptr) ctx_->setStatus("Recording could not be committed: " + e.error().message);
    return;
  }
  if (Status st = store->dispatch(e.value().command); !st.ok()) {
    setError("recording: " + st.error().message);
    return;
  }
  ++recordCommits_;
  if (ctx_ != nullptr && r.droppedFrames > 0)
    ctx_->setStatus("Recording: " + std::to_string(r.droppedFrames) + " frames were lost (the disk was too slow)");
}

bool ProjectLink::recordingOverlay(views::RecordingOverlay& out) {  // UI thread
  const uint32_t before = recLive_.session;
  const size_t passes = recLive_.passes.size();
  const int64_t frames = recLive_.fileFrames;
  const size_t notes = recLive_.notes.size();
  recording_->live(recLive_);
  const bool active = recording_->active();
  if (recLive_.session == before && recLive_.passes.size() == passes && recLive_.fileFrames == frames &&
      recLive_.notes.size() == notes && out.active == active && out.revision == overlayRevision_ && out.session == recLive_.session &&
      (recLive_.passes.empty() || recLive_.passes.back().songEnd == overlaySongEnd_))
    return false;
  overlaySongEnd_ = recLive_.passes.empty() ? 0 : recLive_.passes.back().songEnd;
  const doc::Project& p = *current_;
  const TempoMap& tm = p.timeline->tempoMap;
  const double sr = cfg_.sampleRate;
  const int64_t off = engine_->recorder().latencyOffset(outputLatency());
  auto ticks = [&](int64_t s) { return static_cast<Ticks>(std::llround(tm.samplesToTicks(std::max<int64_t>(0, s), sr))); };
  if (out.session != recLive_.session) out.peaks.clear();
  out.session = recLive_.session;
  out.active = active;
  out.regions.clear();
  if (out.peaks.size() != recLive_.peaks.size()) out.peaks.resize(recLive_.peaks.size());
  for (size_t i = 0; i < recLive_.peaks.size(); ++i)
    if (out.peaks[i].size() < recLive_.peaks[i].size())
      out.peaks[i].insert(out.peaks[i].end(), recLive_.peaks[i].begin() + static_cast<ptrdiff_t>(out.peaks[i].size()),
                          recLive_.peaks[i].end());
  // Which tracks record: the audio tracks of the files, the MIDI tracks of the notes' slots (any armed MIDI track).
  std::vector<uint32_t> midi;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    if (t.kind == doc::TrackKind::Midi && t.mixer.arm && t.active) midi.push_back(t.id.value);
  });
  for (const RecordingPass& ps : recLive_.passes) {
    for (size_t i = 0; i < recLive_.audioTracks.size(); ++i) {
      if (ps.audio1 <= ps.audio0) continue;
      views::RecordingOverlay::Region g;
      g.trackId = recLive_.audioTracks[i].value;
      g.audio = true;
      g.start = g.audioStart = ticks(ps.audio0 - off);
      g.end = std::max(g.start + 1, ticks(ps.audio1 - off));
      g.peakList = static_cast<uint32_t>(i);
      g.peak0 = static_cast<uint32_t>(ps.file0 / RecordingService::kPeakFrames);
      g.peakCount = static_cast<uint32_t>((ps.audio1 - ps.audio0) / RecordingService::kPeakFrames);
      out.regions.push_back(g);
    }
    for (uint32_t id : midi) {
      views::RecordingOverlay::Region g;
      g.trackId = id;
      g.start = ticks(ps.songStart);
      g.end = std::max(g.start + 1, ticks(ps.songEnd));
      out.regions.push_back(g);
    }
  }
  // Notes (the slots of the armed MIDI tracks), approximately placed (the commit maps them exactly).
  out.notes.clear();
  std::unordered_map<uint32_t, uint32_t> slotTrack;
  for (uint32_t id : midi) slotTrack[eventSlot(doc::TrackId{id})] = id;
  const int64_t noteBack = int64_t{engine_->liveInput().latency()} + engine_->recorder().outputLatency();
  std::map<std::tuple<uint32_t, uint8_t, uint8_t, uint8_t>, size_t> held;
  const Ticks now = recLive_.passes.empty() ? 0 : ticks(recLive_.passes.back().songEnd);
  for (const engine::LiveNoteRecord& n : recLive_.notes) {
    if (recLive_.passes.empty()) break;
    const auto it = slotTrack.find(n.slot);
    if (it == slotTrack.end()) continue;
    const Ticks tick = ticks(songAt(recLive_.passes, n.engineTime - noteBack).first - outputLatency());
    const auto key = std::make_tuple(n.slot, n.raw.port, static_cast<uint8_t>(n.raw.status & 15u), n.raw.d1);
    if (n.kind == engine::LiveNoteRecord::Kind::On) {
      if (!n.pitch.mapped()) continue;
      held[key] = out.notes.size();
      out.notes.push_back({it->second, tick, std::max<Ticks>(1, now - tick), n.pitch.hz, n.velocity});
    } else if (auto h = held.find(key); h != held.end()) {
      views::RecordingOverlay::Note& note = out.notes[h->second];
      note.length = std::max<Ticks>(1, tick - note.start);
      held.erase(h);
    }
  }
  out.revision = ++overlayRevision_;
  return true;
}

}  // namespace pq::app
