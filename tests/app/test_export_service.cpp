// EX.2: the app's Export Audio port (pq/app/export_service.h): a tiny master export of the default project through the
// real worker (interpreter images, one beat) writes its file; a cancel ends Cancelled with nothing left; the job →
// request mapping (the whole song gets the 10 s tail, Selected → Tracks); the window's choices round-trip through the
// settings file. EX.3: a tiny FLAC export through runExport holds the same samples as the WAV export of that request.
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pq/app/export_service.h"
#include "pq/app/user_settings.h"
#include "pq/document/model.h"
#include "pq/media/decoder.h"

using namespace pq;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / ("pq_test_export_service_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

std::string utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

views::ExportJob tinyJob(const fs::path& dir, Ticks to) {
  doc::IdSource ids;
  views::ExportJob j;
  j.snapshot = doc::makeDefaultProject(ids);
  j.from = 0;
  j.to = to;
  j.sampleRate = 48000;
  j.bits = views::ExportBitDepth::Float32;
  j.dither = views::ExportDither::None;
  j.folder = utf8(dir);
  j.baseName = "Tiny";
  return j;
}

// Polls like the window does, until the job ends (at most 60 s).
views::ExportStatus waitEnd(app::ExportService& s) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  views::ExportStatus st = s.status();
  while (st.state == views::ExportStatus::State::Running && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    st = s.status();
  }
  return st;
}

}  // namespace

TEST_CASE("export service: a tiny master export runs on the worker and writes its file") {
  const fs::path dir = freshDir("tiny");
  app::ExportService service([] { return 44100u; }, /*jit=*/false);
  CHECK(service.engineSampleRate() == 44100);
  CHECK(service.status().state == views::ExportStatus::State::Idle);
  REQUIRE(service.start(tinyJob(dir, kPpq)));  // one beat
  const views::ExportStatus st = waitEnd(service);
  REQUIRE_MESSAGE(st.state == views::ExportStatus::State::Done, st.message);
  REQUIRE(st.files.size() == 1);
  CHECK(fs::path(std::u8string(st.files[0].begin(), st.files[0].end())).filename() == "Tiny.wav");
  CHECK(fs::exists(dir / "Tiny.wav"));
  CHECK(fs::file_size(dir / "Tiny.wav") > 44);
  // A second job on the same service (the previous thread joined).
  REQUIRE(service.start(tinyJob(dir, kPpq)));
  CHECK(waitEnd(service).state == views::ExportStatus::State::Done);
  fs::remove_all(dir);
}

TEST_CASE("export service: cancel ends the job Cancelled with no file left; one job at a time") {
  const fs::path dir = freshDir("cancel");
  app::ExportService service([] { return 48000u; }, /*jit=*/false);
  REQUIRE(service.start(tinyJob(dir, 4000 * kTicksPerWhole)));  // far longer than the test waits
  CHECK_FALSE(service.start(tinyJob(dir, kPpq)));
  service.cancel();
  const views::ExportStatus st = waitEnd(service);
  CHECK(st.state == views::ExportStatus::State::Cancelled);
  CHECK(fs::is_empty(dir));
  fs::remove_all(dir);
}

TEST_CASE("export service: the job maps onto the request (tail for the whole song only, Selected = Tracks)") {
  views::ExportJob j = tinyJob(fs::temp_directory_path(), kTicksPerWhole);
  j.song = true;
  j.tracks = views::ExportTracks::AllTracks;
  j.bits = views::ExportBitDepth::Pcm16;
  j.dither = views::ExportDither::Shaped;
  app::ExportRequest r = app::exportRequestOf(j, true);
  CHECK(r.tailMaxSeconds == 10.0);
  CHECK(r.what == app::ExportWhat::Stems);
  CHECK(r.bits == app::ExportBits::Pcm16);
  CHECK(r.dither == app::ExportDither::Shaped);
  CHECK(r.jit);
  j.renderAsLoop = true;  // a loop render has no tail
  CHECK(app::exportRequestOf(j, true).tailMaxSeconds == 0.0);
  j.renderAsLoop = false;
  j.song = false;
  j.tracks = views::ExportTracks::Selected;
  j.trackIds = {doc::TrackId{7}};
  r = app::exportRequestOf(j, false);
  CHECK(r.tailMaxSeconds == 0.0);
  CHECK(r.what == app::ExportWhat::Tracks);
  CHECK(r.tracks == std::vector<doc::TrackId>{doc::TrackId{7}});
  CHECK_FALSE(r.jit);
}

TEST_CASE("export: a tiny FLAC export (runExport) decodes to the WAV export's samples; FLAC refuses 32-bit float") {
  const fs::path dir = freshDir("flac");
  views::ExportJob job = tinyJob(dir, kPpq / 2);  // half a beat of the default project
  job.dither = views::ExportDither::Tpdf;         // seeded: the same noise in both files (on silence: the content)
  auto decode = [](const fs::path& p) {
    auto d = media::Decoder::open(p);
    REQUIRE_MESSAGE(d.ok(), (d.ok() ? "" : d.error().message));
    std::vector<std::vector<float>> out((*d)->info().channels, std::vector<float>((*d)->info().frames));
    std::vector<float*> ptr;
    for (auto& c : out) ptr.push_back(c.data());
    auto got = (*d)->read(ptr.data(), (*d)->info().frames);
    REQUIRE(got.ok());
    CHECK(*got == (*d)->info().frames);
    return std::pair{(*d)->info().bitsPerSample, out};
  };
  for (const views::ExportBitDepth bits : {views::ExportBitDepth::Pcm16, views::ExportBitDepth::Pcm24}) {
    CAPTURE(static_cast<int>(bits));
    job.bits = bits;
    job.format = views::ExportFileFormat::Wav;
    app::ExportRequest wav = app::exportRequestOf(job, /*jit=*/false);
    job.format = views::ExportFileFormat::Flac;
    app::ExportRequest flac = app::exportRequestOf(job, /*jit=*/false);
    CHECK(flac.format == app::ExportFormat::Flac);
    auto rw = app::runExport(wav);
    REQUIRE_MESSAGE(rw.ok(), (rw.ok() ? "" : rw.error().message));
    auto rf = app::runExport(flac);
    REQUIRE_MESSAGE(rf.ok(), (rf.ok() ? "" : rf.error().message));
    REQUIRE(rf->files.size() == 1);
    CHECK(rf->files[0].path == dir / "Tiny.flac");
    const auto [wavBits, w] = decode(dir / "Tiny.wav");
    const auto [flacBits, f] = decode(dir / "Tiny.flac");
    CHECK(flacBits == (bits == views::ExportBitDepth::Pcm16 ? 16 : 24));
    CHECK(flacBits == wavBits);
    REQUIRE(f.size() == w.size());
    REQUIRE(!f.empty());
    CHECK(f[0].size() == rf->frames);
    CHECK(f == w);  // bit for bit (float == on the integer grid)
    bool anyNonZero = false;
    for (float x : f[0]) anyNonZero = anyNonZero || x != 0.0f;
    CHECK(anyNonZero);  // the dither noise: the comparison is not of silence
  }
  // No 32-bit float FLAC; nothing is written.
  fs::remove(dir / "Tiny.flac");
  job.bits = views::ExportBitDepth::Float32;
  auto bad = app::runExport(app::exportRequestOf(job, false));
  CHECK_FALSE(bad.ok());
  CHECK_FALSE(fs::exists(dir / "Tiny.flac"));
  fs::remove_all(dir);
}

TEST_CASE("export service: the window's choices round-trip through the settings file") {
  const fs::path dir = freshDir("settings");
  const std::string path = utf8(dir / "settings.json");
  views::ViewPrefs p;
  p.exportAudio.tracks = views::ExportTracks::Selected;
  p.exportAudio.range = views::ExportRange::TimeSelection;
  p.exportAudio.includeReturnsMaster = true;
  p.exportAudio.renderAsLoop = true;
  p.exportAudio.sampleRate = 96000;
  p.exportAudio.bits = views::ExportBitDepth::Pcm16;
  p.exportAudio.dither = views::ExportDither::Shaped;
  p.exportAudio.normalize = true;
  p.exportAudio.folder = "D:\\M\xC3\xBCsic\\Exports";
  REQUIRE(app::saveViewPrefs(path, p).ok());
  CHECK(app::loadViewPrefs(path) == p);
  p.exportAudio.format = views::ExportFileFormat::Flac;  // EX.3
  REQUIRE(app::saveViewPrefs(path, p).ok());
  CHECK(app::loadViewPrefs(path) == p);
  fs::remove_all(dir);
}
