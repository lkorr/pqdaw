#pragma once

// Forwarding header (T5.1): WAV reading and writing moved to the media module, pq/media/wav_file.h.

#include "pq/media/wav_file.h"

namespace pq::engine {

using media::kMaxWavChannels;
using media::parseWav;
using media::readWav;
using media::WavData;
using media::WavWriter;
using media::writeWav;

}  // namespace pq::engine
