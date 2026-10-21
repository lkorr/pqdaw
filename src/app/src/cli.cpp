#include "pq/app/cli.h"

#include <charconv>

namespace pq::app {

namespace {
Error usageError(std::string msg) { return Error{ErrorCode::InvalidArgument, std::move(msg), {}}; }

template <class T>
bool parseNumber(std::string_view s, T& out) noexcept {
  T v{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
  out = v;
  return true;
}
}  // namespace

bool parseSize(std::string_view s, uint32_t& w, uint32_t& h) noexcept {
  const size_t x = s.find_first_of("xX");
  if (x == std::string_view::npos) return false;
  auto num = [](std::string_view t, uint32_t& out) {
    if (t.empty()) return false;
    const auto r = std::from_chars(t.data(), t.data() + t.size(), out);
    return r.ec == std::errc{} && r.ptr == t.data() + t.size() && out >= 1 && out <= 16384;
  };
  uint32_t pw = 0, ph = 0;
  if (!num(s.substr(0, x), pw) || !num(s.substr(x + 1), ph)) return false;
  w = pw;
  h = ph;
  return true;
}

Result<CliOptions> parseCli(std::span<const std::string_view> args) {
  CliOptions o;
  bool modeSet = false;
  std::string_view sineOnly;  // last flag seen that only --sine accepts
  auto setMode = [&](Mode m, std::string_view flag) -> Status {
    if (modeSet) return usageError("only one mode flag may be given (got another: " + std::string(flag) + ")");
    o.mode = m;
    modeSet = true;
    return Ok{};
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--help" || a == "-h") {
      if (Status s = setMode(Mode::Help, a); !s) return std::move(s).error();
    } else if (a == "--screenshot") {
      if (Status s = setMode(Mode::Screenshot, a); !s) return std::move(s).error();
      if (i + 2 >= args.size()) return usageError("--screenshot needs <scene> <out.png>");
      o.scene = std::string(args[++i]);
      o.outPath = std::string(args[++i]);
    } else if (a == "--window-selftest") {
      if (Status s = setMode(Mode::WindowSelfTest, a); !s) return std::move(s).error();
    } else if (a == "--size") {
      if (i + 1 >= args.size()) return usageError("--size needs WxH");
      if (!parseSize(args[++i], o.width, o.height)) return usageError("bad --size '" + std::string(args[i]) + "' (want WxH, 1..16384)");
    } else if (a == "--scene") {
      if (i + 1 >= args.size()) return usageError("--scene needs a name");
      o.scene = std::string(args[++i]);
    } else if (a == "--animate") {
      o.animate = true;
    } else if (a == "--list-devices") {
      if (Status s = setMode(Mode::ListDevices, a); !s) return std::move(s).error();
    } else if (a == "--sine") {
      if (Status s = setMode(Mode::Sine, a); !s) return std::move(s).error();
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.sineHz) || !(o.sineHz > 0.0))
        return usageError("--sine needs a frequency in Hz > 0");
    } else if (a == "--seconds") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.seconds) || !(o.seconds > 0.0) || o.seconds > 86400.0)
        return usageError("--seconds needs a duration in (0, 86400]");
      sineOnly = a;
    } else if (a == "--device") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.device))
        return usageError("--device needs a device id from --list-devices");
      sineOnly = a;
    } else if (a == "--rate") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.sampleRate) || o.sampleRate == 0)
        return usageError("--rate needs a sample rate in Hz");
      sineOnly = a;
    } else if (a == "--buffer") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.bufferFrames) || o.bufferFrames == 0)
        return usageError("--buffer needs a frame count >= 1");
      sineOnly = a;
    } else if (a == "--block") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.blockFrames) || o.blockFrames == 0 ||
          o.blockFrames > 4096)
        return usageError("--block needs a frame count in [1, 4096]");
      sineOnly = a;
    } else if (a == "--exclusive") {
      o.exclusive = true;
      sineOnly = a;
    } else if (a == "--asio") {
      o.asio = true;
    } else {
      return usageError("unknown argument '" + std::string(a) + "'");
    }
  }
  if (o.mode != Mode::Screenshot && o.mode != Mode::Window && (o.width != 0))
    return usageError("--size only applies to --screenshot and the window");
  if (!sineOnly.empty() && o.mode != Mode::Sine) return usageError(std::string(sineOnly) + " only applies to --sine");
  if (o.asio && o.mode != Mode::Sine && o.mode != Mode::ListDevices)
    return usageError("--asio only applies to --sine and --list-devices");
  return o;
}

std::string usage() {
  return "usage: pqdaw [options]\n"
         "  (no mode)                                open the window (test pattern for now)\n"
         "      --scene <name>                       scene to show (default test-pattern)\n"
         "      --size WxH                           initial window size\n"
         "      --animate                            redraw continuously (frame pacing demo); key A toggles\n"
         "  --screenshot <scene> <out.png> [--size WxH]  render a scene headless to a PNG (default 800x480)\n"
         "  --window-selftest                        scripted resize/minimise/restore run (exit 77 = no display)\n"
         "  --help                                   this text\n";
}

}  // namespace pq::app
