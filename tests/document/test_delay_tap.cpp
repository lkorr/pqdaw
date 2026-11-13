// DT.1 (docs/plans/DT.md): the Delay Tap module's document side — the time law (62.5 ms · (e^(k·x) − 1), k = 2·ln 9),
// the note values and their seconds at a tempo, the group it builds (roles, the delay atom's 5 s ring, its panel).
#include <doctest/doctest.h>

#include <cmath>
#include <ostream>
#include <string>

#include "pq/core/time.h"
#include "pq/document/modules.h"

using namespace pq;
using namespace pq::doc;

TEST_CASE("delay tap: the time law — 0 at 0, 500 ms at 0.5, 5 s at 1, monotonic, the inverse round-trips") {
  const dsp::Curve c = delayTimeCurve();
  CHECK(c.unit == dsp::CurveUnit::Seconds);
  CHECK(dsp::evalCurve(c, 0.0) == 0.0);
  CHECK(dsp::evalCurve(c, 0.5) == doctest::Approx(0.5).epsilon(1e-12));
  CHECK(dsp::evalCurve(c, 1.0) == doctest::Approx(5.0).epsilon(1e-12));
  const double k = 2.0 * std::log(9.0);
  double last = -1.0;
  for (int i = 0; i <= 1000; ++i) {
    const double x = i / 1000.0, v = dsp::evalCurve(c, x);
    CHECK(v == doctest::Approx(0.0625 * std::expm1(k * x)).epsilon(1e-12));
    CHECK(v > last);
    last = v;
    if (i > 0) CHECK(dsp::curvePosition(c, v, 0.3) == doctest::Approx(x).epsilon(1e-9));
  }
}

TEST_CASE("delay tap: note values 1/64T … 1/1. ascending; seconds at 120 and 90 BPM; the 5 s clamp") {
  const auto v = noteValues();
  REQUIRE(v.size() == 21);
  for (size_t i = 1; i < v.size(); ++i) CHECK(v[i].beats > v[i - 1].beats);
  CHECK(v[kNoteValueQuarter].label == "1/4");
  CHECK(v[kNoteValueQuarter].beats == 1.0);
  CHECK(v[kNoteValueQuarter].beats * kPpq == kPpq);
  uint32_t straight = 0, dotted = 0, triplet = 0;
  for (const NoteValue& n : v) {
    const std::string l(n.label);
    const char want = n.kind == NoteValueKind::Dotted ? '.' : n.kind == NoteValueKind::Triplet ? 'T' : 0;
    straight += n.kind == NoteValueKind::Straight ? 1 : 0;
    dotted += n.kind == NoteValueKind::Dotted ? 1 : 0;
    triplet += n.kind == NoteValueKind::Triplet ? 1 : 0;
    if (want != 0) CHECK(l.back() == want);
    else CHECK(std::isdigit(static_cast<unsigned char>(l.back())));
  }
  CHECK(straight == 7);
  CHECK(dotted == 7);
  CHECK(triplet == 7);
  // 1/8. = 0.75 beats: 375 ms at 120, 500 ms at 90; 1/4T = 2/3 beat: 333 ms at 120.
  CHECK(noteValueSeconds(12, 120.0) == doctest::Approx(0.375));
  CHECK(noteValueSeconds(12, 90.0) == doctest::Approx(0.5));
  CHECK(noteValueSeconds(11, 120.0) == doctest::Approx(1.0 / 3.0));
  CHECK(noteValueSeconds(20, 120.0) == doctest::Approx(3.0));  // 1/1. = 6 beats
  CHECK(noteValueSeconds(20, 60.0) == kDelayTapMaxSeconds);    // 6 s → 5
  CHECK(noteValueSeconds(19, 48.0) == kDelayTapMaxSeconds);    // 1/1 at 48 BPM = 5 s exactly
  CHECK(noteValueSeconds(99, 120.0) == noteValueSeconds(20, 120.0));  // clamped index
}

TEST_CASE("delay tap: the group — roles, In → delay_cubic (max 5 s) → Out, the tempo slider bound to Time, Sync, Mode") {
  IdSource ids;
  DelayTapRoles r;
  const BlockSet set = delayTapBlocks(ids, {}, &r);
  REQUIRE_FALSE(set.devices.empty());
  CHECK(set.devices.front()->id == r.group);
  CHECK(set.devices.front()->name == "Delay Tap");
  const Device* time = nullptr;
  const Device* delay = nullptr;
  for (const DevicePtr& d : set.devices) {
    if (d->id == r.time) time = d.get();
    if (d->id == r.delay) delay = d.get();
    if (d->id == r.time) CHECK(delayRole(*d) == DelayRole::Time);
    if (d->id == r.sync) CHECK(delayRole(*d) == DelayRole::Sync);
    if (d->id == r.mode) CHECK(delayRole(*d) == DelayRole::Mode);
  }
  REQUIRE(time != nullptr);
  REQUIRE(delay != nullptr);
  CHECK(delay->patch == std::string(kAtomDevicePrefix) + "delay_cubic");
  REQUIRE(delay->setting("max") != nullptr);
  CHECK(delay->setting("max")->value == kDelayTapMaxSeconds);
  CHECK(*time->curve(kValueParam) == delayTimeCurve());
  CHECK(dsp::evalCurve(*time->curve(kValueParam), time->param(kValueParam)->value) == doctest::Approx(0.5));  // default 500 ms
  bool in = false, out = false;
  for (const Wire& w : set.wires) {
    in = in || (w.from == r.group && w.port == kGroupInPort && w.to == r.delay && w.param == "x");
    out = out || (w.from == r.delay && w.to == r.group && w.param == kGroupOutPort);
  }
  CHECK(in);
  CHECK(out);
  const Panel& p = *set.devices.front()->panel;
  REQUIRE(p.elements.size() == 2);
  const PanelElement& s = p.elements[1];
  CHECK(s.kind == "temposlider");
  REQUIRE(s.bindings.size() == 3);
  CHECK(s.bindings[0] == controlBinding(r.time, std::string(kValueParam)));
  CHECK(s.bindings[1] == controlBinding(r.sync, std::string(kValueParam)));
  CHECK(s.bindings[2] == controlBinding(r.mode, std::string(kValueParam)));
  CHECK(findModule("delay_tap") != nullptr);
  CHECK(findModule("Delay Tap")->kind == ModuleKind::DelayTap);
}
