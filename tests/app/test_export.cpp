// EX.1: the export render core (pq/app/export_render.h) on the playback fixture (120 BPM: a bar is 2 s, 96 000 frames):
//   --render   a master export (32f) equals `pqdaw --render` of the same range bit for bit
//   stems      the top-level stems (post-fader `<id>:out`, captured in one pass) sum to the master within −120 dB, also
//              with a −10 ms Track Delay (PDC 480: each bus trimmed by its own latency); a muted track's stem is silent
//   solo       stems through the returns and the master (one solo pass each, on one link) equal the captured buses
//              that carry them
//   settle     EX.x-fix-settle: on one link every pass equals the first bit for bit (an automated volume; a solo pass
//              after another equals that solo on a fresh link)
//   loop       render as loop: frame 0 carries the first lap's release; the file is exactly the loop long
//   PDC        an oversampled (latent) group on Lead: the export lines up with the plain project at lag 0 (the raw
//              render is 5 frames late)
//   tail       a whole-song tail ends at the last frame above −90 dBFS
//   cancel     leaves no new file and keeps an existing one untouched
//   formats    PCM16 TPDF / shaped dither and PCM24 within their error bounds of the normalized float export (peak
//              −0.3 dBFS), shaped noise lower at low frequencies than TPDF
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/commands.h"
#include "pq/app/cli.h"
#include "pq/app/export_render.h"
#include "pq/document/edit_ops.h"
#include "pq/document/modules.h"
#include "pq/document/serialize.h"
#include "pq/media/wav_file.h"

using namespace pq;
using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kBar = 96000;  // frames of a bar at 120 BPM, 48 kHz
using Channels = std::vector<std::vector<float>>;

fs::path freshDir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / ("pq_test_export_" + name);
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

app::ExportRequest request(const Snapshot& s, Ticks to, const fs::path& dir, std::string name = "Master") {
  app::ExportRequest r;
  r.snapshot = s;
  r.from = 0;
  r.to = to;
  r.bits = app::ExportBits::Float32;
  r.folder = dir;
  r.baseName = std::move(name);
  r.jit = false;  // interpreter images: the JIT's compile time would dominate these short renders
  return r;
}

app::ExportReport run(const app::ExportRequest& r) {
  auto rep = app::runExport(r);
  REQUIRE_MESSAGE(rep.ok(), (rep.ok() ? std::string() : rep.error().message));
  REQUIRE_FALSE(rep->cancelled);
  return *rep;
}

media::WavData readBack(const fs::path& p) {
  auto w = media::readWav(p);
  REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
  return std::move(*w);
}

const fs::path& fileOf(const app::ExportReport& r, TrackId t) {
  for (const app::ExportedFile& f : r.files)
    if (f.track == t) return f.path;
  FAIL("no file for track " << t.value);
  return r.files.front().path;
}

float peakOf(const Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (float x : c) p = std::max(p, std::fabs(x));
  return p;
}

double maxDiff(const Channels& a, const Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i)
      m = std::max(m, std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i])));
  }
  return m;
}

Channels sum(const std::vector<Channels>& parts) {
  Channels s = parts.front();
  for (size_t k = 1; k < parts.size(); ++k)
    for (size_t c = 0; c < s.size(); ++c)
      for (size_t i = 0; i < s[c].size(); ++i) s[c][i] += parts[k][c][i];
  return s;
}

// The lag (frames, |lag| ≤ 8) at which y best matches x over the first `n` frames of channel 0 (y[i + lag] ~ x[i]).
int bestLag(const std::vector<float>& x, const std::vector<float>& y, size_t n) {
  int best = 0;
  double bestC = -1e300;
  for (int lag = -8; lag <= 8; ++lag) {
    double c = 0.0;
    for (size_t i = 16; i < n; ++i) c += static_cast<double>(x[i]) * y[static_cast<size_t>(static_cast<int64_t>(i) + lag)];
    if (c > bestC) {
      bestC = c;
      best = lag;
    }
  }
  return best;
}

}  // namespace

TEST_CASE("EX.1: a master export (32f) equals pqdaw --render of the same range bit for bit" *
          doctest::test_suite("slow")) {  // two JIT compiles (~5 s each in the check build)
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("render");
  const fs::path proj = dir / "proj";
  REQUIRE(saveProject(proj.string(), *f.snap()).ok());
  app::CliOptions o;
  o.mode = app::Mode::Render;
  o.project = proj.string();
  o.outPath = (dir / "render.wav").string();
  o.seconds = 2.0;  // one bar
  o.secondsGiven = true;
  REQUIRE(app::runRender(o) == 0);
  // The export of the document --render loads (the same load: knob positions through the device library).
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  LoadOptions lo;
  lo.devices = lib->get();
  auto loaded = loadProject(proj.string(), lo);
  REQUIRE(loaded.ok());
  app::ExportRequest r = request(loaded->project, kTicksPerWhole, dir);
  r.projectDir = proj.string();
  r.jit = true;  // as --render
  const app::ExportReport rep = run(r);
  REQUIRE(rep.files.size() == 1);
  CHECK(rep.frames == kBar);
  const media::WavData a = readBack(dir / "render.wav"), b = readBack(dir / "Master.wav");
  CHECK(b.isFloat);
  REQUIRE(a.frames() == kBar);
  CHECK(peakOf(b.channels) > 0.01f);
  CHECK(a.channels == b.channels);
  fs::remove_all(dir);
}

TEST_CASE("EX.1: stems (post-fader out buses, one pass) sum to the master within -120 dB, with and without PDC; mute "
          "respected") {
  for (const float delayMs : {0.0f, -10.0f}) {
    CAPTURE(delayMs);
    playback_fixture::Fixture f;
    if (delayMs != 0.0f) REQUIRE(f.store.dispatch(SetTrackDelay{f.lead, delayMs}).ok());
    const fs::path dir = freshDir("stems");
    const app::ExportReport master = run(request(f.snap(), kTicksPerWhole, dir));
    app::ExportRequest r = request(f.snap(), kTicksPerWhole, dir, "Stems");
    r.what = app::ExportWhat::Stems;
    const app::ExportReport stems = run(r);
    CHECK(stems.passes == 1);
    CHECK(stems.files.size() == 5);  // Echo, Lead, Grp, Mos, Muted
    CHECK(stems.frames == kBar);
    const Channels m = readBack(master.files.at(0).path).channels;
    std::vector<Channels> top;
    for (const TrackId t : {f.echo, f.lead, f.grp, f.muted}) top.push_back(readBack(fileOf(stems, t)).channels);
    const float pk = peakOf(m);
    REQUIRE(pk > 0.01f);
    const double nullDb = 20.0 * std::log10(std::max(maxDiff(sum(top), m), 1e-30) / pk);
    MESSAGE("stems vs master null: " << nullDb << " dB (Track Delay " << delayMs << " ms)");
    CHECK(nullDb <= -120.0);
    CHECK(peakOf(readBack(fileOf(stems, f.muted)).channels) == 0.0f);  // muted: silence
    CHECK(peakOf(readBack(fileOf(stems, f.lead)).channels) > 0.01f);
    CHECK(fs::exists(dir / "Stems Mos.wav"));
    fs::remove_all(dir);
  }
}

TEST_CASE("EX.1: stems through the returns and the master: one solo pass per track on one link, each as the first pass") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("solo");
  app::ExportRequest cap = request(f.snap(), kTicksPerWhole, dir / "cap", "");
  cap.what = app::ExportWhat::Stems;
  const app::ExportReport buses = run(cap);
  app::ExportRequest solo = request(f.snap(), kTicksPerWhole, dir / "solo", "");
  solo.what = app::ExportWhat::Tracks;
  solo.tracks = {f.lead, f.mos};
  solo.includeReturnsMaster = true;
  const app::ExportReport passes = run(solo);
  CHECK(passes.passes == 2);
  REQUIRE(passes.files.size() == 2);
  CHECK(passes.files[0].path.filename() == "Lead.wav");
  // Mos soloed: the master plays its group (Grp holds only Mos; Lead's send is closed by the solo).
  const Channels mos = readBack(fileOf(passes, f.mos)).channels, grp = readBack(fileOf(buses, f.grp)).channels;
  REQUIRE(peakOf(grp) > 0.01f);
  CHECK(20.0 * std::log10(std::max(maxDiff(mos, grp), 1e-30) / peakOf(grp)) <= -120.0);
  // Lead soloed: Lead and what it sends to Echo.
  const Channels lead = readBack(fileOf(passes, f.lead)).channels;
  const Channels ref = sum({readBack(fileOf(buses, f.lead)).channels, readBack(fileOf(buses, f.echo)).channels});
  CHECK(20.0 * std::log10(std::max(maxDiff(lead, ref), 1e-30) / peakOf(ref)) <= -120.0);
  fs::remove_all(dir);
}

namespace {

struct Collect final : engine::AudioSink {
  Channels ch;
  Status begin(uint32_t n, uint32_t, uint64_t) override {
    ch.assign(n, {});
    return Ok{};
  }
  Status write(const float* const* x, uint32_t frames) override {
    for (size_t c = 0; c < ch.size(); ++c) ch[c].insert(ch[c].end(), x[c], x[c] + frames);
    return Ok{};
  }
  Status end() override { return Ok{}; }
};

Channels masterPass(app::ProjectLink& link, const Project& p, Ticks to) {
  Collect out;
  app::PassSpec spec;
  spec.from = 0;
  spec.to = to;
  spec.targets.push_back(app::PassTarget{engine::kNoBus, &out});
  auto r = app::renderPass(link, p, spec);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return out.ch;
}

std::unique_ptr<app::ProjectLink> offlineLink(const Snapshot& s, const fs::path& dir) {
  auto l = app::makeOfflineLink(s, 48000.0, dir.string(), 0, false);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(*l);
}

}  // namespace

TEST_CASE("EX.x-fix-settle: every pass on one link equals the first, bit for bit (automated volume; solo passes)") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("settle");
  const Ticks to = kTicksPerWhole;
  // Grp's volume is automated from tick 0 (−18 dB, rising to 0 dB at beat 2): a pass ends at 0 dB, the next starts
  // at −18 dB again. Before the fix the second pass faded from 0 dB for ~30 ms (the gain fell asleep mid-ramp in the
  // settle and kept the previous pass's value).
  auto link = offlineLink(f.snap(), dir);
  const Channels first = masterPass(*link, *f.snap(), to);
  REQUIRE(peakOf(first) > 0.01f);
  CHECK(maxDiff(first, masterPass(*link, *f.snap(), to)) == 0.0);
  CHECK(maxDiff(first, masterPass(*link, *f.snap(), to)) == 0.0);
  // Solo passes on the link (param writes), Lead then Mos: Mos's equals Mos soloed on a fresh link (Q251's bleed).
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  auto solo = [&](TrackId t, bool on) {
    REQUIRE(f.store.dispatch(SetTrackSolo{t, on}).ok());
    link->flush();
  };
  solo(f.lead, true);
  (void)masterPass(*link, *f.snap(), to);
  solo(f.lead, false);
  solo(f.mos, true);
  const Channels mos = masterPass(*link, *f.snap(), to);
  const Channels fresh = masterPass(*offlineLink(f.snap(), dir), *f.snap(), to);
  REQUIRE(peakOf(fresh) > 0.01f);
  CHECK(maxDiff(mos, fresh) == 0.0);
  f.store.unsubscribe(sub);
  link.reset();
  fs::remove_all(dir);
}

TEST_CASE("EX.1: render as loop keeps the second lap: frame 0 carries the first lap's release") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("loop");
  // [0, 5 beats): Lead's dyad (beats 4 … 6) is held at the loop end and released by the wrap.
  const Ticks end = 5 * kPpq;
  const app::ExportReport plain = run(request(f.snap(), end, dir, "Plain"));
  app::ExportRequest r = request(f.snap(), end, dir, "Loop");
  r.renderAsLoop = true;
  r.tailMaxSeconds = 10.0;  // ignored: a loop is exactly its length
  const app::ExportReport loop = run(r);
  CHECK(loop.frames == 5 * kBar / 4);
  const Channels a = readBack(plain.files[0].path).channels, b = readBack(loop.files[0].path).channels;
  REQUIRE(b[0].size() == 5 * kBar / 4);
  double startDiff = 0.0;
  for (size_t i = 0; i < 256; ++i) startDiff = std::max(startDiff, static_cast<double>(std::fabs(a[0][i] - b[0][i])));
  MESSAGE("loop vs plain, first 256 frames: max difference " << startDiff);
  CHECK(startDiff > 1e-3);
  fs::remove_all(dir);
}

TEST_CASE("EX.1: PDC: an oversampled group's latency is trimmed (the export lines up with the plain project)") {
  playback_fixture::Fixture plainDoc, latentDoc;
  playback_fixture::Fixture& d = latentDoc;
  d.store.setDeviceLibrary(&playback_fixture::unitsLibrary());
  EqBandOptions bo;
  bo.freqHz = 3000.0;
  auto pasted = ops::pasteBlocks(*d.snap(), d.store.ids(), d.lead, eqBandBlocks(d.store.ids(), bo));
  REQUIRE(pasted.ok());
  REQUIRE(d.store.dispatch(pasted->command).ok());
  const DeviceId band = pasted->devices.at(0);
  auto ins = ops::insertOnPath(*d.snap(), d.store.ids(), band, 1);
  REQUIRE(ins.ok());
  REQUIRE(d.store.dispatch(ins->command).ok());
  auto os = ops::setDeviceSetting(*d.snap(), band, std::string(kGroupOversample), 2.0);  // 4×: 4.74 frames
  REQUIRE(os.ok());
  REQUIRE(d.store.dispatch(os->command).ok());
  const fs::path dir = freshDir("pdc");
  const Channels plain = readBack(run(request(plainDoc.snap(), kTicksPerWhole, dir, "Plain")).files[0].path).channels;
  const Channels latent = readBack(run(request(d.snap(), kTicksPerWhole, dir, "Latent")).files[0].path).channels;
  // The same project rendered raw (no trim): late by the master's latency.
  auto link = app::makeOfflineLink(d.snap(), 48000.0, "", 0, false);
  REQUIRE(link.ok());
  REQUIRE((*link)->settleOffline(0, app::kSettleMinSeconds, app::kSettleMaxSeconds).ok());
  CHECK((*link)->outputLatency() == 5);
  engine::BufferSink raw;
  REQUIRE((*link)->renderOffline(0, 8192, raw).ok());
  CHECK(bestLag(plain[0], raw.channels[0], 8000) == 5);
  CHECK(bestLag(plain[0], latent[0], 8000) == 0);
  fs::remove_all(dir);
}

TEST_CASE("EX.1: a whole-song tail ends at the last frame above -90 dBFS") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("tail");
  // [0, 6 beats): Lead's dyad ends with the range; its release is the tail (nothing plays after it).
  app::ExportRequest r = request(f.snap(), 6 * kPpq, dir);
  r.tailMaxSeconds = 10.0;
  const app::ExportReport rep = run(r);
  const uint64_t range = 6 * kBar / 4;
  MESSAGE("tail: " << (static_cast<double>(rep.frames) - static_cast<double>(range)) / 48.0 << " ms");
  CHECK(rep.frames > range);
  CHECK(rep.frames < range + 48000);
  const Channels x = readBack(rep.files[0].path).channels;
  REQUIRE(x[0].size() == rep.frames);
  CHECK(std::max(std::fabs(x[0].back()), std::fabs(x[1].back())) > app::kTailFloor);
  fs::remove_all(dir);
}

TEST_CASE("EX.1: cancel leaves no new file and an existing one untouched") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("cancel");
  { std::ofstream(dir / "Master.wav", std::ios::binary) << "keep me"; }
  std::atomic<bool> cancel{true};
  std::atomic<float> progress{0.0f};
  for (const app::ExportWhat what : {app::ExportWhat::Master, app::ExportWhat::Stems}) {
    app::ExportRequest r = request(f.snap(), kTicksPerWhole, dir);
    r.what = what;
    r.bits = app::ExportBits::Pcm24;
    r.normalize = true;
    auto rep = app::runExport(r, &progress, &cancel);
    REQUIRE(rep.ok());
    CHECK(rep->cancelled);
    CHECK(rep->files.empty());
  }
  std::vector<fs::path> left;
  for (const auto& e : fs::directory_iterator(dir)) left.push_back(e.path().filename());
  REQUIRE(left.size() == 1);
  CHECK(left[0] == "Master.wav");
  std::ifstream in(dir / "Master.wav", std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(s == "keep me");
  in.close();
  fs::remove_all(dir);
}

TEST_CASE("EX.1: PCM16 (TPDF, shaped) and PCM24 within their error bounds of the normalized float; values in range") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("formats");
  auto exportAs = [&](app::ExportBits bits, app::ExportDither dither, const std::string& name) {
    app::ExportRequest r = request(f.snap(), kTicksPerWhole, dir, name);
    r.bits = bits;
    r.dither = dither;
    r.normalize = true;
    const app::ExportReport rep = run(r);
    return readBack(rep.files[0].path);
  };
  const media::WavData ref = exportAs(app::ExportBits::Float32, app::ExportDither::Tpdf, "F32");
  CHECK(peakOf(ref.channels) == doctest::Approx(0.96605088).epsilon(1e-6));  // −0.3 dBFS
  const media::WavData tpdf = exportAs(app::ExportBits::Pcm16, app::ExportDither::Tpdf, "Tpdf");
  const media::WavData shaped = exportAs(app::ExportBits::Pcm16, app::ExportDither::Shaped, "Shaped");
  const media::WavData p24 = exportAs(app::ExportBits::Pcm24, app::ExportDither::None, "P24");
  CHECK_FALSE(tpdf.isFloat);
  CHECK(tpdf.bitsPerSample == 16);
  CHECK(p24.bitsPerSample == 24);
  constexpr double lsb16 = 1.0 / 32768.0, lsb24 = 1.0 / 8388608.0;
  CHECK(maxDiff(tpdf.channels, ref.channels) <= 1.5 * lsb16 + 1e-9);   // rounding ± ½ + TPDF ± 1
  CHECK(maxDiff(shaped.channels, ref.channels) <= 6.0 * lsb16);        // the error through 1 − H(z)
  CHECK(maxDiff(p24.channels, ref.channels) <= 0.5 * lsb24 + 1e-12);  // rounding only
  CHECK(maxDiff(tpdf.channels, ref.channels) > 0.5 * lsb16);          // dithered, not just rounded
  for (const auto* w : {&tpdf, &shaped})
    for (const auto& c : w->channels)
      for (float x : c) REQUIRE((x >= -1.0f && x <= 1.0f - static_cast<float>(lsb16)));
  // Error power below ~1.5 kHz (32-frame means): the shaped dither's is well under TPDF's.
  auto lowRms = [&](const media::WavData& w) {
    double s = 0.0;
    size_t n = 0;
    const auto& x = w.channels[0];
    const auto& y = ref.channels[0];
    for (size_t i = 0; i + 32 <= x.size(); i += 32, ++n) {
      double m = 0.0;
      for (size_t k = 0; k < 32; ++k) m += static_cast<double>(x[i + k]) - y[i + k];
      s += (m / 32.0) * (m / 32.0);
    }
    return std::sqrt(s / static_cast<double>(n));
  };
  const double lt = lowRms(tpdf), ls = lowRms(shaped);
  MESSAGE("low-frequency error RMS (LSB): TPDF " << lt / lsb16 << ", shaped " << ls / lsb16);
  CHECK(ls < 0.6 * lt);
  fs::remove_all(dir);
}

// ---- AU.app (docs/reviews/RB-DY.md L3, L4) --------------------------------------------------------------------------

TEST_CASE("AU.app L3: an export error names a non-ANSI folder in UTF-8 (path::string() would throw on the worker)") {
  playback_fixture::Fixture f;
  const fs::path base = freshDir("l3");
  const fs::path folder = base / fs::path(u8"Ωμέγα");  // Greek: outside the ANSI code page
  std::ofstream(folder) << "a file where the folder goes";
  app::ExportRequest r = request(f.snap(), kTicksPerWhole, folder, "\xCE\x94");
  bool threw = false;
  std::string message;
  try {
    auto rep = app::runExport(r);
    REQUIRE_FALSE(rep.ok());
    message = rep.error().message;
  } catch (...) {
    threw = true;
  }
  CHECK_FALSE(threw);
  CHECK(message.find("\xCE\xA9") != std::string::npos);  // the folder's name, in UTF-8
  fs::remove_all(base);
}

TEST_CASE("AU.app L4: exportFiles names runExport's files; a replace refused mid-way puts every old file back") {
  playback_fixture::Fixture f;
  const fs::path dir = freshDir("l4");
  app::ExportRequest r = request(f.snap(), kTicksPerWhole, dir, "Mix");
  r.what = app::ExportWhat::Stems;
  const std::vector<fs::path> names = app::exportFiles(r);
  REQUIRE(names.size() >= 2);
  for (const fs::path& n : names) std::ofstream(n, std::ios::binary) << "old";
  auto content = [](const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  auto leftovers = [&] {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) n += e.path().u8string().find(u8".pqexport") != std::u8string::npos;
    return n;
  };
  {  // The last file is open in a player (no delete sharing): the export fails and no old file is replaced.
    std::ifstream held(names.back(), std::ios::binary);
    REQUIRE(held.is_open());
    auto rep = app::runExport(r);
    CHECK_FALSE(rep.ok());
  }
  for (const fs::path& n : names) CHECK(content(n) == "old");
  CHECK(leftovers() == 0);
  // Not held: every file is replaced; they are the files exportFiles named, in its order.
  const app::ExportReport rep = run(r);
  REQUIRE(rep.files.size() == names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    CHECK(rep.files[i].path == names[i]);
    CHECK(content(names[i]) != "old");
  }
  CHECK(leftovers() == 0);
  fs::remove_all(dir);
}
