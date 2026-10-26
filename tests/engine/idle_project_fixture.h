#pragma once

// T1.x-silence: the "idle project with reverbs and delays" of the acceptance (tests/engine/test_project_silence.cpp,
// bench/engine/bench_silence.cpp), built from stdlib: `numTracks` MIDI tracks, each the stock instrument
// (instruments/basic_synth.pqp, voice end by its `done`) → a Schroeder reverb (four filters/comb.pqp combs, one of them
// comb_lp, then two allpass_delay diffusers) and a feedback echo (a long comb, 375 ms) → EQ Eight (devices/
// eq_eight.pqp, stereo, its default curve) → a smoothed volume (gain), plus a send of the wet signal to a return with
// its own comb/allpass reverb; a master (tanh) plays the sum. Each track plays one short chord at the start
// (`chord(k)`), after which every tail rings out: the echoes for ≈ 6–7 s, the return's reverb a little longer.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../graph/text_util.h"
#include "pq/engine/note_list.h"
#include "pq/graph/parse.h"
#include "pq/graph/project.h"

namespace idle_fixture {

namespace graph = pq::graph;
namespace fs = std::filesystem;

inline std::string trackText(uint32_t k) {
  char buf[2048];
  std::snprintf(buf, sizeof buf, R"(
import "instruments/basic_synth.pqp"
import "filters/comb.pqp"
import "filters/allpass.pqp"
import "devices/eq_eight.pqp"

patch Track%u {
  param vol = %.3f [0..1]
  out   out  : audio global stereo
  out   send : audio global mono
  out   done : bool voice mono
  syn  = BasicSynth(release: %.0fms)
  x    = voice_sum(syn.out)
  done = syn.done
  c1   = comb(x, time: %.1fms, fb: 0.78)
  c2   = comb(x, time: %.1fms, fb: 0.76)
  c3   = comb(x, time: %.1fms, fb: 0.74)
  c4   = comb_lp(x, time: %.1fms, fb: 0.72, damp: 0.3)
  a1   = allpass_delay((c1 + c2 + c3 + c4) * 0.25, time: 5ms, g: 0.7)
  a2   = allpass_delay(a1, time: 1.7ms, g: 0.7)
  echo = comb(x, 600ms, time: %.0fms, fb: 0.45)
  wet  = x + a2 * 0.35 + echo * 0.3
  eq   = eq_eight((wet, wet))
  out  = gain(eq, vol, 2ms)
  send = wet * 0.2
}
)",
                k, 0.4 + 0.01 * (k % 10), 150.0 + 10.0 * (k % 5), 29.7 + 0.3 * (k % 7), 37.1 + 0.2 * (k % 5),
                41.1 + 0.3 * (k % 3), 43.7 + 0.2 * (k % 4), 300.0 + 15.0 * (k % 6));
  return buf;
}

inline const char* busText() {
  return R"(
import "filters/comb.pqp"
import "filters/allpass.pqp"

patch Return {
  in    in : audio global mono
  out   out : audio global stereo
  c1 = comb(in, time: 31.3ms, fb: 0.8)
  c2 = comb(in, time: 35.9ms, fb: 0.79)
  a  = allpass_delay((c1 + c2) * 0.5, time: 4.1ms, g: 0.7)
  out = (a, allpass_delay(a, time: 2.3ms, g: 0.6))
}

patch Master {
  in    in : audio global stereo
  out   out : audio global stereo
  out = tanh(in)
}
)";
}

struct Project {
  std::vector<std::string> texts;
  std::vector<graph::PatchFile> files;
  graph::PatchLibrary library;
  std::vector<std::unique_ptr<graph::Patch>> patches;
  graph::ProjectGraph graph;
  std::unique_ptr<text_util::DirLoader> loader;
  uint32_t ret = 0, master = 0;
};

inline const graph::PatchFile* parse(Project& p, std::string text, const std::string& name) {
  p.texts.push_back(std::move(text));
  graph::ParseOptions po;
  po.imports = p.loader.get();
  auto r = graph::parse(p.texts.back(), (fs::weakly_canonical(text_util::stdlibDir()) / name).string(), po);
  if (!r) {
    std::fprintf(stderr, "%s\n", r.error().message.c_str());
    return nullptr;
  }
  p.files.push_back(std::move(r.value()));
  return &p.files.back();
}

inline std::unique_ptr<Project> makeProject(uint32_t numTracks) {
  auto p = std::make_unique<Project>();
  const fs::path stdlib = fs::weakly_canonical(text_util::stdlibDir());
  p->loader = std::make_unique<text_util::DirLoader>(stdlib);
  p->files.reserve(64 + numTracks);
  // The whole stdlib (the library every call resolves into), then the tracks.
  for (const fs::path& f : text_util::pqpFiles(stdlib))
    if (parse(*p, text_util::readFile(f), fs::relative(f, stdlib).string()) == nullptr) return nullptr;
  for (uint32_t k = 0; k < numTracks; ++k) {
    const graph::PatchFile* f = parse(*p, trackText(k), "__idle_track" + std::to_string(k) + ".pqp");
    if (f == nullptr) return nullptr;
    p->patches.push_back(std::make_unique<graph::Patch>(f->patches[0]));
  }
  const graph::PatchFile* bus = parse(*p, busText(), "__idle_buses.pqp");
  if (bus == nullptr) return nullptr;
  p->patches.push_back(std::make_unique<graph::Patch>(bus->patches[0]));
  p->patches.push_back(std::make_unique<graph::Patch>(bus->patches[1]));
  for (const graph::PatchFile& f : p->files) p->library.add(f);
  graph::ProjectGraph& g = p->graph;
  g.library = &p->library;
  for (uint32_t k = 0; k < numTracks; ++k)
    g.tracks.push_back({k + 1, "Track" + std::to_string(k), p->patches[k].get(), graph::TrackRole::Instrument, k});
  p->ret = static_cast<uint32_t>(g.tracks.size());
  g.tracks.push_back({1000, "Return", p->patches[numTracks].get(), graph::TrackRole::Return, graph::kNone});
  p->master = static_cast<uint32_t>(g.tracks.size());
  g.tracks.push_back({1001, "Master", p->patches[numTracks + 1].get(), graph::TrackRole::Master, graph::kNone});
  for (uint32_t k = 0; k < numTracks; ++k) {
    g.wires.push_back({{k, "out"}, {p->master, "in"}, graph::WireKind::Audio});
    g.wires.push_back({{k, "send"}, {p->ret, "in"}, graph::WireKind::Audio});
  }
  g.wires.push_back({{p->ret, "out"}, {p->master, "in"}, graph::WireKind::Audio});
  g.output = {p->master, "out"};
  return p;
}

// Track k's chord: three notes from frame 0, 0.25 s long.
inline std::vector<pq::engine::TimedNote> chord(uint32_t k) {
  const float base = 110.0f * (1.0f + 0.125f * static_cast<float>(k % 8));
  return {{0, 12000, base, 0.8f, 0.5f}, {0, 12000, base * 1.25f, 0.7f, 0.5f}, {0, 12000, base * 1.5f, 0.6f, 0.5f}};
}

}  // namespace idle_fixture
