// pqc command line: argument parsing and the four commands.
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <ostream>

#include "pq/engine/wav_file.h"
#include "pq/pqc/pqc.h"

namespace pq::pqc {

namespace {

namespace fs = std::filesystem;

struct Args {
  std::string command, input, output, patch, notes, stage = "optimised", backend = "interp";
  double rate = 48000.0, seconds = 2.0, gainDb = 0.0;
  uint32_t block = 64, voices = 16;
  bool visual = false, keys = false;
  std::vector<std::pair<std::string, float>> params;
};

bool parseNum(std::string_view s, double& v) {
  if (s.empty()) return false;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return ec == std::errc() && p == s.data() + s.size() && std::isfinite(v);
}

bool parseUint(std::string_view s, uint32_t& v) {
  if (s.empty()) return false;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return ec == std::errc() && p == s.data() + s.size();
}

// Maps an error to the documented exit code.
int exitCodeFor(const Error& e) {
  switch (e.code) {
    case ErrorCode::Parse:
    case ErrorCode::Type: return kExitCompileError;
    case ErrorCode::Io: return kExitIo;
    case ErrorCode::InvalidArgument:
    case ErrorCode::NotFound: return kExitUsage;
    default: return kExitBackend;
  }
}

int fail(std::ostream& err, const Error& e) {
  err << "pqc: " << e.message << '\n';
  return exitCodeFor(e);
}

int usageError(std::ostream& err, const std::string& m) {
  err << "pqc: " << m << "\n(run `pqc help` for usage)\n";
  return kExitUsage;
}

// Writes text to -o or stdout.
int emit(const Args& a, const std::string& text, std::ostream& out, std::ostream& err) {
  if (a.output.empty()) {
    out << text;
    return kExitOk;
  }
  std::ofstream f(pathFromUtf8(a.output), std::ios::binary | std::ios::trunc);
  f << text;
  if (!f) return fail(err, Error{ErrorCode::Io, "cannot write " + a.output, {}});
  return kExitOk;
}

int cmdCheck(const Args& a, std::ostream& out, std::ostream& err) {
  auto src = SourceFile::load(pathFromUtf8(a.input));
  if (!src) return fail(err, src.error());
  std::vector<size_t> which;
  if (!a.patch.empty()) {
    auto k = (*src)->findPatch(a.patch);
    if (!k) return fail(err, k.error());
    which.push_back(*k);
  } else {
    for (size_t k = 0; k < (*src)->file().patches.size(); ++k) which.push_back(k);
    if (which.empty()) return fail(err, Error{ErrorCode::Type, (*src)->fileName() + " has no patch", {}});
  }
  CompileOptions co;
  co.sampleRate = a.rate;
  co.visualMode = a.visual;
  int failed = 0;
  for (size_t k : which) {
    auto c = compilePatch(**src, k, co);
    if (!c) {
      err << c.error().message << '\n';
      ++failed;
      continue;
    }
    for (const std::string& w : c->warnings) err << w << '\n';
    const graph::Schedule& s = c->schedule;
    out << "ok   " << c->patchName << ": " << c->optimiseStats.nodesBefore << " nodes -> " << c->optimiseStats.nodesAfter
        << " optimised, " << s.ops.size() << " ops in " << s.stages.size() << " stages, " << s.blockBuffers()
        << " block buffers, " << s.params.size() << " params, state " << s.state.globalBytes << " B global + "
        << s.state.voiceBytes << " B per voice group" << (c->warnings.empty() ? "" : " (with warnings)") << '\n';
  }
  if (failed > 0) {
    err << "pqc: " << failed << " of " << which.size() << " patch(es) failed\n";
    return kExitCompileError;
  }
  return kExitOk;
}

int cmdDumpIr(const Args& a, std::ostream& out, std::ostream& err) {
  auto src = SourceFile::load(pathFromUtf8(a.input));
  if (!src) return fail(err, src.error());
  auto k = (*src)->findPatch(a.patch);
  if (!k) return fail(err, k.error());
  if (a.stage == "parsed") return emit(a, graph::print((*src)->file().patches[*k]), out, err);
  CompileOptions co;
  co.sampleRate = a.rate;
  auto c = compilePatch(**src, *k, co);
  if (!c) return fail(err, c.error());
  return emit(a, graph::print(a.stage == "inferred" ? c->inferred.patch : c->optimised.patch), out, err);
}

int cmdDumpSchedule(const Args& a, std::ostream& out, std::ostream& err) {
  auto src = SourceFile::load(pathFromUtf8(a.input));
  if (!src) return fail(err, src.error());
  auto k = (*src)->findPatch(a.patch);
  if (!k) return fail(err, k.error());
  CompileOptions co;
  co.sampleRate = a.rate;
  co.visualMode = a.visual;
  auto c = compilePatch(**src, *k, co);
  if (!c) return fail(err, c.error());
  for (const std::string& w : c->warnings) err << w << '\n';
  return emit(a, graph::dump(c->schedule, c->optimised, a.keys), out, err);
}

// Streams into a WAV file.
class WavSink final : public engine::AudioSink {
public:
  explicit WavSink(fs::path path) : path_(std::move(path)) {}
  Status begin(uint32_t ch, uint32_t sr, uint64_t) override { return w_.open(path_, ch, sr); }
  Status write(const float* const* ch, uint32_t frames) override { return w_.write(ch, frames); }
  Status end() override { return w_.close(); }

private:
  fs::path path_;
  engine::WavWriter w_;
};

int cmdRender(const Args& a, std::ostream& out, std::ostream& err) {
  if (a.output.empty()) return usageError(err, "render needs -o <out.wav>");
  RenderOptions ro;
  ro.patch = a.patch;
  ro.compile.sampleRate = a.rate;
  ro.compile.visualMode = a.visual;
  ro.backend = a.backend == "jit" ? Backend::Jit : Backend::Interpreter;
  ro.seconds = a.seconds;
  ro.blockFrames = a.block;
  ro.maxVoices = a.voices;
  ro.notes = a.notes;
  ro.params = a.params;
  ro.gainDb = static_cast<float>(a.gainDb);
  WavSink sink(pathFromUtf8(a.output));
  auto r = renderFile(pathFromUtf8(a.input), ro, sink);
  if (!r) return fail(err, r.error());
  char line[512];
  std::snprintf(line, sizeof line,
                "rendered '%s' (%u ops, %s) -> %s: %llu frames, %u ch, %.0f Hz, block %u; %.3f s audio in %.3f s "
                "(%.1fx realtime; compile %.1f ms)\n",
                r->patchName.c_str(), r->ops, ro.backend == Backend::Jit ? "jit" : "interpreter", a.output.c_str(),
                static_cast<unsigned long long>(r->stats.frames), r->stats.numChannels, a.rate, a.block,
                static_cast<double>(r->stats.frames) / a.rate, r->stats.wallSeconds, r->stats.realtimeFactor,
                r->compileMs);
  out << line;
  if (r->voices) {
    std::snprintf(line, sizeof line, "voices: %zu notes scheduled, %llu note-ons, %llu ended, %llu steals\n", r->notes,
                  static_cast<unsigned long long>(r->voiceStats.noteOns),
                  static_cast<unsigned long long>(r->voiceStats.voicesEnded),
                  static_cast<unsigned long long>(r->voiceStats.steals));
    out << line;
  }
  return kExitOk;
}

}  // namespace

std::string usage() {
  return "usage: pqc <command> <file.pqp> [options]\n"
         "commands:\n"
         "  check          parse + infer + optimise + schedule; diagnostics on stderr, a summary line per patch\n"
         "                 (every patch of the file unless --patch is given)\n"
         "  dump-ir        print the patch IR as patch-language text (--stage parsed|inferred|optimised,\n"
         "                 default optimised: flattened, typed, optimised)\n"
         "  dump-schedule  print the schedule (stages, regions, ops, buffers, params, state layout; --keys adds\n"
         "                 the state keys)\n"
         "  render         render offline to a 32-bit float WAV (-o required)\n"
         "  help           this text\n"
         "options:\n"
         "  --patch NAME          patch of the file (default: the first)\n"
         "  --rate HZ             sample rate the program is specialised for (default 48000)\n"
         "  --visual              a delay-free cycle gets a z1 and a warning instead of an error\n"
         "  -o FILE               output file (render: required; dumps: default stdout)\n"
         "render:\n"
         "  --seconds S           length (default 2)\n"
         "  --notes \"...\"         notes for patches with voice stages (default \"60 64 67 72\"): a plain list of\n"
         "                        key numbers / <f>hz is the pqdaw --play arpeggio (one note per 0.25 s, 0.125 s\n"
         "                        long, repeated); pitch[@start][:length][*velocity] tokens are played once\n"
         "                        (times in s, or ms with the suffix), e.g. \"60@0:1 64@0.5:0.5*0.9 440hz@1:250ms\"\n"
         "  --backend interp|jit  ProgramImage backend (default interp)\n"
         "  --block N             fixed engine block in frames (default 64)\n"
         "  --voices N            polyphony (default 16)\n"
         "  --param NAME=VALUE    set a param (normalised units: seconds, Hz, linear gain); repeatable\n"
         "  --gain DB             output gain (default 0; no clipping)\n"
         "exit codes: 0 ok, 1 compile error, 2 usage error, 3 I/O error, 4 backend/render error\n";
}

int run(std::span<const std::string> args, std::ostream& out, std::ostream& err) {
  if (args.empty()) return usageError(err, "no command");
  Args a;
  a.command = args[0];
  if (a.command == "help" || a.command == "--help" || a.command == "-h") {
    out << usage();
    return kExitOk;
  }
  const bool render = a.command == "render";
  if (!render && a.command != "check" && a.command != "dump-ir" && a.command != "dump-schedule")
    return usageError(err, "unknown command '" + a.command + "'");
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& f = args[i];
    auto value = [&](std::string& dst) {
      if (i + 1 >= args.size()) return false;
      dst = args[++i];
      return true;
    };
    std::string v;
    if (f == "--patch") {
      if (!value(a.patch)) return usageError(err, "--patch needs a name");
    } else if (f == "-o" || f == "--output") {
      if (!value(a.output)) return usageError(err, "-o needs a file name");
    } else if (f == "--rate") {
      if (!value(v) || !parseNum(v, a.rate) || a.rate < 1000.0 || a.rate > 768000.0)
        return usageError(err, "--rate needs a sample rate in 1000..768000");
    } else if (f == "--visual") {
      a.visual = true;
    } else if (f == "--keys" && a.command == "dump-schedule") {
      a.keys = true;
    } else if (f == "--stage" && a.command == "dump-ir") {
      if (!value(a.stage) || (a.stage != "parsed" && a.stage != "inferred" && a.stage != "optimised"))
        return usageError(err, "--stage must be parsed, inferred or optimised");
    } else if (render && f == "--seconds") {
      if (!value(v) || !parseNum(v, a.seconds) || !(a.seconds > 0.0) || a.seconds > 3600.0)
        return usageError(err, "--seconds needs a length in (0, 3600]");
    } else if (render && f == "--notes") {
      if (!value(a.notes)) return usageError(err, "--notes needs a note list");
      if (auto n = parseNotes(a.notes); !n) return usageError(err, n.error().message);
    } else if (render && f == "--backend") {
      if (!value(a.backend) || (a.backend != "interp" && a.backend != "jit"))
        return usageError(err, "--backend must be interp or jit");
    } else if (render && f == "--block") {
      if (!value(v) || !parseUint(v, a.block) || a.block == 0 || a.block > 8192)
        return usageError(err, "--block needs a frame count in 1..8192");
    } else if (render && f == "--voices") {
      if (!value(v) || !parseUint(v, a.voices) || a.voices == 0 || a.voices > 256)
        return usageError(err, "--voices needs a count in 1..256");
    } else if (render && f == "--gain") {
      if (!value(v) || !parseNum(v, a.gainDb) || a.gainDb > 60.0 || a.gainDb < -200.0)
        return usageError(err, "--gain needs dB in -200..60");
    } else if (render && f == "--param") {
      double pv = 0.0;
      const size_t eq = value(v) ? v.find('=') : std::string::npos;
      if (eq == std::string::npos || eq == 0 || !parseNum(std::string_view(v).substr(eq + 1), pv))
        return usageError(err, "--param needs NAME=VALUE");
      a.params.emplace_back(v.substr(0, eq), static_cast<float>(pv));
    } else if (!f.empty() && f[0] == '-' && f.size() > 1) {
      return usageError(err, "unknown option '" + f + "' for " + a.command);
    } else if (a.input.empty()) {
      a.input = f;
    } else {
      return usageError(err, "unexpected argument '" + f + "'");
    }
  }
  if (a.input.empty()) return usageError(err, a.command + " needs a .pqp file");
  if (a.command == "check") return cmdCheck(a, out, err);
  if (a.command == "dump-ir") return cmdDumpIr(a, out, err);
  if (a.command == "dump-schedule") return cmdDumpSchedule(a, out, err);
  return cmdRender(a, out, err);
}

}  // namespace pq::pqc
