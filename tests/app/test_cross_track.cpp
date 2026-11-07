// TX.cross-track (SPEC §15 2026-11-03 "tracks are not walls"; QUESTIONS Q209, Q?-TX.cross-track-a): the project is
// one big circuit. (1) A wire that closes a loop between tracks — a cable into another track's mixer, a modulation
// wire, a send — is lowered like any feedback loop in a circuit: the wire that closes it (the circuit editor's rule:
// deterministic) gets the one-sample delay z⁻¹, and the loop runs PER SAMPLE (the partition merges the loop's tracks
// into one unit, the scheduler runs the loop's strongly connected part as a per-sample Loop region; everything else
// keeps block processing). No refusal, no block-sized delay. (2) A notes cable from any MIDI track's source into a
// block on any track plays that block with the source's voices (a voice context per source, not per track).
//
// Every loop case is checked against a HAND-WRITTEN per-sample reference: the same project rendered without the
// closing wire gives the loop's input, and a scalar recurrence written here (y[n] = input[n] + K · y[n − 1], or the
// device's own law) gives what the loop must play, sample by sample. Mixer gains fade in from silence (2 ms one-pole,
// snapped onto their value within 3e-8 after ~1700 frames), so the comparison starts once they are settled; an error
// before that decays with the loop gain. A block-delayed loop (a z⁻¹ of one 64-frame block) is checked to be far from
// the render, so the test tells the two apart. Interpreter == JIT bit for bit; every block under the allocation trap.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/modules.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/meta/registry.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 48000;
constexpr size_t kSettled = 4800;  // every mixer gain snapped onto its value (and the loop forgot the fade-in)

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

// The notes each MIDI track plays (by track id; tracks not listed play none).
struct Score {
  std::vector<std::pair<TrackId, std::vector<engine::TimedNote>>> tracks;
};

std::vector<engine::TimedNote> chord() {
  return {{0, 30000, 220.0f, 0.8f, 0.5f}, {3000, 26000, 277.18f, 0.6f, 0.5f}, {9000, 20000, 329.63f, 0.9f, 0.5f}};
}
std::vector<engine::TimedNote> otherChord() {
  return {{1500, 25000, 196.0f, 0.7f, 0.5f}, {6000, 21000, 246.94f, 0.5f, 0.5f}};
}

Result<LoweredProject> lowered(const Project& p, const Score* score = nullptr) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  // One event slot per scored track (in score order), the rest the last (an empty source).
  const size_t n = score != nullptr ? score->tracks.size() : 1;
  lo.slotOf = [score, n](TrackId t) {
    if (score == nullptr) return 0u;
    for (size_t i = 0; i < score->tracks.size(); ++i)
      if (score->tracks[i].first == t) return static_cast<uint32_t>(i);
    return static_cast<uint32_t>(n);
  };
  return lower(p, lo);
}

// The lowered project of `p` rendered for kFrames (the score's notes per track; by default every track's notes are the
// chord through slot 0).
golden::Channels render(const Project& p, const Score* score, engine::UnitBackend backend = {}) {
  auto low = lowered(p, score);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  CHECK(low->missingDevices.empty());
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& x : low->params) {
    links.push_back({x.track, x.name, static_cast<uint32_t>(values.size())});
    values.push_back(x.value);
  }
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  co.backend = backend;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low->graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  std::vector<std::unique_ptr<engine::NoteListSource>> sources;
  if (score == nullptr) {
    sources.push_back(std::make_unique<engine::NoteListSource>(chord()));
  } else {
    for (const auto& t : score->tracks) sources.push_back(std::make_unique<engine::NoteListSource>(t.second));
    sources.push_back(std::make_unique<engine::NoteListSource>(std::vector<engine::TimedNote>{}));
  }
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  for (auto& s : sources) pc.eventSources.push_back(s->source());
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
  for (uint32_t u = 0; u < build->images.size(); ++u) {
    const auto pub = (*prog)->publish(u, std::move(build->images[u]));
    REQUIRE_MESSAGE(pub.ok(), (pub.ok() ? std::string() : pub.error().message));
  }
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  for (int64_t t = 0; t < kFrames; t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    CHECK(alloc_trap::countRtViolations([&] {
            DenormalGuard dg;
            (*prog)->process(ab);
          }) == 0);
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  return out;
}

double maxDiff(const golden::Channels& a, const golden::Channels& b, size_t from = 0, size_t to = SIZE_MAX) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = from; i < std::min(to, a[c].size()); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  }
  return m;
}

float peak(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (float v : c) p = std::max(p, std::fabs(v));
  return p;
}

golden::Channels plus(const golden::Channels& a, const golden::Channels& b) {
  golden::Channels s = a;
  for (size_t c = 0; c < s.size(); ++c)
    for (size_t i = 0; i < s[c].size(); ++i) s[c][i] = static_cast<float>(static_cast<double>(a[c][i]) + b[c][i]);
  return s;
}

// The renders of `p` through the interpreter and the JIT: equal bit for bit (checked); the interpreter's returned.
golden::Channels played(const Project& p, const std::string& what, const Score* score = nullptr) {
  const golden::Channels a = render(p, score);
  const golden::Channels j = render(p, score, compile::jitUnitBackend());
  const double dj = maxDiff(a, j);
  MESSAGE(what << ": interpreter vs JIT max |diff| = " << dj << ", peak " << peak(a));
  CHECK(dj == 0.0);
  return a;
}

// ---- building projects --------------------------------------------------------------------------------------------

template <class E>
E run(DocumentStore& s, Result<E> e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  return *e;
}

void apply(DocumentStore& s, Command c) {
  const Status st = s.dispatch(playback_fixture::inUnits(s, std::move(c)));
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
}

// TX.pan-laws: a MIDI track pans with constant power here (centrePan's 0.707: the loops' references decay with it).
TrackId track(DocumentStore& s, TrackKind kind) {
  const TrackId t = run(s, ops::createTrack(*s.current(), s.ids(), kind)).tracks.at(0);
  if (kind == TrackKind::Midi) REQUIRE(s.dispatch(SetTrackPanMode{t, PanMode::ConstantPower}).ok());
  return t;
}
// The block heading track `t`'s path (its instrument group for a new MIDI track).
DeviceId pathHead(DocumentStore& s, TrackId t) { return mainPath(*s.current(), *findTrackPtr(*s.current(), t)).blocks.at(0)->id; }
// A MIDI track without blocks (source → mixer): its mixer the same law as a MIDI track with a synth.
TrackId emptyMidi(DocumentStore& s) {
  const TrackId t = track(s, TrackKind::Midi);
  const DeviceId head = pathHead(s, t);
  std::vector<DeviceId> gone{head};
  // (the group's members go with it)
  run(s, ops::deleteDevices(*s.current(), gone));  // IG.1-2: the group and its instrument
  run(s, ops::connect(*s.current(), s.ids(), [&] {
    Wire w;
    w.noteSource = t;
    w.mixer = t;
    return w;
  }()));
  return t;
}
DeviceId addOn(DocumentStore& s, TrackId t, std::string patch, DeviceKind kind, uint32_t slot = ops::kOffPath) {
  return run(s, ops::addDevice(*s.current(), s.ids(), t, std::move(patch), kind, slot)).devices.at(0);
}

// GU.11: a module's control (its value block named `control`) set in units.
void setControl(DocumentStore& s, DeviceId m, std::string_view control, double value) {
  const DeviceId v = test_blocks::member(*s.current(), m, control);
  REQUIRE(v.valid());
  run(s, ops::setDeviceParamValue(*s.current(), library(), v, std::string(kValueParam), value));
}
// GU.11: "a synth" — the default instrument (an instrument group) pasted off the path of `t`.
DeviceId synthOff(DocumentStore& s, TrackId t) {
  IdSource local;
  return run(s, ops::pasteBlocks(*s.current(), s.ids(), t, defaultInstrumentBlocks(local))).devices.at(0);
}
// The synth's Pitch (its Phase Accumulator's, cents): a per-voice control (what Basic Synth's brightness was here).
DeviceId pitchOf(DocumentStore& s, DeviceId synth) {
  const DeviceId p = test_blocks::member(*s.current(), synth, kPaPitch);
  REQUIRE(p.valid());
  return p;
}
// An LFO module off the path of `t` at 3 Hz, depth 0.4.
DeviceId lfoOff(DocumentStore& s, TrackId t) {
  const DeviceId lfo = run(s, ops::addDevice(*s.current(), s.ids(), t, "module:lfo", DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  setControl(s, lfo, "Rate", 3.0);
  setControl(s, lfo, "Depth", 0.4);
  return lfo;
}

WireId cable(DocumentStore& s, Wire w) { return run(s, ops::connect(*s.current(), s.ids(), std::move(w))).wires.at(0); }
Wire link(DeviceId from, DeviceId to, std::string port = {}, std::string param = {}) {
  Wire w;
  w.from = from;
  w.port = std::move(port);
  w.to = to;
  w.param = std::move(param);
  return w;
}
Wire intoMixer(DeviceId from, TrackId mixer) {
  Wire w;
  w.from = from;
  w.mixer = mixer;
  return w;
}
Wire ownSignalInto(TrackId source, TrackId mixer) {
  Wire w;
  w.noteSource = source;
  w.mixer = mixer;
  return w;
}
Wire notesInto(TrackId t, DeviceId to, std::string param = {}) {
  Wire w;
  w.noteSource = t;
  w.to = to;
  w.param = std::move(param);
  return w;
}

WireState stateOf(const Project& p, WireId w, const Score* score = nullptr) {
  auto low = lowered(p, score);
  REQUIRE(low.ok());
  for (const WireState& st : low->wires)
    if (st.id == w) return st;
  FAIL("no state for the wire");
  return {};
}

// A MIDI track's constant-power pan at the centre, as the lowering computes it (float32): sin(π/4 + π/2), sin(π/4).
double centrePan() {
  const float th = (0.0f + 1.0f) * static_cast<float>(std::numbers::pi / 4);
  return static_cast<double>(std::sin(th));
}

// y[n] = x[n] + k · y[n − delay] per channel (delay 1: the per-sample loop; kBlock: what a block delay would play).
golden::Channels recurrence(const golden::Channels& x, double k, size_t delay) {
  golden::Channels y(x.size());
  for (size_t c = 0; c < x.size(); ++c) {
    std::vector<double> acc(x[c].size(), 0.0);
    y[c].resize(x[c].size());
    for (size_t i = 0; i < x[c].size(); ++i) {
      acc[i] = static_cast<double>(x[c][i]) + (i >= delay ? k * acc[i - delay] : 0.0);
      y[c][i] = static_cast<float>(acc[i]);
    }
  }
  return y;
}

// The check every loop case makes: the render equals the per-sample reference once the mixer gains are settled
// (relative to the signal's peak), differs from what the loop's input alone plays, and is far from a block-delayed loop.
void checkLoop(const golden::Channels& got, const golden::Channels& ref, const golden::Channels& input,
               const golden::Channels& blockDelayed, const std::string& what) {
  const double p = peak(ref);
  const double d = maxDiff(got, ref, kSettled);
  const double din = maxDiff(got, input, kSettled);
  const double dblk = maxDiff(got, blockDelayed, kSettled);
  MESSAGE(what << ": vs the per-sample reference max |diff| = " << d << " (peak " << p << "); vs the loop's input "
               << din << "; vs a block-delayed loop " << dblk);
  CHECK(p > 1e-2);
  CHECK(d <= 2e-5 * p);
  CHECK(din >= 1e-2 * p);    // the loop is heard
  CHECK(dblk >= 1e-2 * p);   // and it is not a block delay
}

}  // namespace

// ---- (1) loops between tracks --------------------------------------------------------------------------------------

TEST_CASE("cross-track loops: a track's group fed back into the track's mixer runs per sample (z⁻¹ on the cable)") {
  // A (source → its instrument group → mixer) in a group G (no blocks). A cable from G's own signal (what reaches G: A's output)
  // into A's mixer closes the loop A's mixer → A's out → G's input → the cable → A's mixer. Its loop gain is A's pan
  // law at the centre (≈ 0.707 < 1): it rings and decays.
  DocumentStore s;
  const TrackId a = track(s, TrackKind::Midi);
  const TrackId g = run(s, ops::groupTracks(*s.current(), s.ids(), std::vector<TrackId>{a})).tracks.at(0);
  const golden::Channels open = played(*s.current(), "A in a group (no loop)");
  const WireId w = cable(s, ownSignalInto(g, a));
  const WireState st = stateOf(*s.current(), w);
  CHECK(st.status == WireStatus::Delayed);  // lowered, the z⁻¹ on it (no refusal)
  CHECK(st.reason == WireRefusal::None);
  const golden::Channels closed = played(*s.current(), "A's group fed back into A's mixer");
  // Per sample: o[n] = P · (s[n] + o[n − 1]) = open[n] + P · o[n − 1] (G and the master are unity once settled).
  const double k = centrePan();
  checkLoop(closed, recurrence(open, k, 1), open, recurrence(open, k, kBlock), "group → track mixer loop");
}

TEST_CASE("cross-track loops: two tracks' blocks wired into each other's mixers through a return run per sample") {
  // A (source → its instrument group → mixer) sends to a return R (post fader, 0 dB). R's Utility (on R's path, −6 dB) is cabled
  // into A's mixer: A's mixer → A's post → the send → R → Utility → the cable → A's mixer. The cable closes the loop.
  DocumentStore s;
  const TrackId a = track(s, TrackKind::Midi);
  const TrackId r = track(s, TrackKind::Return);
  const DeviceId u = addOn(s, r, "module:utility", DeviceKind::AudioEffect, 0);
  setControl(s, u, "Gain", -6.0);
  apply(s, SetSendLevel{a, r, 0.0f});
  const golden::Channels open = played(*s.current(), "A sending to R (no loop)");
  const WireId w = cable(s, intoMixer(u, a));
  CHECK(stateOf(*s.current(), w).status == WireStatus::Delayed);
  const golden::Channels closed = played(*s.current(), "R's Utility cabled into A's mixer");
  // open = a + g·a (A's output, then R's: the send at unity, the Utility g). Closed: m[n] = s[n] + u[n − 1] with
  // u[n] = g·P·m[n], so u[n] = g·P·s[n] + g·P·u[n − 1]: the loop input is open's R part g·a, its gain g·P; A's output
  // a' = P·m = a + P·u[n − 1]. Written per sample from open (= (1 + g)·a):
  const double gu = std::pow(10.0, -6.0 / 20.0), k = centrePan();
  golden::Channels ref = open, blk = open;
  for (size_t c = 0; c < 2; ++c) {
    double un = 0.0;
    std::vector<double> ub(open[c].size(), 0.0);
    for (size_t i = 0; i < open[c].size(); ++i) {
      const double an = static_cast<double>(open[c][i]) / (1.0 + gu);  // A's own output without the loop
      const double prev = un;
      un = gu * an + gu * k * prev;
      ref[c][i] = static_cast<float>(an + k * prev + un);
      ub[i] = gu * an + (i >= kBlock ? gu * k * ub[i - kBlock] : 0.0);
      blk[c][i] = static_cast<float>(an + (i >= kBlock ? k * ub[i - kBlock] : 0.0) + ub[i]);
    }
  }
  checkLoop(closed, ref, open, blk, "send → return → cable into the sender's mixer");
}

TEST_CASE("cross-track loops: a looping send (return → return → return) gets the z⁻¹, per sample, no block delay") {
  DocumentStore s;
  const TrackId a = track(s, TrackKind::Midi);
  const TrackId r1 = track(s, TrackKind::Return);
  const TrackId r2 = track(s, TrackKind::Return);
  apply(s, SetSendLevel{a, r1, 0.0f});
  apply(s, SetSendLevel{r1, r2, -3.0f});
  const golden::Channels open = played(*s.current(), "A → R1 → R2 (no loop)");
  apply(s, SetSendLevel{r2, r1, -6.0f});  // closes R1 → R2 → R1
  const std::vector<DelayedSend> d = delayedSends(*s.current());
  REQUIRE(d.size() == 1);
  CHECK(d[0].from == r2);  // the send that closes the loop (sends are lowered in track order)
  CHECK(d[0].to == r1);
  const golden::Channels closed = played(*s.current(), "A → R1 ⇄ R2");
  // open = a + r1 + r2 with r1 = a, r2 = g12·a: a = open / (2 + g12). Closed, per sample:
  // r1[n] = a[n] + g21·r2[n − 1], r2[n] = g12·r1[n], out = a + r1 + r2.
  const double g12 = std::pow(10.0, -3.0 / 20.0), g21 = std::pow(10.0, -6.0 / 20.0);
  auto loop = [&](size_t delay) {
    golden::Channels y = open;
    for (size_t c = 0; c < 2; ++c) {
      std::vector<double> r2v(open[c].size(), 0.0);
      for (size_t i = 0; i < open[c].size(); ++i) {
        const double an = static_cast<double>(open[c][i]) / (2.0 + g12);
        const double r1n = an + (i >= delay ? g21 * r2v[i - delay] : 0.0);
        r2v[i] = g12 * r1n;
        y[c][i] = static_cast<float>(an + r1n + r2v[i]);
      }
    }
    return y;
  };
  checkLoop(closed, loop(1), open, loop(kBlock), "looping send");
}

TEST_CASE("cross-track loops: a loop through modulation (B's block modulating A's) runs per sample") {
  // A: source → instrument group → Utility U → mixer. B (no instrument): a Utility V off its path. U's output → V's
  // input (across: its first channel), V's output → U's Balance (an Add wire: balance = clip(V[n − 1], −1, 1), the wire that
  // closes the loop delayed). U: L·min(1, 1 − balance), R·min(1, 1 + balance); V passes its input (L = R = U's L).
  DocumentStore s;
  const TrackId a = track(s, TrackKind::Midi);
  const DeviceId u = addOn(s, a, "module:utility", DeviceKind::AudioEffect, 1);
  const TrackId b = emptyMidi(s);
  const DeviceId v = addOn(s, b, "module:utility", DeviceKind::AudioEffect);
  const WireId first = cable(s, link(u, v, "y", "x"));
  CHECK(stateOf(*s.current(), first).status == WireStatus::Lowered);
  const golden::Channels open = played(*s.current(), "U → V (no loop)");
  const WireId w = cable(s, link(v, test_blocks::member(*s.current(), u, "Balance"), "y", std::string(kValueParam)));
  CHECK(stateOf(*s.current(), w).status == WireStatus::Delayed);
  const golden::Channels closed = played(*s.current(), "V's output modulating U's balance");
  const double k = centrePan();
  auto loop = [&](size_t delay) {
    golden::Channels y = open;
    std::vector<double> yl(open[0].size(), 0.0);
    for (size_t i = 0; i < open[0].size(); ++i) {
      const double sn = static_cast<double>(open[0][i]) / k;  // the synth into U (mono: L = R)
      const double bal = i >= delay ? std::clamp(yl[i - delay], -1.0, 1.0) : 0.0;
      yl[i] = sn * std::min(1.0, 1.0 - bal);
      y[0][i] = static_cast<float>(k * yl[i]);
      y[1][i] = static_cast<float>(k * sn * std::min(1.0, 1.0 + bal));
    }
    return y;
  };
  checkLoop(closed, loop(1), open, loop(kBlock), "modulation loop between two tracks");
}

// ---- (2) notes cables across tracks --------------------------------------------------------------------------------

namespace {

// A synth's sound made different from the default one's (same envelope: the same voice end): a fifth up.
void voiced(DocumentStore& s, DeviceId synth) { setControl(s, synth, kPaPitch, 702.0); }

// Whether the lowered patch of track `host` calls device `d` (a guest is called in its source's patch).
bool calls(const Project& p, TrackId host, DeviceId d, const Score* score) {
  auto low = lowered(p, score);
  REQUIRE(low.ok());
  const std::string name = "d" + std::to_string(d.value);
  for (size_t j = 0; j < low->trackIds.size(); ++j) {
    if (low->trackIds[j] != host) continue;
    const graph::Patch& patch = low->patches[j]->patch;
    for (graph::NodeIndex n = 0; n < patch.nodeCount(); ++n)
      if (patch.nodeName(n) == name) return true;
  }
  return false;
}

// A meta module off the path (raw: the type's defaults).
DeviceId metaOffPath(DocumentStore& s, TrackId t, std::string_view type) {
  const meta::Type* mt = meta::findType(type);
  REQUIRE(mt != nullptr);
  const DeviceId id = s.ids().device();
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kMetaDevicePrefix) + std::string(type), DeviceKind::AudioEffect));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*mt)}});
  apply(s, AddDevice{t, std::move(d), kAppend});
  return id;
}

}  // namespace

TEST_CASE("cross-track notes: track A's notes cabled to a synth on track B play it with A's voices (= the synth on A)") {
  // The reference: A plays its own synth (source → its instrument group → mixer); B (a MIDI track, nothing on it) is
  // silent.
  DocumentStore r;
  const TrackId ra = track(r, TrackKind::Midi);
  const TrackId rb = emptyMidi(r);
  const Score rs{{{ra, chord()}, {rb, {}}}};
  const golden::Channels ref = played(*r.current(), "A playing its own synth", &rs);
  // A without blocks; the synth off B's path, A's notes cabled into it, its output into B's mixer.
  DocumentStore s;
  const TrackId a = emptyMidi(s);
  const TrackId b = emptyMidi(s);
  const DeviceId synth = synthOff(s, b);
  const Score sc{{{a, chord()}, {b, {}}}};
  const WireId n = cable(s, notesInto(a, synth));
  const WireId m = cable(s, intoMixer(synth, b));
  CHECK(stateOf(*s.current(), n, &sc).status == WireStatus::Lowered);
  CHECK(stateOf(*s.current(), m, &sc).status == WireStatus::Lowered);
  CHECK(calls(*s.current(), a, synth, &sc));        // its instance plays in A's voice context (A's patch)
  CHECK_FALSE(calls(*s.current(), b, synth, &sc));  // B's own notes do not reach it: no instance of its own there
  const golden::Channels got = played(*s.current(), "A's notes playing the synth on B", &sc);
  const double d = maxDiff(got, ref);
  MESSAGE("A's notes into B's synth vs the synth on A: max |diff| = " << d);
  CHECK(peak(ref) > 1e-2);
  CHECK(d == 0.0);
  // IG.1-2: one notes source per voice context (no guest instances): B's notes into the same synth are not played —
  // the cable from another track wins — so A's notes alone still play it.
  const WireId nb = cable(s, notesInto(b, synth));  // B's notes into the same synth too
  const Score sc2{{{a, chord()}, {b, otherChord()}}};
  CHECK(stateOf(*s.current(), nb, &sc2).status == WireStatus::Inactive);
  CHECK_FALSE(calls(*s.current(), b, synth, &sc2));
  const golden::Channels both = played(*s.current(), "A's and B's notes into B's synth", &sc2);
  const double d2 = maxDiff(both, ref);
  MESSAGE("A's and B's notes into one synth (A's play it) vs A alone: max |diff| = " << d2);
  CHECK(d2 == 0.0);
}

TEST_CASE("cross-track notes: a synth on another track's path, played by A's notes, feeds that path") {
  // B: a MIDI track whose path is source → instrument group → Saturator → mixer, B playing nothing; A's notes cabled into the
  // synth: the path hears A's voices of it (B's own instance plays nothing: no notes). The reference: A's own path
  // source → its instrument group → Saturator → mixer.
  auto build = [](DocumentStore& s, TrackId t) {
    const DeviceId sat = addOn(s, t, "module:saturator", DeviceKind::AudioEffect, 1);
    setControl(s, sat, "Drive", std::pow(10.0, 9.0 / 20.0));
    return sat;
  };
  DocumentStore r;
  const TrackId ra = track(r, TrackKind::Midi);
  (void)build(r, ra);
  const TrackId rb = emptyMidi(r);
  const Score rs{{{ra, chord()}, {rb, {}}}};
  const golden::Channels ref = played(*r.current(), "A: synth → Saturator", &rs);
  DocumentStore s;
  const TrackId a = emptyMidi(s);
  const TrackId b = track(s, TrackKind::Midi);
  (void)build(s, b);
  const DeviceId synth = pathHead(s, b);  // IG.1-2: its instrument group (A's notes into it win over B's path)
  const Score sc{{{a, chord()}, {b, {}}}};
  const WireId n = cable(s, notesInto(a, synth));
  CHECK(stateOf(*s.current(), n, &sc).status == WireStatus::Lowered);
  const golden::Channels got = played(*s.current(), "A's notes into B's path synth", &sc);
  const double d = maxDiff(got, ref);
  MESSAGE("A's notes into the synth on B's path vs A's own path: max |diff| = " << d);
  CHECK(peak(ref) > 1e-2);
  CHECK(d == 0.0);
}

TEST_CASE("cross-track notes: two sources into blocks of one track (each its own voices)") {
  // The reference: A and B each play their own synth (two sounds).
  DocumentStore r;
  const TrackId ra = track(r, TrackKind::Midi);
  const TrackId rb = track(r, TrackKind::Midi);
  voiced(r, pathHead(r, rb));
  const Score rs{{{ra, chord()}, {rb, otherChord()}}};
  const golden::Channels ref = played(*r.current(), "A and B on their own synths", &rs);
  // A and B without blocks; C holds both synths: x played by A's notes, y by B's, each into C's mixer.
  DocumentStore s;
  const TrackId a = emptyMidi(s);
  const TrackId b = emptyMidi(s);
  const TrackId c = emptyMidi(s);
  const DeviceId x = synthOff(s, c);
  const DeviceId y = synthOff(s, c);
  voiced(s, y);
  cable(s, notesInto(a, x));
  cable(s, notesInto(b, y));
  cable(s, intoMixer(x, c));
  cable(s, intoMixer(y, c));
  const Score sc{{{a, chord()}, {b, otherChord()}, {c, {}}}};
  CHECK(calls(*s.current(), a, x, &sc));
  CHECK(calls(*s.current(), b, y, &sc));
  const golden::Channels got = played(*s.current(), "C's two synths played by A and B", &sc);
  const double d = maxDiff(got, ref);
  MESSAGE("two sources into one track's blocks vs each on its own track: max |diff| = " << d);
  CHECK(peak(ref) > 1e-2);
  CHECK(d <= 1e-6);
  // Both sources into ONE block (an instance per source, their voices summed where its home reads it): = each source
  // playing that synth on its own track.
  DocumentStore r1;
  const TrackId r1a = track(r1, TrackKind::Midi);
  const TrackId r1b = track(r1, TrackKind::Midi);
  const Score rs1{{{r1a, chord()}, {r1b, otherChord()}}};
  const golden::Channels ref1 = played(*r1.current(), "A and B on the default synth each", &rs1);
  DocumentStore o;
  const TrackId oa = emptyMidi(o);
  const TrackId ob = emptyMidi(o);
  const TrackId oc = emptyMidi(o);
  const DeviceId z = synthOff(o, oc);
  // IG.1-2: both sources into ONE block: one voice context, one source (the lowest wire id from another track: A's);
  // B's cable is not played. The reference: A alone on the default synth.
  cable(o, notesInto(oa, z));
  const WireId ob2 = cable(o, notesInto(ob, z));
  cable(o, intoMixer(z, oc));
  const Score os{{{oa, chord()}, {ob, otherChord()}, {oc, {}}}};
  CHECK(calls(*o.current(), oa, z, &os));
  CHECK_FALSE(calls(*o.current(), ob, z, &os));
  CHECK(stateOf(*o.current(), ob2, &os).status == WireStatus::Inactive);
  DocumentStore r3;
  const TrackId r3a = track(r3, TrackKind::Midi);
  const TrackId r3b = emptyMidi(r3);
  const Score rs3{{{r3a, chord()}, {r3b, {}}}};
  const golden::Channels aAlone = played(*r3.current(), "A alone on the default synth", &rs3);
  const golden::Channels got1 = played(*o.current(), "one synth on C played by A (B's cable not played)", &os);
  const double d1 = maxDiff(got1, aAlone);
  MESSAGE("two sources into one block vs A alone: max |diff| = " << d1);
  CHECK(d1 <= 1e-6);
  (void)ref1;
}

TEST_CASE("cross-track notes: into a meta module off another track's path; a modulation on its home reaches it") {
  // Notes into an off-path Wavetable (its phase input: the note's phase), the Wavetable into the mixer: on A itself
  // (the reference) and on B played by A's notes — the same voices, the same signal.
  DocumentStore r;
  const TrackId ra = emptyMidi(r);
  const DeviceId rwt = metaOffPath(r, ra, "wavetable");
  cable(r, notesInto(ra, rwt));
  cable(r, intoMixer(rwt, ra));
  const TrackId rb = emptyMidi(r);
  const Score rs{{{ra, chord()}, {rb, {}}}};
  const golden::Channels ref = played(*r.current(), "notes into A's own off-path Wavetable", &rs);
  DocumentStore s;
  const TrackId a = emptyMidi(s);
  const TrackId b = emptyMidi(s);
  const DeviceId wt = metaOffPath(s, b, "wavetable");
  const WireId n = cable(s, notesInto(a, wt));
  cable(s, intoMixer(wt, b));
  const Score sc{{{a, chord()}, {b, {}}}};
  CHECK(stateOf(*s.current(), n, &sc).status == WireStatus::Lowered);
  // IG.1-2: outside every instrument group a meta module reads the newest note (single-signal): it stays on B, A's
  // newest note crossing to it (the reference reads A's newest note on A the same way).
  CHECK_FALSE(calls(*s.current(), a, wt, &sc));
  CHECK(calls(*s.current(), b, wt, &sc));
  const golden::Channels got = played(*s.current(), "A's notes into B's off-path Wavetable", &sc);
  const double d = maxDiff(got, ref);
  MESSAGE("notes into another track's meta module vs into its own: max |diff| = " << d);
  CHECK(peak(ref) > 1e-2);
  CHECK(d == 0.0);
  // A modulation of a block by its home track (an LFO on B into the synth's Pitch) reaches the instance A's notes
  // play as a global signal (the home's value of the pin, sent to the guest): the same as that LFO on another track
  // modulating the synth on A. (An LFO on A itself would run per voice there: a free-running source that feeds voice
  // processing in its own track's voice context does — pq/graph/infer.h; a home's modulation is not in that context.)
  DocumentStore r2;
  const TrackId r2a = track(r2, TrackKind::Midi);
  const TrackId r2b = emptyMidi(r2);
  const DeviceId r2lfo = lfoOff(r2, r2b);
  cable(r2, link(r2lfo, pitchOf(r2, pathHead(r2, r2a)), "y", std::string(kValueParam)));
  const Score s2{{{r2a, chord()}, {r2b, {}}}};
  const golden::Channels ref2 = played(*r2.current(), "an LFO on B modulating A's synth", &s2);
  DocumentStore m;
  const TrackId ma = emptyMidi(m);
  const TrackId mb = emptyMidi(m);
  const DeviceId synth = synthOff(m, mb);
  const DeviceId lfo = lfoOff(m, mb);
  const WireId mod = cable(m, link(lfo, pitchOf(m, synth), "y", std::string(kValueParam)));
  cable(m, notesInto(ma, synth));
  cable(m, intoMixer(synth, mb));
  const Score ms{{{ma, chord()}, {mb, {}}}};
  CHECK(stateOf(*m.current(), mod, &ms).status == WireStatus::Lowered);
  const golden::Channels got2 = played(*m.current(), "an LFO on B modulating B's synth played by A", &ms);
  const double d2 = maxDiff(got2, ref2);
  MESSAGE("a home modulation of a block another track's notes play vs that LFO modulating the synth on A: max |diff| = " << d2);
  CHECK(peak(ref2) > 1e-2);
  CHECK(d2 <= 1e-6);
}

TEST_CASE("cross-track notes: refused only where the host and the block's track would loop through two instruments") {
  // B plays its own synth (an instrument track) and modulates the synth A's notes play on it: A's voices feed B (the
  // synth's output), B feeds A (the modulation): one unit could not hold both tracks' voices (IG.3). IG.1-2: the synth
  // is a voice context of A (lowered in A's patch), so the cable that closes the loop between the two instrument tracks
  // — its output into B's mixer — is refused (VoiceLoop); the rest plays as without it.
  DocumentStore s;
  const TrackId a = emptyMidi(s);
  const TrackId b = track(s, TrackKind::Midi);
  const DeviceId synth = synthOff(s, b);
  const DeviceId lfo = lfoOff(s, b);
  cable(s, link(lfo, pitchOf(s, synth), "y", std::string(kValueParam)));
  const WireId mix = cable(s, intoMixer(synth, b));
  const Score sc{{{a, chord()}, {b, otherChord()}}};
  const golden::Channels before = played(*s.current(), "B alone", &sc);
  const WireId n = cable(s, notesInto(a, synth));
  CHECK(stateOf(*s.current(), n, &sc).status == WireStatus::Lowered);
  const WireState st = stateOf(*s.current(), mix, &sc);
  CHECK(st.status == WireStatus::Refused);
  CHECK(st.reason == WireRefusal::VoiceLoop);
  const golden::Channels after = played(*s.current(), "B, with A's refused notes cable", &sc);
  CHECK(maxDiff(before, after) == 0.0);
}
