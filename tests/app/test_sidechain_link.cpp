// DY.2 (docs/plans/DY.md "DY.2"; SPEC §8 "sidechain", §15): a sidechain cable is latency-compensated through the link.
// Kick and Bass play the same note on the same instrument; Kick's path ends in `latency(x, 480)`, Bass's in
// `sub(a = its own audio, b = Kick's Post FX cable)`. Compensated, the cable's signal arrives aligned with Bass's own
// audio (Bass's sources play 480 frames late), so Bass is exactly silent and the master equals Kick alone; the total
// (480) is reported. Uncompensated (a Modulation wire, as before DY.2) Bass would play s[n] − s[n − 480].
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/export_render.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/engine/offline_render.h"
#include "../support/blocks.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint64_t kFrames = 24000;  // half a second: the note (a beat at 120 BPM) and its 480-frame shift

struct Project2 {
  DocumentStore store;
  TrackId kick, bass;
  DeviceId lat, sub;
  WireId cable;
  Project2() {
    store.setDeviceLibrary(&playback_fixture::unitsLibrary());
    kick = midi("Kick");
    bass = midi("Bass");
    lat = test_blocks::appendAfter(store, kick, "atom:latency");
    test_blocks::dispatchEdit(store, ops::setDeviceSetting(*store.current(), lat, "frames", 480.0));
    sub = test_blocks::appendAfter(store, bass, "atom:sub");
    Wire w;
    w.from = lat;
    w.to = sub;
    w.param = "b";
    auto e = ops::connect(*store.current(), store.ids(), w);
    test_blocks::dispatchEdit(store, e);
    cable = e->wires.at(0);
  }
  TrackId midi(const char* name) {
    auto e = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi, {}, name);
    test_blocks::dispatchEdit(store, e);
    const TrackId t = e->tracks.at(0);
    IdSource& ids = store.ids();
    std::vector<Note> n{playback_fixture::note(ids, 0, kPpq, 0, 0, 100.0f)};
    REQUIRE(store.dispatch(AddClip{t, playback_fixture::clip(ids, 0, 2 * kPpq, std::move(n), name)}).ok());
    return t;
  }
};

struct Rendered {
  std::vector<std::vector<float>> out;
  uint32_t latency = 0;
  double bassMs = -1.0;  // Bass's own latency (L of its out bus): 10 ms when it reads Kick's latent bus compensated
};
Rendered render(const Snapshot& s, TrackId bass = {}) {
  auto link = app::makeOfflineLink(s, 48000.0, "", 0, false);
  REQUIRE_MESSAGE(link.ok(), (link.ok() ? std::string() : link.error().message));
  REQUIRE((*link)->settleOffline(0, app::kSettleMinSeconds, app::kSettleMaxSeconds).ok());
  engine::BufferSink sink;
  REQUIRE((*link)->renderOffline(0, kFrames, sink).ok());
  return {std::move(sink.channels), (*link)->outputLatency(), bass.valid() ? (*link)->trackLatencyMs(bass.value) : -1.0};
}

float peak(const std::vector<std::vector<float>>& a, size_t from = 0, size_t to = kFrames) {
  float p = 0.0f;
  for (const auto& c : a)
    for (size_t i = from; i < std::min(to, c.size()); ++i) p = std::max(p, std::fabs(c[i]));
  return p;
}

}  // namespace

TEST_CASE("DY.2 sidechain: a latent Kick cabled into Bass's input arrives aligned with Bass's own audio (Bass nulls); "
          "the total is reported") {
  Project2 p;
  // The reference: Kick alone (Bass deleted), late by its latency at the master.
  Snapshot kickOnly;
  {
    DocumentStore copy{p.store.current()};
    const TrackId del[] = {p.bass};
    test_blocks::dispatchEdit(copy, ops::deleteTracks(*copy.current(), del));
    kickOnly = copy.current();
  }
  const Rendered ref = render(kickOnly);
  CHECK(ref.latency == 480);
  CHECK(peak(ref.out) > 0.01f);
  CHECK(peak(ref.out, 0, 480) == 0.0f);  // the note starts 480 frames late
  // Kick + Bass: Bass = its own audio − Kick's delayed audio, both 480 late: silent, so the master is Kick alone.
  const Rendered both = render(p.store.current(), p.bass);
  CHECK(both.latency == 480);
  CHECK(both.bassMs == doctest::Approx(10.0));  // (a Modulation read — before DY.2 — is never compensated: 0)
  REQUIRE(both.out.size() == ref.out.size());
  double diff = 0.0;
  for (size_t c = 0; c < both.out.size(); ++c)
    for (size_t i = 0; i < both.out[c].size(); ++i)
      diff = std::max(diff, std::fabs(static_cast<double>(both.out[c][i]) - static_cast<double>(ref.out[c][i])));
  MESSAGE("max |master - kick alone| = " << diff);
  CHECK(diff < 1e-6);
}
