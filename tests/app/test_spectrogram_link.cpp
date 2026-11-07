// TX.spectrogram: the Spectrogram module through the playback link — its tap exists once its unit compiled; unwatched the
// audio thread copies nothing; watched, every block lands in the tap (RtScope: the engine block allocates nothing either
// way) with the transport's beat and the signal; a feed on that tap turns it into spectrogram columns with the chord's
// partials on their rows; the module is bit-exact pass-through.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/views/devices/analysis.h"

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
  void run(uint32_t blocks, bool keep = true) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      if (!keep) continue;
      out.insert(out.end(), l.begin(), l.end());
      out.insert(out.end(), r.begin(), r.end());
    }
  }
};

// A MIDI track playing `degrees` through the default synth, then (optionally) the Spectrogram.
struct GramProject {
  DocumentStore store;
  TrackId track;
  DeviceId gram;
  GramProject(bool withGram, std::vector<int32_t> degrees) {
    IdSource& ids = store.ids();
    track = ids.track();
    Batch b{"gram", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys"), {}, kAppend});
    (void)playback_fixture::instrument(b, ids, track, ids.device());
    std::vector<Note> notes;
    for (const int32_t d : degrees) notes.push_back(playback_fixture::note(ids, 0, 64 * kPpq, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 64 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
    if (withGram) {
      auto e = ops::addMetaDevice(*store.current(), store.ids(), track, "spectrogram");
      REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
      REQUIRE(store.dispatch(e->command).ok());
      gram = e->devices.at(0);
    }
  }
};

}  // namespace

TEST_CASE("spectrogram link: the tap carries the beat and the signal; unwatched nothing; a feed makes columns") {
  GramProject p(true, {0, 7});
  auto link = makeLink(p.store.current());
  CHECK(link->lastError().empty());
  const std::shared_ptr<TapRing> tap = link->deviceAnalysisTap(p.gram);
  REQUIRE(tap != nullptr);
  CHECK(tap->channels() == 4);
  Driver d{*link};
  link->play(0);
  d.run(50, false);
  CHECK(tap->written() == 0);  // nobody watches: one load per block, no copy
  // A feed on the tap (as the AnalysisHub makes one), wanting only the spectrogram.
  views::devices::SpectrumFeed feed(*tap, views::devices::AnalysisHub::config(48000.0), views::devices::SpectrumFeed::Manual{});
  views::devices::SpectrogramRequest q;
  q.config.hop = 480;
  q.columns = 400;
  feed.setSpectrogram(&feed, q);
  feed.acquire(views::devices::kWantSpectrogram);  // registers the tap's reader
  double now = 1.0;
  for (int i = 0; i < 60; ++i) {  // 60 × 16 blocks ≈ 1.3 s of audio
    d.run(16, false);
    feed.step(now += 16.0 * kBlock / 48000.0);
  }
  CHECK(tap->written() == 60u * 16u * kBlock);
  CHECK(feed.hopsAnalysed() == 0);  // no spectrum: only the columns
  const views::devices::GramState st = feed.gramState();
  REQUIRE(st.running);
  CHECK(st.hop == 480);
  CHECK(st.end > 80);
  // The newest columns: the tapped beat moves forward; the chord's root (degree 0: the reference C3 = 261.6 Hz) and its
  // fifth (12-EDO) are among the brightest rows.
  std::vector<float> rows(8 * views::devices::SpectrumFeed::kGramRows);
  std::vector<views::devices::GramColumn> info(8);
  REQUIRE(feed.gramRead(st.end - 8, 8, rows.data(), info.data()) == 8);
  CHECK(std::isfinite(info[0].beat));
  CHECK(info[7].beat > info[0].beat);
  const float* col = rows.data() + 4 * views::devices::SpectrumFeed::kGramRows;
  auto rowOf = [&](float hz) {
    return static_cast<uint32_t>(std::lround((1.0f - dsp::gramUnit(dsp::GramScale::Log, hz, st.fMin, st.fMax)) * 1023.0f));
  };
  auto near = [&](float hz) {
    float m = 0.0f;
    const uint32_t r = rowOf(hz);
    for (uint32_t k = r - 2; k <= r + 2; ++k) m = std::max(m, col[k]);
    return m;
  };
  const float root = near(261.626f), fifth = near(261.626f * 1.4983071f), between = near(261.626f * 1.2f);
  MESSAGE("root " << 10 * std::log10(root) << " dB, fifth " << 10 * std::log10(fifth) << " dB, between "
                  << 10 * std::log10(std::max(between, 1e-30f)) << " dB");
  CHECK(root > 1e-4f);  // louder than −40 dB
  CHECK(fifth > 1e-4f);
  CHECK(between < root * 1e-2f);  // 20 dB darker between the two partials
  feed.release(views::devices::kWantSpectrogram);
  CHECK_FALSE(tap->wanted());
}

TEST_CASE("spectrogram link: the Spectrogram is bit-exact pass-through, watched or not") {
  GramProject with(true, {0, 4, 7, 11}), without(false, {0, 4, 7, 11});
  auto la = makeLink(with.store.current()), lb = makeLink(without.store.current());
  Driver da{*la}, db{*lb};
  la->play(0);
  lb->play(0);
  da.run(200);
  db.run(200);
  la->deviceAnalysisTap(with.gram)->addReader();
  da.run(200);
  db.run(200);
  REQUIRE(da.out.size() == db.out.size());
  float peak = 0.0f;
  for (const float v : da.out) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.01f);
  CHECK(std::memcmp(da.out.data(), db.out.data(), da.out.size() * sizeof(float)) == 0);
  la->deviceAnalysisTap(with.gram)->removeReader();
}

TEST_CASE("spectrogram link: the engine block with a Spectrogram unwatched / watched" * doctest::test_suite("perf")) {
  GramProject k(true, {0, 4, 7, 11});
  auto link = makeLink(k.store.current());
  Driver d{*link};
  link->play(0);
  d.run(200, false);
  auto timeRun = [](Driver& dr, uint32_t blocks) {
    const auto t0 = std::chrono::steady_clock::now();
    dr.run(blocks, false);
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / blocks;
  };
  GramProject plain(false, {0, 4, 7, 11});
  auto lp = makeLink(plain.store.current());
  Driver dp{*lp};
  lp->play(0);
  dp.run(200, false);
  const double none = timeRun(dp, 4000);
  const double idle = timeRun(d, 4000);
  link->deviceAnalysisTap(k.gram)->addReader();
  const double watched = timeRun(d, 4000);
  link->deviceAnalysisTap(k.gram)->removeReader();
  MESSAGE("engine block (64 frames), a synth chord: without " << none << " us, + Spectrogram unwatched " << idle
                                                             << " us, watched " << watched << " us");
}
