// SM.1: a note's MIDI key (pq/document/midi_key.h): Degree identities 60 + absolute degree or the lowest .kbm key of
// that degree; Cents / Ratio pitches the nearest 12-TET key; outside 0 … 127 none (−1).
#include <doctest/doctest.h>

#include <cmath>
#include <optional>

#include "pq/document/input_map.h"
#include "pq/document/midi_key.h"
#include "pq/document/model.h"
#include "pq/document/pitch_resolve.h"
#include "pq/tuning/edo.h"
#include "pq/tuning/scala.h"

using namespace pq;
using namespace pq::doc;

namespace {

// The key as the sequence lowering computes it: the note resolved in `def`, then midiKeyOf.
int16_t keyOf(const TuningDef& def, const tuning::PitchId& id) {
  auto t = buildTuning(def);
  REQUIRE(t.ok());
  const NotePitch np = resolvePitch(id, *t, TuningId{});
  REQUIRE(np.hz.has_value());
  return midiKeyOf(id, np.keyDegree, *np.hz, DegreeKeyMap(&def));
}

TuningDef scalaKbm(const char* kbmText) {
  auto scl = tuning::parseScl("! t.scl\nfour notes\n 4\n 9/8\n 386.31371386\n 3/2\n 2/1\n");
  REQUIRE(scl.ok());
  auto kbm = tuning::parseKbm(kbmText);
  REQUIRE(kbm.ok());
  auto s = makeScaleDef(std::move(scl).value(), std::move(kbm).value());
  REQUIRE(s.ok());
  return std::move(s).value();
}

}  // namespace

TEST_CASE("midi key (SM.1): Degree notes are 60 + their absolute degree; Cents / Ratio notes the nearest 12-TET key") {
  TuningDef edo12;  // the default: 12-EDO from 261.63 Hz
  CHECK(keyOf(edo12, tuning::PitchId::fromDegree(0)) == 60);
  CHECK(keyOf(edo12, tuning::PitchId::fromDegree(4, 1)) == 76);
  CHECK(keyOf(edo12, tuning::PitchId::fromDegree(5, -2)) == 41);
  TuningDef edo31;
  edo31.system = tuning::Edo::make(31).value();
  CHECK(keyOf(edo31, tuning::PitchId::fromDegree(4, 1)) == 95);   // 60 + 35, not a 12-TET key
  CHECK(keyOf(edo31, tuning::PitchId::fromDegree(18, 0)) == 78);  // 60 + 18 (≈ a fifth, 12-TET key 67)
  // Cents and Ratio: the nearest 12-TET key of the Hz (key 69 = 440 Hz), in any tuning.
  const double c440 = 1200.0 * std::log2(440.0 / edo31.referenceHz);
  CHECK(keyOf(edo31, tuning::PitchId::fromCents(c440)) == 69);
  CHECK(keyOf(edo31, tuning::PitchId::fromCents(c440 + 49.0)) == 69);
  CHECK(keyOf(edo31, tuning::PitchId::fromCents(c440 + 51.0)) == 70);
  CHECK(keyOf(edo31, tuning::PitchId::fromRatio(tuning::Rational(3, 2))) == 67);  // 392.4 Hz from 261.63
  CHECK(keyOf(edo12, tuning::PitchId::fromRatio(tuning::Rational(5, 4))) == 64);  // 327.0 Hz: −14 ¢ from key 64
  // Outside 0 … 127: none.
  CHECK(keyOf(edo12, tuning::PitchId::fromDegree(68)) == tuning::kNoMidiKey);
  CHECK(keyOf(edo12, tuning::PitchId::fromDegree(67)) == 127);
  CHECK(keyOf(edo12, tuning::PitchId::fromDegree(-61)) == tuning::kNoMidiKey);
  CHECK(keyOf(edo12, tuning::PitchId::fromDegree(-60)) == 0);
  CHECK(tuning::nearestMidiKeyOrNone(0.0) == tuning::kNoMidiKey);
  CHECK(tuning::nearestMidiKeyOrNone(std::nan("")) == tuning::kNoMidiKey);
  // A Degree identity without a degree (an unresolvable tuning) has no key.
  CHECK(midiKeyOf(tuning::PitchId::fromDegree(3), std::nullopt, 300.0, DegreeKeyMap(nullptr)) == tuning::kNoMidiKey);
}

TEST_CASE("midi key (SM.1): with a .kbm a Degree note takes the lowest key that plays its degree; an unplayed degree none") {
  // Middle key 60 = degree 0, mapping 0 1 x 3 per 4 keys (formal octave 4 degrees).
  const TuningDef def = scalaKbm("4\n0\n127\n60\n69\n440.0\n4\n0\n1\nx\n3\n");
  const DegreeKeyMap keys(&def);
  CHECK(keys.hasKbm());
  CHECK(keys.key(0) == 60);
  CHECK(keys.key(1) == 61);
  CHECK(keys.key(2) == tuning::kNoMidiKey);  // 'x': no key plays it
  CHECK(keys.key(3) == 63);
  CHECK(keys.key(4) == 64);
  CHECK(keys.key(-4) == 56);
  CHECK(keyOf(def, tuning::PitchId::fromDegree(3, 1)) == 67);  // absolute degree 7
  CHECK(keyOf(def, tuning::PitchId::fromDegree(2)) == tuning::kNoMidiKey);
  // Several keys mapping one degree: the lowest of them, as MIDI export's Degrees mode writes it.
  const TuningDef dup = scalaKbm("4\n0\n127\n60\n69\n440.0\n4\n0\n0\n1\n3\n");
  const DegreeKeyMap dk(&dup);
  for (int64_t d = -8; d <= 8; ++d) {
    CAPTURE(d);
    int64_t lowest = tuning::kNoMidiKey;
    for (int32_t k = 127; k >= 0; --k)
      if (tuning::mapKeyToDegree(*dup.scale()->mapping, k) == d) lowest = k;
    CHECK(dk.key(d) == lowest);
  }
  CHECK(dk.key(0) == 60);  // keys 60 and 61 both play degree 0
}
