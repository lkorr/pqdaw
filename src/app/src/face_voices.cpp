// The faces' voice dots in the running app (TMM.5): pq/app/face_voices.h.
#include "pq/app/face_voices.h"

#include "pq/app/project_link.h"

namespace pq::app {

views::VoiceTapReader* FaceVoiceHub::voices(doc::DeviceId device, std::string_view path) {
  auto key = std::make_pair(device.value, std::string(path));
  auto it = readers_.find(key);
  if (it == readers_.end()) {
    ProjectLink* link = &link_;
    const std::string p(path);
    // TMM.16: and the table its slot plays (the Wavetable face draws what is heard), with its live params.
    auto r = std::make_unique<views::TapVoices>([link, device, p] { return link->metaVoiceTap(device, p, 0); },
                                                [link, device, p] {
                                                  views::TapVoices::TableSlotRef t;
                                                  t.slot = link->metaFrameTable(device, p, 0, t.modulated);
                                                  return t;
                                                },
                                                // TMM.17: increments per sample at the engine's rate (voice colours)
                                                [link] { return link->config().sampleRate; },
                                                // TMM.13-fix1 M12: look up again only when the slots changed
                                                [link] { return link->metaGeneration(); });
    it = readers_.emplace(std::move(key), Entry{std::move(r), false}).first;
  }
  it->second.parked = false;  // asked for: polled again (until the next prune finds no slot)
  return it->second.reader.get();
}

bool FaceVoiceHub::anyFresh() {
  if (const uint64_t g = link_.metaGeneration(); g != prunedAt_) {
    prunedAt_ = g;
    for (auto& [key, e] : readers_) {
      const bool gone = link_.metaSlot(doc::DeviceId{key.first}, key.second, 0) == 0;
      if (gone && !e.parked) e.reader->park();
      e.parked = gone;
    }
  }
  for (const auto& [key, e] : readers_)
    if (!e.parked && e.reader->fresh()) return true;
  return false;
}

size_t FaceVoiceHub::polled() const noexcept {
  size_t n = 0;
  for (const auto& [key, e] : readers_) n += e.parked ? 0u : 1u;
  return n;
}

}  // namespace pq::app
