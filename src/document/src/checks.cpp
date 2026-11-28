// Validation helpers shared by command application (apply.cpp) and the project loader (serialize.cpp).

#include "checks.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "pq/document/audio_in.h"
#include "pq/document/devices.h"
#include "pq/document/meta.h"
#include "pq/document/note_devices.h"
#include "pq/document/racks.h"
#include "pq/document/wire_types.h"
#include "pq/meta/registry.h"

namespace pq::doc::detail {

Error invalid(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m), {}}; }
Error notFound(std::string m) { return Error{ErrorCode::NotFound, std::move(m), {}}; }
Error unsupported(std::string m) { return Error{ErrorCode::Unsupported, std::move(m), {}}; }
Status ok() { return Ok{}; }

// ---- Validation helpers ------------------------------------------------------------------------------------------

Status checkTick(Ticks t, const char* what) {
  if (t < -kMaxTicks || t > kMaxTicks) return invalid(std::string(what) + " out of range");
  return ok();
}
Status checkLength(Ticks l, const char* what) {
  if (l < 1 || l > kMaxTicks) return invalid(std::string(what) + " must be at least one tick");
  return ok();
}
bool finiteIn(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

Status checkParamValue(ParamKind k, double v) {
  const ParamRange r = paramRange(k);
  if (!finiteIn(v, r.min, r.max)) return invalid("parameter value out of range");
  return ok();
}

Status checkTuningDef(const TuningDef& d) {
  Result<tuning::Tuning> t = buildTuning(d);
  if (!t) return std::move(t).error();
  return ok();
}

Status checkAssignment(const Project& p, const TuningAssignment& a) {
  switch (a.mode) {
    case TuningMode::Inherit: return ok();
    case TuningMode::Main:
      if (!findTuning(p, a.main)) return notFound("main tuning not found");
      return ok();
  }
  return invalid("bad tuning mode");
}

Status checkSignature(TimeSignature sig) {
  if (sig.numerator < 1 || sig.numerator > 1024 || !noteValueIsExact(sig.denominator))
    return invalid("invalid time signature");
  return ok();
}

Status checkLoop(const ClipLoop& l) {
  PQ_DOC_TRY(checkTick(l.start, "loop start"));
  PQ_DOC_TRY(checkTick(l.end, "loop end"));
  if (l.end <= l.start) return invalid("loop end must be after loop start");
  return ok();
}

// Everything about a clip except note validity (NoteSeq contents are validated where notes enter: fromNotes and
// the note commands).
Status checkClip(const Project& p, const NoteClip& c) {
  if (!c.id.valid()) return invalid("clip has no id");
  PQ_DOC_TRY(checkTick(c.start, "clip start"));
  PQ_DOC_TRY(checkLength(c.length, "clip length"));
  PQ_DOC_TRY(checkTick(c.contentStart, "clip content start"));
  PQ_DOC_TRY(checkLoop(c.loop));
  PQ_DOC_TRY(checkSignature(c.signature));
  PQ_DOC_TRY(checkFundamental(c.fundamental));
  return checkAssignment(p, c.tuning);
}

Status checkAudioClip(const Project& p, const AudioClip& c) {
  if (!c.id.valid()) return invalid("clip has no id");
  PQ_DOC_TRY(checkTick(c.start, "clip start"));
  PQ_DOC_TRY(checkLength(c.length, "clip length"));
  if (!findSample(p, c.sample)) return notFound("unknown sample id " + std::to_string(c.sample.value));
  if (static_cast<uint8_t>(c.warp) > static_cast<uint8_t>(kLastAudioWarp)) return invalid("unknown warp mode");
  if (!std::isfinite(c.contentStartFrame) || std::abs(c.contentStartFrame) > 9.0e15)
    return invalid("content start frame out of range");
  if (!finiteIn(c.seconds, 1e-9, 1.0e8)) return invalid("clip duration must be positive (seconds)");
  PQ_DOC_TRY(checkTick(c.contentStart, "clip content start"));
  PQ_DOC_TRY(checkLoop(c.loop));
  if (!finiteIn(c.sourceBpm, kMinBpm, kMaxBpm)) return invalid("source BPM out of range");
  if (!finiteIn(c.gainDb, kMinClipGainDb, kMaxClipGainDb)) return invalid("clip gain out of range");
  switch (c.transpose.unit) {
    case TransposeUnit::Cents:
      if (!finiteIn(c.transpose.cents, -kMaxTransposeCents, kMaxTransposeCents)) return invalid("transpose out of range");
      break;
    case TransposeUnit::Steps:
      if (c.transpose.steps < -kMaxTransposeSteps || c.transpose.steps > kMaxTransposeSteps)
        return invalid("transpose steps out of range");
      break;
    case TransposeUnit::Ratio:
      if (!c.transpose.ratio.isPositive() || std::abs(tuning::cents(c.transpose.ratio)) > kMaxTransposeCents + 1e-9)
        return invalid("transpose ratio out of range");
      break;
    default: return invalid("unknown transpose unit");
  }
  if (!finiteIn(c.detuneCents, -kMaxDetuneCents, kMaxDetuneCents)) return invalid("detune out of range");
  for (const AudioFade* f : {&c.fadeIn, &c.fadeOut}) {
    if (f->length < 0 || f->length > kMaxTicks) return invalid("fade length out of range");
    if (!finiteIn(f->curve, -1.0, 1.0)) return invalid("fade curve must be in [-1, 1]");
  }
  if (c.crossfadeIn < 0 || c.crossfadeIn > kMaxTicks) return invalid("crossfade length out of range");
  PQ_DOC_TRY(checkWarpMarkers(c.markers));
  PQ_DOC_TRY(checkWarpParams(c.warpParams));
  return checkAssignment(p, c.tuning);
}

// T6.1 (model.h WarpMarker): strictly increasing in both coordinates, finite, bounded.
Status checkWarpMarkers(const std::vector<WarpMarker>& m) {
  if (m.size() > kMaxWarpMarkers) return invalid("too many warp markers");
  for (size_t i = 0; i < m.size(); ++i) {
    if (!std::isfinite(m[i].sourceFrame) || std::abs(m[i].sourceFrame) > 9.0e15)
      return invalid("warp marker frame out of range");
    PQ_DOC_TRY(checkTick(m[i].content, "warp marker"));
    if (i > 0 && !(m[i - 1].sourceFrame < m[i].sourceFrame && m[i - 1].content < m[i].content))
      return invalid("warp markers must increase strictly in source frame and content tick");
  }
  return ok();
}

Status checkWarpParams(const WarpParams& w) {
  if (w.beatsGranulation < 0 || w.beatsGranulation > kTicksPerWhole) return invalid("Beats granulation out of range");
  if (static_cast<uint8_t>(w.beatsLoop) > static_cast<uint8_t>(BeatsLoop::BackForward)) return invalid("unknown Beats loop");
  if (!finiteIn(w.beatsEnvelope, 0.0, 100.0)) return invalid("Beats envelope out of range");
  if (!finiteIn(w.grainSizeMs, kMinGrainMs, kMaxGrainMs)) return invalid("grain size out of range");
  if (!finiteIn(w.flux, 0.0, 100.0)) return invalid("flux out of range");
  if (!finiteIn(w.formants, 0.0, 100.0)) return invalid("formants out of range");
  if (!finiteIn(w.envelope, kMinWarpEnvelope, kMaxWarpEnvelope)) return invalid("envelope out of range");
  return ok();
}

// T6.1 (model.h "Recording inputs").
Status checkRecordInputs(TrackKind kind, const Mixer& m) {
  if (m.arm && kind != TrackKind::Midi && kind != TrackKind::Audio) return invalid("only MIDI and audio tracks can be armed");
  if (static_cast<uint8_t>(m.monitor) > static_cast<uint8_t>(MonitorMode::Off)) return invalid("unknown monitor mode");
  if (m.monitor != MonitorMode::Auto && kind != TrackKind::Midi && kind != TrackKind::Audio)
    return invalid("only MIDI and audio tracks monitor an input");
  if (m.audioIn.channels > 2) return invalid("an audio input is mono or a stereo pair");
  if (!m.audioIn.none() && uint32_t{m.audioIn.firstChannel} + m.audioIn.channels > kMaxInputChannels)
    return invalid("audio input channel out of range");
  if (!m.audioIn.none() && kind != TrackKind::Audio) return invalid("only audio tracks have an audio input");
  if (m.audioIn.none() && m.audioIn.firstChannel != 0) return invalid("no audio input: its first channel must be 0");
  if (static_cast<uint8_t>(m.midiIn.kind) > static_cast<uint8_t>(MidiInputKind::None)) return invalid("unknown MIDI input");
  if ((m.midiIn.kind == MidiInputKind::Port) == m.midiIn.port.empty())
    return invalid("a MIDI input names a port exactly when it is one port");
  if (m.midiIn.channel > 16) return invalid("MIDI channel out of range (0 = all, 1 … 16)");
  if (!(m.midiIn == MidiInput{}) && kind != TrackKind::Midi) return invalid("only MIDI tracks have a MIDI input");
  if (!finiteIn(m.trackDelayMs, -kMaxTrackDelayMs, kMaxTrackDelayMs)) return invalid("track delay out of range");
  return ok();
}

Status checkNoteInput(const NoteInput& n) {
  if (static_cast<uint8_t>(n.mode) > static_cast<uint8_t>(NoteInputMode::Hz)) return invalid("unknown note input mode");
  if (static_cast<uint8_t>(n.keyMap) > static_cast<uint8_t>(InputKeyMap::TwelveTetHz)) return invalid("unknown key map");
  return ok();
}

Status checkPunch(const PunchRegion& r) {
  PQ_DOC_TRY(checkTick(r.in, "punch in"));
  PQ_DOC_TRY(checkTick(r.out, "punch out"));
  if (r.in < 0 || r.out <= r.in) return invalid("punch region must satisfy 0 <= in < out");
  return ok();
}

Status checkRecordSettings(const RecordSettings& r) {
  if (r.countInBars != 0 && r.countInBars != 1 && r.countInBars != 2 && r.countInBars != 4)
    return invalid("count-in is 0, 1, 2 or 4 bars");
  PQ_DOC_TRY(checkVolume(r.metronomeDb));
  if (r.recordQuantize < 0 || r.recordQuantize > kTicksPerWhole) return invalid("record quantize out of range");
  return ok();
}

Status checkSampleRef(const SampleRef& r) {
  if (!r.id.valid()) return invalid("sample has no id");
  if (r.path.empty()) return invalid("sample path is empty");
  if (r.frames < 1) return invalid("sample frames must be at least 1");
  if (!finiteIn(r.rate, 1e-3, 1.0e7)) return invalid("sample rate out of range");
  if (r.channels < 1 || r.channels > 1024) return invalid("sample channels out of range");
  return ok();
}

Status checkFundamental(const ClipFundamental& f) {
  for (size_t i = 0; i < f.steps.size(); ++i) {
    const HarmonicStep& h = f.steps[i];
    if (!h.note.valid()) return invalid("harmonic step without a note");
    if (i > 0 && !(f.steps[i - 1].note.value < h.note.value)) return invalid("harmonic steps must be sorted by note id");
    if (h.denominator < 1 || h.denominator > kMaxFundamentalDenominator)
      return invalid("harmonic step denominator out of range");
    if (!h.ratio.isPositive()) return invalid("harmonic step ratio must be positive");
  }
  return ok();
}

// ---- JI groups, written identities and note curves (T7.13) -----------------------------------------------------------------------------

Status checkCurve(const std::vector<Breakpoint>& pts, const char* what) {
  const std::string w(what);
  if (pts.size() > kMaxCurvePoints) return invalid(w + " has too many points");
  for (size_t i = 0; i < pts.size(); ++i) {
    const Breakpoint& b = pts[i];
    if (b.id.valid()) return invalid(w + " points have no ids");
    if (b.tick < 0 || b.tick > kMaxTicks) return invalid(w + " point out of range");
    if (i > 0 && b.tick < pts[i - 1].tick) return invalid(w + " points must be sorted by tick");
    if (!finiteIn(b.value, -kMaxCurveCents, kMaxCurveCents)) return invalid(w + " value out of range");
    if (!finiteIn(b.curve, -1.0, 1.0)) return invalid(w + " point curve must be in [-1, 1]");
  }
  return ok();
}

Status checkGlideShape(const GlideShape& s) {
  if (s.kind > GlideShape::Bezier) return invalid("unknown glide shape");
  if (!finiteIn(s.x1, 0.0, 1.0) || !finiteIn(s.x2, 0.0, 1.0) || !finiteIn(s.y1, -1.0, 2.0) || !finiteIn(s.y2, -1.0, 2.0))
    return invalid("glide shape handles out of range (x in [0, 1], y in [-1, 2])");
  return ok();
}

Status checkJiGroup(const JiGroup& g) {
  if (!g.id.valid()) return invalid("JI group has no id");
  if (!g.anchor.valid()) return invalid("a JI group names its anchor note");
  if (g.index < 1 || g.index > kMaxHarmonicIndex) return invalid("the harmonic number goes from 1 to 4096");
  if (g.warps.size() > kMaxJiWarps) return invalid("a JI group has too many warps");
  for (size_t i = 0; i < g.warps.size(); ++i) {
    const JiWarp& w = g.warps[i];
    if (w.at < -kMaxTicks || w.at > kMaxTicks) return invalid("JI warp out of range");  // from the anchor's start
    if (w.glide < 0 || w.glide > kMaxTicks) return invalid("JI warp glide out of range");
    PQ_DOC_TRY(checkGlideShape(w.shape));
    if (i == 0) continue;
    const JiWarp& before = g.warps[i - 1];
    if (w.at <= before.at) return invalid("JI warps must ascend");
    if (w.at < before.at + before.glide) return invalid("a JI warp starts before the glide before it has ended");
    if (w.toJi == before.toJi) return invalid("JI warps alternate between to-JI and back");
  }
  return ok();
}

Status checkJiGroups(const JiGroupList& l) {
  std::vector<uint32_t> anchors;
  anchors.reserve(l.size());
  for (size_t i = 0; i < l.size(); ++i) {
    PQ_DOC_TRY(checkJiGroup(l[i]));
    if (i > 0 && !(l[i - 1].id < l[i].id)) return invalid("JI groups must be sorted by id");
    anchors.push_back(l[i].anchor.value);
  }
  std::sort(anchors.begin(), anchors.end());
  if (std::adjacent_find(anchors.begin(), anchors.end()) != anchors.end())
    return invalid("a note is the anchor of one JI group only");
  return ok();
}

Status checkWrittenPitch(const tuning::PitchId& p) {
  switch (p.kind()) {
    case tuning::PitchKind::Degree: return ok();
    case tuning::PitchKind::Cents:
      if (std::isfinite(p.cents())) return ok();
      break;
    case tuning::PitchKind::Ratio:
      if (p.anchorKind() == tuning::AnchorKind::Reference && p.rawNum() > 0 && p.rawDen() > 0) return ok();
      break;
  }
  return invalid("a written identity is a degree, cents or a ratio to the reference");
}

Status checkNotesWritten(const NoteWrittenList& l) {
  for (size_t i = 0; i < l.size(); ++i) {
    if (!l[i].note.valid()) return invalid("a written identity without a note");
    if (i > 0 && !(l[i - 1].note < l[i].note)) return invalid("written identities must be sorted by note id");
    PQ_DOC_TRY(checkWrittenPitch(l[i].written));
  }
  return ok();
}

Status checkNoteBend(const NoteBend& b, NoteId note) {
  if (!b.set()) return b == NoteBend{} ? ok() : invalid("a note bend without a neighbour has no length or shape");
  if (b.other == note) return invalid("a note cannot bend to itself");
  if (b.length < 1 || b.length > kMaxTicks) return invalid("note bend length out of range");
  return checkGlideShape(b.shape);
}

Status checkNoteVibrato(const NoteVibrato& v) {
  if (!v.set()) return v == NoteVibrato{} ? ok() : invalid("a vibrato without a depth has no other setting");
  if (!std::isfinite(v.depthCents) || std::fabs(v.depthCents) > kMaxVibratoCents) return invalid("vibrato depth out of range (-1200 to 1200 cents)");
  const bool beats = v.unit == NoteVibrato::RateUnit::Beats;
  if (v.unit != NoteVibrato::RateUnit::Hz && !beats) return invalid("unknown vibrato rate unit");
  const float lo = beats ? kMinVibratoBeats : kMinVibratoHz, hi = beats ? kMaxVibratoBeats : kMaxVibratoHz;
  if (!std::isfinite(v.rate) || v.rate < lo || v.rate > hi) return invalid("vibrato rate out of range");
  if (v.onset < 0 || v.onset > kMaxTicks) return invalid("vibrato onset out of range");
  if (v.fadeIn < 0 || v.fadeIn > kMaxTicks || v.fadeOut < 0 || v.fadeOut > kMaxTicks) return invalid("vibrato fade out of range");
  if (v.end != 0 && (v.end <= v.onset || v.end > kMaxTicks)) return invalid("a vibrato ends after it starts");
  return ok();
}

Status checkNoteCurveRecord(const NoteCurves& c) {
  if (!c.note.valid()) return invalid("note curve without a note");
  PQ_DOC_TRY(checkCurve(c.pitch, "note curve"));
  PQ_DOC_TRY(checkNoteBend(c.bendIn, c.note));
  PQ_DOC_TRY(checkNoteBend(c.bendOut, c.note));
  return checkNoteVibrato(c.vibrato);
}

Status checkNoteCurves(const NoteCurveList& l) {
  for (size_t i = 0; i < l.size(); ++i) {
    if (i > 0 && !(l[i - 1].note < l[i].note)) return invalid("note curves must be sorted by note id");
    if (l[i].empty()) return invalid("an empty note curve is not stored");
    PQ_DOC_TRY(checkNoteCurveRecord(l[i]));
  }
  return ok();
}

Status checkJiNotes(const NoteClip& c, std::span<const JiGroup> groups, std::span<const NoteWritten> written) {
  if (groups.empty() && written.empty()) return ok();
  // One pass over the notes (a NoteSeq is ordered by start, not id) against the few ids needed.
  std::vector<NoteId> need;
  need.reserve(groups.size() + written.size());
  for (const JiGroup& g : groups) need.push_back(g.anchor);
  for (const NoteWritten& w : written) need.push_back(w.note);
  std::sort(need.begin(), need.end());
  need.erase(std::unique(need.begin(), need.end()), need.end());
  size_t found = 0;
  for (const Note& n : c.notes) found += std::binary_search(need.begin(), need.end(), n.id) ? 1 : 0;
  if (found != need.size()) return notFound("a JI group's anchor or a written note is not in the clip");
  return ok();
}

Status checkClipJi(const NoteClip& c) {
  if (c.groups) {
    if (c.groups->empty()) return invalid("an empty JI group list is not stored");
    PQ_DOC_TRY(checkJiGroups(*c.groups));
  }
  if (c.written) {
    if (c.written->empty()) return invalid("an empty written-identity list is not stored");
    PQ_DOC_TRY(checkNotesWritten(*c.written));
  }
  if (c.curves) {
    if (c.curves->empty()) return invalid("an empty note curve list is not stored");
    PQ_DOC_TRY(checkNoteCurves(*c.curves));
  }
  return checkJiNotes(c, jiGroupsOf(c), notesWrittenOf(c));
}

}  // namespace pq::doc::detail

namespace pq::doc {
Status validateClipJi(const NoteClip& c) { return detail::checkClipJi(c); }
}  // namespace pq::doc

namespace pq::doc::detail {

Status checkVolume(float db) { return checkParamValue(ParamKind::TrackVolume, db); }

Status checkPoints(const ParamRef& ref, const std::vector<Breakpoint>& pts) {
  std::vector<uint32_t> ids;
  ids.reserve(pts.size());
  const ParamRange r = paramRange(ref);
  for (const Breakpoint& b : pts) {
    if (!b.id.valid()) return invalid("breakpoint has no id");
    PQ_DOC_TRY(checkTick(b.tick, "breakpoint"));
    if (!finiteIn(b.value, r.min, r.max)) return invalid("breakpoint value out of the parameter's range");
    if (!finiteIn(b.curve, -1.0, 1.0)) return invalid("breakpoint curve must be in [-1, 1]");
    ids.push_back(b.id.value);
  }
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return invalid("duplicate breakpoint id");
  return ok();
}

bool pointLess(const Breakpoint& a, const Breakpoint& b) {
  return a.tick != b.tick ? a.tick < b.tick : a.id < b.id;
}

bool tuningParamApplies(const TuningDef& d, ParamKind k) {
  switch (k) {
    case ParamKind::TuningReference: return true;
    case ParamKind::TuningEquave: return d.edo() != nullptr;
    case ParamKind::MosGenerator:
    case ParamKind::MosPeriod:
    case ParamKind::MosStretch:
      return d.mos() != nullptr;
    default: return false;
  }
}

bool trackHasSends(TrackKind k) {
  return k == TrackKind::Midi || k == TrackKind::Audio || k == TrackKind::Group || k == TrackKind::Return;
}

// Whether `host` may carry a lane for `r` (see AddAutomationLane).
namespace {
// T4.18: whether device `id` is in `list`.
bool treeHasDevice(const std::vector<DevicePtr>& list, DeviceId id, const Device** out = nullptr) {
  bool found = false;
  forEachDeviceIn(list, [&](const Device& d) {
    if (found || d.id != id) return;
    found = true;
    if (out != nullptr) *out = &d;
  });
  return found;
}
bool identifier(std::string_view name) {
  if (name.empty() || (name[0] >= '0' && name[0] <= '9')) return false;
  for (const char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}
}  // namespace

Status checkLaneParam(const Project& p, const Track& host, const ParamRef& r, std::string_view name) {
  if (!isAutomatable(r)) return invalid("parameter is not automatable");
  if (r.kind != ParamKind::DeviceParam && !name.empty()) return invalid("only device parameter lanes carry a name");
  if (isDeviceParamKind(r.kind)) {  // T4.18
    switch (r.kind) {
      case ParamKind::DeviceParam: {
        const Device* d = nullptr;
        if (!treeHasDevice(host.devices, DeviceId{r.owner}, &d))
          return invalid("a device parameter can only be automated on the device's own track");
        if (!identifier(name)) return invalid("a device parameter lane needs the parameter's name");
        if (deviceParamHash(name) != r.target) return invalid("the lane's parameter name does not match its target");
        if (std::string why = metaParamBakedOnly(*d, name); !why.empty()) return invalid(why);  // TMM.5
        return ok();
      }
      default: {  // WireAmount
        if (r.target != 0) return invalid("a wire amount has no target");
        const Wire* w = findWire(p, WireId{r.owner});
        if (w == nullptr) return notFound("automated wire not found");
        if (!w->travels()) return invalid("a wire that adds in its target's units has no amount to automate");  // TX.patching
        if (!treeHasDevice(host.devices, w->to)) return invalid("a wire amount can only be automated on its target's track");
        return ok();
      }
    }
  }
  if (isTrackParam(r.kind)) {
    if (r.owner != host.id.value) return invalid("a track parameter can only be automated on its own track");
    if (r.kind == ParamKind::TrackMute && host.kind == TrackKind::Master) return invalid("the master has no mute");
    if (r.kind == ParamKind::TrackSend) {
      if (!trackHasSends(host.kind)) return invalid("this track has no sends");
      const Track* target = findTrackPtr(p, TrackId{r.target});
      if (!target || target->kind != TrackKind::Return || target->id == host.id)
        return invalid("send target must be another return track");
    } else if (r.target != 0) {
      return invalid("only sends have a target");
    }
    return ok();
  }
  const TuningSlot* slot = findTuning(p, TuningId{r.owner});
  if (!slot) return notFound("automated tuning not found");
  if (r.target != 0) return invalid("tuning parameters have no target");
  if (!tuningParamApplies(slot->def, r.kind)) return invalid("parameter does not apply to this tuning's kind");
  return ok();
}

Status checkDeviceValue(const std::string& name, const std::optional<double>& value) {
  // A patch identifier ([A-Za-z_][A-Za-z0-9_]*, PATCH_LANGUAGE §1): it is a JSON key in the project file.
  bool ident = !name.empty() && !(name[0] >= '0' && name[0] <= '9');
  for (const char c : name)
    ident = ident && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
  if (!ident) return invalid("'" + name + "' is not a device param or setting name");
  if (value && !std::isfinite(*value)) return invalid("device value '" + name + "' is not finite");
  return ok();
}

namespace {
Status checkValues(const std::vector<DeviceValue>& v, const char* what) {
  for (size_t i = 0; i < v.size(); ++i) {
    PQ_DOC_TRY(checkDeviceValue(v[i].name, v[i].value));
    if (i > 0 && !(v[i - 1].name < v[i].name))
      return invalid(std::string("device ") + what + " must be sorted by name, one per name");
  }
  return ok();
}
}  // namespace

Status checkParamCurves(const Device& d) {  // GU.1 (model.h ControlCurve)
  for (size_t i = 0; i < d.curves.size(); ++i) {
    const ControlCurve& c = d.curves[i];
    PQ_DOC_TRY(checkDeviceValue(c.param, std::nullopt));
    if (!dsp::validCurve(c.curve)) return invalid("'" + c.param + "': not a valid curve (pq/dsp/curve.h validCurve)");
    if (i > 0 && !(d.curves[i - 1].param < c.param)) return invalid("device knob curves must be sorted by param, one per param");
  }
  if (isGroupDevice(d) && !d.curves.empty()) return invalid("a group has no knobs (no knob curves)");  // TMM.13-fix1 L7
  return ok();
}

Status checkChain(TrackKind kind, const std::vector<DevicePtr>& devices, std::vector<size_t>* where) {
  return checkList(listKindOf(kind), devices, where);
}

ListKind listKindOf(TrackKind kind) noexcept { return kind == TrackKind::Midi ? ListKind::MidiTrack : ListKind::OtherTrack; }

// TMM.13-fix1 M8: a refusal says which device (`where`: its index, checks.h checkChain).
Status checkList(ListKind k, const std::vector<DevicePtr>& list, std::vector<size_t>* where) {
  {
    for (size_t i = 0; i < list.size(); ++i) {
      const Status st = [&]() -> Status {
        const DevicePtr& d = list[i];
        if (!d) return invalid("null device");
        if (!d->id.valid()) return invalid("device has no id");
        if (d->patch.empty()) return invalid("device has no patch");
        // GU.11 (model.h Device): a block is a group, an atom block, a value block or a native engine — nothing else.
        if (d->patch == kAudioEffectRack || d->patch == kInstrumentRack)
          return invalid("'" + d->patch + "' names a rack spawn, not a device patch (a rack is a group: ops::addRack)");
        const NoteDeviceType* note = noteDeviceTypeOf(*d);
        if (!isGroupDevice(*d) && !isValueDevice(*d) && !isMetaDevice(*d) && note == nullptr &&
            !(isAtomDevice(*d) && atomBlockPatch(atomDeviceName(*d)) != nullptr))
          return invalid("'" + d->patch + "' is not a block (a group, an atom block, a value block, a native engine or a "
                         "note device)");
        if (note != nullptr) {  // AP.2 (note_devices.h): a note stage on a MIDI track's notes link
          if (k != ListKind::MidiTrack) return invalid("a note device goes on a MIDI track (no notes reach it elsewhere)");
          if (d->kind != DeviceKind::AudioEffect) return invalid("a note device is an audio effect (its signals decide)");
          if (d->group.valid()) return invalid("a note device sits on a track's notes link, not in a group");
          if (d->meta) return invalid("a note device has no meta modules");
          for (const DeviceValue& v : d->params)
            if (const NoteParamSpec* sp = note->param(v.name); sp == nullptr || !sp->runtime)
              return invalid("'" + v.name + "' is not a param of " + note->name);
          for (const DeviceValue& v : d->settings)
            if (const NoteParamSpec* sp = note->param(v.name); sp == nullptr || sp->runtime)
              return invalid("'" + v.name + "' is not a setting of " + note->name);
          if (!d->curves.empty()) return invalid("a note device's knobs keep their declared curves");
        }
        for (size_t j = 0; j < i; ++j)
          if (list[j]->id == d->id) return invalid("device id twice in a chain");
        // The notes exist only on MIDI tracks (an instrument plays where they reach it: lower.h "The main path").
        if (d->kind == DeviceKind::Instrument && k == ListKind::OtherTrack)
          return invalid("instruments go on MIDI tracks (the notes are their main input)");
        if (isGroupDevice(*d)) {  // TX.groups (model.h "Groups")
          if (d->kind != DeviceKind::AudioEffect) return invalid("a group is an audio effect");
          if (!d->params.empty() || d->meta) return invalid("a group has no params or meta modules of its own");
          // IG.1-2 (model.h "Instrument groups"): its settings are an instrument group's polyphony only.
          for (const DeviceValue& v : d->settings)
            if (v.name != kGroupVoices && v.name != kGroupMono && v.name != kGroupLegato && v.name != kGroupGlide &&
                v.name != kGroupSteal && v.name != kGroupOversample && !rackGroupSetting(v.name))
              return invalid("a group's settings are its polyphony (voices, mono, legato, glide, steal) and its rack "
                             "chain's on / solo / key unit, its role (racks.h)");
        }
        if (isAtomDevice(*d) || isValueDevice(*d)) {  // GU.2 (model.h "Atom blocks and value blocks")
          if (d->kind != DeviceKind::AudioEffect) return invalid("an atom or value block is an audio effect");
          if (d->meta) return invalid("an atom or value block has no meta modules");
          if (isAtomDevice(*d) && !d->params.empty()) return invalid("an atom block has no params (its inputs are wired)");
          if (isValueDevice(*d)) {
            for (const DeviceValue& v : d->settings)  // GU.9: a macro's slot, variations and Rand exclusion (racks.h)
              if (!rackValueSetting(v.name)) return invalid("a value block's settings are its rack macro's and its rack role (racks.h)");
            for (const DeviceValue& v : d->params)
              if (v.name != kValueParam) return invalid("a value block's one control is 'value'");
          }
        }
        if (d->defaultPosition) {  // TX.reset-default (model.h Device::defaultPosition)
          if (!isValueDevice(*d)) return invalid("only a value block has a default knob position");
          if (!(*d->defaultPosition >= 0.0 && *d->defaultPosition <= 1.0))
            return invalid("a value block's default knob position is in [0, 1]");
        }
        if (d->group.valid()) {
          // GU.2: groups nest (model.h "Groups"): the groups a member sits in, outward, end within kMaxGroupDepth and
          // never come back to it.
          const Device* at = d.get();
          for (uint32_t depth = 0; at->group.valid(); ++depth) {
            if (at->group == d->id) return invalid("a group cannot be inside itself");
            if (depth >= kMaxGroupDepth)
              return invalid("groups nest at most " + std::to_string(kMaxGroupDepth) + " deep");
            const auto g = std::find_if(list.begin(), list.end(), [&](const DevicePtr& x) { return x && x->id == at->group; });
            if (g == list.end()) return invalid("a group member's group is not in its track");
            if (!isGroupDevice(**g)) return invalid("a group member's group is not a group");
            at = g->get();
          }
        }
        PQ_DOC_TRY(checkValues(d->params, "params"));
        PQ_DOC_TRY(checkValues(d->settings, "settings"));
        PQ_DOC_TRY(checkParamCurves(*d));  // TX.knob-curves
        if (d->panel) {  // GU.3 (pq/document/panel.h); its bindings: SetGroupPanel, the reader, apply's final pass
          if (!isGroupDevice(*d)) return invalid("only a group has a panel");
          if (d->panel->empty()) return invalid("an empty panel is no panel");
          PQ_DOC_TRY(checkPanel(*d->panel));
        }
        PQ_DOC_TRY(checkDeviceMeta(*d));  // TMM.3
        return ok();
      }();
      if (!st) {
        if (where != nullptr) *where = {i};
        return st;
      }
    }
  }
  if (where != nullptr) where->clear();
  return ok();
}

// ---- Meta modules (TMM.3) --------------------------------------------------------------------------------------------

Status checkMetaInstance(const Device& d, std::string_view path, const MetaInstance& inst) {
  const std::string at = path.empty() ? std::string("the device's meta module") : "meta module '" + std::string(path) + "'";
  if (!meta::validPath(path)) return invalid("'" + std::string(path) + "' is not a meta module path");
  const meta::Type* t = meta::findType(inst.type);
  if (t == nullptr) return unsupported(at + ": unknown meta module type '" + inst.type + "'");
  if (inst.version != t->version)
    return invalid(at + ": version " + std::to_string(inst.version) + " is not the type's (" + std::to_string(t->version) + ")");
  if (path.empty()) {
    if (metaDeviceType(d) != inst.type) return invalid(at + ": only a '" + std::string(kMetaDevicePrefix) + inst.type + "' device has it");
    if (!inst.params.empty() || !inst.settings.empty())
      return invalid(at + ": a chain meta device keeps its params and settings as the device's own");
  }
  PQ_DOC_TRY(checkValues(inst.params, "params"));
  PQ_DOC_TRY(checkValues(inst.settings, "settings"));
  for (const DeviceValue& v : inst.params)
    if (t->paramIndex(v.name) < 0) return invalid(at + ": '" + v.name + "' is not a param of " + inst.type);
  for (const DeviceValue& v : inst.settings)
    if (t->settingIndex(v.name) < 0) return invalid(at + ": '" + v.name + "' is not a setting of " + inst.type);
  if (inst.data) {
    if (std::string p = meta::dataProblem(*inst.data); !p.empty()) return invalid(at + ": data: " + p);
    if (t->dataProblem == nullptr) return invalid(at + ": " + inst.type + " keeps no data");
    if (std::string p = t->dataProblem(*inst.data); !p.empty()) return invalid(at + ": data: " + p);
  }
  return ok();
}

Status checkDeviceMeta(const Device& d) {
  if (isMetaDevice(d)) {
    const meta::Type* t = meta::findType(metaDeviceType(d));
    if (t == nullptr) return unsupported("unknown meta module type '" + std::string(metaDeviceType(d)) + "'");
    (void)t;  // TMM.10-fix1: any type in either kind (its kind is its position: model.h "Meta modules")
  }
  if (!d.meta) return ok();
  if (d.meta->empty()) return invalid("an empty meta map is stored as none");
  for (size_t i = 0; i < d.meta->size(); ++i) {
    const MetaEntry& e = (*d.meta)[i];
    if (i > 0 && !((*d.meta)[i - 1].path < e.path)) return invalid("meta modules must be sorted by path, one per path");
    // TMM.13-fix1 L6: a chain meta device plays its type's expansion, which calls no nested meta modules: state at
    // any other path was never used (and was saved back forever).
    if (isMetaDevice(d) && !e.path.empty())
      return invalid("meta module '" + e.path + "': a chain meta device holds only its own state (path \"\")");
    PQ_DOC_TRY(checkMetaInstance(d, e.path, e.instance));
  }
  return ok();
}


Status checkWire(const Wire& w) {
  if (!w.id.valid()) return invalid("wire has no id");
  if (w.fromMixerOut()) {  // RT.1: a track's mixer output, only onto an Audio In (model.h "Modulation wires")
    if (w.from.valid() || w.fromNoteSource()) return invalid("a wire from a mixer's output has no other source");
    if (!w.port.empty()) return invalid("a mixer has one output (no port)");
    if (!w.toRecordInput()) return invalid("a cable from a mixer's output goes onto an Audio In");
  } else if (w.fromNoteSource()) {  // TMM.10-fix2: a track's note source (no device), one of its note outputs
    if (w.from.valid()) return invalid("a wire from a note source has no source device");
    // TX.all-free: kMainPort is the track's own signal (any track kind; main_path.h).
    if (!w.port.empty() && findNoteOutput(w.port) == nullptr) return invalid("the note source has no output '" + w.port + "'");
  } else if (!w.from.valid()) {
    return invalid("wire end has no device");
  }
  if (w.toRecordInput()) {  // RT.1: onto an audio track's Audio In (model.h "Modulation wires")
    if (w.to.valid() || w.toMixer()) return invalid("a wire onto an Audio In has no other target");
    if (!w.param.empty()) return invalid("an Audio In is one input (no param)");
    if (w.travels()) return invalid("a wire onto an Audio In adds (no travel law)");
    if (w.chain) return invalid("a chain link joins devices (a grouped series' link)");
  } else if (w.toMixer()) {  // TX.all-free: into a track's mixer input (model.h "Modulation wires")
    if (w.to.valid()) return invalid("a wire into a mixer has no target device");
    if (!w.param.empty()) return invalid("a wire into a mixer enters its one input (no param)");
    if (w.travels()) return invalid("a wire into a mixer adds (no travel law)");
  } else if (!w.to.valid()) {
    return invalid("wire end has no device");
  }
  if (!w.port.empty()) PQ_DOC_TRY(checkDeviceValue(w.port, std::nullopt));  // TX.all-free: "" = the main port
  if (!w.param.empty()) PQ_DOC_TRY(checkDeviceValue(w.param, std::nullopt));
  if ((w.port.empty() || w.param.empty()) && w.travels()) return invalid("a wire on a main port adds (no travel law)");
  if (static_cast<uint8_t>(w.law) > static_cast<uint8_t>(WireLaw::Units)) return invalid("unknown wire law");
  if (w.travels()) {
    if (!finiteIn(w.amount, -1.0, 1.0)) return invalid("wire amount must be in [-1, 1]");
  } else if (w.amount != 1.0) {  // TX.patching: an added wire has no depth (model.h "Modulation wires")
    return invalid("a wire that adds in its target's units has no amount (scale its signal with a VCA)");
  }
  if (static_cast<uint8_t>(w.reducer) > static_cast<uint8_t>(WireReducer::Loudest)) return invalid("unknown wire reducer");
  if (w.tap && (!w.fromNoteSource() || !(std::fabs(*w.tap) <= kMaxWireTap)))  // TL.1
    return invalid("only a cable from a track's lane has a tap (finite, within ±1e6)");
  return ok();
}

std::string chainLinkProblem(const Project& p, const Wire& w) {
  if (!w.chain) return {};
  if (w.fromNoteSource() || w.toMixer()) return "a chain link joins devices (a grouped series' link)";
  const DeviceLoc a = findDevice(p, w.from), b = findDevice(p, w.to);
  if (a.device == nullptr || b.device == nullptr) return {};  // (a missing end is refused on its own)
  if (a.track != b.track) return "a chain link joins devices of one track (a grouped series)";
  // A group's end is one of its terminals (In: its output kGroupInPort; Out: its input kGroupOutPort), or — GU.2, a
  // nested group in its parent's series — a member of its own group by its main ports.
  auto groupAt = [](const Device& d, bool terminal) { return isGroupDevice(d) && terminal ? d.id : d.group; };
  const DeviceId g = groupAt(*a.device, w.port == kGroupInPort);
  if (!g.valid() || groupAt(*b.device, w.param == kGroupOutPort) != g)
    return "a chain link joins a group's terminals and its members (a grouped series), here devices of no one group";
  // (A group end that is not the series' own group is a nested member: its main ports, any.)
  const bool fromIn = isGroupDevice(*a.device) && a.device->id == g, toOut = isGroupDevice(*b.device) && b.device->id == g;
  if (fromIn && w.port != kGroupInPort) return "a chain link leaves a group by its In terminal";
  if (toOut && w.param != kGroupOutPort) return "a chain link enters a group by its Out terminal";
  return {};
}

// Rejects routing cycles: following each track's output (Default → parent group or master) must end at the
// master or at a track with no output, within one step per track.
Status checkRouting(const Project& p) {
  struct Hop {
    uint32_t id;
    uint32_t next;  // 0 = ends here
  };
  std::vector<Hop> hops;
  const uint32_t masterId = p.master ? p.master->id.value : 0;
  forEachTrack(p, [&](const Track& t, const Track* parent) {
    uint32_t next = 0;
    if (t.kind != TrackKind::Master) {
      switch (t.mixer.output.kind) {
        case OutputKind::Default: next = parent ? parent->id.value : masterId; break;
        case OutputKind::Master: next = masterId; break;
        case OutputKind::Track: next = t.mixer.output.track.value; break;
        case OutputKind::None: next = 0; break;
      }
    }
    hops.push_back({t.id.value, next});
  });
  std::sort(hops.begin(), hops.end(), [](const Hop& a, const Hop& b) { return a.id < b.id; });
  auto nextOf = [&](uint32_t id) -> uint32_t {
    auto it = std::lower_bound(hops.begin(), hops.end(), id, [](const Hop& h, uint32_t v) { return h.id < v; });
    return (it != hops.end() && it->id == id) ? it->next : 0;
  };
  for (const Hop& h : hops) {
    uint32_t at = h.next;
    for (size_t steps = 0; at != 0; ++steps) {
      if (at == h.id || steps > hops.size()) return invalid("routing would create a cycle");
      at = nextOf(at);
    }
  }
  return ok();
}

// ---- RT.1: Audio In (audio_in.h "Loops") ---------------------------------------------------------------------------

AudioFeeds::AudioFeeds(const Project& p) {
  master = p.master ? p.master->id.value : 0;
  forEachTrack(p, [&](const Track& t, const Track* parent) {
    for (const DevicePtr& d : t.devices) deviceTrack.emplace(d->id.value, t.id.value);
    if (t.kind != TrackKind::Master) {
      uint32_t next = 0;
      switch (t.mixer.output.kind) {
        case OutputKind::Default: next = parent ? parent->id.value : master; break;
        case OutputKind::Master: next = master; break;
        case OutputKind::Track: next = t.mixer.output.track.value; break;
        case OutputKind::None: break;
      }
      if (next != 0) output.emplace(t.id.value, next);
    }
    if (trackHasSends(t.kind))
      for (const Send& sd : t.mixer.sends) others[t.id.value].push_back(sd.target.value);
  });
  for (const Wire& w : wiresOf(p)) {
    const uint32_t a = sourceTrack(w), b = targetTrack(w);
    if (a != 0 && b != 0 && a != b) others[a].push_back(b);
  }
}

uint32_t AudioFeeds::sourceTrack(const Wire& w) const noexcept {
  if (w.fromMixerOut()) return w.mixerOut.value;
  if (w.fromNoteSource()) return w.noteSource.value;
  const auto it = deviceTrack.find(w.from.value);
  return it != deviceTrack.end() ? it->second : 0;
}

uint32_t AudioFeeds::targetTrack(const Wire& w) const noexcept {
  if (w.toRecordInput()) return w.recordInput.value;
  if (w.toMixer()) return w.mixer.value;
  const auto it = deviceTrack.find(w.to.value);
  return it != deviceTrack.end() ? it->second : 0;
}

std::vector<uint32_t> AudioFeeds::fedBy(uint32_t track, bool ownOutput) const {
  std::vector<uint32_t> seen{track}, todo{track};
  auto visit = [&](uint32_t t) {
    if (std::find(seen.begin(), seen.end(), t) != seen.end()) return;
    seen.push_back(t);
    todo.push_back(t);
  };
  while (!todo.empty()) {
    const uint32_t t = todo.back();
    todo.pop_back();
    if (ownOutput || t != track)
      if (const auto o = output.find(t); o != output.end()) visit(o->second);
    if (const auto x = others.find(t); x != others.end())
      for (const uint32_t n : x->second) visit(n);
  }
  std::sort(seen.begin(), seen.end());
  return seen;
}

std::string_view audioInLoop(const AudioFeeds& f, uint32_t track, const Wire& start) {
  const uint32_t s = f.sourceTrack(start);
  if (s == 0) return {};  // (a missing source is refused on its own)
  const bool resampling = start.fromMixerOut() && s == f.master;
  if (s == track && !resampling) return kAudioInSelf;
  const std::vector<uint32_t> fed = f.fedBy(track, !resampling);
  return std::binary_search(fed.begin(), fed.end(), s) ? kAudioInLoop : std::string_view{};
}

Status checkAudioIns(const Project& p) {
  const WireList& wl = wiresOf(p);
  if (std::none_of(wl.begin(), wl.end(), [](const Wire& w) { return w.toRecordInput(); })) return ok();
  const AudioFeeds f(p);
  std::vector<uint32_t> fed;  // the Audio Ins seen
  for (const Wire& w : wl) {
    if (!w.toRecordInput()) continue;
    const Track* t = findTrackPtr(p, w.recordInput);
    if (t == nullptr) return invalid("an Audio In's track is not in the project");
    if (t->kind != TrackKind::Audio) return invalid(std::string(kAudioInTrack));
    if (!t->mixer.audioIn.none() || std::find(fed.begin(), fed.end(), t->id.value) != fed.end())
      return invalid(std::string(kAudioInOneSource));
    fed.push_back(t->id.value);
    if (w.fromMixerOut() && findTrackPtr(p, w.mixerOut) == nullptr) return invalid("an Audio In's source track is not in the project");
    if (!carriesAudio(p, w, nullptr)) return invalid(std::string(kAudioInNotAudio));
    if (const std::string_view why = audioInLoop(f, t->id.value, w); !why.empty()) return invalid(std::string(why));
  }
  return ok();
}

}  // namespace pq::doc::detail
