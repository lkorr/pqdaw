// T5.8: project samples — referenced by default; resolution after a move; missing → relink (not an undo step);
// Collect All and Save (collision rules, incremental, project.pqd last: a failure mid-copy leaves the old file).
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "edit_helpers.h"
#include "pq/document/samples.h"
#include "pq/document/serialize.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
fs::path pathOf(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A 16-bit PCM WAV of `frames` frames (`seed` makes the content differ between files of one length).
void writeWav(const fs::path& p, uint32_t frames, uint32_t channels, uint32_t rate, uint32_t seed) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  const uint32_t bytes = frames * channels * 2;
  f.write("RIFF", 4);
  u32(36 + bytes);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(static_cast<uint16_t>(channels));
  u32(rate);
  u32(rate * channels * 2);
  u16(static_cast<uint16_t>(channels * 2));
  u16(16);
  f.write("data", 4);
  u32(bytes);
  std::mt19937 rng(seed);
  for (uint32_t i = 0; i < frames * channels; ++i) u16(static_cast<uint16_t>(rng()));
}

// The test's probe: the fmt and data chunks of a canonical WAV (what media's probe will report for these files).
Result<SampleProbe> probeWav(std::string_view path) {
  const std::string d = readFile(pathOf(std::string(path)));
  if (d.size() < 44 || d.compare(0, 4, "RIFF") != 0) return Error{ErrorCode::Parse, "not a WAV", {}};
  uint16_t ch = 0;
  uint32_t rate = 0, bytes = 0;
  std::memcpy(&ch, d.data() + 22, 2);
  std::memcpy(&rate, d.data() + 24, 4);
  std::memcpy(&bytes, d.data() + 40, 4);
  return SampleProbe{static_cast<int64_t>(bytes / (2u * ch)), static_cast<double>(rate), ch};
}

// A temporary tree removed at the end.
struct TempTree {
  fs::path root;
  TempTree() {
    root = fs::temp_directory_path() / ("pq_samples_" + std::to_string(std::random_device{}()));
    root += u8"_ä";  // a non-ASCII path
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~TempTree() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

// A project with an audio track playing `files` (absolute paths, referenced).
struct Proj {
  DocumentStore store;
  TrackId track;
  std::vector<SampleId> samples;
  ClipRef firstClip;
  Proj(const std::vector<fs::path>& files, const std::string& projectDir) {
    IdSource& ids = store.ids();
    const Result<ops::Edit> made = ops::createTrack(*store.current(), ids, TrackKind::Audio);
    edit_test::commit(store, made);
    track = made->tracks.at(0);
    std::vector<SampleRef> refs;
    std::vector<ops::NewAudioClip> clips;
    pq::Ticks at = 0;
    for (const fs::path& f : files) {
      Result<SampleRef> r = makeSampleRef(ids, u8(f), projectDir, probeWav);
      REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
      samples.push_back(r->id);
      clips.push_back({track, at, r->id, {}});
      at += pq::kTicksPerWhole;
      refs.push_back(std::move(*r));
    }
    const ops::Edit e = *ops::addAudioClips(*store.current(), ids, refs, clips);
    REQUIRE(store.dispatch(e.command).ok());
    firstClip = e.clips.at(0);
  }
  const Project& doc() const { return *store.current(); }
};

}  // namespace

TEST_CASE("samples: paths are referenced (absolute outside the project, relative inside) and survive a move") {
  TempTree t;
  const fs::path ext = t.root / "library" / "Drums" / "kick.wav";
  writeWav(ext, 4800, 1, 48000, 1);
  const fs::path dir1 = t.root / "Song";
  const fs::path inside = dir1 / "samples" / "vox.wav";
  writeWav(inside, 9600, 2, 44100, 2);
  Proj p({ext, inside}, u8(dir1));
  const SampleRef& k = *findSample(p.doc(), p.samples[0]);
  const SampleRef& v = *findSample(p.doc(), p.samples[1]);
  CHECK(k.path == u8(ext));
  CHECK(isAbsoluteSamplePath(k.path));
  CHECK(v.path == "samples/vox.wav");
  CHECK(k.frames == 4800);
  CHECK(v.rate == 44100.0);
  CHECK(v.channels == 2);
  CHECK(k.displayName == "kick");
  CHECK(k.contentHash != 0);
  CHECK(k.size == fs::file_size(ext));
  REQUIRE(saveProject(u8(dir1), p.store).ok());
  // A plain save copies nothing (reference by default).
  CHECK_FALSE(fs::exists(dir1 / "samples" / "imported"));

  // Move the project folder: the relative sample follows it, the absolute one is where it was.
  const fs::path dir2 = t.root / "Moved" / "Song 2";
  fs::create_directories(dir2.parent_path());
  fs::rename(dir1, dir2);
  Result<LoadedProject> l = loadProject(u8(dir2));
  REQUIRE(l.ok());
  std::vector<SampleResolution> res = resolveSamples(*l->project, SampleSearch{u8(dir2), {}});
  REQUIRE(res.size() == 2);
  CHECK(res[0].path == u8(ext));
  CHECK(res[1].path == u8(dir2 / "samples" / "vox.wav"));
  CHECK_FALSE(res[0].relocated);
  CHECK(missingSamples(*l->project, res).empty());

  // The library moved into the project folder: found by a search (name + size + hash), with its new stored path.
  fs::create_directories(dir2 / "found");
  fs::rename(ext, dir2 / "found" / "kick.wav");
  res = resolveSamples(*l->project, SampleSearch{u8(dir2), {}});
  REQUIRE(res[0].relocated);
  CHECK(*res[0].relocated == "found/kick.wav");
  CHECK(res[0].path == u8(dir2 / "found" / "kick.wav"));
  // A different file of the same name is not taken.
  fs::remove(dir2 / "found" / "kick.wav");
  writeWav(dir2 / "other" / "kick.wav", 4800, 1, 48000, 99);
  res = resolveSamples(*l->project, SampleSearch{u8(dir2), {}});
  CHECK(res[0].path.empty());
  CHECK(missingSamples(*l->project, res) == std::vector<SampleId>{p.samples[0]});
}

TEST_CASE("samples: missing → relink is not an undo step; every snapshot of the history follows") {
  TempTree t;
  const fs::path a = t.root / "old" / "pad.wav";
  writeWav(a, 2400, 2, 48000, 3);
  Proj p({a}, u8(t.root / "Song"));
  // An edit after the drop, so the history holds snapshots with the old path.
  const ClipRef c = p.firstClip;
  edit_test::commit(p.store, ops::setAudioClipGain(p.doc(), std::span<const ClipRef>(&c, 1), -6.0f));
  const fs::path moved = t.root / "new" / "deep" / "pad.wav";
  fs::create_directories(moved.parent_path());
  fs::rename(a, moved);
  std::vector<SampleResolution> res = resolveSamples(p.doc(), SampleSearch{u8(t.root / "Song"), {}});
  REQUIRE(missingSamples(p.doc(), res) == p.samples);
  const std::vector<SampleRef> fixed = relinkSamples(p.doc(), p.samples, u8(t.root / "new"), u8(t.root / "Song"));
  REQUIRE(fixed.size() == 1);
  CHECK(fixed[0].path == u8(moved));
  const size_t cursor = p.store.history().cursor();
  p.store.relocateSamples(fixed);
  CHECK(p.store.history().cursor() == cursor);  // no history entry
  CHECK(findSample(p.doc(), p.samples[0])->path == u8(moved));
  // Undo the gain: the snapshot before it (from before the relink) has the new path too.
  REQUIRE(p.store.undo());
  CHECK(findSample(p.doc(), p.samples[0])->path == u8(moved));
  REQUIRE(p.store.redo());
  res = resolveSamples(p.doc(), SampleSearch{u8(t.root / "Song"), {}});
  CHECK(missingSamples(p.doc(), res).empty());
  // The relocation keeps sharing: undo still restores identical snapshot pointers.
  const Snapshot now = p.store.current();
  REQUIRE(p.store.undo());
  REQUIRE(p.store.redo());
  CHECK(p.store.current() == now);
}

TEST_CASE("samples: Collect All and Save — copies used files, collision rules, incremental, project.pqd last") {
  TempTree t;
  const fs::path a1 = t.root / "A" / "x.wav";
  const fs::path b1 = t.root / "B" / "x.wav";  // same name, different content
  const fs::path c1 = t.root / "C" / "x.wav";  // same name and content as a1
  writeWav(a1, 3000, 1, 48000, 10);
  writeWav(b1, 3000, 1, 48000, 11);
  fs::create_directories(c1.parent_path());
  fs::copy_file(a1, c1);
  const fs::path dir = t.root / "Song";
  Proj p({a1, b1, c1}, u8(dir));
  REQUIRE(saveProject(u8(dir), p.store).ok());
  const std::string before = readFile(dir / "project.pqd");

  // A disk error halfway: the old project file is intact, nothing half-written is left, the paths are unchanged.
  CollectOptions fail;
  fail.failAt = CollectFailPoint::MidCopy;
  CHECK_FALSE(collectAllAndSave(p.store, u8(dir), SampleSearch{}, fail).ok());
  CHECK(readFile(dir / "project.pqd") == before);
  for (const auto& e : fs::recursive_directory_iterator(dir)) CHECK(e.path().extension() != ".part");
  CHECK(findSample(p.doc(), p.samples[0])->path == u8(a1));

  std::vector<uint64_t> progress;
  CollectOptions ok;
  ok.progress = [&](uint64_t done, uint64_t total) {
    CHECK(done <= total);
    progress.push_back(done);
  };
  const size_t cursor = p.store.history().cursor();
  REQUIRE(collectAllAndSave(p.store, u8(dir), SampleSearch{}, ok).ok());
  CHECK(p.store.history().cursor() == cursor);  // not an undo step
  REQUIRE(!progress.empty());
  CHECK(progress.back() == 2 * fs::file_size(a1));  // a1 and b1 copied; c1 reuses a1's copy
  CHECK(findSample(p.doc(), p.samples[0])->path == "samples/imported/x.wav");
  CHECK(findSample(p.doc(), p.samples[1])->path == "samples/imported/x (2).wav");
  CHECK(findSample(p.doc(), p.samples[2])->path == "samples/imported/x.wav");
  CHECK(readFile(dir / "samples" / "imported" / "x.wav") == readFile(a1));
  CHECK(readFile(dir / "samples" / "imported" / "x (2).wav") == readFile(b1));
  // The saved file has the new paths; it loads and resolves without the originals.
  fs::remove_all(t.root / "A");
  fs::remove_all(t.root / "B");
  fs::remove_all(t.root / "C");
  Result<LoadedProject> l = loadProject(u8(dir));
  REQUIRE(l.ok());
  CHECK(*l->project == p.doc());
  const std::vector<SampleResolution> res = resolveSamples(*l->project, SampleSearch{u8(dir), {}});
  CHECK(missingSamples(*l->project, res).empty());
  // Incremental: everything is in the project now, nothing to copy.
  Result<CollectPlan> again = planCollect(p.doc(), u8(dir), res);
  REQUIRE(again.ok());
  CHECK(again->items.empty());

  // A new external file with a taken name and the same content as the copy reuses it; a different one gets (3).
  const fs::path d1 = t.root / "D" / "x.wav";
  const fs::path e1 = t.root / "E" / "x.wav";
  fs::create_directories(d1.parent_path());
  fs::copy_file(dir / "samples" / "imported" / "x (2).wav", d1);
  writeWav(e1, 3000, 1, 48000, 12);
  Proj q({d1, e1}, u8(dir));
  const std::vector<SampleResolution> qr = resolveSamples(q.doc(), SampleSearch{u8(dir), {}});
  Result<CollectPlan> plan = planCollect(q.doc(), u8(dir), qr);
  REQUIRE(plan.ok());
  REQUIRE(plan->items.size() == 2);
  CHECK(plan->items[0].stored == "samples/imported/x (2).wav");
  CHECK_FALSE(plan->items[0].copy);
  CHECK(plan->items[1].stored == "samples/imported/x (3).wav");
  CHECK(plan->items[1].copy);
  CHECK(plan->bytes == fs::file_size(e1));
}

TEST_CASE("samples: content hash = size + three 64 KiB windows") {
  TempTree t;
  const fs::path big = t.root / "big.wav";
  writeWav(big, 200000, 1, 48000, 5);  // 400 KB: three separate windows
  Result<FileIdentity> a = fileIdentity(u8(big));
  REQUIRE(a.ok());
  CHECK(a->size == fs::file_size(big));
  // A byte outside the windows does not change it; one inside does.
  std::string bytes = readFile(big);
  bytes[100000] ^= 0x5A;
  std::ofstream(big, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  CHECK(fileIdentity(u8(big))->hash == a->hash);
  bytes[10] ^= 0x5A;
  std::ofstream(big, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  CHECK(fileIdentity(u8(big))->hash != a->hash);
  CHECK(fileIdentity(u8(t.root / "nope.wav")).error().code == ErrorCode::NotFound);
  CHECK(storedSamplePath("C:\\a\\b\\c.wav", "C:/a") == "b/c.wav");
  CHECK(storedSamplePath("C:/ab/c.wav", "C:/a") == "C:/ab/c.wav");
  CHECK(absoluteSamplePath("b/c.wav", "C:/a") == "C:/a/b/c.wav");
  CHECK(isAbsoluteSamplePath("//server/share/x.wav"));
  CHECK_FALSE(isAbsoluteSamplePath("samples/x.wav"));
}
