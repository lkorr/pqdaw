// Project file costs (T3.5 perf criteria, release): a project with 100,000 notes
//   - save (serialize + write temp + flush + rename) < 150 ms
//   - load (read + parse + validate + build snapshot) < 250 ms
// Also times nlohmann/json on the same text (SAX with a no-op handler, and DOM), the alternative this module's
// specialised reader replaces.
//
// Project: 16 MIDI tracks × 10 clips × 625 notes; pitches 70 % Degree, 10 % Ratio, 10 % Cents, 10 % Harmonic;
// velocities and chances are non-integer floats (the slow path of shortest-float printing), every 4th note has a
// non-default release velocity.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pq/document/serialize.h"
#include "pq_bench.h"

using namespace pq::doc;
using pq::Result;

namespace {

Snapshot buildProject(IdSource& ids, size_t& noteCount) {
  std::mt19937_64 rng(7);
  auto p = std::const_pointer_cast<Project>(makeDefaultProject(ids));
  noteCount = 0;
  for (int t = 0; t < 16; ++t) {
    Track tr = makeTrack(ids.track(), TrackKind::Midi, "Track " + std::to_string(t));
    for (int c = 0; c < 10; ++c) {
      NoteClip clip;
      clip.id = ids.clip();
      clip.name = "Clip";
      clip.start = static_cast<pq::Ticks>(c) * 8 * pq::kTicksPerWhole;
      clip.length = 8 * pq::kTicksPerWhole;
      std::vector<Note> notes;
      for (int i = 0; i < 625; ++i) {
        Note n;
        n.id = ids.note();
        n.start = static_cast<pq::Ticks>(i) * (pq::kPpq / 5);
        n.length = pq::kPpq / 7 + static_cast<pq::Ticks>(rng() % 1000);
        const int kind = static_cast<int>(rng() % 10);
        if (kind < 7)
          n.pitch = pq::tuning::PitchId::fromDegree(static_cast<int32_t>(rng() % 31), static_cast<int32_t>(rng() % 5) - 2);
        else if (kind == 7)
          n.pitch = pq::tuning::PitchId::fromRatio(pq::tuning::Rational{static_cast<int64_t>(rng() % 31 + 1), 16});
        else if (kind == 8)
          n.pitch = pq::tuning::PitchId::fromCents(std::uniform_real_distribution<double>(-2400.0, 2400.0)(rng));
        else
          n.pitch = pq::tuning::PitchId::fromHarmonic(1, static_cast<uint32_t>(rng() % 16));
        n.velocity = 1.0f + static_cast<float>(rng() % 12600) / 100.0f;
        n.chance = static_cast<float>(rng() % 1000) / 1000.0f;
        if (i % 4 == 0) n.releaseVelocity = static_cast<float>(rng() % 128);
        notes.push_back(n);
      }
      noteCount += notes.size();
      clip.notes = NoteSeq::fromNotes(std::move(notes)).value();
      tr.clips.push_back(ClipSlot{clip.id, std::make_shared<const NoteClip>(std::move(clip))});
    }
    p->tracks.push_back(std::make_shared<const Track>(std::move(tr)));
  }
  return p;
}

struct NopSax {
  using string_t = std::string;
  bool null() { return true; }
  bool boolean(bool) { return true; }
  bool number_integer(int64_t) { return true; }
  bool number_unsigned(uint64_t) { return true; }
  bool number_float(double, const std::string&) { return true; }
  bool string(std::string&) { return true; }
  bool binary(nlohmann::json::binary_t&) { return true; }
  bool start_object(size_t) { return true; }
  bool key(std::string&) { return true; }
  bool end_object() { return true; }
  bool start_array(size_t) { return true; }
  bool end_array() { return true; }
  bool parse_error(size_t, const std::string&, const nlohmann::detail::exception&) { return false; }
};

template <class F>
void timeIt(const char* name, int reps, F&& f) {
  std::vector<double> ms;
  for (int r = 0; r < reps; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  std::sort(ms.begin(), ms.end());
  std::printf("%-44s median %7.1f ms   min %7.1f ms   max %7.1f ms\n", name, ms[ms.size() / 2], ms.front(), ms.back());
}

}  // namespace

int main() {
  IdSource ids;
  size_t notes = 0;
  const Snapshot p = buildProject(ids, notes);
  ViewState view;
  const std::string dir = (std::filesystem::temp_directory_path() / "pqdaw-bench-serialize").string();
  std::printf("project: %zu notes\n", notes);

  std::string text = serializeProject(*p, view).value();
  std::printf("project.pqd: %.2f MB\n", static_cast<double>(text.size()) / 1e6);
  constexpr int kReps = 9;

  timeIt("serializeProject (to string)", kReps, [&] { text = serializeProject(*p, view).value(); });
  timeIt("saveProject (serialize+write+flush+rename)", kReps, [&] {
    if (!saveProject(dir, *p, view).ok()) std::abort();
  });
  timeIt("parseProject (from string)", kReps, [&] {
    if (!parseProject(text).ok()) std::abort();
  });
  {
    Result<LoadedProject> r = loadProject(dir);
    if (!r.ok() || !(*r->project == *p)) std::abort();  // checked once, outside the timing
  }
  timeIt("loadProject (read+parse+validate)", kReps, [&] {
    if (!loadProject(dir).ok()) std::abort();
  });
  timeIt("nlohmann sax_parse, no-op handler", 3, [&] {
    NopSax sax;
    if (!nlohmann::json::sax_parse(text.data(), text.data() + text.size(), &sax)) std::abort();
  });
  timeIt("nlohmann::json::parse (DOM)", 3, [&] {
    nlohmann::json d = nlohmann::json::parse(text);
    if (d.empty()) std::abort();
  });
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return 0;
}
