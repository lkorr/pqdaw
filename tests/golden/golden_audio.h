#pragma once

// Golden-audio comparison (T1.10; ARCHITECTURE §4: 32-bit float WAV, absolute + spectral tolerance). Tests only.
//
// A render matches its golden when ALL of these hold:
//   format    same channel count, frame count and sample rate; no NaN/inf in the render;
//   absolute  max |render − golden| over every sample ≤ absTol;
//   spectral  per channel, short-time spectra (Hann window of fftSize, hop fftSize/2, whole windows plus one aligned to the end)
//             summed into semitone (1/12-octave) bands from 20 Hz (each at least one bin wide); each band's level
//             in dB re a full-scale sine, clamped below at floorDb; max |level(render) − level(golden)| over frames
//             and bands ≤ spectralDb.
// Why both: the absolute check catches any audible change of the waveform (the default 1e-4 is −80 dBFS); the
// spectral check catches what a loosened absolute tolerance would let through, so a test may relax absTol for a
// render that may legitimately drift in phase (e.g. a future oscillator change) and still fail on a changed
// spectrum. Interpreter vs JIT differences (≤ ~1e-6 per sample, TASKS T1b.1) pass both with a wide margin.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "pq/engine/wav_file.h"

namespace golden {

struct Tolerance {
  double absTol = 1e-4;       // linear
  double spectralDb = 0.2;    // per band and frame
  double floorDb = -80.0;     // band levels below this are treated as this (dB re a full-scale sine)
  uint32_t fftSize = 4096;    // power of two (11.7 Hz bins at 48 kHz: a 25-cent shift of A4 moves a bin)
};

struct Comparison {
  bool ok = false;
  bool formatOk = false;
  double maxAbs = 0.0;
  uint32_t maxAbsChannel = 0;
  uint64_t maxAbsFrame = 0;
  double maxSpectralDb = 0.0;
  uint32_t worstChannel = 0, worstFrame = 0;  // spectral
  double worstBandHz = 0.0;                   // centre of the worst band
  std::string message;                        // one line: what failed (or the measured margins when ok)
};

using Channels = std::vector<std::vector<float>>;

Comparison compare(const Channels& golden, uint32_t goldenRate, const Channels& actual, uint32_t actualRate,
                   const Tolerance& tol = {});
inline Comparison compare(const pq::engine::WavData& golden, const Channels& actual, uint32_t actualRate,
                          const Tolerance& tol = {}) {
  return compare(golden.channels, golden.sampleRate, actual, actualRate, tol);
}

// Band levels (dB re a full-scale sine, floored) of one channel: [frame][band]. Exposed for the helper's own tests.
std::vector<std::vector<double>> bandLevels(const std::vector<float>& x, uint32_t sampleRate, const Tolerance& tol,
                                            std::vector<double>* bandCentresHz = nullptr);

inline std::filesystem::path goldenDir() { return std::filesystem::path(PQ_TEST_DATA_DIR) / "golden"; }

}  // namespace golden
