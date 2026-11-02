// TX.spectrum: analysis taps through the playback link — EQ Eight's and a chain Spectrum module's taps exist once their
// unit compiled (and nothing else has one); unwatched the audio thread copies nothing; watched, every block lands in the
// tap (RtScope: the engine block allocates nothing either way); the Spectrum module is bit-exact pass-through (the
// project renders the same with and without it); EQ Eight's measured response from its real input and output matches
// its exact curve (the display's math) where the signal excites it; the per-block cost of a watched tap (perf).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/views/src/devices/device_math.h"  // the EQ display's exact curve (tests)
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/dsp/transfer.h"
#include "pq/engine/project_program.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  std::vector<float> out;
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out.insert(out.end(), l.begin(), l.end());
      out.insert(out.end(), r.begin(), r.end());
    }
  }
};

// A MIDI track (the default synth playing a chord), EQ Eight, and optionally a Spectrum module after it.
struct Keys {
  DocumentStore store;
  TrackId track;
  DeviceId inst, eq, spectrum;
  explicit Keys(bool withSpectrum) {
    IdSource& ids = store.ids();
    track = ids.track();
    inst = ids.device();
    Batch b{"keys", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys", inst), {}, kAppend});
    std::vector<Note> notes;
    for (int32_t d : {0, 4, 7, 11}) notes.push_back(playback_fixture::note(ids, 0, 64 * kPpq, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 64 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
    eq = edit(ops::addDevice(*store.current(), store.ids(), track, "eq_eight", DeviceKind::AudioEffect)).devices.at(0);
    // A +6 dB bell at 1 kHz (Q 2), −4 dB at 250 Hz: a curve to measure.
    edit(ops::setDeviceParam(*store.current(), eq, "freq4", 1000.0));
    edit(ops::setDeviceParam(*store.current(), eq, "gain4", 1.9952623));
    edit(ops::setDeviceParam(*store.current(), eq, "q4", 2.0));
    edit(ops::setDeviceParam(*store.current(), eq, "freq3", 250.0));
    edit(ops::setDeviceParam(*store.current(), eq, "gain3", 0.6309573));
    if (withSpectrum) spectrum = edit(ops::addMetaDevice(*store.current(), store.ids(), track, "spectrum")).devices.at(0);
  }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
    return *e;
  }
};

// EQ Eight's exact response with Keys' settings (the panel's math).
double eqExactDb(double hz) {
  namespace m = pq::views::devices::math;
  std::complex<double> h = 1.0;
  const struct {
    double f, g, q;
  } bands[] = {{250.0, 0.6309573, 0.7071}, {1000.0, 1.9952623, 2.0}};
  for (const auto& b : bands) {
    m::EqBand band;
    band.on = true;
    band.type = m::kBell;
    band.freq = b.f;
    band.gain = b.g;
    band.q = b.q;
    h *= m::eqBandResponse(band, m::eqCoef(band, false, 1.0, 48000.0, 1.0), hz, 48000.0, 1.0);
  }
  return 20.0 * std::log10(std::abs(h));
}

}  // namespace

TEST_CASE("analysis link: taps exist for EQ Eight and Spectrum; unwatched nothing is copied; watched every block") {
  Keys k(true);
  auto link = makeLink(k.store.current());
  CHECK(link->lastError().empty());
  const std::shared_ptr<TapRing> eqTap = link->deviceAnalysisTap(k.eq), spTap = link->deviceAnalysisTap(k.spectrum);
  REQUIRE(eqTap != nullptr);
  REQUIRE(spTap != nullptr);
  CHECK(eqTap != spTap);
  CHECK(eqTap->channels() == 4);
  CHECK(link->deviceAnalysisTap(k.inst) == nullptr);  // the synth is not analysed
  Driver d{*link};
  link->play(0);
  d.run(50);
  CHECK(eqTap->written() == 0);  // nobody watches: one load per block, no copy
  CHECK(spTap->written() == 0);
  eqTap->addReader();
  spTap->addReader();
  d.run(100);
  CHECK(eqTap->written() == 100 * kBlock);
  CHECK(spTap->written() == 100 * kBlock);
  // The Spectrum module's input and output channels are the same signal: EQ Eight's output.
  std::vector<float> a(1024), b(1024), c(1024);
  const uint64_t from = spTap->written() - 1024;
  REQUIRE(spTap->read(0, from, 1024, a.data()) == 1024);
  REQUIRE(spTap->read(2, from, 1024, b.data()) == 1024);
  REQUIRE(eqTap->read(2, from, 1024, c.data()) == 1024);
  CHECK(std::memcmp(a.data(), b.data(), sizeof(float) * 1024) == 0);
  CHECK(std::memcmp(a.data(), c.data(), sizeof(float) * 1024) == 0);
  float peak = 0.0f;
  for (const float v : a) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.01f);  // the chord plays
  eqTap->removeReader();
  spTap->removeReader();
}

TEST_CASE("analysis link: the Spectrum module is bit-exact pass-through") {
  Keys with(true), without(false);
  auto la = makeLink(with.store.current()), lb = makeLink(without.store.current());
  Driver da{*la}, db{*lb};
  la->play(0);
  lb->play(0);
  la->deviceAnalysisTap(with.spectrum)->addReader();  // watched or not, the sound is the same
  da.run(400);
  db.run(400);
  REQUIRE(da.out.size() == db.out.size());
  CHECK(std::memcmp(da.out.data(), db.out.data(), da.out.size() * sizeof(float)) == 0);
  la->deviceAnalysisTap(with.spectrum)->removeReader();
}

TEST_CASE("analysis link: EQ Eight's measured response matches its exact curve where the chord excites it") {
  Keys k(false);
  auto link = makeLink(k.store.current());
  const std::shared_ptr<TapRing> tap = link->deviceAnalysisTap(k.eq);
  REQUIRE(tap != nullptr);
  std::vector<float> axis(512);
  for (uint32_t i = 0; i < 512; ++i) axis[i] = 20.0f * std::pow(1000.0f, static_cast<float>(i) / 511.0f);
  dsp::TransferAnalyzer t(dsp::TransferConfig{}, axis);
  Driver d{*link};
  link->play(0);
  tap->addReader();
  std::vector<float> in(4096), out(4096), l(4096), r(4096);
  uint64_t pos = 0;
  for (int s = 0; s < 60; ++s) {  // 60 × 64 blocks ≈ 5 s
    d.run(64);
    const uint64_t w = tap->written();
    while (pos < w) {
      const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(w - pos, 4096));
      REQUIRE(tap->read(0, pos, n, l.data()) == n);
      REQUIRE(tap->read(1, pos, n, r.data()) == n);
      for (uint32_t i = 0; i < n; ++i) in[i] = 0.5f * (l[i] + r[i]);
      REQUIRE(tap->read(2, pos, n, l.data()) == n);
      REQUIRE(tap->read(3, pos, n, r.data()) == n);
      for (uint32_t i = 0; i < n; ++i) out[i] = 0.5f * (l[i] + r[i]);
      t.push(in.data(), out.data(), n);
      pos += n;
    }
  }
  tap->removeReader();
  uint32_t confident = 0;
  double worst = 0.0;
  for (uint32_t p = 0; p < t.points(); ++p) {
    if (t.confidence()[p] < 0.99f) continue;
    ++confident;
    worst = std::max(worst, std::fabs(t.magnitudeDb()[p] - eqExactDb(t.pointHz(p))));
  }
  MESSAGE("EQ Eight measured vs exact: " << confident << " confident points, worst " << worst << " dB");
  CHECK(confident >= 20);  // the chord's partials (a synth: no noise between them)
  CHECK(worst < 0.75);  // measured 0.52 dB: leakage of the chord's partials across the bells' slopes
}

TEST_CASE("analysis link: a watched tap's cost per engine block" * doctest::test_suite("perf")) {
  Keys k(true);
  auto link = makeLink(k.store.current());
  Driver d{*link};
  link->play(0);
  d.run(200);
  auto timeRun = [&](uint32_t blocks) {
    const auto t0 = std::chrono::steady_clock::now();
    d.run(blocks);
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
  };
  const double idle = timeRun(4000);
  link->deviceAnalysisTap(k.eq)->addReader();
  link->deviceAnalysisTap(k.spectrum)->addReader();
  const double watched = timeRun(4000);
  MESSAGE("engine block (64 frames), 2 taps: unwatched " << idle << " us, watched " << watched << " us");
  link->deviceAnalysisTap(k.eq)->removeReader();
  link->deviceAnalysisTap(k.spectrum)->removeReader();
}
