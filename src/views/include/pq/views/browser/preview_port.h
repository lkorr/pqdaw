#pragma once

// The browser's preview (T5.10), as the views see it: plain data, like TransportPort. The app implements it over the
// engine's PreviewPlayer, the playback link's sample pool and AudioOutput's cue source (pq/app/preview_service.h);
// tests use FakePreviewPort below.
//
// UI thread only.

#include <string>
#include <vector>

namespace pq::views {

struct PreviewRequest {
  std::string path;          // absolute UTF-8
  double startSeconds = 0;   // file time
  bool sync = false;         // Re-Pitch to the project tempo (needs fileBpm)
  double fileBpm = 0;
  double projectBpm = 120;
};

class PreviewPort {
public:
  virtual ~PreviewPort() = default;
  // The browser selected a file: load its head now, so a play starts at once.
  virtual void select(const std::string& path) = 0;
  // Plays (replacing what plays; a load still running starts it when done).
  virtual void play(const PreviewRequest& r) = 0;
  // Jumps the playing preview to `fileSeconds` (scrub on click). False when nothing plays.
  virtual bool scrub(double fileSeconds) = 0;
  virtual void stop() = 0;
  // Polled by the browser (a timer while it shows a playing preview): whether it still plays (a file that ended
  // reports false; the app then lets the output sleep again), and where (file seconds; < 0 when not playing).
  virtual bool playing() = 0;
  virtual double position() = 0;
  virtual void setVolumeDb(float db) = 0;
};

// A scripted port for tests and scenes: records requests; plays until stop() (or `ended` is set).
class FakePreviewPort final : public PreviewPort {
public:
  std::vector<std::string> selected;
  std::vector<PreviewRequest> played;
  std::vector<double> scrubs;
  int stops = 0;
  bool isPlaying = false;
  double pos = -1;
  float volumeDb = 0;
  void select(const std::string& path) override { selected.push_back(path); }
  void play(const PreviewRequest& r) override {
    played.push_back(r);
    isPlaying = true;
    pos = r.startSeconds;
  }
  bool scrub(double s) override {
    if (!isPlaying) return false;
    scrubs.push_back(s);
    pos = s;
    return true;
  }
  void stop() override {
    ++stops;
    isPlaying = false;
    pos = -1;
  }
  bool playing() override { return isPlaying; }
  double position() override { return pos; }
  void setVolumeDb(float db) override { volumeDb = db; }
};

}  // namespace pq::views
