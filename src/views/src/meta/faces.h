#pragma once

// The built-in faces (TMM.4): their factories, registered by meta_face.cpp. Private to views.

#include <memory>
#include <vector>

#include "pq/views/meta/meta_face.h"

namespace pq::views {

std::unique_ptr<MetaFace> makePhaseAccFace(const meta::Type& t);
std::unique_ptr<MetaFace> makeWavetableFace(const meta::Type& t);  // TMM.7
std::unique_ptr<MetaFace> makeEnvelopeFace(const meta::Type& t);   // TMM.11
std::unique_ptr<MetaFace> makeVcaFace(const meta::Type& t);        // TMM.14

// TMM.16 (tests): the Wavetable face's main curve as its last paint drew it — the frame as it sounds at `position`
// (kSoundPoints + 1 values, phases i / kSoundPoints), in draw mode or as the highlighted frame of the stack. False:
// not a Wavetable face, or not painted yet.
struct WavetableShown {
  std::vector<float> values;
  float position = 0.0f;
  bool drawMode = false;
  bool heard = false;      // read from the published table (the host's reader knows the slot); else the face's Model
  uint32_t level = 0;      // ... at this mip level
  const dsp::FrameTable* table = nullptr;  // ... of this table (the one the face holds)
  std::vector<std::vector<float>> stack;  // the stacked display's frames (σ-smoothed thumbnails, kPoints + 1 each)
};
bool wavetableShown(const MetaFace& f, WavetableShown& out);

}  // namespace pq::views
