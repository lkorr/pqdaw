#pragma once

// T5.10: a browser index without a disk scan, for the view scenes, the goldens and the tests (view_scenes.cpp).

#include <cstdint>
#include <memory>
#include <string_view>

#include "pq/library/index.h"

namespace pq::views {

// T5.10: a browser index for scenes, goldens and tests (no disk scan): place 0 is a User Library rooted at
// `samplesDir` holding its real files beat.wav and vox.wav (tests/views/fixtures/audio/samples: probed, so the preview
// shows their waveforms) plus folders of listed-only files (Drums, Loops, Presets/Operator); place 1 a folder
// "Field Recordings". `extraFiles` > 0 adds that many listed-only samples in a "Generated" folder (the 200k-row tests).
std::shared_ptr<const library::Index> browserDemoIndex(std::string_view samplesDir, uint32_t extraFiles = 0);

}  // namespace pq::views
