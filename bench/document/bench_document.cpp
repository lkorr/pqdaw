// Document model costs (T3.4 perf criteria, release):
//   project of 100 MIDI tracks (in 10 groups) / 10,000 clips / 1,000,000 notes
//   - move one note through DocumentStore::dispatch: < 20 µs and < 16 KB retained per edit
//   - undo / redo: < 1 µs
//   - 10,000 one-note edits (all kept in history): total memory < 200 MB
//
// Memory is measured with a byte-counting global operator new/delete in this executable (bench executables do not
// link the allocation trap): "live" = bytes currently allocated through operator new. The process's private bytes
// (GetProcessMemoryInfo) are printed as well.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <random>
#include <vector>

#include <windows.h>
// psapi.h needs windows.h first.
#include <psapi.h>

#include "pq/document/commands.h"
#include "pq/document/store.h"
#include "pq_bench.h"

// ---- Byte-counting allocator ---------------------------------------------------------------------------------------

namespace {
long long gLiveBytes = 0;  // single-threaded bench
long long gAllocs = 0;

void* countedAlloc(std::size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  gLiveBytes += static_cast<long long>(_msize(p));
  ++gAllocs;
  return p;
}
void countedFree(void* p) noexcept {
  if (!p) return;
  gLiveBytes -= static_cast<long long>(_msize(p));
  std::free(p);
}
void* countedAlignedAlloc(std::size_t n, std::size_t a) {
  void* p = _aligned_malloc(n ? n : 1, a);
  if (!p) throw std::bad_alloc();
  gLiveBytes += static_cast<long long>(_aligned_msize(p, a, 0));
  ++gAllocs;
  return p;
}
void countedAlignedFree(void* p, std::size_t a) noexcept {
  if (!p) return;
  gLiveBytes -= static_cast<long long>(_aligned_msize(p, a, 0));
  _aligned_free(p);
}
}  // namespace

void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return countedAlloc(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return countedAlloc(n);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { countedFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { countedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { countedFree(p); }
void* operator new(std::size_t n, std::align_val_t a) { return countedAlignedAlloc(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return countedAlignedAlloc(n, static_cast<std::size_t>(a)); }
void operator delete(void* p, std::align_val_t a) noexcept { countedAlignedFree(p, static_cast<std::size_t>(a)); }
void operator delete[](void* p, std::align_val_t a) noexcept { countedAlignedFree(p, static_cast<std::size_t>(a)); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept {
  countedAlignedFree(p, static_cast<std::size_t>(a));
}
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept {
  countedAlignedFree(p, static_cast<std::size_t>(a));
}

// ---- Bench ---------------------------------------------------------------------------------------------------------

using namespace pq::doc;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kGroups = 10;
constexpr int kTracksPerGroup = 10;
constexpr int kClipsPerTrack = 100;
constexpr int kNotesPerClip = 100;
constexpr int kEdits = 10000;

double mib(long long bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

double privateMib() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
  return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

DWORD pageFaults() {
  PROCESS_MEMORY_COUNTERS pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
  return pmc.PageFaultCount;
}

struct ClipAddr {
  TrackId track;
  ClipId clip;
};

ClipPtr buildClip(IdSource& ids, std::mt19937& rng, pq::Ticks start, int notes) {
  NoteClip c;
  c.id = ids.clip();
  c.name = "clip";
  c.start = start;
  c.length = 2 * pq::kTicksPerWhole;
  std::vector<Note> v;
  v.reserve(static_cast<size_t>(notes));
  const pq::Ticks step = c.length / notes;
  for (int i = 0; i < notes; ++i) {
    Note n;
    n.id = ids.note();
    n.start = i * step;
    n.length = step;
    n.pitch = pq::tuning::PitchId::fromDegree(static_cast<int32_t>(rng() % 31), 0);
    v.push_back(n);
  }
  c.notes = NoteSeq::fromNotes(std::move(v)).value();
  return std::make_shared<const NoteClip>(std::move(c));
}

Snapshot buildProject(IdSource& ids, std::vector<ClipAddr>& clips) {
  std::mt19937 rng(7);
  auto p = std::make_shared<Project>(*makeDefaultProject(ids));
  for (int g = 0; g < kGroups; ++g) {
    Track group = makeTrack(ids.track(), TrackKind::Group, "Group");
    for (int t = 0; t < kTracksPerGroup; ++t) {
      Track tr = makeTrack(ids.track(), TrackKind::Midi, "Track");
      for (int c = 0; c < kClipsPerTrack; ++c) {
        ClipPtr clip = buildClip(ids, rng, c * 2 * pq::kTicksPerWhole, kNotesPerClip);
        clips.push_back({tr.id, clip->id});
        tr.clips.push_back({clip->id, std::move(clip)});
      }
      group.children.push_back(std::make_shared<const Track>(std::move(tr)));
    }
    p->tracks.push_back(std::make_shared<const Track>(std::move(group)));
  }
  return p;
}

const NoteClip& clipOf(const Project& p, const ClipAddr& a) {
  const Track& t = *findTrackPtr(p, a.track);
  return *t.clips[static_cast<size_t>(findClipIndex(t, a.clip))].clip;
}

void printRow(const char* name, double v, const char* unit) { std::printf("BENCH %-48s %12.3f %s\n", name, v, unit); }

}  // namespace

int main() {
  std::printf("document bench: %d tracks in %d groups, %d clips, %d notes\n", kGroups * kTracksPerGroup, kGroups,
              kGroups * kTracksPerGroup * kClipsPerTrack, kGroups * kTracksPerGroup * kClipsPerTrack * kNotesPerClip);
  const long long live0 = gLiveBytes;
  std::vector<ClipAddr> clips;
  clips.reserve(static_cast<size_t>(kGroups * kTracksPerGroup * kClipsPerTrack));
  IdSource buildIds;
  const auto tb0 = Clock::now();
  Snapshot project = buildProject(buildIds, clips);
  const double buildMs = std::chrono::duration<double, std::milli>(Clock::now() - tb0).count();
  const long long clipsVecBytes = static_cast<long long>(clips.capacity() * sizeof(ClipAddr));
  const long long projectBytes = gLiveBytes - live0 - clipsVecBytes;
  printRow("build project (1M notes)", buildMs, "ms");
  printRow("project heap", mib(projectBytes), "MiB");

  DocumentStore store(project);
  project.reset();
  std::mt19937 rng(99);

  // ---- 10,000 one-note moves, each a separate history entry. ----
  std::vector<double> editNs;
  std::vector<long long> editBytes;
  std::vector<DWORD> editFaults;
  editFaults.reserve(kEdits);
  editNs.reserve(kEdits);
  editBytes.reserve(kEdits);
  const long long liveBeforeEdits = gLiveBytes;
  for (int i = 0; i < kEdits; ++i) {
    const ClipAddr& a = clips[rng() % clips.size()];
    const NoteClip& c = clipOf(*store.current(), a);
    const auto& chunks = c.notes.chunks();
    const auto& ch = chunks[rng() % chunks.size()];
    const Note& n = ch.notes[rng() % ch.count];
    const Command cmd = MoveNote{{a.track, a.clip}, keyOf(n), n.start + 1 + static_cast<pq::Ticks>(rng() % 500),
                                 pq::tuning::PitchId::fromDegree(n.pitch.degree() + 1, 0)};
    const long long b0 = gLiveBytes;
    const DWORD f0 = pageFaults();
    const auto t0 = Clock::now();
    const pq::Status st = store.dispatch(cmd);
    const auto t1 = Clock::now();
    editFaults.push_back(pageFaults() - f0);
    editBytes.push_back(gLiveBytes - b0);
    if (!st.ok()) {
      std::printf("edit failed: %s\n", st.error().message.c_str());
      return 1;
    }
    editNs.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  const long long editsTotal = gLiveBytes - liveBeforeEdits;
  std::vector<double> sortedNs = editNs;
  std::sort(sortedNs.begin(), sortedNs.end());
  std::vector<long long> sortedBytes = editBytes;
  std::sort(sortedBytes.begin(), sortedBytes.end());
  double meanNs = 0;
  for (double v : editNs) meanNs += v;
  meanNs /= static_cast<double>(editNs.size());
  printRow("move one note: median", sortedNs[sortedNs.size() / 2] / 1000.0, "us");
  printRow("move one note: mean", meanNs / 1000.0, "us");
  printRow("move one note: p90", sortedNs[sortedNs.size() * 90 / 100] / 1000.0, "us");
  printRow("move one note: p95", sortedNs[sortedNs.size() * 95 / 100] / 1000.0, "us");
  printRow("move one note: p99", sortedNs[sortedNs.size() * 99 / 100] / 1000.0, "us");
  {
    size_t over = 0;
    for (double v : editNs) over += v >= 20000.0;
    printRow("move one note: edits >= 20 us", static_cast<double>(over), "of 10000");
    // Split by whether the OS had to fault in fresh pages for the heap during the edit (first touch of newly
    // committed memory as the history grows), to separate the model's cost from the OS's.
    std::vector<double> noFault, withFault;
    for (size_t i = 0; i < editNs.size(); ++i) (editFaults[i] ? withFault : noFault).push_back(editNs[i]);
    std::sort(noFault.begin(), noFault.end());
    std::sort(withFault.begin(), withFault.end());
    size_t overNoFault = 0;
    for (double v : noFault) overNoFault += v >= 20000.0;
    printRow("  edits without page faults", static_cast<double>(noFault.size()), "");
    if (!noFault.empty()) {
      printRow("  without faults: p99", noFault[noFault.size() * 99 / 100] / 1000.0, "us");
      printRow("  without faults: max", noFault.back() / 1000.0, "us");
      printRow("  without faults: >= 20 us", static_cast<double>(overNoFault), "");
    }
    if (!withFault.empty()) printRow("  with faults: median", withFault[withFault.size() / 2] / 1000.0, "us");
  }
  printRow("move one note: max", sortedNs.back() / 1000.0, "us");
  printRow("move one note: retained bytes median", static_cast<double>(sortedBytes[sortedBytes.size() / 2]) / 1024.0, "KiB");
  printRow("move one note: retained bytes max", static_cast<double>(sortedBytes.back()) / 1024.0, "KiB");
  printRow("10k edits: retained by history", mib(editsTotal), "MiB");
  printRow("10k edits: total heap (project + history)", mib(gLiveBytes - live0 - clipsVecBytes), "MiB");
  printRow("10k edits: process private bytes", privateMib(), "MiB");

  // ---- Undo / redo through the whole history. ----
  {
    const auto t0 = Clock::now();
    int n = 0;
    while (store.undo()) ++n;
    const auto t1 = Clock::now();
    int m = 0;
    while (store.redo()) ++m;
    const auto t2 = Clock::now();
    printRow("undo (mean over 10k)", std::chrono::duration<double, std::nano>(t1 - t0).count() / n, "ns");
    printRow("redo (mean over 10k)", std::chrono::duration<double, std::nano>(t2 - t1).count() / m, "ns");
    // Worst single step, timed individually (includes ~30 ns of clock overhead).
    double worst = 0;
    for (int i = 0; i < 2000; ++i) {
      const auto a = Clock::now();
      store.undo();
      const auto b = Clock::now();
      worst = std::max(worst, std::chrono::duration<double, std::nano>(b - a).count());
    }
    for (int i = 0; i < 2000; ++i) {
      const auto a = Clock::now();
      store.redo();
      const auto b = Clock::now();
      worst = std::max(worst, std::chrono::duration<double, std::nano>(b - a).count());
    }
    printRow("undo/redo: worst single step", worst, "ns");
  }

  // ---- Other edits on the big project (for reference). ----
  {
    const TrackId t = clips[5000].track;
    float v = -1.0f;
    const auto s = pq::bench::measure(
        [&] {
          v = v < -60.0f ? -1.0f : v - 0.5f;
          (void)store.dispatch(SetTrackVolume{t, v});
        },
        1, 7, 1, 1e6);
    printRow("set volume (nested track, with history)", s.medianNs / 1000.0, "us");
  }
  {
    // A gesture of 60 volume steps then commit: per step cost.
    const TrackId t = clips[100].track;
    const auto t0 = Clock::now();
    (void)store.beginGesture("Fader");
    for (int i = 0; i < 60; ++i) (void)store.dispatch(SetTrackVolume{t, -0.25f * static_cast<float>(i)});
    (void)store.commitGesture();
    printRow("volume gesture step (60 steps + commit)",
             std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / 60.0 / 1000.0, "us");
  }

  // ---- One large clip (100,000 notes): one-note move cost is dominated by the chunk directory copy. ----
  {
    IdSource& ids = store.ids();
    std::mt19937 r2(3);
    const ClipAddr& host = clips[0];
    ClipPtr big = buildClip(ids, r2, 1000 * pq::kTicksPerWhole, 100000);
    const ClipRef ref{host.track, big->id};
    if (!store.dispatch(AddClip{host.track, big}).ok()) return 1;
    std::vector<double> ns;
    std::vector<long long> bytes;
    for (int i = 0; i < 2000; ++i) {
      const NoteClip& c = clipOf(*store.current(), {host.track, big->id});
      const auto& ch = c.notes.chunks()[r2() % c.notes.chunks().size()];
      const Note& n = ch.notes[r2() % ch.count];
      const Command cmd = MoveNote{ref, keyOf(n), n.start + 1, n.pitch};
      const long long b0 = gLiveBytes;
      const auto t0 = Clock::now();
      (void)store.dispatch(cmd);
      ns.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
      bytes.push_back(gLiveBytes - b0);
    }
    std::sort(ns.begin(), ns.end());
    std::sort(bytes.begin(), bytes.end());
    printRow("move one note in a 100k-note clip: median", ns[ns.size() / 2] / 1000.0, "us");
    printRow("move one note in a 100k-note clip: retained median", static_cast<double>(bytes[bytes.size() / 2]) / 1024.0,
             "KiB");
  }
  std::printf("allocations: %lld\n", gAllocs);
  return 0;
}
