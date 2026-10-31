#pragma once

// T5.6: audio arrangement fixtures for the views tests and benchmarks: a synthetic waveform overview and the large
// audio project (tracks × clips of warp-Off audio clips over one real tiny WAV file, the overviews injected through
// Waveforms::put so nothing is decoded).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/edit_ops.h"
#include "pq/document/samples.h"
#include "pq/media/wav_file.h"
#include "pq/media/waveform.h"
#include "pq/views/view_context.h"
#include "pq/views/waveforms.h"

namespace pq::views::test {

// A stereo overview of `frames` frames of a decaying-hit pattern (one hit every half second at 48 kHz), built by the
// real builder.
inline media::OverviewPtr syntheticOverview(uint64_t frames, uint32_t channels = 2) {
  std::shared_ptr<media::WaveformOverview> o = media::WaveformOverview::create(channels, frames, 48000, 1);
  media::OverviewBuilder b(*o);
  constexpr uint64_t kChunk = 1 << 16;
  std::vector<std::vector<float>> buf(channels, std::vector<float>(kChunk));
  std::vector<const float*> ptrs(channels);
  uint32_t rng = 1;
  for (uint64_t at = 0; at < frames; at += kChunk) {
    const uint64_t n = std::min(kChunk, frames - at);
    for (uint32_t c = 0; c < channels; ++c) {
      for (uint64_t i = 0; i < n; ++i) {
        const uint64_t f = at + i;
        rng = rng * 1664525u + 1013904223u;
        const double env = std::exp(-static_cast<double>(f % 24000) / 4000.0);
        buf[c][i] = static_cast<float>(env * (0.7 * std::sin(static_cast<double>(f) * 0.01 + c) +
                                              0.3 * (static_cast<double>(rng >> 8) / 8388608.0 - 1.0)));
      }
      ptrs[c] = buf[c].data();
    }
    b.add(ptrs.data(), n);
  }
  b.finish();
  return o;
}

// A tiny real WAV (so the samples resolve) at `path`.
inline void writeTinyWav(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::vector<std::vector<float>> ch(2, std::vector<float>(480, 0.25f));
  if (!media::writeWav(path, ch, 48000).ok()) {
    std::fprintf(stderr, "audio fixture: cannot write %s\n", path.string().c_str());
    std::abort();
  }
}

// `tracks` audio tracks of `clips` one-bar warp-Off clips each (every other bar; 120 BPM: 2 s each), each track its own
// sample entry pointing at `wav` and claiming `ov`'s length; the overview is injected for every sample.
inline void buildLargeAudioArrangement(ViewContext& ctx, int tracks, int clips, const std::filesystem::path& wav,
                                       const media::OverviewPtr& ov) {
  doc::DocumentStore& store = ctx.store();
  auto must = [](bool ok, const char* what) {
    if (!ok) {
      std::fprintf(stderr, "audio fixture: %s failed\n", what);
      std::abort();
    }
  };
  const std::u8string w8 = wav.generic_u8string();
  const std::string path(reinterpret_cast<const char*>(w8.data()), w8.size());
  auto ident = doc::fileIdentity(path);
  must(ident.ok(), "file identity");
  const uint32_t palette[] = {0xEF7B7B, 0xE68632, 0xB39E16, 0x4BB416, 0x16B293, 0x46A5E8, 0x8499F0, 0xBB86F0};
  doc::TrackId prev{};
  for (int t = 0; t < tracks; ++t) {
    auto tr = doc::ops::createTrack(*store.current(), store.ids(), doc::TrackKind::Audio, prev, {}, doc::Colour{palette[t % 8]});
    must(tr.ok() && store.dispatch(tr->command).ok(), "create track");
    const doc::TrackId id = tr->tracks.at(0);
    prev = id;
    doc::SampleRef s;
    s.id = store.ids().sample();
    s.path = path;
    s.size = ident->size;
    s.contentHash = ident->hash;
    s.frames = static_cast<int64_t>(ov->frames());
    s.rate = 48000.0;
    s.channels = ov->channels();
    s.displayName = "loop " + std::to_string(t + 1);
    doc::Batch batch;
    batch.label = "fixture";
    batch.commands.push_back(doc::AddSample{s});
    for (int c = 0; c < clips; ++c) {
      doc::AudioClip a;
      a.id = store.ids().clip();
      a.name = "Audio " + std::to_string(c + 1);
      a.colour = doc::Colour{palette[(t + c) % 8]};
      a.start = static_cast<Ticks>(c) * 2 * kTicksPerWhole;
      a.length = kTicksPerWhole;
      a.sample = s.id;
      a.seconds = 2.0;
      a.contentStartFrame = static_cast<double>((c % 8) * 48000);
      batch.commands.push_back(doc::AddClip{id, nullptr, std::make_shared<const doc::AudioClip>(std::move(a))});
    }
    must(store.dispatch(doc::Command(std::move(batch))).ok(), "add clips");
    ctx.waveforms().put(s.id, ov);
  }
}

}  // namespace pq::views::test
