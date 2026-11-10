// M6 review (recording): is a recording WAV readable after the process is killed mid-write?
// Parent: starts itself as a child ("child <path>") that writes a 2-channel 48 kHz float WAV with media::WavWriter exactly
// as RecordingService does (open(..., patchEveryFrames = 48000), write() of 64-frame blocks, sample value = frame index
// scaled), paced at ~8x real time; after a while the parent TerminateProcess()es it (no destructors, no close()).
// Then readWav: the frames it yields must be whole patched seconds, each sample bit-exact.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "pq/media/wav_file.h"

namespace fs = std::filesystem;

static float value(uint32_t c, uint64_t f) { return (c == 0 ? 1.0f : -1.0f) * static_cast<float>(f % (1u << 22)) / static_cast<float>(1u << 22); }

static int child(const fs::path& path) {
  pq::media::WavWriter w;
  if (!w.open(path, 2, 48000, 48000).ok()) return 2;
  std::vector<float> l(64), r(64);
  for (uint64_t f = 0;; f += 64) {
    for (uint32_t i = 0; i < 64; ++i) l[i] = value(0, f + i), r[i] = value(1, f + i);
    const float* ch[2] = {l.data(), r.data()};
    if (!w.write(ch, 64).ok()) return 3;
    if ((f & 4095) == 0) std::this_thread::sleep_for(std::chrono::microseconds(10600));  // ~8x real time
  }
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "child") return child(argv[2]);
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::random_device rd;
  const fs::path dir = fs::temp_directory_path() / ("pq_m6rev_wavkill_" + std::to_string(rd()));
  fs::create_directories(dir);
  int bad = 0;
  const int killAfterMs[] = {50, 700, 1300, 2900, 4100};
  for (int k : killAfterMs) {
    const fs::path path = dir / ("kill_" + std::to_string(k) + ".wav");
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" child \"" + path.wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return 9;
    Sleep(static_cast<DWORD>(k));
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    const auto bytes = fs::file_size(path);
    auto r = pq::media::readWav(path);
    if (!r.ok()) {
      std::printf("kill@%5d ms: %llu bytes on disk, readWav FAILED: %s\n", k, static_cast<unsigned long long>(bytes),
                  r.error().message.c_str());
      ++bad;
      continue;
    }
    const auto& w = r.value();
    uint64_t wrong = 0;
    for (uint32_t c = 0; c < w.numChannels(); ++c)
      for (uint64_t f = 0; f < w.frames(); ++f) wrong += w.channels[c][f] != value(c, f) ? 1 : 0;
    const uint64_t onDisk = (bytes - 80) / 8;  // approx frames of sample data on disk (header ≈ 80 bytes)
    std::printf("kill@%5d ms: %9llu bytes on disk (~%llu frames written), readable %llu frames (%s whole seconds), "
                "%llu wrong samples, lost %.2f s\n",
                k, static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(onDisk),
                static_cast<unsigned long long>(w.frames()), w.frames() % 48000 == 0 ? "=" : "NOT",
                static_cast<unsigned long long>(wrong), (static_cast<double>(onDisk) - static_cast<double>(w.frames())) / 48000.0);
    bad += wrong != 0 || w.frames() % 48000 != 0 ? 1 : 0;
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::printf("%d failures\n", bad);
  return bad;
}
