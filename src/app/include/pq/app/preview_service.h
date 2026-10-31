#pragma once

// The browser's preview in the app (T5.10): views::PreviewPort over the engine's PreviewPlayer (pq/engine/preview_player.h),
// the playback link's sample pool and AudioOutput's cue source.
//
//   select(path)   loads the file into the pool on the service's loader thread (probe + resident data, or the head of
//                  a long file + its stream source), so a play starts within the next device callback;
//   play(r)        plays at once when the file is loaded, else when its load finishes (posted to the UI thread; without
//                  a poster: waits here, tests); attaches the player as the output's cue source and engages the pool's
//                  disk thread (streams) while it plays;
//   playing()      polled by the browser while it shows a preview: once the player has stopped (the end of the file or
//                  stop()) the cue source is detached and the disk thread disengaged, so a stopped preview costs
//                  nothing (the output may suspend again).
// The last few loaded files stay referenced (a small LRU), so going back and forth in the browser does not reload them.
//
// UI thread, except the loader thread.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pq/engine/preview_player.h"
#include "pq/media/sample_pool.h"
#include "pq/views/browser/preview_port.h"

namespace pq::app {

class AudioOutput;

class PreviewService final : public views::PreviewPort {
public:
  using Poster = std::function<void(std::function<void()>)>;
  // `pool` must outlive the service. `output` may be null (no cue source: tests drive player() themselves).
  PreviewService(media::SamplePool& pool, double sampleRate, AudioOutput* output, Poster post = {});
  ~PreviewService() override;
  PreviewService(const PreviewService&) = delete;
  PreviewService& operator=(const PreviewService&) = delete;

  // The engine rate changed (the device's): the player is rebuilt at it (a playing preview stops).
  void setSampleRate(double rate);
  double sampleRate() const noexcept { return rate_; }
  void setOutput(AudioOutput* output);
  engine::PreviewPlayer& player() noexcept { return *player_; }
  // Whether `path` is loaded (tests).
  bool loaded(const std::string& path) const;
  bool attached() const noexcept { return attached_; }

  // ---- views::PreviewPort ----
  void select(const std::string& path) override;
  void play(const views::PreviewRequest& r) override;
  bool scrub(double fileSeconds) override;
  void stop() override;
  bool playing() override;
  double position() override;
  void setVolumeDb(float db) override;

private:
  struct Loaded {
    std::string path;
    media::SampleRef sample;  // null: failed
    bool done = false;
  };
  void loaderMain();
  void request(const std::string& path);
  media::SampleRef find(const std::string& path) const;  // done + ok, else null
  void start(const views::PreviewRequest& r, media::SampleRef s);
  void attach(bool on);
  void loadFinished();  // UI thread (posted)

  media::SamplePool& pool_;
  double rate_;
  AudioOutput* output_ = nullptr;
  Poster post_;
  std::unique_ptr<engine::PreviewPlayer> player_;
  engine::AudioCallback callback_{};
  bool attached_ = false;
  bool engaged_ = false;
  float gain_ = 1.0f;
  std::optional<views::PreviewRequest> pending_;  // a play waiting for its load

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  std::vector<Loaded> loaded_;  // most recent last; at most kKeep
  bool quit_ = false;
  std::thread loader_;
  std::shared_ptr<char> life_ = std::make_shared<char>();
  static constexpr size_t kKeep = 8;
};

}  // namespace pq::app
