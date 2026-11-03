// TX.all-free (SPEC §15 2026-11-03 "everything is free; the chain is just cables"): every fixture project of the test
// tree renders exactly as it did before the chain became cables. The references (tests/app/fixtures/all_free/
// renders.txt) were written by this test on the code before TX.all-free (PQ_UPDATE_GOLDENS=1): per project the 64-bit
// hash of its interpreted render and of its JIT render (4.5 s through the real ProjectLink: clips, automation, sends,
// groups), its peak, and the patchHash of every lowered track. After the change each project loads through the schema
// migration (its chain turned into blocks wired in series) and must give the same hashes: diff 0, interpreter == JIT,
// and the same IR (so every state survives the migration). tests/app/fixtures/all_free/* are schema-16 projects saved
// before TX.all-free that cover what the older fixtures do not: free devices and their wires, a switched-off free
// device passing through, a meta voice run, a group with chain links, racks, sends and returns, group tracks.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/document/lower.h"
#include "pq/document/serialize.h"
#include "pq/engine/wav_file.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

namespace fs = std::filesystem;

constexpr double kRate = 48000.0;
constexpr double kSeconds = 4.5;

bool updateGoldens() {
  const char* v = std::getenv("PQ_UPDATE_GOLDENS");
  return v != nullptr && *v != '\0' && *v != '0';
}

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

fs::path testRoot() { return fs::path(PQ_TEST_DATA_DIR); }
fs::path refsFile() { return testRoot() / "app" / "fixtures" / "all_free" / "renders.txt"; }

// Every project.pqd of the test tree, as paths relative to it ('/' separators), sorted.
std::vector<std::string> fixtureProjects() {
  std::vector<std::string> out;
  for (const auto& e : fs::recursive_directory_iterator(testRoot())) {
    if (!e.is_regular_file() || e.path().filename() != kProjectFileName) continue;
    out.push_back(fs::relative(e.path().parent_path(), testRoot()).generic_string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

uint64_t hashChannels(const golden::Channels& c) {
  uint64_t h = 1469598103934665603ull;
  for (const auto& ch : c)
    for (float v : ch) {
      uint32_t bits = 0;
      std::memcpy(&bits, &v, sizeof bits);
      for (int k = 0; k < 4; ++k) {
        h ^= (bits >> (8 * k)) & 0xFFu;
        h *= 1099511628211ull;
      }
    }
  return h;
}

float peakOf(const golden::Channels& c) {
  float p = 0.0f;
  for (const auto& ch : c)
    for (float v : ch) p = std::max(p, std::fabs(v));
  return p;
}

golden::Channels renderWith(const Snapshot& s, const std::string& dir, bool jit) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  c.projectDir = dir;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  if (jit) REQUIRE((*l)->waitForJit(240.0));
  engine::BufferSink sink;
  auto st = (*l)->renderOffline(0, static_cast<uint64_t>(std::llround(kSeconds * kRate)), sink);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  return sink.channels;
}

// The patchHash of every lowered track ("<track id>:<hash>"), in graph order.
std::string patchHashes(const Project& p) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  std::unordered_map<uint32_t, uint32_t> slots;
  lo.slotOf = [&](TrackId t) { return slots.emplace(t.value, static_cast<uint32_t>(slots.size())).first->second; };
  lo.clipSlotOf = lo.slotOf;
  lo.tuningTableOf = [](TuningId, const TuningDef&) { return 0u; };
  auto low = lower(p, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  std::ostringstream s;
  for (size_t i = 0; i < low->patches.size(); ++i) {
    if (i != 0) s << ',';
    s << low->trackIds[i].value << ':' << std::hex << low->patches[i]->hash << std::dec;
  }
  return s.str();
}

struct Ref {
  uint64_t interp = 0, jit = 0;
  float peak = 0.0f;
  std::string patches;
};

std::map<std::string, Ref> readRefs() {
  std::map<std::string, Ref> out;
  std::ifstream in(refsFile());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    std::string name;
    Ref r;
    s >> name >> std::hex >> r.interp >> r.jit >> std::dec >> r.peak >> r.patches;
    out[name] = r;
  }
  return out;
}

// The projects whose index is `part` modulo kParts (the work is split so the slow tier's shards run it in parallel; a
// JIT build per project dominates). With PQ_UPDATE_GOLDENS, part 0 renders everything and rewrites the references.
constexpr size_t kParts = 6;

void checkPart(size_t part) {
  const std::vector<std::string> projects = fixtureProjects();
  REQUIRE(projects.size() >= 30);
  if (updateGoldens() && part != 0) return;
  std::map<std::string, Ref> refs = updateGoldens() ? std::map<std::string, Ref>{} : readRefs();
  std::ostringstream written;
  written << "# TX.all-free migration references (tests/app/test_all_free.cpp): project, interpreter hash, JIT hash, "
             "peak, track patch hashes\n";
  size_t checked = 0;
  for (size_t i = 0; i < projects.size(); ++i) {
    const std::string& rel = projects[i];
    if (!updateGoldens() && i % kParts != part) {
      refs.erase(rel);
      continue;
    }
    CAPTURE(rel);
    const std::string dir = (testRoot() / rel).string();
    auto loaded = loadProject(dir);
    if (!loaded.ok()) {
      MESSAGE(rel << ": does not load (" << loaded.error().message << "): skipped");
      continue;
    }
    const golden::Channels a = renderWith(loaded->project, dir, false);
    const golden::Channels j = renderWith(loaded->project, dir, true);
    Ref r;
    r.interp = hashChannels(a);
    r.jit = hashChannels(j);
    r.peak = peakOf(a);
    r.patches = patchHashes(*loaded->project);
    if (r.patches.empty()) r.patches = "-";
    CHECK_MESSAGE(r.interp == r.jit, rel << ": interpreter and JIT renders differ");
    if (updateGoldens()) {
      written << rel << ' ' << std::hex << r.interp << ' ' << r.jit << std::dec << ' ' << r.peak << ' ' << r.patches << '\n';
      continue;
    }
    const auto it = refs.find(rel);
    REQUIRE_MESSAGE(it != refs.end(), rel << ": no reference (PQ_UPDATE_GOLDENS=1 on the code before TX.all-free)");
    CHECK_MESSAGE(r.interp == it->second.interp, rel << ": interpreted render changed (peak " << r.peak << ", was "
                                                     << it->second.peak << ")");
    CHECK_MESSAGE(r.jit == it->second.jit, rel << ": JIT render changed");
    CHECK_MESSAGE(r.patches == it->second.patches, rel << ": lowered IR changed");
    ++checked;
    refs.erase(it);
  }
  if (updateGoldens()) {
    std::ofstream(refsFile(), std::ios::binary) << written.str();
    MESSAGE("updated " << refsFile().string());
    return;
  }
  CHECK(checked >= projects.size() / kParts - 1);
  for (const auto& [name, r] : refs) FAIL_CHECK(name << ": referenced but no longer loads");
}

}  // namespace

#define PQ_ALL_FREE_PART(k)                                                                                               TEST_CASE("all-free: every fixture project renders as before (diff 0, interpreter == JIT, the same IR), part " #k *             doctest::test_suite("slow")) {                                                                                  checkPart(k);                                                                                                         }
PQ_ALL_FREE_PART(0)
PQ_ALL_FREE_PART(1)
PQ_ALL_FREE_PART(2)
PQ_ALL_FREE_PART(3)
PQ_ALL_FREE_PART(4)
PQ_ALL_FREE_PART(5)
