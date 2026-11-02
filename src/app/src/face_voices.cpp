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
                                                [link] { return link->config().sampleRate; });
    it = readers_.emplace(std::move(key), std::move(r)).first;
  }
  return it->second.get();
}

bool FaceVoiceHub::anyFresh() const {
  for (const auto& [key, r] : readers_)
    if (r->fresh()) return true;
  return false;
}

}  // namespace pq::app
