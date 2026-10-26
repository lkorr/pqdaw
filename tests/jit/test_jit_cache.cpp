// T1b.4: the JIT compile cache (pq/jit/cache.h). Images whose machine code comes from memory, from the disk cache or
// from a read-only (AOT) directory compute exactly what freshly compiled images compute (bit for bit, against the
// interpreter, under RtScope); a cached load runs no LLVM compile (CacheCounters::compiles); every hit regenerates the
// IR in verify mode and must match the IR the code was compiled from (the key misses nothing code generation reads);
// identities (state keys, seeds) are not part of the key but stay per image; anything else that differs misses;
// truncated, flipped or foreign entries are rejected and replaced; concurrent writers and readers never see half an
// entry; the read-write directory stays under its bound, least recently used first; baseline-target code (the AOT
// seed) is bit-identical to host code.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../engine/interp_harness.h"
#include "pq/jit/compiler.h"

using namespace interp_test;
namespace jit = pq::jit;

namespace {

constexpr uint32_t kFrames = 1024;
constexpr uint32_t kBlock = 64;

// A fresh directory under the temp dir, removed at the end of the test.
struct TempDir {
  fs::path path;
  explicit TempDir(const char* tag) {
    static std::atomic<uint32_t> n{0};
    path = fs::temp_directory_path() / ("pq_jit_cache_" + std::string(tag) + "_" +
                                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                                        std::to_string(n++));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::vector<fs::path> entries(const char* ext = ".pqjit") const {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(path))
      if (e.path().extension() == ext) out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
  }
  uint64_t bytes() const {
    uint64_t b = 0;
    for (const fs::path& p : entries()) b += fs::file_size(p);
    return b;
  }
};

std::unique_ptr<jit::Compiler> makeCompiler(const jit::CacheConfig& cache, jit::Target target = jit::Target::Host,
                                            uint32_t codeGenLevel = jit::CompilerConfig::kDefaultCodeGenLevel) {
  jit::CompilerConfig cfg;
  cfg.cache = cache;
  cfg.target = target;
  cfg.codeGenLevel = codeGenLevel;
  auto r = jit::Compiler::create(cfg);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(r.value());
}

jit::CacheConfig cacheIn(const fs::path& dir, bool verify = true) {
  jit::CacheConfig c;
  c.dir = dir;
  c.verify = verify;
  return c;
}

ImageBuilder builder(jit::Compiler& c, jit::CompileOptions base = {}, jit::CompileStats* stats = nullptr) {
  return [&c, base, stats](const Schedule& s, const eng::InterpretOptions& io) {
    jit::CompileOptions o = base;
    o.maxBlockFrames = io.maxBlockFrames;
    o.maxVoiceGroups = io.maxVoiceGroups;
    o.resources = io.resources;
    return c.compile(s, o, stats);
  };
}

uint32_t voicesFor(const Compiled& c) {
  for (const Stage& s : c.sched.stages)
    if (s.poly == Poly::Voice) return 11;
  return 0;
}

// The geometry runInterp builds `c` with (another geometry is another memory layout: another key).
jit::CompileOptions optionsFor(const Compiled& c) {
  jit::CompileOptions o;
  o.maxBlockFrames = 1024;
  o.maxVoiceGroups = std::max<uint32_t>(1, (voicesFor(c) + 7) / 8);
  return o;
}

uint64_t bitDifferences(const Streams& a, const Streams& b) {
  uint64_t n = 0;
  if (a.size() != b.size()) return UINT64_MAX;
  for (const auto& [key, x] : a) {
    const auto it = b.find(key);
    if (it == b.end() || it->second.size() != x.size()) return UINT64_MAX;
    for (size_t t = 0; t < x.size(); ++t) {
      const double p = x[t], q = it->second[t];
      if (std::isnan(p) && std::isnan(q)) continue;
      if (std::memcmp(&p, &q, sizeof p) != 0) ++n;
    }
  }
  return n;
}

// The patch's streams through `c` must equal the interpreter's bit for bit (and run allocation-free).
void checkAgainstInterpreter(const Compiled& patch, jit::Compiler& c, jit::CompileStats* stats = nullptr,
                             jit::CompileOptions base = {}) {
  const uint32_t voices = voicesFor(patch);
  const Setup st(patch, voices, 1);
  INFO(patch.file << ":" << patch.name);
  const InterpRun ref = runInterp(st, kFrames, kBlock);
  REQUIRE_MESSAGE(ref.error.empty(), ref.error);
  const InterpRun run = runInterp(st, kFrames, kBlock, 1024, builder(c, base, stats));
  REQUIRE_MESSAGE(run.error.empty(), run.error);
  CHECK(run.rtViolations == 0);
  CHECK(bitDifferences(ref.streams, run.streams) == 0);
}

std::vector<Compiled> compileAll(const std::vector<fs::path>& files) {
  std::vector<Compiled> out;
  std::vector<std::string> skipped;
  for (const fs::path& f : files)
    for (Compiled& c : compileFile(f, 48000.0, skipped)) out.push_back(std::move(c));
  return out;
}

std::vector<fs::path> corpusFiles() {
  std::vector<fs::path> files = text_util::pqpFiles(text_util::casesDir());
  for (const fs::path& p : text_util::pqpFiles(patchesDir())) files.push_back(p);
  for (const fs::path& p : text_util::pqpFiles(fs::path(PQ_TEST_DATA_DIR) / "jit" / "patches")) files.push_back(p);
  return files;
}

// A few files that cover every region kind, voices, solves, delay lines, tables and noise (default tier).
std::vector<fs::path> sampleFiles() {
  return {text_util::casesDir() / "fm_voice.pqp", text_util::casesDir() / "solve.pqp", text_util::casesDir() / "feedback.pqp",
          patchesDir() / "interp.pqp", fs::path(PQ_TEST_DATA_DIR) / "jit" / "patches" / "stateful_loops.pqp"};
}

// Runs every patch through a caching compiler (A), then through a second one on the same directory (B): B must
// compile nothing, every hit must verify, and both must equal the interpreter bit for bit.
void roundTrip(const std::vector<Compiled>& patches) {
  REQUIRE(!patches.empty());
  TempDir dir("roundtrip");
  {
    auto a = makeCompiler(cacheIn(dir.path));
    for (const Compiled& p : patches) checkAgainstInterpreter(p, *a);
    const jit::CacheCounters ca = a->counters();
    CHECK(ca.compiles > 0);
    CHECK(ca.stores == ca.compiles);
    CHECK(ca.uncacheable == 0);
    CHECK(ca.storeErrors == 0);
    CHECK(ca.verifyMismatches == 0);
    CHECK(dir.entries().size() <= ca.stores);  // patches with equal code share an entry
  }
  auto b = makeCompiler(cacheIn(dir.path));
  jit::CompileStats st;
  for (const Compiled& p : patches) checkAgainstInterpreter(p, *b, &st);
  const jit::CacheCounters cb = b->counters();
  CHECK(cb.compiles == 0);
  CHECK(cb.diskHits + cb.memoryHits == patches.size());  // one image per patch
  CHECK(cb.rejected == 0);
  CHECK(cb.verified == cb.diskHits + cb.memoryHits);
  CHECK(cb.verifyMismatches == 0);
  MESSAGE(patches.size() << " patches, " << cb.diskHits << " entries (" << dir.bytes() / 1024 << " KiB) loaded from disk, "
                         << cb.memoryHits << " memory hits; last load " << st.cacheMs << " ms");
}

}  // namespace

TEST_CASE("jit cache: patches loaded from the disk cache compile nothing and are bit-identical (sample of the corpus)") {
  roundTrip(compileAll(sampleFiles()));
}

TEST_CASE("jit cache: the whole corpus through the disk cache: no compile, every key verified, bit-identical" *
          doctest::test_suite("slow")) {
  roundTrip(compileAll(corpusFiles()));
}

TEST_CASE("jit cache: identities are not in the key: equal code is shared in memory, state keys and seeds stay per image") {
  // Noise (a seed mixed with the state key), a delay line, a param: two schedules that differ only in identities.
  const Compiled a = compileText(
      "patch P { in x : audio\n param g = 0.5 [0..1]\n out y : audio\n"
      " n = noise()\n d = delay(x + n * 0.1, 3ms, 10ms)\n fb = d * g + z1(fb) * 0.25\n y = fb }");
  Compiled b = a;
  for (StateEntry& e : b.sched.state.entries) {
    e.key ^= 0x5DEECE66Dull;
    e.init.seed ^= 0x9E3779B9u;
  }
  for (ParamSlot& p : b.sched.params) p.key ^= 0x1234ull;

  auto c = makeCompiler(jit::CacheConfig{});  // memory only
  jit::CompileStats sa, sb;
  const jit::CompileOptions o = optionsFor(a);  // the geometry the checks below build with
  auto ia = c->compile(a.sched, o, &sa);
  auto ib = c->compile(b.sched, o, &sb);
  REQUIRE(ia.ok());
  REQUIRE(ib.ok());
  CHECK(sa.source == jit::CompileStats::Source::Compiled);
  CHECK(sb.source == jit::CompileStats::Source::Memory);
  CHECK(c->counters().compiles == 1);
  CHECK(c->counters().memoryHits == 1);
  // Each image has its own schedule's keys …
  REQUIRE((*ib)->stateLayout().entries.size() == b.sched.state.entries.size());
  for (size_t i = 0; i < b.sched.state.entries.size(); ++i) CHECK((*ib)->stateLayout().entries[i].key == b.sched.state.entries[i].key);
  // … and computes what its own schedule computes (the other seed: a different noise stream), bit for bit.
  checkAgainstInterpreter(b, *c);
  checkAgainstInterpreter(a, *c);
  const Setup sta(a, 0, 0), stb(b, 0, 0);
  const InterpRun ra = runInterp(sta, kFrames, kBlock), rb = runInterp(stb, kFrames, kBlock);
  CHECK(bitDifferences(ra.streams, rb.streams) > 0);  // the seeds matter: shared code, separate state
  // The code stays alive while one image uses it and is released with the last.
  ia.value().reset();
  auto ic = c->compile(a.sched, o);
  REQUIRE(ic.ok());
  CHECK(c->counters().compiles == 1);  // ib still holds it
  ib.value().reset();
  ic.value().reset();
  auto id = c->compile(a.sched, o);
  REQUIRE(id.ok());
  CHECK(c->counters().compiles == 2);  // nothing alive held it: compiled again (no disk cache here)
}

TEST_CASE("jit cache: whatever code generation reads is in the key: other constants, layouts and options miss") {
  TempDir dir("key");
  const char* text = "patch P { in x : audio\n param g = 0.5 [0..1]\n out y : audio\n y = tanh(x * %s) * g }";
  auto withConst = [&](const char* k) {
    char buf[256];
    std::snprintf(buf, sizeof buf, text, k);
    return compileText(buf);
  };
  const Compiled p3 = withConst("3"), p4 = withConst("4");
  auto c = makeCompiler(cacheIn(dir.path));
  auto compileWith = [&](const Compiled& p, jit::CompileOptions o) {
    auto img = c->compile(p.sched, o);
    REQUIRE(img.ok());
    return std::move(img.value());
  };
  std::vector<std::unique_ptr<eng::ProgramImage>> alive;  // keep them alive: memory hits would hide misses otherwise
  alive.push_back(compileWith(p3, {}));
  alive.push_back(compileWith(p4, {}));  // another constant
  jit::CompileOptions block;
  block.maxBlockFrames = 256;
  alive.push_back(compileWith(p3, block));  // another memory layout
  jit::CompileOptions groups;
  groups.maxVoiceGroups = 2;
  alive.push_back(compileWith(p3, groups));
  jit::CompileOptions opt;
  opt.optLevel = 2;
  alive.push_back(compileWith(p3, opt));
  jit::CompileOptions contract;
  contract.contract = true;
  alive.push_back(compileWith(p3, contract));
  CHECK(c->counters().compiles == 6);
  CHECK(c->counters().memoryHits == 0);
  alive.push_back(compileWith(p3, {}));  // the first again: shared
  CHECK(c->counters().memoryHits == 1);
  CHECK(dir.entries().size() == 6);
  // Another machine-code level and another target are other compilers' keys.
  auto level0 = makeCompiler(cacheIn(dir.path), jit::Target::Host, 0);
  REQUIRE(level0->compile(p3.sched).ok());
  CHECK(level0->counters().compiles == 1);
  auto baseline = makeCompiler(cacheIn(dir.path), jit::Target::Baseline);
  REQUIRE(baseline->compile(p3.sched).ok());
  CHECK(baseline->counters().compiles == 1);
  CHECK(dir.entries().size() == 8);
  // A third host compiler at the default level finds all six of its entries.
  auto again = makeCompiler(cacheIn(dir.path));
  for (const jit::CompileOptions& o : {jit::CompileOptions{}, block, groups, opt, contract}) REQUIRE(again->compile(p3.sched, o).ok());
  REQUIRE(again->compile(p4.sched).ok());
  CHECK(again->counters().compiles == 0);
  CHECK(again->counters().diskHits == 6);
}

TEST_CASE("jit cache: images that differ only in their voice code (T1b.2) never share code or an entry") {
  TempDir dir("voicecode");
  const std::vector<Compiled> fm = compileAll({text_util::casesDir() / "fm_voice.pqp"});
  REQUIRE(!fm.empty());
  const Compiled& p = fm.front();
  REQUIRE(voicesFor(p) > 0);
  const jit::VoiceCode codes[] = {jit::VoiceCode::Groups, jit::VoiceCode::Pairs256, jit::VoiceCode::Pairs512};
  auto c = makeCompiler(cacheIn(dir.path));
  const bool avx512 = c->hostFeatures().find("+avx512f") != std::string::npos;
  std::vector<std::unique_ptr<eng::ProgramImage>> alive;  // all alive at once: a shared key would be a memory hit
  for (jit::VoiceCode vc : codes) {
    jit::CompileOptions o = optionsFor(p);
    o.voiceCode = vc;
    jit::CompileStats st;
    auto img = c->compile(p.sched, o, &st);
    REQUIRE(img.ok());
    CHECK(st.source == jit::CompileStats::Source::Compiled);
    CHECK((st.pairedStages > 0) == (vc != jit::VoiceCode::Groups));
    alive.push_back(std::move(img.value()));
  }
  // Pairs512 on a host without AVX-512 is Pairs256 (the same code, so the same key: shared).
  CHECK(c->counters().compiles == (avx512 ? 3u : 2u));
  CHECK(c->counters().memoryHits == (avx512 ? 0u : 1u));
  CHECK(dir.entries().size() == (avx512 ? 3u : 2u));
  // From disk, each voice code finds its own entry and computes the interpreter's bits.
  auto again = makeCompiler(cacheIn(dir.path));
  for (jit::VoiceCode vc : codes) {
    jit::CompileOptions o;
    o.voiceCode = vc;
    jit::CompileStats st;
    checkAgainstInterpreter(p, *again, &st, o);
    CHECK(st.source == jit::CompileStats::Source::Disk);
    CHECK((st.pairedStages > 0) == (vc != jit::VoiceCode::Groups));
  }
  CHECK(again->counters().compiles == 0);
  CHECK(again->counters().verifyMismatches == 0);
}

TEST_CASE("jit cache: truncated, corrupted and foreign entries are rejected, recompiled and replaced") {
  TempDir dir("corrupt");
  const std::vector<Compiled> fm = compileAll({text_util::casesDir() / "fm_voice.pqp"});
  REQUIRE(!fm.empty());
  const Compiled& p = fm.front();
  {
    auto a = makeCompiler(cacheIn(dir.path));
    REQUIRE(a->compile(p.sched, optionsFor(p)).ok());
  }
  REQUIRE(dir.entries().size() == 1);
  const fs::path entry = dir.entries().front();
  std::vector<char> good;
  {
    std::ifstream f(entry, std::ios::binary);
    good.assign(std::istreambuf_iterator<char>(f), {});
  }
  REQUIRE(good.size() > 256);
  struct Damage {
    const char* what;
    std::vector<char> (*apply)(std::vector<char>);
  };
  const Damage damages[] = {
      {"empty file", [](std::vector<char>) { return std::vector<char>(); }},
      {"header only", [](std::vector<char> b) { b.resize(64); return b; }},
      {"truncated object", [](std::vector<char> b) { b.resize(b.size() - 100); return b; }},
      {"one bit flipped in the object", [](std::vector<char> b) { b[b.size() - 50] ^= 4; return b; }},
      {"one bit flipped in the payload hash", [](std::vector<char> b) { b[41] ^= 1; return b; }},
      {"another key in the header", [](std::vector<char> b) { b[16] ^= 1; return b; }},
      {"another format version", [](std::vector<char> b) { b[8] ^= 1; return b; }},
      {"wrong magic", [](std::vector<char> b) { b[0] = 'X'; return b; }},
      {"a byte appended", [](std::vector<char> b) { b.push_back(0); return b; }},
      {"garbage", [](std::vector<char> b) {
         std::mt19937 rng(7);
         for (char& c : b) c = static_cast<char>(rng());
         return b;
       }},
  };
  for (const Damage& d : damages) {
    INFO(std::string(d.what));
    {
      std::ofstream f(entry, std::ios::binary | std::ios::trunc);
      const std::vector<char> bad = d.apply(good);
      f.write(bad.data(), static_cast<std::streamsize>(bad.size()));
    }
    auto c = makeCompiler(cacheIn(dir.path));
    checkAgainstInterpreter(p, *c);
    CHECK(c->counters().rejected == 1);
    CHECK(c->counters().compiles == 1);
    CHECK(c->counters().stores == 1);
    // The replacement is valid.
    auto c2 = makeCompiler(cacheIn(dir.path));
    REQUIRE(c2->compile(p.sched, optionsFor(p)).ok());
    CHECK(c2->counters().diskHits == 1);
    CHECK(c2->counters().rejected == 0);
  }
}

TEST_CASE("jit cache: concurrent writers and readers of one directory never see half an entry") {
  TempDir dir("concurrent");
  std::vector<Compiled> patches = compileAll({text_util::casesDir() / "fm_voice.pqp", text_util::casesDir() / "feedback.pqp"});
  patches.push_back(compileText("patch Q { in x : audio\n out y : audio\n fb = sin(x * 2) + z1(fb) * 0.5\n y = fb }"));
  REQUIRE(patches.size() >= 3);
  constexpr int kThreads = 6, kRounds = 4;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([&, t] {
      for (int r = 0; r < kRounds; ++r) {
        // A new compiler per round (no memory hits): every compile reads or writes the directory.
        auto c = jit::Compiler::create(jit::CompilerConfig{jit::CompilerConfig::kDefaultCodeGenLevel, jit::Target::Host,
                                                           cacheIn(dir.path)});
        if (!c.ok()) {
          ++failures;
          return;
        }
        for (size_t i = 0; i < patches.size(); ++i) {
          const Compiled& p = patches[(i + static_cast<size_t>(t)) % patches.size()];
          auto img = (*c)->compile(p.sched, optionsFor(p));
          if (!img.ok()) ++failures;
        }
        if ((*c)->counters().rejected != 0 || (*c)->counters().verifyMismatches != 0) ++failures;
      }
    });
  for (std::thread& th : threads) th.join();
  CHECK(failures.load() == 0);
  CHECK(dir.entries(".tmp").empty());
  auto c = makeCompiler(cacheIn(dir.path));
  for (const Compiled& p : patches) REQUIRE(c->compile(p.sched, optionsFor(p)).ok());
  CHECK(c->counters().compiles == 0);
  CHECK(c->counters().rejected == 0);
  CHECK(c->counters().diskHits == patches.size());
  for (const Compiled& p : patches) checkAgainstInterpreter(p, *c);
}

TEST_CASE("jit cache: the read-write directory stays under its bound, least recently used entries go first") {
  TempDir dir("evict");
  auto patch = [](int k) {
    return compileText("patch P { in x : audio\n out y : audio\n fb = tanh(x * " + std::to_string(0.37 + 0.11 * k) + ") + z1(fb) * 0.5\n y = fb }");
  };
  // One entry's size, to set the bound to about five of them.
  uint64_t one = 0;
  {
    TempDir probe("evict_probe");
    auto c = makeCompiler(cacheIn(probe.path));
    REQUIRE(c->compile(patch(1).sched).ok());
    one = probe.bytes();
  }
  REQUIRE(one > 0);
  jit::CacheConfig cfg = cacheIn(dir.path);
  cfg.maxBytes = one * 5 + one / 2;
  std::vector<Compiled> ps;
  for (int k = 1; k <= 12; ++k) ps.push_back(patch(k));
  {
    auto c = makeCompiler(cfg);
    for (int k = 0; k < 5; ++k) {
      REQUIRE(c->compile(ps[static_cast<size_t>(k)].sched).ok());
      std::this_thread::sleep_for(std::chrono::milliseconds(30));  // distinct write times
    }
  }
  CHECK(dir.entries().size() == 5);
  {
    auto c = makeCompiler(cfg);  // entry 0 is used again: the most recent now
    REQUIRE(c->compile(ps[0].sched).ok());
    CHECK(c->counters().diskHits == 1);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  {
    auto c = makeCompiler(cfg);  // the sixth entry takes the directory over the bound: the oldest go
    REQUIRE(c->compile(ps[5].sched).ok());
    CHECK(c->counters().evictedFiles >= 1);  // down to 3/4 of the bound: the oldest (1, 2)
    CHECK(dir.bytes() <= cfg.maxBytes);
  }
  {
    // Entry 0 (refreshed by its hit) outlived entries 1 and 2 (written before it was used). No bound here, so the
    // check itself evicts nothing.
    auto check = makeCompiler(cacheIn(dir.path));
    REQUIRE(check->compile(ps[0].sched).ok());
    REQUIRE(check->compile(ps[3].sched).ok());
    CHECK(check->counters().diskHits == 2);
    REQUIRE(check->compile(ps[1].sched).ok());
    CHECK(check->counters().compiles == 1);
  }
  // However many more are written, the directory stays under the bound.
  auto c = makeCompiler(cfg);
  for (int k = 6; k < 12; ++k) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(c->compile(ps[static_cast<size_t>(k)].sched).ok());
    CHECK(dir.bytes() <= cfg.maxBytes);
  }
  CHECK(c->counters().evictedFiles > 0);
}

TEST_CASE("jit cache: baseline-target code is bit-identical; a host compiler takes it from a read-only (AOT) directory") {
  TempDir aot("aot"), user("aot_user");
  const std::vector<Compiled> patches = compileAll(sampleFiles());
  {
    auto base = makeCompiler(cacheIn(aot.path), jit::Target::Baseline);
    CHECK(base->hostCpu() == jit::kBaselineCpu);
    for (const Compiled& p : patches) checkAgainstInterpreter(p, *base);
    CHECK(base->counters().stores > 0);
  }
  const size_t aotEntries = aot.entries().size();
  std::vector<fs::file_time_type> times;
  for (const fs::path& e : aot.entries()) times.push_back(fs::last_write_time(e));
  jit::CacheConfig cfg = cacheIn(user.path);
  cfg.readOnly = {aot.path};
  auto host = makeCompiler(cfg);
  for (const Compiled& p : patches) checkAgainstInterpreter(p, *host);
  const jit::CacheCounters cc = host->counters();
  CHECK(cc.compiles == 0);
  CHECK(cc.aotHits == aotEntries);
  CHECK(cc.verifyMismatches == 0);
  // Read-only means read-only: nothing written, refreshed or deleted there; nothing copied to the user directory.
  REQUIRE(aot.entries().size() == aotEntries);
  for (size_t i = 0; i < aotEntries; ++i) CHECK(fs::last_write_time(aot.entries()[i]) == times[i]);
  CHECK(user.entries().empty());
}

TEST_CASE("jit cache: an image loaded from disk outlives its compiler") {
  TempDir dir("outlive");
  const Compiled c = compileText("patch P { in x : audio\n out y : audio\n y = tanh(x * 3) }");
  {
    auto a = makeCompiler(cacheIn(dir.path));
    REQUIRE(a->compile(c.sched).ok());
  }
  std::unique_ptr<eng::ProgramImage> img;
  {
    auto b = makeCompiler(cacheIn(dir.path));
    auto r = b->compile(c.sched);
    REQUIRE(r.ok());
    CHECK(b->counters().diskHits == 1);
    img = std::move(r.value());
  }
  std::vector<float> x(16, 0.25f), y(16);
  const float* ins[1] = {x.data()};
  float* outs[1] = {y.data()};
  eng::ProcessContext ctx;
  ctx.frames = 16;
  ctx.in = ins;
  ctx.numIn = 1;
  ctx.out = outs;
  ctx.numOut = 1;
  img->process(ctx);
  CHECK(y[15] == doctest::Approx(std::tanh(0.75)).epsilon(1e-6));
  img.reset();
}

// M1 review L6: an image's machine code used to be released under the compiler's lock, so freeing an image (off the
// audio thread: the program's reclaimer, a stale JIT image) waited for whatever compile was running (seconds for a
// big unit). The release now never waits: with a compile in progress it is deferred, and the compile releases it
// when it finishes (CacheCounters::releasesDeferred / codesReleased).
TEST_CASE("jit: freeing an image never waits for a compile in progress; its code is released when the compile ends") {
  struct Hold {
    std::mutex m;
    std::condition_variable cv;
    bool open = true, entered = false;
  };
  Hold hold;
  jit::CompilerConfig cfg;
  cfg.beforeCompile = [](void* user) {
    Hold& h = *static_cast<Hold*>(user);
    std::unique_lock lock(h.m);
    h.entered = true;
    h.cv.notify_all();
    h.cv.wait(lock, [&] { return h.open; });
  };
  cfg.beforeCompileUser = &hold;
  auto made = jit::Compiler::create(cfg);
  REQUIRE(made.ok());
  jit::Compiler& comp = *made.value();
  const Compiled a = compileText("patch A { in x : audio\n out y : audio\n y = tanh(x * 3) }");
  const Compiled b = compileText("patch B { in x : audio\n out y : audio\n y = sin(x * 5) + x }");
  auto imgA = comp.compile(a.sched);
  REQUIRE(imgA.ok());
  CHECK(comp.counters().compiles == 1);
  {
    std::lock_guard lock(hold.m);
    hold.open = false;
    hold.entered = false;
  }
  std::unique_ptr<eng::ProgramImage> imgB;
  std::thread compiling([&] {
    auto r = comp.compile(b.sched);  // held in beforeCompile, with the compiler's lock taken
    REQUIRE(r.ok());
    imgB = std::move(r.value());
  });
  {
    std::unique_lock lock(hold.m);
    REQUIRE(hold.cv.wait_for(lock, std::chrono::seconds(30), [&] { return hold.entered; }));
  }
  // Free A on another thread while B's compile holds the lock: it must return without waiting for B.
  std::atomic<bool> freed{false};
  std::thread release([&] {
    imgA.value().reset();
    freed = true;
  });
  const auto t0 = std::chrono::steady_clock::now();
  while (!freed && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool returned = freed.load();
  {
    std::lock_guard lock(hold.m);
    hold.open = true;  // let B finish either way (a blocked release would otherwise never return)
  }
  hold.cv.notify_all();
  release.join();
  compiling.join();
  CHECK_MESSAGE(returned, "freeing an image waited for the compile in progress");
  const jit::CacheCounters c = comp.counters();
  CHECK(c.releasesDeferred == 1);
  CHECK(c.codesReleased == 1);  // released by the compile that held the lock, when it ended
  CHECK(c.compiles == 2);
  // A's code is gone from the live code: compiling A again runs LLVM (no memory hit).
  REQUIRE(comp.compile(a.sched).ok());
  CHECK(comp.counters().compiles == 3);
  CHECK(comp.counters().memoryHits == 0);
  // B still runs.
  REQUIRE(imgB != nullptr);
  std::vector<float> x(16, 0.25f), y(16);
  const float* ins[1] = {x.data()};
  float* outs[1] = {y.data()};
  eng::ProcessContext ctx;
  ctx.frames = 16;
  ctx.in = ins;
  ctx.numIn = 1;
  ctx.out = outs;
  ctx.numOut = 1;
  imgB->process(ctx);
  CHECK(y[15] == doctest::Approx(std::sin(1.25) + 0.25).epsilon(1e-6));
}

// T1b.x-perf (QUESTIONS Q111 option C): CompileOptions::quickFirst gives a voice-groups image first when pairs would
// add code and the cache has no final image; that first image is not stored, is bit-identical to the interpreter (so
// to the final image), and a later compile without quickFirst gives (and stores) the final one. Once the final image
// is cached, quickFirst returns it at once.
TEST_CASE("jit: quickFirst compiles voice groups first, the final pair image later; a cached final image comes at once") {
  TempDir dir("quick");
  std::vector<std::string> skipped;
  const auto voice = compileFile(text_util::casesDir() / "fm_voice.pqp", 48000.0, skipped);
  REQUIRE(!voice.empty());
  const Compiled& fm = voice.front();
  REQUIRE(voicesFor(fm) > 0);
  jit::CompileOptions quick = optionsFor(fm);
  quick.quickFirst = true;
  {
    auto a = makeCompiler(cacheIn(dir.path));
    jit::CompileStats st;
    checkAgainstInterpreter(fm, *a, &st, quick);  // the first image plays exactly what the interpreter plays
    CHECK(st.refine);
    CHECK(st.pairedStages == 0);
    CHECK(a->counters().stores == 0);  // a first image is never stored
    jit::CompileStats fin;
    checkAgainstInterpreter(fm, *a, &fin, optionsFor(fm));
    CHECK_FALSE(fin.refine);
    CHECK(fin.pairedStages > 0);
    CHECK(a->counters().stores == 1);
    // A global patch has nothing to pair: quickFirst compiles its final image directly.
    const Compiled g = compileText("patch G { in x : audio\n out y : audio\n y = tanh(x * 3) }");
    jit::CompileOptions gq;
    gq.quickFirst = true;
    jit::CompileStats gs;
    REQUIRE(a->compile(g.sched, gq, &gs).ok());
    CHECK_FALSE(gs.refine);
  }
  auto b = makeCompiler(cacheIn(dir.path));
  jit::CompileStats st;
  checkAgainstInterpreter(fm, *b, &st, quick);
  CHECK_FALSE(st.refine);  // the final image, from the disk cache
  CHECK(st.pairedStages > 0);
  CHECK(st.source == jit::CompileStats::Source::Disk);
  CHECK(b->counters().compiles == 0);
}
