// EX.1: the export render core (pq/app/export_render.h).
#include "pq/app/export_render.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <system_error>

#include "pq/document/store.h"
#include "pq/media/decoder.h"
#include "pq/media/flac_file.h"
#include "pq/media/wav_file.h"

namespace pq::app {

namespace fs = std::filesystem;

namespace {

Error bad(const std::string& m) { return Error{ErrorCode::InvalidArgument, "export: " + m, {}}; }

// ---- renderPass's sinks --------------------------------------------------------------------------------------------

// Forwards stream frames [shift, shift + limit) of what the engine renders to the caller's sink as file frames
// [0, limit) (a negative shift: zeros first), and measures them.
class TrimSink final : public engine::AudioSink {
public:
  TrimSink(engine::AudioSink& out, int64_t shift, uint64_t limit, uint32_t rate)
      : out_(out), shift_(shift), limit_(static_cast<int64_t>(limit)), rate_(rate) {}
  Status begin(uint32_t numChannels, uint32_t, uint64_t) override {
    channels_ = numChannels;
    ptrs_.resize(numChannels);
    return out_.begin(numChannels, rate_, static_cast<uint64_t>(limit_));
  }
  Status write(const float* const* ch, uint32_t frames) override {
    const int64_t f0 = pos_ - shift_;  // the file frame of this chunk's first frame
    pos_ += frames;
    // Zeros where the stream starts after the file (a target early by its latency).
    while (written_ < std::min(f0, limit_)) {
      const auto n = static_cast<uint32_t>(std::min<int64_t>(std::min(f0, limit_) - written_, kZeros));
      if (zeros_.empty()) zeros_.assign(kZeros, 0.0f);
      for (uint32_t c = 0; c < channels_; ++c) ptrs_[c] = zeros_.data();
      if (auto s = out_.write(ptrs_.data(), n); !s) return s;
      written_ += n;
    }
    const int64_t a = std::max(f0, written_), b = std::min(f0 + static_cast<int64_t>(frames), limit_);
    if (a >= b) return Ok{};
    const auto n = static_cast<uint32_t>(b - a);
    for (uint32_t c = 0; c < channels_; ++c) {
      ptrs_[c] = ch[c] + (a - f0);
      for (uint32_t f = 0; f < n; ++f) {
        const float x = std::fabs(ptrs_[c][f]);
        peak_ = std::max(peak_, x);
        if (x > kTailFloor) loudEnd_ = std::max(loudEnd_, static_cast<uint64_t>(a + f + 1));
      }
    }
    if (auto s = out_.write(ptrs_.data(), n); !s) return s;
    written_ = b;
    return Ok{};
  }
  Status end() override { return Ok{}; }  // renderPass ends the caller's sink (also after a stop)

  uint64_t written() const noexcept { return static_cast<uint64_t>(written_); }
  PassTargetStats stats() const noexcept { return {written(), loudEnd_, peak_}; }

private:
  static constexpr int64_t kZeros = 4096;
  engine::AudioSink& out_;
  int64_t shift_, limit_;
  uint32_t rate_;
  uint32_t channels_ = 0;
  int64_t pos_ = 0;      // stream frames seen
  int64_t written_ = 0;  // file frames forwarded
  float peak_ = 0.0f;
  uint64_t loudEnd_ = 0;
  std::vector<const float*> ptrs_;
  std::vector<float> zeros_;
};

// The engine's main sink: the master target (if any), then progress, cancel and the end of the tail. The capture
// sinks have this chunk already (Engine::renderOffline writes them first), so the tail test sees every target.
class PassSink final : public engine::AudioSink {
public:
  TrimSink* master = nullptr;
  std::vector<TrimSink*> all;
  const PassSpec* spec = nullptr;
  uint64_t total = 0, done = 0;
  uint64_t range = 0, quiet = 0;  // N; a second of frames
  bool tail = false;
  bool cancelled = false, tailDone = false;

  Status begin(uint32_t ch, uint32_t rate, uint64_t frames) override {
    return master != nullptr ? master->begin(ch, rate, frames) : Status{Ok{}};
  }
  Status write(const float* const* ch, uint32_t frames) override {
    if (master != nullptr)
      if (auto s = master->write(ch, frames); !s) return s;
    done += frames;
    if (spec->progress) spec->progress(done, total);
    if (spec->cancel != nullptr && spec->cancel->load(std::memory_order_relaxed)) {
      cancelled = true;
      return stop();
    }
    if (tail) {
      bool over = true;
      for (const TrimSink* t : all) {
        const PassTargetStats s = t->stats();
        over &= s.frames >= range + quiet && s.frames - std::max(s.loudEnd, range) >= quiet;
      }
      if (over) {
        tailDone = true;
        return stop();
      }
    }
    return Ok{};
  }
  Status end() override { return Ok{}; }

private:
  static Status stop() { return Error{ErrorCode::Unknown, "export: pass stopped", {}}; }
};

// ---- runExport's files ----------------------------------------------------------------------------------------------

class WavFileSink final : public engine::AudioSink {
public:
  explicit WavFileSink(fs::path path) : path_(std::move(path)) {}
  Status begin(uint32_t ch, uint32_t rate, uint64_t) override {
    w_.enableRf64(longWavLimit());  // M2: pass 1 is always 32f (4 GiB is 46 min of stereo at 192 kHz)
    return w_.open(path_, ch, rate);
  }
  Status write(const float* const* ch, uint32_t frames) override { return w_.write(ch, frames); }
  Status end() override { return w_.close(); }

private:
  fs::path path_;
  media::WavWriter w_;
};

// EX.3: pass 2's output, a WAV (16 / 24-bit / 32f) or a FLAC (16 / 24-bit) file.
class OutFile {
public:
  Status open(const fs::path& path, uint32_t channels, uint32_t rate, ExportFormat format, ExportBits bits) {
    flac_ = format == ExportFormat::Flac;
    if (flac_) return flacW_.open(path, channels, rate, bits == ExportBits::Pcm16 ? 16u : 24u);
    wavW_.enableRf64(longWavLimit());  // M2
    return wavW_.open(path, channels, rate,
                      bits == ExportBits::Pcm16   ? media::WavFormat::Pcm16
                      : bits == ExportBits::Pcm24 ? media::WavFormat::Pcm24
                                                  : media::WavFormat::Float32);
  }
  Status write(const float* const* ch, uint32_t frames) {
    return flac_ ? flacW_.write(ch, frames) : wavW_.write(ch, frames);
  }
  Status close() { return flac_ ? flacW_.close() : wavW_.close(); }

private:
  bool flac_ = false;
  media::WavWriter wavW_;
  media::FlacWriter flacW_;
};

fs::path pathFromUtf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
// L3: a path for a message (path::string() throws on MSVC for characters outside the ANSI code page: stems are named
// after tracks, and a throw on the export worker is std::terminate).
std::string u8str(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// A file name part: no characters Windows refuses, no trailing dots or spaces, never empty.
std::string safeName(std::string s) {
  for (char& c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
        c == '*')
      c = '_';
  }
  while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  return s.empty() ? std::string("Track") : s;
}

struct Job {
  doc::TrackId track{};
  fs::path final, temp, part;
  uint32_t pass = 0;          // which render pass writes it
  bool silent = false;        // a track without an out bus: zeros, no temp
  PassTargetStats stats;
  uint32_t channels = 2;
};

void removeQuietly(const fs::path& p) {
  if (p.empty()) return;
  std::error_code ec;
  fs::remove(p, ec);
}

// Dither and quantize state of one channel (pass 2).
struct Ditherer {
  uint64_t rng = 0;
  double e1 = 0.0, e2 = 0.0, e3 = 0.0;  // the last quantization errors (shaped)
  double uniform() noexcept {           // [0, 1), xorshift64*
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return static_cast<double>((rng * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
  }
  // x (gain applied) → the value on the integer grid of `scale` steps per unit, clamped to full scale.
  float process(float x, double scale, ExportDither mode) noexcept {
    double v = static_cast<double>(x) * scale;
    if (v != v) v = 0.0;
    if (mode == ExportDither::Shaped) v -= 1.623 * e1 - 0.982 * e2 + 0.109 * e3;
    const double d = mode == ExportDither::None ? 0.0 : uniform() - uniform();  // TPDF, ±1 LSB
    const double q = std::clamp(std::nearbyint(v + d), -scale, scale - 1.0);
    if (mode == ExportDither::Shaped) {
      e3 = e2;
      e2 = e1;
      e1 = std::clamp(q - v, -4.0, 4.0);  // a clipped sample would feed back its whole overshoot
    }
    return static_cast<float>(q / scale);
  }
};

}  // namespace

// ---- The core -----------------------------------------------------------------------------------------------------

Result<std::unique_ptr<ProjectLink>> makeOfflineLink(const doc::Snapshot& snapshot, double rate,
                                                     const std::string& projectDir, uint64_t chanceSeed, bool jit,
                                                     std::vector<doc::TrackId> captureOwn) {
  if (!snapshot) return bad("no document");
  if (!(rate >= 8000.0 && rate <= 768000.0)) return bad("sample rate outside 8 kHz … 768 kHz");
  LinkConfig cfg;
  cfg.sampleRate = rate;
  cfg.compileThread = false;
  cfg.sleepAfterMs = 0.0f;  // an offline render never sleeps
  cfg.jit = jit;
  cfg.chanceSeed = chanceSeed;
  cfg.projectDir = projectDir;
  cfg.captureOwn = std::move(captureOwn);  // FZ.2
  auto link = ProjectLink::create(cfg, snapshot);
  if (!link) return link.error();
  if (jit && !(*link)->waitForJit()) return Error{ErrorCode::Overflow, "export: the JIT compile did not finish in time", {}};
  return std::move(link.value());
}

Result<PassResult> renderPass(ProjectLink& link, const doc::Project& project, const PassSpec& spec) {
  if (spec.to <= spec.from) return bad("empty range");
  if (spec.targets.empty()) return bad("nothing to render");
  const double rate = link.engine().config().sampleRate;
  const auto rateHz = static_cast<uint32_t>(std::lround(rate));
  const TempoMap& tempo = project.timeline->tempoMap;
  const int64_t range = tempo.ticksToSamples(spec.to, rate) - tempo.ticksToSamples(spec.from, rate);
  if (range <= 0) return bad("empty range");
  link.setLoop(views::TransportLoop{spec.from, spec.to, spec.loop});
  if (auto s = link.settleOffline(spec.from, kSettleMinSeconds, kSettleMaxSeconds); !s) return s.error();
  const int64_t lap = spec.loop ? range : 0;
  const int64_t tail = spec.loop ? 0 : std::max<int64_t>(0, std::llround(spec.tailMaxSeconds * rate));
  const auto limit = static_cast<uint64_t>(range + tail);
  const auto outLatency = static_cast<int64_t>(link.outputLatency());

  std::vector<std::unique_ptr<TrimSink>> trims;
  engine::BusCapture capture;
  PassSink main;
  main.spec = &spec;
  int64_t frames = 0;
  for (const PassTarget& t : spec.targets) {
    if (t.sink == nullptr) return bad("a target without a sink");
    const bool master = t.bus == engine::kNoBus;
    if (!master && (link.engine().program() == nullptr || t.bus >= link.engine().program()->layout().busWidth.size()))
      return bad("bus " + std::to_string(t.bus) + " is not in the layout");
    const int64_t shift = lap + (master ? outLatency : std::llround(link.busLatency(t.bus)));
    trims.push_back(std::make_unique<TrimSink>(*t.sink, shift, limit, rateHz));
    frames = std::max(frames, shift + static_cast<int64_t>(limit));
    main.all.push_back(trims.back().get());
    if (master) {
      if (main.master != nullptr) return bad("the master output twice");
      main.master = trims.back().get();
    } else {
      capture.buses.push_back(t.bus);
      capture.sinks.push_back(trims.back().get());
    }
  }
  main.total = static_cast<uint64_t>(std::max<int64_t>(frames, 0));
  main.range = static_cast<uint64_t>(range);
  main.quiet = static_cast<uint64_t>(std::llround(kTailQuietSeconds * rate));
  main.tail = tail > 0;
  auto r = link.renderOffline(spec.from, main.total, main, 0, capture.buses.empty() ? nullptr : &capture);
  Status ended = Ok{};
  for (const PassTarget& t : spec.targets)
    if (auto s = t.sink->end(); !s && ended) ended = s;
  if (!r && !main.cancelled && !main.tailDone) return r.error();
  if (!ended) return ended.error();
  PassResult out;
  out.rangeFrames = static_cast<uint64_t>(range);
  out.cancelled = main.cancelled;
  for (const auto& t : trims) out.targets.push_back(t->stats());
  return out;
}

// ---- Export Audio -------------------------------------------------------------------------------------------------

namespace {

std::atomic<uint64_t> gLongWavLimit{0};

struct Named {
  doc::TrackId track{};  // {} for the master
  fs::path file;
};
// The final files of `rq` over document `p` (names made safe, duplicates numbered); `why`: a bad track list.
std::vector<Named> namedFiles(const doc::Project& p, const ExportRequest& rq, std::string* why) {
  std::vector<Named> out;
  std::map<std::string, int> names;
  const std::string ext = rq.format == ExportFormat::Flac ? ".flac" : ".wav";
  auto add = [&](doc::TrackId id, const std::string& name) {
    std::string stem = safeName(name);
    if (const int n = ++names[stem]; n > 1) stem += " (" + std::to_string(n) + ")";
    out.push_back(Named{id, rq.folder / pathFromUtf8(stem + ext)});
  };
  if (rq.what == ExportWhat::Master) {
    add({}, rq.baseName);
    return out;
  }
  std::vector<const doc::Track*> tracks;
  if (rq.what == ExportWhat::Stems) {
    doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
      if (t.kind != doc::TrackKind::Master && t.active) tracks.push_back(&t);
    });
  } else {
    for (doc::TrackId id : rq.tracks) {
      const doc::Track* t = doc::findTrackPtr(p, id);
      if (t == nullptr) {
        if (why != nullptr) *why = "track " + std::to_string(id.value) + " is not in the project";
        return {};
      }
      tracks.push_back(t);
    }
  }
  if (tracks.empty()) {
    if (why != nullptr) *why = "no tracks to export";
    return {};
  }
  for (const doc::Track* t : tracks) add(t->id, rq.baseName.empty() ? t->name : rq.baseName + " " + t->name);
  return out;
}

}  // namespace

uint64_t longWavLimit() noexcept {
  const uint64_t v = gLongWavLimit.load(std::memory_order_relaxed);
  return v != 0 ? v : 0xFFFFFFFFull;
}
void setLongWavLimitForTests(uint64_t riffBytes) noexcept { gLongWavLimit.store(riffBytes, std::memory_order_relaxed); }

std::vector<fs::path> exportFiles(const ExportRequest& rq) {
  std::vector<fs::path> out;
  if (!rq.snapshot) return out;
  for (Named& n : namedFiles(*rq.snapshot, rq, nullptr)) out.push_back(std::move(n.file));
  return out;
}

Result<ExportReport> runExport(const ExportRequest& rq, std::atomic<float>* progress, const std::atomic<bool>* cancel) {
  if (!rq.snapshot) return bad("no document");
  if (rq.to <= rq.from) return bad("empty range");
  if (rq.folder.empty()) return bad("no folder");
  if (rq.what == ExportWhat::Master && rq.baseName.empty()) return bad("no file name");
  if (rq.format == ExportFormat::Flac && rq.bits == ExportBits::Float32) return bad("FLAC is 16 or 24-bit");
  auto report = [&](float f) {
    if (progress != nullptr) progress->store(f, std::memory_order_relaxed);
  };
  report(0.0f);

  // The export's own document: solo cleared (mute respected). Solo passes are dispatched on it.
  doc::DocumentStore store(rq.snapshot);
  {
    doc::Batch clear{"export: solo off", {}};
    doc::forEachTrack(*store.current(), [&](const doc::Track& t, const doc::Track*) {
      if (t.mixer.solo) clear.commands.push_back(doc::SetTrackSolo{t.id, false});
    });
    if (!clear.commands.empty())
      if (auto s = store.dispatch(clear); !s) return s.error();
  }

  // The files.
  std::vector<Job> jobs;
  {
    std::string why;
    std::vector<Named> named = namedFiles(*store.current(), rq, &why);
    if (!why.empty()) return bad(why);
    for (Named& n : named) {
      Job j;
      j.track = n.track;
      j.final = std::move(n.file);
      j.temp = fs::path(j.final).concat(L".pqexport.tmp");  // always a 32f WAV
      j.part = fs::path(j.final).concat(L".pqexport.part");
      jobs.push_back(std::move(j));
    }
  }
  const fs::path folder = rq.folder;

  auto cleanup = [&] {
    for (const Job& j : jobs) {
      removeQuietly(j.temp);
      removeQuietly(j.part);
    }
  };
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (!fs::is_directory(folder, ec)) return Error{ErrorCode::Io, "export: cannot create " + u8str(folder), {}};

  // Passes: one with every target, or (stems through the returns and the master) one solo pass per track — all on ONE
  // link: a solo pass is the track's solo written into the export's document (a param write on the link: the gates,
  // no recompile; D7), then the pass (settled first: QUESTIONS Q251, EX.x-fix-settle).
  const bool soloPasses = rq.what != ExportWhat::Master && rq.includeReturnsMaster;
  const uint32_t passes = soloPasses ? static_cast<uint32_t>(jobs.size()) : 1u;
  uint64_t rangeFrames = 0;
  std::unique_ptr<ProjectLink> link;
  {
    auto made = makeOfflineLink(store.current(), static_cast<double>(rq.rate), rq.projectDir, rq.chanceSeed, rq.jit);
    if (!made) {
      cleanup();
      return made.error();
    }
    link = std::move(made.value());
  }
  const doc::ListenerId listener = store.subscribe([&](const doc::StoreEvent& e) { link->onDocumentChange(e); });
  struct Unsubscribe {
    doc::DocumentStore& s;
    doc::ListenerId id;
    ~Unsubscribe() { s.unsubscribe(id); }
  } unsubscribe{store, listener};
  auto solo = [&](doc::TrackId t, bool on) -> Status {
    if (auto s = store.dispatch(doc::SetTrackSolo{t, on}); !s) return s;
    link->flush();
    return Ok{};
  };
  for (uint32_t p = 0; p < passes; ++p) {
    if (soloPasses)
      if (auto s = solo(jobs[p].track, true); !s) {
        cleanup();
        return s.error();
      }
    const doc::Snapshot doc = store.current();
    PassSpec spec;
    spec.from = rq.from;
    spec.to = rq.to;
    spec.loop = rq.renderAsLoop;
    spec.tailMaxSeconds = rq.renderAsLoop ? 0.0 : rq.tailMaxSeconds;
    spec.cancel = cancel;
    spec.progress = [&](uint64_t done, uint64_t total) {
      report(0.9f * (static_cast<float>(p) + static_cast<float>(done) / static_cast<float>(std::max<uint64_t>(total, 1))) /
             static_cast<float>(passes));
    };
    std::vector<std::unique_ptr<WavFileSink>> sinks;
    std::vector<Job*> inPass;
    if (soloPasses) {
      inPass.push_back(&jobs[p]);
      sinks.push_back(std::make_unique<WavFileSink>(jobs[p].temp));
      spec.targets.push_back({engine::kNoBus, sinks.back().get()});
    } else {
      for (Job& j : jobs) {
        const uint32_t bus = j.track.value == 0 ? engine::kNoBus : link->trackOutBus(j.track);
        if (j.track.value != 0 && bus == engine::kNoBus) {  // plays nothing (no out bus in the layout)
          j.silent = true;
          continue;
        }
        inPass.push_back(&j);
        sinks.push_back(std::make_unique<WavFileSink>(j.temp));
        spec.targets.push_back({bus, sinks.back().get()});
      }
    }
    if (spec.targets.empty()) continue;  // every track silent
    auto r = renderPass(*link, *doc, spec);
    sinks.clear();  // closed by renderPass (end); the writers go before their files are touched
    if (soloPasses && r && !r->cancelled)
      if (auto s = solo(jobs[p].track, false); !s) {
        cleanup();
        return s.error();
      }
    if (!r || r->cancelled) {
      cleanup();
      if (!r) return r.error();
      ExportReport rep;
      rep.cancelled = true;
      rep.passes = p + 1;
      return rep;
    }
    rangeFrames = r->rangeFrames;
    for (size_t i = 0; i < inPass.size(); ++i) {
      inPass[i]->stats = r->targets[i];
      inPass[i]->pass = p;
    }
  }
  link.reset();  // the second link's compile memory goes now (RB "Risks")
  if (rangeFrames == 0) {  // every track was silent: the range at the export rate
    const TempoMap& tempo = store.current()->timeline->tempoMap;
    rangeFrames = static_cast<uint64_t>(tempo.ticksToSamples(rq.to, rq.rate) - tempo.ticksToSamples(rq.from, rq.rate));
  }

  // The common length: the range, or with a tail up to the last frame above −90 dBFS of any file.
  uint64_t length = rangeFrames;
  if (!rq.renderAsLoop && rq.tailMaxSeconds > 0.0)
    for (const Job& j : jobs) length = std::max(length, j.stats.loudEnd);

  // Pass 2: normalize, dither, quantize into the part files.
  const bool isFloat = rq.bits == ExportBits::Float32;  // WAV only
  const double scale = rq.bits == ExportBits::Pcm16 ? 32768.0 : 8388608.0;
  const ExportDither dither = rq.bits == ExportBits::Float32 ? ExportDither::None : rq.dither;
  constexpr float kNormalizePeak = 0.96605088f;  // −0.3 dBFS
  ExportReport out;
  out.frames = length;
  out.passes = passes;
  constexpr uint32_t kChunk = 8192;
  for (size_t ji = 0; ji < jobs.size(); ++ji) {
    Job& j = jobs[ji];
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      cleanup();
      ExportReport rep;
      rep.cancelled = true;
      rep.passes = passes;
      return rep;
    }
    const float peak = j.silent ? 0.0f : j.stats.peak;
    const float gain = rq.normalize && peak > 0.0f ? kNormalizePeak / peak : 1.0f;
    ExportedFile f;
    f.path = j.final;
    f.track = j.track;
    f.peakDb = peak > 0.0f ? 20.0f * std::log10(peak) : -200.0f;
    f.gainDb = 20.0f * std::log10(gain);
    out.files.push_back(f);
    // 32f, no gain, the exact length: the temp file is the result.
    if (!j.silent && isFloat && gain == 1.0f && j.stats.frames == length) {
      if (fs::rename(j.temp, j.part, ec); ec) {
        cleanup();
        return Error{ErrorCode::Io, "export: cannot write " + u8str(j.part) + ": " + ec.message(), {}};
      }
      continue;
    }
    std::unique_ptr<media::Decoder> dec;
    if (!j.silent) {
      auto d = media::Decoder::open(j.temp);
      if (!d) {
        cleanup();
        return d.error();
      }
      dec = std::move(d.value());
      j.channels = dec->info().channels;
    }
    OutFile w;
    if (auto s = w.open(j.part, j.channels, rq.rate, rq.format, rq.bits); !s) {
      cleanup();
      return s.error();
    }
    std::vector<std::vector<float>> buf(j.channels, std::vector<float>(kChunk));
    std::vector<float*> ptr(j.channels);
    for (uint32_t c = 0; c < j.channels; ++c) ptr[c] = buf[c].data();
    std::vector<Ditherer> dith(j.channels);
    for (uint32_t c = 0; c < j.channels; ++c) dith[c].rng = 0x9E3779B97F4A7C15ull ^ ((ji + 1) * 0x100000001B3ull + c * 0xBF58476D1CE4E5B9ull);
    for (uint64_t at = 0; at < length;) {
      const auto n = static_cast<uint32_t>(std::min<uint64_t>(kChunk, length - at));
      uint64_t got = 0;
      if (dec) {
        auto rd = dec->read(ptr.data(), n);
        if (!rd) {
          cleanup();
          return rd.error();
        }
        got = *rd;
      }
      for (uint32_t c = 0; c < j.channels; ++c) {
        float* x = ptr[c];
        std::fill(x + got, x + n, 0.0f);  // past the pass's frames (a shorter solo pass): silence
        if (gain != 1.0f)
          for (uint32_t i = 0; i < n; ++i) x[i] *= gain;
        if (!isFloat && dither != ExportDither::None)
          for (uint32_t i = 0; i < n; ++i) x[i] = dith[c].process(x[i], scale, dither);
      }
      if (auto s = w.write(ptr.data(), n); !s) {
        cleanup();
        return s.error();
      }
      at += n;
      report(0.9f + 0.1f * (static_cast<float>(ji) + static_cast<float>(at) / static_cast<float>(length)) /
                        static_cast<float>(jobs.size()));
    }
    if (auto s = w.close(); !s) {
      cleanup();
      return s.error();
    }
    dec.reset();  // unmaps the temp file
    removeQuietly(j.temp);
  }
  if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
    cleanup();
    ExportReport rep;
    rep.cancelled = true;
    rep.passes = passes;
    return rep;
  }
  // Every part is complete: they become the files. L4: the files they replace are moved aside first and put back when
  // any rename fails (a file open in a player refuses it), so a failed export never leaves some of them replaced.
  std::vector<std::pair<fs::path, fs::path>> aside;  // a final name -> its old file, moved aside
  std::vector<fs::path> placed;                      // the finals renamed into place so far
  auto undo = [&] {
    std::error_code uec;
    for (const fs::path& f : placed) fs::remove(f, uec);
    for (const auto& [f, old] : aside) fs::rename(old, f, uec);
    cleanup();
  };
  for (const Job& j : jobs) {
    if (!fs::exists(j.final, ec)) continue;
    const fs::path old = fs::path(j.final).concat(L".pqexport.old");
    removeQuietly(old);
    if (fs::rename(j.final, old, ec); ec) {
      undo();
      return Error{ErrorCode::Io, "export: cannot replace " + u8str(j.final) + ": " + ec.message(), {}};
    }
    aside.emplace_back(j.final, old);
  }
  for (const Job& j : jobs) {
    if (fs::rename(j.part, j.final, ec); ec) {
      undo();
      return Error{ErrorCode::Io, "export: cannot write " + u8str(j.final) + ": " + ec.message(), {}};
    }
    placed.push_back(j.final);
  }
  for (const auto& [f, old] : aside) removeQuietly(old);
  report(1.0f);
  return out;
}

}  // namespace pq::app
