// pqdaw's JIT cache configuration and the AOT seed (pq/app/jit_cache.h).
#include "pq/app/jit_cache.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <vector>

#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/model.h"

namespace pq::app {

namespace fs = std::filesystem;

fs::path aotSeedDir() {
  wchar_t buf[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return {};
  return fs::path(buf).parent_path() / "jit-aot";
}

jit::CacheConfig appJitCache() {
  jit::CacheConfig c;
  if (const char* env = std::getenv("PQ_JIT_CACHE_DIR"); env && *env) {
    const std::string v = env;
    if (v != "off") c.dir = compile::pathFromUtf8(v);
  } else {
    c.dir = jit::defaultCacheDir();
  }
  if (fs::path aot = aotSeedDir(); !aot.empty()) c.readOnly.push_back(std::move(aot));
  return c;
}

void configureJitCache() { compile::setJitCache(appJitCache()); }

namespace {

using namespace pq::doc;

// As ops::createTrack makes them: a MIDI track starts with the default instrument (T4.1).
doc::TrackPtr newTrack(IdSource& ids, TrackKind kind, const char* name) {
  Track t = makeTrack(ids.track(), kind, name);
  if (kind == TrackKind::Midi) t.devices.push_back(makeDefaultInstrument(ids.device()));
  return std::make_shared<const Track>(std::move(t));
}

// The stock projects: a new project (master only) and new projects with default-instrument MIDI tracks, a group and a
// return — what the first minutes in a new project compile to. Every track is created as the UI creates it.
Result<std::vector<Snapshot>> stockProjects() {
  struct Shape {
    uint32_t midi = 0, returns = 0;
    bool group = false;          // one more MIDI track inside a group
    std::string_view effect;     // T4.1 (Q116): the MIDI tracks' chain is the default instrument + this stock effect
    std::string_view instrument; // T4.12: the MIDI tracks' instrument instead of the default one (e.g. Operator)
  };
  std::vector<Shape> shapes = {{0, 0, false, {}}, {1, 0, false, {}}, {2, 0, false, {}}, {4, 0, false, {}},
                               {8, 0, false, {}}, {1, 1, false, {}}, {2, 1, false, {}}, {1, 0, true, {}}};
  // Each stock audio effect as it is first added from the device menu: at its defaults after the default instrument.
  // Each other stock instrument (T4.12: Operator) as the first device of a new MIDI track, at its defaults.
  for (const StockDevice& d : stockDevices()) {
    if (d.patch == kDefaultInstrument) continue;
    if (d.kind == DeviceKind::Instrument) shapes.push_back({1, 0, false, {}, d.patch});
    else shapes.push_back({1, 0, false, d.patch, {}});
  }
  std::vector<Snapshot> out;
  for (const Shape& sh : shapes) {
    IdSource ids;
    Snapshot s = makeDefaultProject(ids);
    Batch b{"stock project", {}};
    for (uint32_t r = 0; r < sh.returns; ++r) b.commands.push_back(AddTrack{newTrack(ids, TrackKind::Return, "Return"), {}, kAppend});
    for (uint32_t k = 0; k < sh.midi; ++k) {
      TrackPtr t = newTrack(ids, TrackKind::Midi, "MIDI");
      if (!sh.instrument.empty()) {
        Track withInstrument = *t;
        withInstrument.devices.front() = makeDevice(ids.device(), std::string(sh.instrument), DeviceKind::Instrument);
        t = std::make_shared<const Track>(std::move(withInstrument));
      }
      if (!sh.effect.empty()) {
        Track withEffect = *t;
        withEffect.devices.push_back(makeDevice(ids.device(), std::string(sh.effect), DeviceKind::AudioEffect));
        t = std::make_shared<const Track>(std::move(withEffect));
      }
      b.commands.push_back(AddTrack{std::move(t), {}, kAppend});
    }
    if (sh.group) {
      TrackPtr g = newTrack(ids, TrackKind::Group, "Group");
      const TrackId gid = g->id;
      b.commands.push_back(AddTrack{g, {}, kAppend});
      b.commands.push_back(AddTrack{newTrack(ids, TrackKind::Midi, "MIDI"), gid, kAppend});
    }
    if (!b.commands.empty()) {
      ChangeSet changes;
      auto next = apply(s, Command{b}, changes);
      if (!next) return next.error();
      s = std::move(next.value());
    }
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace

Result<AotSeedStats> seedJitAot(const fs::path& dir, const AotSeedOptions& options) {
  const auto t0 = std::chrono::steady_clock::now();
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (!fs::is_directory(dir, ec)) return Error{ErrorCode::Io, "jit-aot: cannot create " + dir.string(), {}};
  for (const auto& e : fs::directory_iterator(dir, ec))  // entries of another build are of no use: replaced
    if (e.path().extension() == ".pqjit" || e.path().extension() == ".tmp") fs::remove(e.path(), ec);
  jit::CompilerConfig cc;
  cc.target = options.target;
  cc.cache.dir = dir;
  cc.cache.maxBytes = uint64_t{4} << 30;  // the seed is never evicted
  cc.cache.verify = options.verify;
  auto compiler = jit::Compiler::create(cc);
  if (!compiler) return compiler.error();
  auto projects = stockProjects();
  if (!projects) return projects.error();
  AotSeedStats st;
  for (double rate : options.sampleRates) {
    for (const Snapshot& s : *projects) {
      LinkConfig lc;
      lc.sampleRate = rate;
      lc.compileThread = false;
      lc.sleepAfterMs = 0.0f;
      lc.stdlib = options.stdlib;
      lc.jitBackend = compile::jitUnitBackend(compiler->get());
      auto link = ProjectLink::create(lc, s);
      if (!link) return link.error();
      if (!(*link)->waitForJit(options.timeoutSeconds))
        return Error{ErrorCode::Unknown, "jit-aot: the JIT images did not arrive in time", {}};
      const JitStats js = (*link)->jitStats();
      if (js.errors != 0) return Error{ErrorCode::Unknown, "jit-aot: a JIT compile failed", {}};
      st.images += js.built;
      ++st.projects;
    }
  }
  const jit::CacheCounters c = (*compiler)->counters();
  if (c.storeErrors != 0 || c.uncacheable != 0)
    return Error{ErrorCode::Io, "jit-aot: " + std::to_string(c.storeErrors) + " entries could not be written, " +
                                    std::to_string(c.uncacheable) + " units are not cacheable", {}};
  st.compiles = c.compiles;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.path().extension() == ".pqjit") {
      ++st.entries;
      st.bytes += fs::file_size(e.path(), ec);
    }
  st.projects /= static_cast<uint32_t>(std::max<size_t>(1, options.sampleRates.size()));
  st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return st;
}

int runJitAot(const std::string& dir) {
  auto st = seedJitAot(compile::pathFromUtf8(dir));
  if (!st) {
    std::fprintf(stderr, "pqdaw: %s\n", st.error().message.c_str());
    return 1;
  }
  std::printf("jit-aot: %u stock projects x %zu rates: %llu images, %llu compiles, %llu entries (%.1f MiB) in %s, %.1f s\n",
              st->projects, AotSeedOptions{}.sampleRates.size(), static_cast<unsigned long long>(st->images),
              static_cast<unsigned long long>(st->compiles), static_cast<unsigned long long>(st->entries),
              static_cast<double>(st->bytes) / (1024.0 * 1024.0), dir.c_str(), st->seconds);
  return 0;
}

}  // namespace pq::app
