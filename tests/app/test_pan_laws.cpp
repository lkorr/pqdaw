// TX.pan-laws (bug report 2026-11-05: "panning hard R makes the signal louder"): every pan law (model.h PanMode)
// measured through the lowered mixer — a mono source on a MIDI track, a stereo source on a group, a return and the
// master — against doc::panMatrix; a law change recompiles its track alone; a pan jump stays smooth and a pan lane
// plays under every law.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/lower.h"
#include "pq/engine/offline_render.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr double kRate = 48000.0;
constexpr double kRef = 261.626;  // the default 12-EDO's reference
constexpr uint32_t kBlock = 64;

using Channels = std::vector<std::vector<float>>;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

Channels render(const Snapshot& s, double seconds) {
  auto link = makeLink(s);
  engine::BufferSink sink;
  auto st = link->renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return sink.channels;
}

double hz(int degree) { return kRef * std::pow(2.0, degree / 12.0); }

// The amplitude of a degree's fundamental per channel (Hann-windowed DFT at that exact frequency over [a, b) s). The
// degrees a test plays lie within one octave, so no tone's 2nd or 3rd harmonic lands on another's fundamental.
std::vector<double> level(const Channels& x, int degree, double a0 = 1.5, double b0 = 2.5) {
  const size_t a = static_cast<size_t>(a0 * kRate), b = static_cast<size_t>(b0 * kRate);
  std::vector<double> out;
  for (const auto& ch : x) {
    std::complex<double> acc = 0.0;
    double wsum = 0.0;
    for (size_t i = a; i < b && i < ch.size(); ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i - a) / static_cast<double>(b - a));
      const double t = static_cast<double>(i - a) / kRate;
      acc += std::polar(w * ch[i], -2.0 * std::numbers::pi * hz(degree) * t);
      wsum += w;
    }
    out.push_back(std::abs(acc) * 2.0 / wsum);
  }
  return out;
}

// A project built through commands: tone tracks (a MIDI track holding one note of its degree), groups, returns.
struct Proj {
  DocumentStore store;
  Batch batch{"probe", {}};
  TrackId master;
  Proj() { master = store.current()->master->id; }
  TrackId add(TrackKind kind, const char* name, TrackId parent = {}) {
    const TrackId id = store.ids().track();
    batch.commands.push_back(AddTrack{playback_fixture::track(id, kind, name, kind == TrackKind::Midi ? store.ids().device() : DeviceId{}), parent, kAppend});
    return id;
  }
  TrackId tone(int degree, TrackId parent = {}, double seconds = 3.0) {
    const TrackId id = add(TrackKind::Midi, "tone", parent);
    const Ticks len = static_cast<Ticks>(seconds * 2.0) * kPpq;  // 120 BPM: a quarter is 0.5 s
    std::vector<Note> n{playback_fixture::note(store.ids(), 0, len, degree)};
    batch.commands.push_back(AddClip{id, playback_fixture::clip(store.ids(), 0, len, n, "tone")});
    return id;
  }
  void cmd(Command c) { batch.commands.push_back(std::move(c)); }
  Snapshot done() {
    const Status st = store.dispatch(batch);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
    batch.commands.clear();
    return store.current();
  }
};

// One tone track, Balance at the centre (0 dB): the instrument's amplitude before any pan law.
double referenceAmplitude() {
  static const double a0 = [] {
    Proj p;
    p.cmd(SetTrackPanMode{p.tone(0), PanMode::Balance});
    return level(render(p.done(), 3.0), 0)[0];
  }();
  return a0;
}

// The degree's level on (L, R) relative to the reference is (l, r) within 0.05 dB (or silent).
void expectGains(const Channels& x, int degree, double l, double r) {
  const std::vector<double> got = level(x, degree);
  const double want[2] = {l, r};
  for (int c = 0; c < 2; ++c) {
    CAPTURE(degree);
    CAPTURE(c);
    const double g = got[static_cast<size_t>(c)] / referenceAmplitude();
    CAPTURE(g);
    CAPTURE(want[c]);
    if (want[c] < 1e-6) CHECK(g < 1e-4);
    else CHECK(std::fabs(20.0 * std::log10(g / want[c])) < 0.05);
  }
}

double db(double g) { return 20.0 * std::log10(std::max(g, 1e-12)); }

struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  Channels out = Channels(2);
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out[0].insert(out[0].end(), l.begin(), l.end());
      out[1].insert(out[1].end(), r.begin(), r.end());
    }
  }
};

constexpr double kPans[] = {-1.0, 0.0, 1.0};

}  // namespace

// ---- the laws themselves ------------------------------------------------------------------------------------------

TEST_CASE("pan laws: each law's gains — centre, edges, no channel above 0 dB, power as the law defines it") {
  const double s = std::sqrt(0.5);
  for (const PanMode m : kPanModes) {
    CAPTURE(std::string(panModeName(m)));
    for (const double p : kPans) {
      CAPTURE(p);
      const PanMatrix g = panMatrix(m, p);
      for (const double v : {g.ll, g.rl, g.lr, g.rr}) {
        CHECK(v >= 0.0);
        CHECK(v <= 1.0 + 1e-12);  // no law raises a channel above 0 dB
      }
      // A mono source (L = R): out L = ll + rl, out R = lr + rr; its power against the centre's.
      const PanMatrix c = panMatrix(m, 0.0);
      const double monoPower = std::pow(g.ll + g.rl, 2) + std::pow(g.lr + g.rr, 2);
      const double monoCentre = std::pow(c.ll + c.rl, 2) + std::pow(c.lr + c.rr, 2);
      // Uncorrelated L and R of equal power: the sum of the squares.
      const double widePower = g.ll * g.ll + g.rl * g.rl + g.lr * g.lr + g.rr * g.rr;
      switch (m) {
        case PanMode::Balance:
          CHECK(monoPower <= monoCentre + 1e-12);  // never louder than the centre
          CHECK(widePower <= 2.0 + 1e-12);
          break;
        case PanMode::ConstantPower: CHECK(monoPower == doctest::Approx(monoCentre).epsilon(1e-12)); break;
        case PanMode::Linear: CHECK(g.ll + g.rr == doctest::Approx(1.0).epsilon(1e-12)); break;  // the mono sum
        case PanMode::Stereo: CHECK(widePower == doctest::Approx(2.0).epsilon(1e-12)); break;
      }
    }
  }
  // Centre and edges, as model.h states them.
  auto eq = [](const PanMatrix& g, double ll, double rl, double lr, double rr) {
    CHECK(g.ll == doctest::Approx(ll).epsilon(1e-12));
    CHECK(g.rl == doctest::Approx(rl).epsilon(1e-12));
    CHECK(g.lr == doctest::Approx(lr).epsilon(1e-12));
    CHECK(g.rr == doctest::Approx(rr).epsilon(1e-12));
  };
  eq(panMatrix(PanMode::Balance, 0.0), 1, 0, 0, 1);
  eq(panMatrix(PanMode::Balance, 1.0), 0, 0, 0, 1);
  eq(panMatrix(PanMode::Balance, -1.0), 1, 0, 0, 0);
  eq(panMatrix(PanMode::ConstantPower, 0.0), s, 0, 0, s);
  CHECK(panMatrix(PanMode::ConstantPower, 1.0).ll < 1e-12);
  CHECK(panMatrix(PanMode::ConstantPower, 1.0).rr == doctest::Approx(1.0));
  eq(panMatrix(PanMode::Linear, 0.0), 0.5, 0, 0, 0.5);
  eq(panMatrix(PanMode::Linear, 1.0), 0, 0, 0, 1);
  eq(panMatrix(PanMode::Stereo, 0.0), 1, 0, 0, 1);  // the image unchanged
  CHECK(panMatrix(PanMode::Stereo, 1.0).ll < 1e-12);  // both inputs out of the right
  CHECK(panMatrix(PanMode::Stereo, 1.0).lr == doctest::Approx(1.0));
  CHECK(panMatrix(PanMode::Stereo, 1.0).rr == doctest::Approx(1.0));
}

// ---- through the lowered mixer ------------------------------------------------------------------------------------

// The diagnosis (MESSAGE lines) and every law on a mono source: five tone tracks at −1 … 1, one render per law.
TEST_CASE("pan laws: a mono source on a MIDI track — every law at -1, 0, 1 through the lowered mixer") {
  const int degrees[] = {0, 4, 7};
  for (const PanMode m : kPanModes) {
    CAPTURE(std::string(panModeName(m)));
    Proj p;
    for (size_t i = 0; i < std::size(kPans); ++i) {
      const TrackId t = p.tone(degrees[i]);
      p.cmd(SetTrackPanMode{t, m});
      p.cmd(SetTrackPan{t, static_cast<float>(kPans[i])});
    }
    const Channels x = render(p.done(), 3.0);
    for (size_t i = 0; i < std::size(kPans); ++i) {
      CAPTURE(kPans[i]);
      const PanMatrix g = panMatrix(m, kPans[i]);
      expectGains(x, degrees[i], g.ll + g.rl, g.lr + g.rr);
    }
    // What the bug report heard, measured: hard right against the centre, on the right channel and in power.
    const std::vector<double> c = level(x, degrees[1]), r = level(x, degrees[2]);
    MESSAGE(panModeName(m) << ": centre L/R " << db(c[0] / referenceAmplitude()) << " / " << db(c[1] / referenceAmplitude())
                           << " dB; hard R: R " << db(r[1] / c[1]) << " dB over the centre's R, power "
                           << db(std::sqrt((r[0] * r[0] + r[1] * r[1]) / (c[0] * c[0] + c[1] * c[1]))) << " dB");
  }
}

// A stereo source: a group whose input is tone A on the left only and tone B on the right only (its two children
// panned hard with Balance), three groups at −1, 0, 1 in one render per law: A's and B's levels on L and R are
// the four entries of the law's matrix.
TEST_CASE("pan laws: a stereo source on a group — the 2x2 matrix of every law") {
  const double pans[] = {-1.0, 0.0, 1.0};
  const int degA[] = {0, 1, 3}, degB[] = {6, 7, 9};
  for (const PanMode m : kPanModes) {
    CAPTURE(std::string(panModeName(m)));
    Proj p;
    for (size_t i = 0; i < std::size(pans); ++i) {
      const TrackId g = p.add(TrackKind::Group, "g");
      const TrackId a = p.tone(degA[i], g), b = p.tone(degB[i], g);
      p.cmd(SetTrackPan{a, -1.0f});
      p.cmd(SetTrackPan{b, 1.0f});
      p.cmd(SetTrackPanMode{g, m});
      p.cmd(SetTrackPan{g, static_cast<float>(pans[i])});
    }
    const Channels x = render(p.done(), 3.0);
    for (size_t i = 0; i < std::size(pans); ++i) {
      CAPTURE(pans[i]);
      const PanMatrix g = panMatrix(m, pans[i]);
      expectGains(x, degA[i], g.ll, g.lr);
      expectGains(x, degB[i], g.rl, g.rr);
    }
  }
}

// A law is structural (one compile job, one unit: the track's); the law change leaves the track playing.
TEST_CASE("pan laws: a law change recompiles its track alone; undo") {
  Proj p;
  const TrackId g = p.add(TrackKind::Group, "g");
  const TrackId a = p.tone(0, g, 8.0), b = p.tone(7, g, 8.0);
  p.cmd(SetTrackPan{a, -1.0f});
  p.cmd(SetTrackPan{b, 1.0f});
  const TrackId other = p.tone(4, {}, 8.0);
  (void)other;
  auto link = makeLink(p.done());
  [[maybe_unused]] const ListenerId sub = p.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(400);
  for (const PanMode m : {PanMode::Stereo, PanMode::Linear, PanMode::ConstantPower, PanMode::Balance}) {
    CAPTURE(std::string(panModeName(m)));
    const app::LinkStats s0 = link->stats();
    REQUIRE(p.store.dispatch(SetTrackPanMode{g, m}).ok());
    link->flush();
    CHECK(link->stats().compiles == s0.compiles + 1);
    CHECK(link->stats().unitCompiles == s0.unitCompiles + 1);  // the group's unit only
    CHECK(link->stats().topologyChanges == s0.topologyChanges);
    d.run(40);
    // Both tones still sound (the change compiled in place).
    const double end = static_cast<double>(d.out[0].size()) / kRate;
    const std::vector<double> la = level(d.out, 0, end - 0.03, end), lb = level(d.out, 7, end - 0.03, end);
    CHECK(std::max(la[0], la[1]) > 0.1 * referenceAmplitude());
    CHECK(std::max(lb[0], lb[1]) > 0.1 * referenceAmplitude());
  }
  // Undo of a law change is a compile of that unit too.
  REQUIRE(p.store.dispatch(SetTrackPanMode{g, PanMode::Stereo}).ok());
  link->flush();
  const app::LinkStats s0 = link->stats();
  REQUIRE(p.store.undo());
  link->flush();
  CHECK(link->stats().unitCompiles == s0.unitCompiles + 1);
  CHECK(findTrackPtr(*p.store.current(), g)->mixer.panMode == PanMode::Balance);
}
