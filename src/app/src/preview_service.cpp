// T5.10: the browser's preview in the app (pq/app/preview_service.h).

#include "pq/app/preview_service.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "pq/app/audio_output.h"

namespace pq::app {

namespace {

std::filesystem::path pathOf(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

}  // namespace

PreviewService::PreviewService(media::SamplePool& pool, double sampleRate, AudioOutput* output, Poster post)
    : pool_(pool), rate_(sampleRate), output_(output), post_(std::move(post)) {
  engine::PreviewConfig pc;
  pc.sampleRate = rate_;
  pc.streams = &pool_.streams();
  player_ = std::make_unique<engine::PreviewPlayer>(pc);
  callback_ = player_->audioCallback();
  loader_ = std::thread([this] { loaderMain(); });
}

PreviewService::~PreviewService() {
  attach(false);
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  loader_.join();
  if (engaged_) pool_.disengage();
}

void PreviewService::setOutput(AudioOutput* output) {
  attach(false);
  output_ = output;
}

void PreviewService::setSampleRate(double rate) {
  if (rate == rate_) return;
  attach(false);  // the audio thread no longer calls the old player
  if (engaged_) {
    pool_.disengage();
    engaged_ = false;
  }
  rate_ = rate;
  engine::PreviewConfig pc;
  pc.sampleRate = rate_;
  pc.streams = &pool_.streams();
  player_ = std::make_unique<engine::PreviewPlayer>(pc);
  player_->setGain(gain_);
  callback_ = player_->audioCallback();
  pending_.reset();
}

void PreviewService::attach(bool on) {
  if (on == attached_) return;
  attached_ = on;
  if (output_) output_->setCueSource(on ? &callback_ : nullptr);
}

// ---- loading ----------------------------------------------------------------------------------------------------

void PreviewService::request(const std::string& path) {
  {
    std::lock_guard lock(mutex_);
    for (Loaded& l : loaded_)
      if (l.path == path) return;  // loaded or loading
    loaded_.push_back({path, nullptr, false});
    // Keep the most recent kKeep finished entries (the pool frees what no one references).
    while (loaded_.size() > kKeep) {
      const auto it = std::find_if(loaded_.begin(), loaded_.end(), [](const Loaded& l) { return l.done; });
      if (it == loaded_.end()) break;
      loaded_.erase(it);
    }
    queue_.push_back(path);
  }
  cv_.notify_all();
}

void PreviewService::loaderMain() {
  for (;;) {
    std::string path;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
      if (quit_) return;
      path = std::move(queue_.back());  // the newest request first (the browser moved on)
      queue_.pop_back();
    }
    auto r = pool_.load(pathOf(path));
    Poster post;
    {
      std::lock_guard lock(mutex_);
      for (Loaded& l : loaded_)
        if (l.path == path) {
          l.done = true;
          if (r) l.sample = *r;
        }
      post = post_;
    }
    cv_.notify_all();
    if (post) post([this, alive = std::weak_ptr<char>(life_)] {
      if (alive.lock()) loadFinished();
    });
  }
}

bool PreviewService::loaded(const std::string& path) const {
  std::lock_guard lock(mutex_);
  for (const Loaded& l : loaded_)
    if (l.path == path) return l.done;
  return false;
}

media::SampleRef PreviewService::find(const std::string& path) const {
  std::lock_guard lock(mutex_);
  for (const Loaded& l : loaded_)
    if (l.path == path && l.done) return l.sample;
  return nullptr;
}

void PreviewService::loadFinished() {
  if (!pending_) return;
  {
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(loaded_.begin(), loaded_.end(), [&](const Loaded& l) { return l.path == pending_->path; });
    if (it == loaded_.end()) {  // dropped from the LRU meanwhile: ask again
      pending_.reset();
      return;
    }
    if (!it->done) return;
  }
  const views::PreviewRequest r = *pending_;
  pending_.reset();
  if (media::SampleRef s = find(r.path)) start(r, std::move(s));
}

// ---- PreviewPort --------------------------------------------------------------------------------------------------

void PreviewService::select(const std::string& path) { request(path); }

void PreviewService::start(const views::PreviewRequest& r, media::SampleRef s) {
  engine::PreviewOptions o;
  o.sync = r.sync;
  o.fileBpm = r.fileBpm;
  o.projectBpm = r.projectBpm;
  if (!player_->play(std::move(s), r.startSeconds, o)) return;
  if (!engaged_) {
    pool_.engage();
    engaged_ = true;
  }
  attach(true);
}

void PreviewService::play(const views::PreviewRequest& r) {
  if (media::SampleRef s = find(r.path)) {
    pending_.reset();
    start(r, std::move(s));
    return;
  }
  request(r.path);
  player_->stop();  // what played before stops while the new file loads
  if (post_) {
    pending_ = r;
    return;
  }
  {  // no UI poster (tests): wait for the load here
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] {
      for (const Loaded& l : loaded_)
        if (l.path == r.path) return l.done;
      return true;
    });
  }
  if (media::SampleRef s = find(r.path)) start(r, std::move(s));
}

bool PreviewService::scrub(double fileSeconds) {
  if (pending_) {
    pending_->startSeconds = fileSeconds;
    return true;
  }
  return player_->state() == engine::PreviewPlayer::State::Playing && player_->scrub(fileSeconds);
}

void PreviewService::stop() {
  pending_.reset();
  player_->stop();
}

bool PreviewService::playing() {
  if (pending_) return true;
  const bool on = player_->state() == engine::PreviewPlayer::State::Playing;
  if (!on) {  // ended or stopped and faded: let the output sleep
    attach(false);
    if (engaged_) {
      pool_.disengage();
      engaged_ = false;
    }
    (void)player_->collect();
  }
  return on;
}

double PreviewService::position() { return pending_ ? pending_->startSeconds : player_->positionSeconds(); }

void PreviewService::setVolumeDb(float db) {
  gain_ = db <= -70.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
  player_->setGain(gain_);
}

}  // namespace pq::app
