// OP.0 (docs/plans/OP.md): whole-engine SCENARIOS through the playback path the app runs (doc::lower → ProjectLink →
// Engine → Timeline + ProjectProgram, JIT images), one row per scenario, ns per engine block. The audit's baseline:
// each OP.n task re-runs the rows it claims to move.
//   idle/…      transport stopped (the engine's sleep gate on, as in the app, and off)
//   silent/…    playing, tracks without notes
//   synth/…     one MIDI track, N held voices of an instrument (default instrument, Operator, Wavetable Synth)
//   arp/…       one MIDI track, 16th-note chords (note-ons / offs every 16th: allocation, envelopes, releases)
//   scale/…     N tracks of one instrument, 4 held voices each (with synth/… 16 tracks: the cost per track against N)
//   fx/…        one MIDI track (default instrument, a held 3-note chord → Utility) + ONE module; cost = row − fx/none
//   viz/…       the same with an analyser; "watched" = its analysis tap has a reader (a visible face)
//   audio/…     8 audio tracks, one 60 s stereo clip each, per warp mode (plain and +300 cents)
//   mostly idle/64 tracks, 4 sounding   60 MIDI tracks without notes + 4 with a held chord
//   session/…   a mixed project: 12 MIDI tracks (3 instruments × 4, 4 voices each, EQ + Compressor), 4 Beats-warped
//               audio tracks with an EQ, a Limiter on the master
// Block size: the engine's image block (LinkConfig::blockFrames). The app runs 64 whatever the device buffer is (the
// BlockAdapter re-blocks), so "256" rows are what a larger engine block WOULD cost, not what the app does today.
// Every row prints "% of a core" = ns per block / the block's period at 48 kHz.
// Sampling profile (no admin rights, no external profiler): with PQ_BENCH_SAMPLE=<file> every scenario then runs 4 s
// more while a second thread suspends the audio thread ~4000 times a second and records its instruction pointer; the
// file gets, per scenario, the sample counts per address: "E <rva>" in this executable (resolve with the linker map,
// build/<preset>/bench/pq_bench_scenarios.map: tools/bench/symbolize_samples.py), "M <dll> <rva>" in another module,
// "J <address>" outside every module (JIT code). Inlined callees count for their caller.
// PQ_BENCH_SAVE=<dir>: every scenario that runs also saves its project to <dir>/<scenario name> (letters, digits and
// '-' only), to open in the app (`pqdaw --project <that folder>`; the audio scenarios' clips point at the WAV in the
// temp directory): the UI rows of docs/plans/OP.md.
// PQ_BENCH_CADENCE=1: every scenario also prints its cost AT A DEVICE'S CADENCE: 256 frames (four 64-frame blocks, or
// one larger block) every 5.33 ms with the thread asleep in between, as an audio thread runs — the core clocks down
// and the caches are shared with whatever ran meanwhile, so a block costs more than in the back-to-back rows above.
// Ship build. Usage: pq_bench_scenarios [runner options] (--filter REGEX, --list, --csv FILE, --no-pin)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/jit/compiler.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/time.h"
#include "pq/core/tap_ring.h"
#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/engine/wav_file.h"
#include "pq_bench.h"

// The stage / unit split (docs/plans/OP.md "Method"): only with the audit's instrumentation header in the tree (rdtsc
// scopes in Engine, Timeline and ProjectProgram; not on main), and PQ_BENCH_STAGES=1.
#if __has_include("pq/engine/op_profile.h")
#include "pq/engine/op_profile.h"
#define PQ_BENCH_HAS_STAGES 1
#endif

#include <windows.h>

using namespace pq;
using namespace pq::doc;

namespace {

constexpr double kRate = 48000.0;

std::string audioFile() {
  static const std::string path = [] {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "pq_bench_scenarios_60s.wav";
    if (!std::filesystem::exists(p)) {
      const size_t n = static_cast<size_t>(60 * kRate);
      std::vector<std::vector<float>> ch(2, std::vector<float>(n));
      // A beat-like signal (a decaying 110 Hz burst + noise every half second over a 220 Hz tone): transients for the
      // Beats / Complex modes to find, broadband content for the phase vocoders.
      uint32_t rng = 12345;
      for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double beat = std::fmod(t, 0.5);
        rng = rng * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(rng >> 8) / 8388608.0 - 1.0) * std::exp(-beat * 40.0);
        const double kick = std::sin(6.283185307179586 * 110.0 * beat) * std::exp(-beat * 12.0);
        const double tone = 0.1 * std::sin(6.283185307179586 * 220.0 * t);
        ch[0][i] = static_cast<float>(0.3 * kick + 0.2 * noise + tone);
        ch[1][i] = static_cast<float>(0.3 * kick - 0.2 * noise + tone);
      }
      if (!engine::writeWav(p, ch, static_cast<uint32_t>(kRate)).ok()) std::exit(4);
    }
    const std::u8string u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
  }();
  return path;
}

struct Proj {
  DocumentStore store;
  IdSource& ids = store.ids();
  SampleRef sample;
  bool haveSample = false;
  std::vector<DeviceId> watch;  // analysers whose taps get a reader

  ops::Edit run(const ops::EditResult& e) {
    if (!e.ok()) {
      std::fprintf(stderr, "edit: %s\n", e.error().message.c_str());
      std::exit(5);
    }
    if (!store.dispatch(e->command).ok()) std::exit(5);
    return *e;
  }
  // A MIDI track: `instrument` ("" = the default instrument) with `voices` held notes (arp: 16th-note chords instead).
  TrackId midi(const char* instrument, uint32_t voices, bool arp = false) {
    const TrackId id = run(ops::createTrack(*store.current(), ids, TrackKind::Midi)).tracks.at(0);
    if (instrument[0] != '\0') (void)run(ops::addDevice(*store.current(), ids, id, instrument, DeviceKind::Instrument));
    if (voices == 0) return id;
    NoteClip c;
    c.id = ids.clip();
    c.start = 0;
    c.length = 1000 * kPpq;
    std::vector<Note> notes;
    const Ticks step = kPpq / 4;
    const uint32_t reps = arp ? 4000 : 1;
    for (uint32_t r = 0; r < reps; ++r)
      for (uint32_t v = 0; v < voices; ++v) {
        Note n;
        n.id = ids.note();
        n.start = arp ? static_cast<Ticks>(r) * step : 0;
        n.length = arp ? step : 1000 * kPpq;
        n.pitch = tuning::PitchId::fromDegree(static_cast<int32_t>((v * 5 + (arp ? r % 7 : 0)) % 36) - 12, 0);
        n.velocity = 90.0f;
        notes.push_back(n);
      }
    c.notes = NoteSeq::fromNotes(std::move(notes)).value();
    if (!store.dispatch(AddClip{id, std::make_shared<const NoteClip>(std::move(c))}).ok()) std::exit(6);
    return id;
  }
  DeviceId fx(TrackId track, const char* module, bool watched = false) {
    const DeviceId d = run(ops::addDevice(*store.current(), ids, track, module, DeviceKind::AudioEffect)).devices.at(0);
    if (watched) {
      watch.push_back(d);
      for (const Device* m : groupMembersDeep(findTrackPtr(*store.current(), track)->devices, d)) watch.push_back(m->id);
    }
    return d;
  }
  TrackId audio(AudioWarp warp, double cents) {
    if (!haveSample) {
      sample.id = ids.sample();
      sample.path = audioFile();
      sample.frames = static_cast<int64_t>(60 * kRate);
      sample.rate = kRate;
      sample.channels = 2;
      sample.displayName = "bench";
    }
    const TrackId id = run(ops::createTrack(*store.current(), ids, TrackKind::Audio)).tracks.at(0);
    const std::vector<ops::NewAudioClip> clips = {{id, 0, sample.id, {}}};
    const ops::Edit add = run(ops::addAudioClips(*store.current(), ids,
                                            haveSample ? std::span<const SampleRef>() : std::span<const SampleRef>(&sample, 1), clips));
    haveSample = true;
    if (warp == AudioWarp::RePitch) (void)run(ops::setAudioClipWarp(*store.current(), add.clips, warp, 100.0));
    else if (warp != AudioWarp::Off) (void)run(ops::setWarpMode(*store.current(), add.clips, warp));
    if (cents != 0.0)
      (void)run(ops::setAudioClipTranspose(*store.current(), add.clips, AudioTranspose{TransposeUnit::Cents, cents, 0, {1}}));
    return id;
  }
};

struct Rig {
  std::unique_ptr<app::ProjectLink> link;
  uint32_t block;
  std::vector<float> l, r;
  int64_t time = 0;
  std::vector<std::shared_ptr<TapRing>> taps;
  // sleepMs: the engine's idle gate (the app: 250); play: start the transport; baseline: the units compiled for
  // x86-64-v3 (what the AOT seed next to pqdaw.exe holds for the stock units: jit::Target::Baseline) instead of the host.
  Rig(Proj& p, uint32_t blockFrames, bool play = true, float sleepMs = 0.0f, bool baseline = false)
      : block(blockFrames), l(blockFrames), r(blockFrames) {
    app::LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = sleepMs;
    c.blockFrames = blockFrames;
    c.maxVoices = 64;
    c.jit = true;
    if (baseline) {
      static const std::unique_ptr<jit::Compiler> compiler = [] {
        jit::CompilerConfig cc;
        cc.target = jit::Target::Baseline;
        auto made = jit::Compiler::create(cc);
        if (!made.ok()) std::exit(8);
        return std::move(made.value());
      }();
      c.jitBackend = compile::jitUnitBackend(compiler.get(), true);
    }
    auto made = app::ProjectLink::create(c, p.store.current());
    if (!made.ok()) {
      std::fprintf(stderr, "%s\n", made.error().message.c_str());
      std::exit(3);
    }
    link = std::move(made.value());
    (void)link->waitForSamples();
    (void)link->waitForMetaTables();
    link->setLoop(views::TransportLoop{0, 60 * kPpq, true});  // 30 s at 120 BPM: clips and notes keep playing
    link->play(0);  // the JIT publishes need blocks; a stopped scenario stops afterwards
    for (int i = 0; i < 400000; ++i) {
      const app::JitStats js = link->jitStats();
      if (js.published + js.errors + js.stale >= js.wanted && js.refined + js.refineStale + js.refineErrors >= js.firstImages) break;
      run();
    }
    link->collect();
    const app::JitStats js = link->jitStats();
    if (js.errors != 0) std::fprintf(stderr, "  (%llu JIT errors: those units are interpreted)\n", static_cast<unsigned long long>(js.errors));
    for (const DeviceId d : p.watch)
      if (auto t = link->deviceAnalysisTap(d)) {
        t->addReader();
        taps.push_back(std::move(t));
      }
    if (!p.watch.empty() && taps.empty()) std::fprintf(stderr, "  (no analysis tap found: the row is unwatched)\n");
    if (!play) link->stop();
    for (uint32_t i = 0; i < 48000 / blockFrames; ++i) run();  // 1 s: attacks, smoothers, the sleep gate
  }
  ~Rig() {
    for (auto& t : taps) t->removeReader();
  }
  void run() {
    float* ch[2] = {l.data(), r.data()};
    link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, block, time});
    time += block;
  }
  float peak() const {
    float m = 0.0f;
    for (uint32_t i = 0; i < block; ++i) m = std::max({m, std::fabs(l[i]), std::fabs(r[i])});
    return m;
  }
};

// The sampling profiler (see the file comment).
class Sampler {
public:
  explicit Sampler(size_t capacity = 1u << 16) {
    ips_.reserve(capacity);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target_, 0, FALSE, DUPLICATE_SAME_ACCESS);
    thread_ = std::thread([this] { loop(); });
  }
  ~Sampler() { stop(); }
  void stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    thread_.join();
    CloseHandle(target_);
  }
  // Appends "# name" and the counts per address to `path`.
  void write(const char* path, const std::string& name) {
    stop();
    std::map<std::string, uint64_t> counts;
    HMODULE exe = GetModuleHandleW(nullptr);
    for (const uint64_t ip : ips_) {
      MEMORY_BASIC_INFORMATION mbi{};
      char buf[600];
      char mod[MAX_PATH] = {};
      if (VirtualQuery(reinterpret_cast<void*>(ip), &mbi, sizeof mbi) != 0 && mbi.Type == MEM_IMAGE &&
          GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), mod, MAX_PATH) != 0) {
        const uint64_t rva = ip - reinterpret_cast<uint64_t>(mbi.AllocationBase);
        if (mbi.AllocationBase == static_cast<void*>(exe)) {
          std::snprintf(buf, sizeof buf, "E %llx", static_cast<unsigned long long>(rva));
        } else {
          const char* base = std::strrchr(mod, '\\');
          std::snprintf(buf, sizeof buf, "M %s %llx", base != nullptr ? base + 1 : mod, static_cast<unsigned long long>(rva));
        }
      } else {
        std::snprintf(buf, sizeof buf, "J %llx", static_cast<unsigned long long>(ip));
      }
      ++counts[buf];
    }
    if (FILE* f = std::fopen(path, "a")) {
      std::fprintf(f, "# %s\t%zu\n", name.c_str(), ips_.size());
      for (const auto& [k, n] : counts) std::fprintf(f, "%s\t%llu\n", k.c_str(), static_cast<unsigned long long>(n));
      std::fclose(f);
    }
  }

private:
  void loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    const int64_t step = f.QuadPart / 4000;
    QueryPerformanceCounter(&t);
    int64_t next = t.QuadPart + step;
    uint32_t rng = 1;
    CONTEXT ctx;
    while (!stop_.load(std::memory_order_relaxed) && ips_.size() < ips_.capacity()) {
      do {
        YieldProcessor();
        QueryPerformanceCounter(&t);
      } while (t.QuadPart < next);
      rng = rng * 1664525u + 1013904223u;  // a jittered period: no beating with the block rate
      next = t.QuadPart + step / 2 + static_cast<int64_t>((rng >> 8) % static_cast<uint32_t>(step));
      if (SuspendThread(target_) == static_cast<DWORD>(-1)) continue;
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(target_, &ctx) != 0) ips_.push_back(ctx.Rip);
      ResumeThread(target_);
    }
  }
  HANDLE target_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::vector<uint64_t> ips_;  // reserved: the sampler never allocates while the target is suspended
};

template <class Build>
void scenario(bench::Runner& runner, std::string name, Build build, uint32_t blockFrames = 64, bool play = true, float sleepMs = 0.0f,
              bool baseline = false) {
  if (blockFrames != 64) name += " @" + std::to_string(blockFrames);
  if (baseline) name += " (x86-64-v3 code)";
  runner.add(name, [=](bench::Context& c) {
    Proj p;
    build(p);
    if (const char* dir = std::getenv("PQ_BENCH_SAVE"); dir != nullptr && *dir != '\0') {
      std::string leaf;
      for (const char ch : name) leaf += (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ? ch : '-';
      const std::string to = std::string(dir) + "/" + leaf;
      std::error_code ec;
      std::filesystem::create_directories(to, ec);
      if (!saveProject(to, p.store).ok()) std::fprintf(stderr, "  (could not save %s)\n", to.c_str());
    }
    Rig rig(p, blockFrames, play, sleepMs, baseline);
    if (!play && sleepMs > 0.0f) {  // how long the stopped project takes to fall asleep (tails, releases), up to 2 min
      double seconds = 1.0;  // the rig's own second after the stop
      for (; !rig.link->engine().asleep() && seconds < 120.0; seconds += 0.25)
        for (uint32_t i = 0; i < 12000 / blockFrames; ++i) rig.run();
      std::printf("  %s: %s %.2f s after the stop\n", name.c_str(), rig.link->engine().asleep() ? "asleep" : "STILL AWAKE", seconds);
    }
    float peak = 0.0f;
    const bench::Stats s = c.measure([&] {
      rig.run();
      peak = std::max(peak, rig.peak());
    }, 1, "ns/block");
    if (const char* out = std::getenv("PQ_BENCH_SAMPLE"); out != nullptr && *out != '\0') {
      Sampler sampler;
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
      while (std::chrono::steady_clock::now() < until)
        for (int i = 0; i < 64; ++i) rig.run();
      sampler.write(out, name);
    }
    if (const char* cad = std::getenv("PQ_BENCH_CADENCE"); cad != nullptr && *cad != '\0') {
      const uint32_t perWake = std::max(1u, 256 / blockFrames);
      const double nsPerTick = 1.0 / bench::tscTicksPerNs();
      std::vector<double> ns;
      HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
      LARGE_INTEGER due;
      due.QuadPart = -53333;  // 5.3333 ms in 100 ns units
      for (int wake = 0; wake < 600; ++wake) {  // 3.2 s
        if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
        else Sleep(5);
        const uint64_t t0 = bench::rdtsc();
        for (uint32_t i = 0; i < perWake; ++i) rig.run();
        if (wake >= 100) ns.push_back(static_cast<double>(bench::rdtsc() - t0) * nsPerTick / (perWake * blockFrames));
      }
      if (timer != nullptr) CloseHandle(timer);
      const bench::Stats cs = bench::summarize(std::move(ns));
      std::printf("  %-44s %8.3f ns/frame  %6.2f %% of a core  AT DEVICE CADENCE (p99 %.3f ns/frame)\n", name.c_str(), cs.medianNs,
                  100.0 * cs.medianNs * kRate / 1e9, cs.p99Ns);
    }
#ifdef PQ_BENCH_HAS_STAGES
    if (const char* st = std::getenv("PQ_BENCH_STAGES"); st != nullptr && *st != '\0') {
      namespace op = engine::opprof;
      op::reset();
      const uint32_t blocks = 4000;
      for (uint32_t i = 0; i < blocks; ++i) rig.run();
      const op::Table& t = op::table();
      const double nsPerFrame = 1.0 / (bench::tscTicksPerNs() * blocks * blockFrames);
      std::printf("  STAGES %s\n", name.c_str());
      for (uint32_t k = 0; k < op::kCount; ++k)
        std::printf("    %-44s %9.3f ns/frame\n", op::kStageNames[k], static_cast<double>(t.stage[k]) * nsPerFrame);
      std::vector<const op::UnitRow*> rows;
      for (uint32_t i = 0; i < t.numUnits; ++i)
        if (t.units[i].runs != 0) rows.push_back(&t.units[i]);
      std::sort(rows.begin(), rows.end(), [](const op::UnitRow* x, const op::UnitRow* y) { return x->cycles > y->cycles; });
      for (size_t i = 0; i < rows.size() && i < 12; ++i)
        std::printf("    unit %-50s %9.3f ns/frame  (%s, idle %3.0f %%)\n", rows[i]->name, static_cast<double>(rows[i]->cycles) * nsPerFrame,
                    rows[i]->voices ? "voices" : "global", 100.0 * static_cast<double>(rows[i]->idle) / static_cast<double>(rows[i]->runs));
      std::printf("    (%zu units ran)\n", rows.size());
    }
#endif
    const double period = 1e9 * blockFrames / kRate;
    std::printf("  %-44s %8.3f ns/frame  %6.2f %% of a core  (out peak %.3f)\n", name.c_str(), s.medianNs / blockFrames,
                100.0 * s.medianNs / period, static_cast<double>(peak));
  });
}

void session(Proj& p) {
  const char* inst[3] = {"", "module:operator", "module:wavetable_synth"};
  for (uint32_t t = 0; t < 12; ++t) {
    const TrackId id = p.midi(inst[t % 3], 4, t % 4 == 3);
    (void)p.fx(id, "module:eq");
    (void)p.fx(id, "module:compressor");
  }
  for (uint32_t t = 0; t < 4; ++t) (void)p.fx(p.audio(AudioWarp::Beats, 0.0), "module:eq");
  (void)p.fx(p.store.current()->master->id, "module:limiter");
}

}  // namespace

int main(int argc, char** argv) {
  DenormalGuard dg;  // FTZ / DAZ, as every audio thread sets them (Engine::process expects it)
  bench::Runner runner("pq_bench_scenarios");
  // ---- idle ----
  scenario(runner, "idle/empty project stopped (sleep gate)", [](Proj&) {}, 64, false, 250.0f);
  scenario(runner, "idle/empty project stopped (no gate)", [](Proj&) {}, 64, false, 0.0f);
  scenario(runner, "idle/session stopped (sleep gate)", session, 64, false, 250.0f);
  scenario(runner, "idle/session stopped (no gate)", session, 64, false, 0.0f);
  scenario(runner, "idle/empty project playing", [](Proj&) {});
  // How long a stopped project keeps running (the tails: releases, followers, delay lines) before the engine sleeps,
  // per instrument and per effect (printed; the row is the cost once asleep).
  for (const char* inst : {"", "module:operator", "module:wavetable_synth"})
    scenario(runner, std::string("idle/stopped after ") + (inst[0] == '\0' ? "default" : inst + 7) + " (sleep gate)",
             [=](Proj& p) { (void)p.midi(inst, 4); }, 64, false, 250.0f);
  for (const char* m : {"module:utility", "module:eq", "module:compressor", "module:limiter", "module:saturator", "module:delay_tap"})
    scenario(runner, std::string("idle/stopped after default + ") + (m + 7) + " (sleep gate)", [=](Proj& p) {
      (void)p.fx(p.midi("", 4), m);
    }, 64, false, 250.0f);
  scenario(runner, "idle/stopped after 4 beats clips (sleep gate)", [](Proj& p) {
    for (int t = 0; t < 4; ++t) (void)p.audio(AudioWarp::Beats, 0.0);
  }, 64, false, 250.0f);
  // ---- silent tracks ----
  for (const uint32_t n : {16u, 64u})
    scenario(runner, "silent/" + std::to_string(n) + " midi tracks playing", [n](Proj& p) {
      for (uint32_t t = 0; t < n; ++t) (void)p.midi("", 0);
    });
  scenario(runner, "silent/16 audio tracks no clips playing", [](Proj& p) {
    for (uint32_t t = 0; t < 16; ++t) (void)p.run(ops::createTrack(*p.store.current(), p.ids, TrackKind::Audio));
  });
  // ---- synths ----
  for (const char* inst : {"", "module:operator", "module:wavetable_synth"})
    for (const uint32_t v : {1u, 8u, 16u, 64u})
      for (const uint32_t bf : {64u, 256u}) {
        if (bf == 256 && v != 16) continue;
        const std::string iname = inst[0] == '\0' ? "default" : std::string(inst).substr(7);
        scenario(runner, "synth/" + iname + " " + std::to_string(v) + " voices", [=](Proj& p) { (void)p.midi(inst, v); }, bf);
      }
  for (const char* inst : {"", "module:operator"}) {
    const std::string iname = inst[0] == '\0' ? "default" : std::string(inst).substr(7);
    scenario(runner, "arp/" + iname + " 4-note 16ths", [=](Proj& p) { (void)p.midi(inst, 4, true); });
  }
  // The AOT seed's code (x86-64-v3: voice pairs on two 256-bit halves) against the host's (AVX-512 here: zmm pairs).
  for (const uint32_t v : {8u, 16u, 64u})
    scenario(runner, "synth/default " + std::to_string(v) + " voices", [=](Proj& p) { (void)p.midi("", v); }, 64, true, 0.0f, true);
  scenario(runner, "synth/operator 64 voices", [](Proj& p) { (void)p.midi("module:operator", 64); }, 64, true, 0.0f, true);
  for (const uint32_t bf : {64u, 32u, 128u, 256u}) {
    scenario(runner, "synth/default 16 tracks x 4 voices", [](Proj& p) {
      for (uint32_t t = 0; t < 16; ++t) (void)p.midi("", 4);
    }, bf);
    scenario(runner, "synth/operator 16 tracks x 4 voices", [](Proj& p) {
      for (uint32_t t = 0; t < 16; ++t) (void)p.midi("module:operator", 4);
    }, bf);
  }
  // How the cost per track grows with the number of tracks (the units' code and buffers against the caches).
  for (const char* inst : {"", "module:operator"})
    for (const uint32_t n : {1u, 4u, 8u, 32u}) {
      const std::string iname = inst[0] == '\0' ? "default" : std::string(inst).substr(7);
      scenario(runner, "scale/" + iname + " " + std::to_string(n) + " tracks x 4 voices", [=](Proj& p) {
        for (uint32_t t = 0; t < n; ++t) (void)p.midi(inst, 4);
      });
    }
  // ---- effects ----
  auto fxBase = [](Proj& p) {
    const TrackId id = p.midi("", 3);
    (void)p.fx(id, "module:utility");
    return id;
  };
  for (const uint32_t bf : {64u, 256u})
    scenario(runner, "fx/none", [=](Proj& p) { (void)fxBase(p); }, bf);
  for (const char* m : {"module:eq", "module:compressor", "module:limiter", "module:saturator", "module:delay_tap", "module:math",
                        "module:lfo", "module:vca", "module:noise"})
    for (const uint32_t bf : {64u, 256u}) {
      if (bf == 256 && std::string(m) != "module:eq" && std::string(m) != "module:compressor") continue;
      scenario(runner, "fx/" + std::string(m).substr(7), [=](Proj& p) { (void)p.fx(fxBase(p), m); }, bf);
    }
  scenario(runner, "fx/eq x8 in series", [=](Proj& p) {
    const TrackId id = fxBase(p);
    for (int i = 0; i < 8; ++i) (void)p.fx(id, "module:eq");
  });
  for (const char* m : {"module:oscilloscope", "module:spectrum", "module:spectrogram", "module:level_meter", "module:lufs_meter",
                        "module:loudness_history", "module:stereo_field", "module:stereo_image", "module:phase_scope",
                        "module:band_levels", "module:waveform", "module:waterfall", "module:stereo_spectrogram"})
    for (const bool watched : {false, true})
      scenario(runner, "viz/" + std::string(m).substr(7) + (watched ? " watched" : " unwatched"),
               [=](Proj& p) { (void)p.fx(fxBase(p), m, watched); });
  // What a busy device view shows: a 4-note arp through five analysers at once.
  for (const bool watched : {false, true})
    scenario(runner, std::string("viz/5 analysers on an arp") + (watched ? " watched" : " unwatched"), [=](Proj& p) {
      const TrackId id = p.midi("", 4, true);
      for (const char* m : {"module:spectrum", "module:oscilloscope", "module:waterfall", "module:spectrogram", "module:level_meter"})
        (void)p.fx(id, m, watched);
    });
  // ---- audio clips ----
  struct Warp {
    const char* name;
    AudioWarp warp;
  };
  for (const Warp w : {Warp{"off", AudioWarp::Off}, Warp{"repitch", AudioWarp::RePitch}, Warp{"beats", AudioWarp::Beats},
                       Warp{"tones", AudioWarp::Tones}, Warp{"texture", AudioWarp::Texture}, Warp{"complex", AudioWarp::Complex},
                       Warp{"complex pro", AudioWarp::ComplexPro}})
    for (const double cents : {0.0, 300.0}) {
      if (w.warp == AudioWarp::RePitch && cents != 0.0) continue;
      scenario(runner, std::string("audio/8 clips ") + w.name + (cents != 0.0 ? " +300c" : ""), [=](Proj& p) {
        for (int t = 0; t < 8; ++t) (void)p.audio(w.warp, cents);
      });
    }
  // ---- many tracks, few sounding ----
  for (const uint32_t bf : {64u, 256u})
    scenario(runner, "mostly idle/64 tracks, 4 sounding", [](Proj& p) {
      for (uint32_t t = 0; t < 64; ++t) (void)p.midi("", t % 16 == 0 ? 4 : 0);
    }, bf);
  scenario(runner, "mostly idle/4 tracks sounding (reference)", [](Proj& p) {
    for (uint32_t t = 0; t < 4; ++t) (void)p.midi("", 4);
  });
  // ---- a mixed session ----
  for (const uint32_t bf : {64u, 32u, 128u, 256u}) scenario(runner, "session/12 midi + 4 audio + master limiter", session, bf);
  scenario(runner, "session/12 midi + 4 audio + master limiter", session, 64, true, 0.0f, true);
  return runner.main(argc, argv);
}
