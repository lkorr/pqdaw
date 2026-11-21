// SM.2: the Sampler engine through the playback link — its Sample request gets a dsp::SampleSlot; the link resolves the
// data's SampleId, loads and pins the file (fully resident, with mips) and publishes it with the slice table; a data
// edit (another sample, new slices) publishes again without a compile; a sample that is missing, or none, is silence.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/document/edit_ops.h"
#include "pq/document/store.h"
#include "pq/engine/wav_file.h"
#include "pq/meta/sampler.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

struct Song {
  DocumentStore store;
  fs::path dir;
  TrackId track;
  DeviceId sampler;
  explicit Song(const char* name) : dir(fs::temp_directory_path() / name) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    auto t = ops::createTrack(*store.current(), store.ids(), TrackKind::Midi);
    REQUIRE(t.ok());
    REQUIRE(store.dispatch(t->command).ok());
    track = t->tracks.at(0);
    auto d = ops::addMetaDevice(*store.current(), store.ids(), track, "sampler");
    REQUIRE_MESSAGE(d.ok(), (d.ok() ? std::string() : d.error().message));
    REQUIRE(store.dispatch(d->command).ok());
    sampler = d->devices.at(0);
  }
  ~Song() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  SampleId sample(const char* file, int64_t frames, uint32_t channels, double rate) {
    std::vector<std::vector<float>> ch(channels, std::vector<float>(static_cast<size_t>(frames)));
    for (uint32_t c = 0; c < channels; ++c)
      for (int64_t i = 0; i < frames; ++i) ch[c][static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(0.01 * i + c));
    const fs::path p = dir / file;
    REQUIRE(engine::writeWav(p, ch, static_cast<uint32_t>(rate)).ok());
    SampleRef s;
    s.id = store.ids().sample();
    s.path = utf8(p);
    s.size = fs::file_size(p);
    s.frames = frames;
    s.rate = rate;
    s.channels = channels;
    s.displayName = file;
    REQUIRE(store.dispatch(Command{AddSample{s}}).ok());
    return s.id;
  }
  void edit(ProjectLink* link, const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const Snapshot before = store.current();
    REQUIRE(store.dispatch(e->command).ok());
    if (link != nullptr) link->onDocumentChange(StoreEvent{StoreEventKind::Dispatch, before, store.current(), store.lastChanges()});
  }
};

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

}  // namespace

TEST_CASE("sampler link: the data's sample is pinned and published with its slices; a data edit republishes, no compile") {
  Song song("pq_sampler_link");
  const SampleId a = song.sample("a.wav", 24000, 2, 44100.0);
  const SampleId b = song.sample("b.wav", 9000, 1, 48000.0);
  song.edit(nullptr, ops::setMetaData(*song.store.current(), song.sampler, "", "sampler",
                                      meta::samplerData(a.value, {12000.0f, 0.0f, 6000.0f})));
  auto link = makeLink(song.store.current());
  link->flush();
  REQUIRE(link->waitForSamples(30.0));
  const std::shared_ptr<dsp::SampleSlot> slot = link->metaSampleSlot(song.sampler, "", 0);
  REQUIRE(slot != nullptr);
  const dsp::SampleSlot::Entry* e = slot->acquire();
  CHECK(e->level[0].length == 24000);
  CHECK(e->channels == 2);
  CHECK(e->levels == dsp::kMaxMipLevels);  // built at pin time
  CHECK(e->rateScale == doctest::Approx(44100.0 / 48000.0));
  REQUIRE(e->sliceCount == 3);
  CHECK(e->slices[0] == 0.0f);
  CHECK(e->slices[1] == 6000.0f);
  CHECK(e->slices[2] == 12000.0f);
  CHECK(e->level[0].ch[1][100] == doctest::Approx(0.5f * std::sin(1.0f + 1.0f)).epsilon(1e-3));  // the right channel
  const uint64_t gen = slot->generation();
  const uint64_t compiles = link->stats().compiles;
  // Another sample: published into the same slot, no compile.
  song.edit(link.get(), ops::setMetaData(*song.store.current(), song.sampler, "", "sampler", meta::samplerData(b.value)));
  REQUIRE(link->waitForSamples(30.0));
  link->flush();
  CHECK(link->stats().compiles == compiles);
  CHECK(link->metaSampleSlot(song.sampler, "", 0) == slot);
  CHECK(slot->generation() > gen);
  e = slot->acquire();
  CHECK(e->level[0].length == 9000);
  CHECK(e->channels == 1);
  CHECK(e->sliceCount == 0);
  CHECK(e->level[0].ch[1] == e->level[0].ch[0]);  // mono: the right channel is the left
  // The same data again: nothing published.
  const uint64_t gen2 = slot->generation();
  song.edit(link.get(), ops::setMetaData(*song.store.current(), song.sampler, "", "sampler", meta::samplerData(b.value)));
  CHECK(slot->generation() == gen2);
  // No sample: silence.
  song.edit(link.get(), ops::setMetaData(*song.store.current(), song.sampler, "", "sampler", meta::samplerData(0)));
  REQUIRE(link->waitForSamples(30.0));
  CHECK(slot->acquire()->level[0].length == 0);
}

TEST_CASE("sampler link: a missing file plays silence (no load error loop)") {
  Song song("pq_sampler_link_missing");
  const SampleId a = song.sample("gone.wav", 4800, 1, 48000.0);
  fs::remove(song.dir / "gone.wav");
  song.edit(nullptr, ops::setMetaData(*song.store.current(), song.sampler, "", "sampler", meta::samplerData(a.value)));
  auto link = makeLink(song.store.current());
  link->flush();
  REQUIRE(link->waitForSamples(30.0));
  const std::shared_ptr<dsp::SampleSlot> slot = link->metaSampleSlot(song.sampler, "", 0);
  REQUIRE(slot != nullptr);
  CHECK(slot->acquire()->level[0].length == 0);
  CHECK(slot->generation() == 2);  // the silent publish, once
  link->flush();
  REQUIRE(link->waitForSamples(30.0));
  CHECK(slot->generation() == 2);
}
