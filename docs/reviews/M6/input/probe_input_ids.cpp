// M6 review (MIDI / live input): identities through LiveInput -> CaptureRing -> Capture's identity rule, the capture
// ring's memory, and a port index reused after a close. Standalone (no doctest); prints numbers.
//   pwsh docs/reviews/M6/input/build_probes.ps1 probe_input_ids
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/input_map.h"
#include "pq/document/store.h"
#include "pq/engine/live_input.h"

using namespace pq;
using namespace pq::doc;
namespace e = pq::engine;

static int fails = 0;
#define EXPECT(c, ...)                         \
  do {                                         \
    if (!(c)) {                                \
      ++fails;                                 \
      std::printf("  FAIL: " __VA_ARGS__);     \
      std::printf("\n");                       \
    }                                          \
  } while (0)

static size_t privateBytes() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return pmc.PrivateUsage;
}
static size_t workingSet() {
  PROCESS_MEMORY_COUNTERS pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
  return pmc.WorkingSetSize;
}

// project_link_input.cpp livePitch (static part: the node index does not matter for the event's hz).
static e::LivePitch livePitch(const InputPitch& ip) {
  e::LivePitch lp;
  if (!ip.mapped || !(ip.hz > 0.0)) return lp;
  lp.id = ip.id;
  lp.hz = static_cast<float>(ip.hz);
  lp.noteDegree = ip.noteDegree;
  return lp;
}

// midi_capture.cpp identityOf, verbatim logic (a note's stored identity at Capture time).
static tuning::PitchId identityOf(const e::CapturedNote& n, const InputPitchMapPtr& hzMap) {
  if (hzMap && n.bendCents != 0.0f && n.raw.port != e::kLiveKeyboard) {
    const InputPitch ip = hzMap->atNoteOn(n.raw.d1, n.bendCents);
    if (ip.mapped) return ip.id;
  }
  return n.id;
}

struct Doc {
  DocumentStore st;
  TrackId midi;
  TuningId tun;
  Doc() {
    auto ed = ops::createTrack(*st.current(), st.ids(), TrackKind::Midi);
    if (!ed.ok() || !st.dispatch(ed->command).ok()) std::printf("createTrack failed\n");
    midi = ed->tracks.at(0);
    tun = st.ids().tuning();
    TuningDef d;
    d.referenceHz = 440.0 / std::exp2(9.0 / 12.0);  // 12-EDO, A4 = 440
    (void)st.dispatch(AddTuning{TuningSlot{tun, "t", Colour{}, d}});
    (void)st.dispatch(SetTrackTuning{midi, TuningAssignment::mainTuning(tun)});
  }
  void mode(NoteInputMode m, InputKeyMap k = InputKeyMap::Tuning) { (void)st.dispatch(SetNoteInput{midi, NoteInput{m, k}}); }
  void def(TuningDef d) { (void)st.dispatch(SetTuningDef{tun, std::move(d)}); }
  InputPitchMapPtr map() {
    auto m = buildInputPitchMap(*st.current(), midi);
    return m.ok() ? *m : nullptr;
  }
};

struct Rig {
  std::shared_ptr<e::LivePorts> ports = std::make_shared<e::LivePorts>();
  std::unique_ptr<e::LiveInput> in;
  std::shared_ptr<e::CaptureRing> ring = std::make_shared<e::CaptureRing>(64);
  e::EventBuffer buf{256};
  int64_t t = 0;
  Rig() { in = std::make_unique<e::LiveInput>(e::LiveInputConfig{}, ports); }
  void route(const InputPitchMap& m) {
    e::LiveRouteDesc d;
    e::LiveTrackDesc td;
    td.slot = 0;
    td.sources = 1u;
    for (uint32_t k = 0; k < 128; ++k) td.keys.push_back(livePitch(m.at(static_cast<uint8_t>(k))));
    td.capture = ring;
    d.tracks.push_back(td);
    in->publish(std::move(e::LiveRouteImage::build(d).value()));
  }
  void push(uint8_t st, uint8_t d1, uint8_t d2) {
    e::RawMidi m;
    m.port = 0;
    m.status = st;
    m.d1 = d1;
    m.d2 = d2;
    ports->push(0, m);
  }
  std::vector<e::NoteEvent> block() {
    e::LiveInput::Block b;
    b.start = t;
    b.frames = 256;
    in->process(b);
    buf.clear();
    in->emit(0, t, 256, buf);
    t += 256;
    std::vector<e::NoteEvent> out(buf.list().events, buf.list().events + buf.list().count);
    return out;
  }
};

static double centsOf(const tuning::PitchId& id) { return id.kind() == tuning::PitchKind::Cents ? id.cents() : NAN; }

int main() {
  std::printf("== 1. Capture ring memory ==\n");
  std::printf("sizeof(CapturedNote) = %zu, sizeof(RawMidi) = %zu, sizeof(PitchId) = %zu, kCaptureNotes = %u\n",
              sizeof(e::CapturedNote), sizeof(e::RawMidi), sizeof(tuning::PitchId), e::kCaptureNotes);
  {
    const size_t p0 = privateBytes(), w0 = workingSet();
    std::vector<std::unique_ptr<e::CaptureRing>> rings;
    for (int i = 0; i < 8; ++i) rings.push_back(std::make_unique<e::CaptureRing>(e::kCaptureNotes));
    const size_t p1 = privateBytes(), w1 = workingSet();
    std::printf("8 rings: private +%.2f MB (%.3f MB / ring = %.1f B / slot), working set +%.2f MB (committed and touched: "
                "value-initialised at construction)\n",
                (p1 - p0) / 1048576.0, (p1 - p0) / 8.0 / 1048576.0, (p1 - p0) / 8.0 / e::kCaptureNotes, (w1 - w0) / 1048576.0);
    std::vector<e::CapturedNote> out;
    for (int i = 0; i < 40000; ++i) {
      e::RawMidi raw;
      rings[0]->begin(i, -1, 100.0f, 0.0f, tuning::PitchId::fromDegree(0, 0), raw);
    }
    const size_t p2 = privateBytes();
    rings[0]->read(0, out);
    std::printf("a full-ring read copies %zu notes into a %.2f MB vector (UI thread, transient; private +%.2f MB)\n", out.size(),
                out.capacity() * sizeof(e::CapturedNote) / 1048576.0, (privateBytes() - p2) / 1048576.0);
  }

  std::printf("\n== 2. Hz mode: key -> stored identity -> captured identity (12-EDO A4=440) ==\n");
  {
    Doc d;
    d.mode(NoteInputMode::Hz);
    InputPitchMapPtr m = d.map();
    Rig r;
    r.route(*m);
    // +37.5 cents of bend on a +-200 cent range: 14-bit value 8192 + 1536 = 9728.
    const int v = 9728;
    r.push(0xE0, static_cast<uint8_t>(v & 127), static_cast<uint8_t>(v >> 7));
    r.push(0x90, 69, 100);
    auto ev = r.block();
    std::vector<e::CapturedNote> cap;
    r.ring->read(0, cap);
    const double heardHz = ev.empty() ? 0 : ev.back().hz;
    std::printf("event hz %.6f (expect %.6f), capture id cents %.6f, bendCents %.6f\n", heardHz, 440.0 * std::exp2(37.5 / 1200),
                centsOf(cap.at(0).id), cap.at(0).bendCents);
    const tuning::PitchId stored = identityOf(cap.at(0), m);
    std::printf("Capture's identity: cents %.9f (expect 937.5: off the 12-TET grid)\n", centsOf(stored));
    EXPECT(std::fabs(centsOf(stored) - 937.5) < 1e-6, "Hz-mode bent identity");
    // 12-TET Hz key map with bend: off-grid too.
    d.mode(NoteInputMode::Hz, InputKeyMap::TwelveTetHz);
    InputPitchMapPtr m12 = d.map();
    std::printf("TwelveTetHz atNoteOn(69, +37.5) cents %.9f\n", centsOf(m12->atNoteOn(69, 37.5).id));

    // 2b. The same captured note, Capture pressed after the tuning's reference moved (A4 = 432): the bent note is
    // re-derived from the CURRENT map's key, the unbent one keeps what was played.
    d.mode(NoteInputMode::Hz);
    TuningDef d432;
    d432.referenceHz = 432.0 / std::exp2(9.0 / 12.0);
    d.def(d432);
    InputPitchMapPtr m2 = d.map();
    const tuning::PitchId later = identityOf(cap.at(0), m2);
    const double playedHz = 440.0 * std::exp2(37.5 / 1200.0);
    const double laterHz = d432.referenceHz * std::exp2(centsOf(later) / 1200.0);  // Cents follow the reference (Q?-T6.1-a 2)
    const double oldRefHz = (440.0 / std::exp2(9.0 / 12.0)) * std::exp2(centsOf(later) / 1200.0);
    std::printf("after reference 440->432: bent note's identity %.6f cents (played as %.6f): identity shifts by %.6f cents; "
                "an unbent neighbour keeps %.6f cents (its played identity)\n",
                centsOf(later), centsOf(stored), centsOf(later) - centsOf(stored), 900.0);
    (void)playedHz;
    (void)laterHz;
    (void)oldRefHz;

    // 2c. Tuning switched to 31-EDO between play and Capture: bent note becomes key 69 of 31-EDO (+bend).
    TuningDef e31;
    e31.referenceHz = 440.0 / std::exp2(9.0 / 12.0);
    e31.system = tuning::Edo::make(31).value();
    d.def(e31);
    InputPitchMapPtr m3 = d.map();
    const tuning::PitchId later31 = identityOf(cap.at(0), m3);
    std::printf("after 12-EDO -> 31-EDO: bent note's identity %.4f cents (played %.4f): %+.2f cents wrong; unbent keeps 900\n",
                centsOf(later31), centsOf(stored), centsOf(later31) - centsOf(stored));
    EXPECT(std::fabs(centsOf(later31) - centsOf(stored)) < 1e-6, "captured bent Hz note keeps the played pitch after a tuning change");
    // The rule that would keep it (one add, no map): Cents id + note-on bend.
    const tuning::PitchId fix = tuning::PitchId::fromCents(cap.at(0).id.cents() + cap.at(0).bendCents);
    std::printf("fromCents(id + bendCents) = %.9f (== atNoteOn result at play time: %s)\n", fix.cents(),
                std::fabs(fix.cents() - centsOf(stored)) < 1e-9 ? "yes" : "no");
  }

  std::printf("\n== 3. Played in Keys mode with a bend, switched to Hz before Capture ==\n");
  {
    Doc d;
    InputPitchMapPtr mk = d.map();
    Rig r;
    r.route(*mk);
    const int v = 9728;  // +37.5 cents
    r.push(0xE0, static_cast<uint8_t>(v & 127), static_cast<uint8_t>(v >> 7));
    r.push(0x90, 64, 100);
    (void)r.block();
    std::vector<e::CapturedNote> cap;
    r.ring->read(0, cap);
    std::printf("played id kind %s (Degree %d)\n", cap.at(0).id.kind() == tuning::PitchKind::Degree ? "Degree" : "other",
                cap.at(0).id.kind() == tuning::PitchKind::Degree ? cap.at(0).id.degree() : -99);
    d.mode(NoteInputMode::Hz);
    InputPitchMapPtr mh = d.map();
    const tuning::PitchId got = identityOf(cap.at(0), mh);
    std::printf("Capture after the switch stores kind %s, %.4f cents (a Keys note played as degree 4 becomes a frozen bent Hz)\n",
                got.kind() == tuning::PitchKind::Cents ? "Cents" : "Degree", centsOf(got));
    EXPECT(got.kind() == tuning::PitchKind::Degree, "a Keys-played note stays a Degree identity");
    // and the other way: Hz played with bend, switched to Keys: the bend is silently dropped
    Doc d2;
    d2.mode(NoteInputMode::Hz);
    InputPitchMapPtr mh2 = d2.map();
    Rig r2;
    r2.route(*mh2);
    r2.push(0xE0, static_cast<uint8_t>(v & 127), static_cast<uint8_t>(v >> 7));
    r2.push(0x90, 69, 100);
    (void)r2.block();
    std::vector<e::CapturedNote> cap2;
    r2.ring->read(0, cap2);
    d2.mode(NoteInputMode::Keys);
    const tuning::PitchId got2 = identityOf(cap2.at(0), nullptr);  // planCapture passes hzMap only in Hz mode
    std::printf("Hz-played (bent +37.5) captured after switching to Keys: %.4f cents (played 937.5)\n", centsOf(got2));
    EXPECT(std::fabs(centsOf(got2) - 937.5) < 1e-6, "an Hz-played bent note keeps its bend");
  }

  std::printf("\n== 4. A port index reused after a close keeps the old device's bend ==\n");
  {
    Doc d;
    InputPitchMapPtr m = d.map();
    Rig r;
    r.route(*m);
    r.push(0xE0, 127, 127);  // device A: wheel at +200 cents, then unplugged (MidiInputService::close pushes CC 123 x16)
    for (uint8_t ch = 0; ch < 16; ++ch) r.push(static_cast<uint8_t>(0xB0 | ch), 123, 0);
    (void)r.block();
    r.push(0x90, 69, 100);  // device B opened at the same index; its first note, wheel centred (no bend message sent)
    auto ev = r.block();
    const double hz = ev.empty() ? 0 : ev.back().hz;
    std::printf("device B's first A4: %.3f Hz (%+.1f cents off 440)\n", hz, 1200 * std::log2(hz / 440.0));
    EXPECT(std::fabs(hz - 440.0) < 0.01, "a new device starts unbent");
  }

  std::printf("\n== 5. Held note across route changes: unrouted, then image null, then note-off ==\n");
  {
    Doc d;
    InputPitchMapPtr m = d.map();
    Rig r;
    r.route(*m);
    r.push(0x90, 60, 100);
    (void)r.block();
    r.in->publish(nullptr);  // every track disarmed
    (void)r.block();
    r.in->collect(false);
    r.push(0x80, 60, 0);
    auto ev = r.block();
    std::printf("note-off after disarm: %zu event(s), kind %d, held now %u\n", ev.size(), ev.empty() ? -1 : (int)ev[0].kind,
                r.in->heldNotes());
    EXPECT(ev.size() == 1 && ev[0].kind == e::NoteEventKind::NoteOff, "note-off reaches the slot");
    std::vector<e::CapturedNote> cap;
    r.ring->read(0, cap);
    std::printf("its capture: end %s (the note is captured open: ends at 'now' when captured)\n",
                cap.at(0).endTime == e::kCaptureOpen ? "OPEN" : "closed");
  }
  std::printf("\n%d failure(s)\n", fails);
  return 0;
}
