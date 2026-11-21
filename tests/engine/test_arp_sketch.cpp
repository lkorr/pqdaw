// AP.10 (docs/plans/AP.md "AP.10"; pq/engine/arp_sketch.h): the resolved cycle the arpeggiator panel draws — the real
// ArpCore run offline over a held set: one cycle's notes (steps, identities, the status position / copy of each step),
// their pitch curves (the voice's ramp formula), Harmonic Walk and Distance / Steps transpositions, determinism.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "pq/engine/arp_sketch.h"

namespace e = pq::engine;

namespace {

e::ArpSketchHeld note(float hz, float harmonic = 0.0f) {
  e::ArpSketchHeld h;
  h.hz = hz;
  h.harmonic = harmonic;
  return h;
}

}  // namespace

TEST_CASE("arp sketch (AP.10): Up over two notes, Distance 12 degrees × Steps 2 — one cycle of four steps") {
  e::ArpParams p;
  p.distance = 12;
  p.steps = 2;
  const std::vector<e::ArpSketchHeld> held = {note(300.0f), note(200.0f)};
  const e::ArpSketch s = e::sketchArp(p, {}, {}, held);
  CHECK(s.cycle == 4.0);
  CHECK_FALSE(s.truncated);
  CHECK(s.stepMs == doctest::Approx(125.0));  // 1/16 at 120 BPM
  REQUIRE(s.notes.size() == 4);
  const float hz[4] = {200.0f, 300.0f, 400.0f, 600.0f};
  for (size_t i = 0; i < 4; ++i) {
    CAPTURE(i);
    CHECK(s.notes[i].on == doctest::Approx(static_cast<double>(i)));
    CHECK(s.notes[i].off == doctest::Approx(static_cast<double>(i) + 0.5));  // gate 50 %
    CHECK(s.notes[i].hz == doctest::Approx(hz[i]).epsilon(1e-5));
    CHECK(s.notes[i].source == (i % 2 == 0 ? 1u : 0u));  // held[1] is the lower
    CHECK(s.notes[i].position == static_cast<int32_t>(i % 2));
    CHECK(s.notes[i].copy == static_cast<int32_t>(i / 2));
    CHECK(s.notes[i].exact.kind == (i < 2 ? e::ArpExact::Kind::Same : e::ArpExact::Kind::Degrees));
    if (i >= 2) CHECK(s.notes[i].exact.degrees == 12);
    CHECK(s.notes[i].numBends == 0);
  }
}

TEST_CASE("arp sketch (AP.10): Harmonic Walk over a JI chord — every walked harmonic, exact ratios, merged by Hz") {
  e::ArpParams p;
  p.style = e::ArpStyle::HarmonicWalk;
  p.unit = e::ArpUnit::Harmonics;
  p.walkRange = 3;
  // h4 and h6 of a 50 Hz anchor: walks 4…7 and 6…9 merge to 4 5 6 7 8 9 (6 and 7 once).
  const std::vector<e::ArpSketchHeld> held = {note(200.0f, 4.0f), note(300.0f, 6.0f)};
  const e::ArpSketch s = e::sketchArp(p, {}, {}, held);
  CHECK(s.cycle == 6.0);
  REQUIRE(s.notes.size() == 6);
  for (size_t i = 0; i < 6; ++i) {
    CAPTURE(i);
    CHECK(s.notes[i].hz == doctest::Approx(50.0 * static_cast<double>(4 + i)).epsilon(1e-5));
    CHECK(s.notes[i].harmonic == static_cast<float>(4 + i));
  }
  CHECK(s.notes[0].exact.kind == e::ArpExact::Kind::Same);  // h4 itself
  CHECK(s.notes[1].exact.kind == e::ArpExact::Kind::Ratio);  // h5 = 5/4 of h4
  CHECK(s.notes[1].exact.num == 5);
  CHECK(s.notes[1].exact.den == 4);
}

TEST_CASE("arp sketch (AP.10): Into and Out glides as the note's curve; Pulse cycle; truncation; deterministic") {
  e::ArpParams p;
  p.into = {e::ArpGlide::Step, 0.0f, true, 50.0f, 0.4f, e::RampShape::Exponential, 0.5f};
  p.out = {e::ArpGlide::Offset, 2.0f, true, 50.0f, 0.2f, e::RampShape::SCurve, 0.0f};
  const std::vector<e::ArpSketchHeld> held = {note(200.0f), note(300.0f)};
  const e::ArpSketch s = e::sketchArp(p, {}, {}, held);
  REQUIRE(s.notes.size() == 2);
  const e::ArpSketchNote& n = s.notes[1];  // 300 Hz: Into from 200 over 40 % of its 0.5-step note, Out at 80 %
  CHECK(n.hz == 200.0f);
  CHECK(n.ownHz == 300.0f);
  REQUIRE(n.numBends == 2);
  CHECK(s.bends[n.firstBend].len == doctest::Approx(0.2).epsilon(1e-3));
  const double mid = n.on + 0.1;  // half way through the Into glide
  CHECK(e::arpSketchHzAt(s, n, mid) ==
        doctest::Approx(200.0 * std::exp2(std::log2(1.5) * e::rampShapeAt(e::RampShape::Exponential, 64, 0.5))).epsilon(1e-3));
  CHECK(e::arpSketchHzAt(s, n, n.on + 0.3) == doctest::Approx(300.0).epsilon(1e-6));  // the hold
  const double outMid = n.off - 0.05;  // half way through the Out glide (0.1 of a step) to +2 degrees
  CHECK(e::arpSketchHzAt(s, n, outMid) == doctest::Approx(300.0 * std::exp2(100.0 / 1200.0)).epsilon(1e-3));
  CHECK(e::arpSketchHzAt(s, n, n.off) == doctest::Approx(300.0 * std::exp2(200.0 / 1200.0)).epsilon(1e-3));

  const e::ArpSketch again = e::sketchArp(p, {}, {}, held);
  REQUIRE(again.notes.size() == s.notes.size());
  for (size_t i = 0; i < s.notes.size(); ++i) {
    CHECK(again.notes[i].on == s.notes[i].on);
    CHECK(again.notes[i].hz == s.notes[i].hz);
  }

  e::ArpParams pu;  // Pulse 8 : 12 with h_ref 8: the cycle is 8 Base periods, 8 + 12 pulses
  pu.pulse = true;
  const e::ArpSketch ps = e::sketchArp(pu, {}, {}, std::vector<e::ArpSketchHeld>{note(200.0f, 8.0f), note(300.0f, 12.0f)});
  CHECK(ps.cycle == doctest::Approx(8.0));
  CHECK(ps.notes.size() == 20);

  e::ArpParams lp;  // 8 notes × Steps 8 = 64 steps: fits; Steps 8 over 9 notes does not
  lp.steps = 8;
  std::vector<e::ArpSketchHeld> nine;
  for (int i = 0; i < 9; ++i) nine.push_back(note(200.0f + 20.0f * static_cast<float>(i)));
  const e::ArpSketch ts = e::sketchArp(lp, {}, {}, nine);
  CHECK(ts.truncated);
  CHECK(ts.cycle == 64.0);
  CHECK(ts.notes.size() == 64);
}
