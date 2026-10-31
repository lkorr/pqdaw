#pragma once

// A synthetic in-memory browser index (no files) for the search tests and pq_bench_library: sample-library-like names
// from a small vocabulary, two places, directories 1–3 deep, 70 % samples with random rate / channels / length / BPM.

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "pq/library/index.h"
#include "pq/library/tags.h"

namespace pq::library::test {

inline const char* const kSyntheticWords[] = {
    "Kick", "kick", "Snare", "SnareRoll", "Hat", "hat-open", "Clap", "Bass", "BassLine", "Pad",
    "Lead", "Pluck", "Vox", "Perc", "Tom", "808", "909", "Dark", "Bright", "Loop",
    "One_Shot", "fx", "Riser", "Sub", "Chord", "Stab", "Ambient", "Texture", "Glitch", "Break"};
inline constexpr std::size_t kSyntheticWordCount = sizeof kSyntheticWords / sizeof kSyntheticWords[0];

inline std::shared_ptr<Index> syntheticIndex(uint32_t entries, uint32_t seed) {
  std::mt19937 rng(seed);
  auto idx = std::make_shared<Index>();
  idx->places = {{"C:\\fake\\lib", PlaceKind::UserLibrary, "User Library"}, {"D:\\packs", PlaceKind::Folder, "Packs"}};
  idx->placeOnline = {1, 1};
  const uint32_t perDir = 100;
  const uint32_t dirsNeeded = (entries + perDir - 1) / perDir;
  uint32_t made = 0;
  uint64_t serial = 1;
  for (uint16_t p = 0; p < 2; ++p) {
    const uint32_t root = idx->addDir(Index::kNone, "", p, 0);
    uint32_t parent = root;
    const uint32_t dirsHere = p == 0 ? dirsNeeded / 2 : dirsNeeded - dirsNeeded / 2;
    for (uint32_t d = 0; d < dirsHere; ++d) {
      // Depths 1..3: every 10th directory starts a new top folder; the others nest under the previous one.
      uint16_t depth = 1;
      uint32_t par = root;
      if (d % 10 != 0) {
        par = parent;
        depth = static_cast<uint16_t>(idx->dirs.depth[parent] + 1);
        if (depth > 3) {
          par = root;
          depth = 1;
        }
      }
      const uint32_t dir =
          idx->addDir(par, std::string(kSyntheticWords[rng() % kSyntheticWordCount]) + " " + std::to_string(d), p, depth);
      parent = dir;
      for (uint32_t k = 0; k < perDir && made < entries; ++k, ++made) {
        EntryRecord r;
        const int words = 1 + static_cast<int>(rng() % 3);
        for (int w = 0; w < words; ++w) {
          if (w) r.name += " _-"[rng() % 3];
          r.name += kSyntheticWords[rng() % kSyntheticWordCount];
        }
        r.name += " " + std::to_string(rng() % 100);
        const uint32_t kind = rng() % 10;
        if (kind < 7) {
          r.kind = ItemKind::Sample;
          r.category = Category::Samples;
          r.name += ".wav";
          r.rate = std::array<uint32_t, 3>{44100, 48000, 96000}[rng() % 3];
          r.channels = static_cast<uint16_t>(1 + rng() % 2);
          r.frames = rng() % (r.rate * 8);
          r.bpm = (rng() % 3 == 0) ? static_cast<float>(80 + rng() % 100) : 0.f;
        } else if (kind < 9) {
          r.kind = ItemKind::Preset;
          r.category = (rng() % 2) ? Category::Instruments : Category::AudioEffects;
          r.name += ".pqpreset";
        } else {
          r.kind = ItemKind::Patch;
          r.category = Category::Patches;
          r.name += ".pqp";
        }
        r.volume = 0x1234;
        std::memcpy(r.fileId.data(), &serial, 8);
        ++serial;
        r.size = 1000 + rng() % 100000;
        idx->addEntry(dir, r);
      }
    }
  }
  return idx;
}

// Tags about one entry in `every / 2` with random collections and free tags.
inline void tagSome(TagStore& store, const Index& idx, uint32_t seed, uint32_t every) {
  std::mt19937 rng(seed);
  const char* tags[] = {"drums", "Dark", "bright", "808", "vocal", "keeper"};
  for (uint32_t e = 0; e < idx.entryCount(); e += 1 + rng() % every) {
    const TagKey k = tagKeyOf(idx, e);
    if (rng() % 2) store.setCollection(k, rng() % kCollectionCount, true);
    const int n = static_cast<int>(rng() % 3);
    for (int i = 0; i < n; ++i) store.addTag(k, tags[rng() % 6]);
  }
}

inline std::vector<std::string> prefixes(std::string_view word) {
  std::vector<std::string> out;
  for (std::size_t i = 1; i <= word.size(); ++i) out.emplace_back(word.substr(0, i));
  return out;
}

// The keystroke sequences the timing checks type.
inline const char* const kTypedWords[] = {"kick 808", "snare roll", "bassline", "dark pad", "hat open",
                                          "one shot", "glitch fx", "ambient texture", "909 loop", "riser"};

}  // namespace pq::library::test
