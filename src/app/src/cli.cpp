#include "pq/app/cli.h"

#include <charconv>

#include "pq/views/view_scenes.h"

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
  std::string_view sineOnly;    // last flag seen that only --sine accepts
  std::string_view streamOnly;  // last flag seen that only the stream modes (--sine, --latency-test) accept
  bool backendFlag = false;
  bool sceneFlag = false;
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
      sceneFlag = true;
    } else if (a == "--scale") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.scale) || !(o.scale >= 0.5f && o.scale <= 4.0f))
        return usageError("--scale needs a UI scale in [0.5, 4] (e.g. 1.25 for 125 %)");
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
      streamOnly = a;
    } else if (a == "--device") {
      if (i + 1 >= args.size()) return usageError("--device needs a device id or endpoint id from --list-devices");
      const std::string_view d = args[++i];
      if (!d.empty() && d.front() == '{')
        o.deviceEndpoint = std::string(d);
      else if (!parseNumber(d, o.device))
        return usageError("--device needs a device id or endpoint id ({...}) from --list-devices");
      streamOnly = a;
    } else if (a == "--rate") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.sampleRate) || o.sampleRate == 0)
        return usageError("--rate needs a sample rate in Hz");
      sineOnly = a;
    } else if (a == "--buffer") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.bufferFrames) || o.bufferFrames == 0)
        return usageError("--buffer needs a frame count >= 1");
      streamOnly = a;
    } else if (a == "--block") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.blockFrames) || o.blockFrames == 0 ||
          o.blockFrames > 4096)
        return usageError("--block needs a frame count in [1, 4096]");
      sineOnly = a;
    } else if (a == "--exclusive") {
      o.exclusive = true;
      sineOnly = a;
    } else if (a == "--level") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.levelDb) || !(o.levelDb >= -120.0 && o.levelDb <= 0.0))
        return usageError("--level needs a level in dBFS in [-120, 0]");
      streamOnly = a;
    } else if (a == "--inject-stall") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.stallAfterWake) || o.stallAfterWake == 0)
        return usageError("--inject-stall needs a device wake-up number >= 1");
      sineOnly = a;
    } else if (a == "--backend") {
      if (i + 1 >= args.size()) return usageError("--backend needs wasapi or rtaudio");
      const std::string_view b = args[++i];
      if (b == "wasapi")
        o.wasapi = WasapiImpl::Own;
      else if (b == "rtaudio")
        o.wasapi = WasapiImpl::RtAudio;
      else
        return usageError("--backend needs wasapi or rtaudio");
      backendFlag = true;
    } else if (a == "--play") {
      if (Status s = setMode(Mode::Play, a); !s) return std::move(s).error();
      if (i + 1 >= args.size() || args[i + 1].empty()) return usageError("--play needs a .pqp file");
      o.playPath = std::string(args[++i]);
    } else if (a == "--patch") {
      if (i + 1 >= args.size() || args[i + 1].empty()) return usageError("--patch needs a patch name");
      o.patchName = std::string(args[++i]);
    } else if (a == "--latency-test") {
      if (Status s = setMode(Mode::LatencyTest, a); !s) return std::move(s).error();
    } else if (a == "--asio") {
      o.asio = true;
    } else if (a == "--project") {
      if (i + 1 >= args.size() || args[i + 1].empty()) return usageError("--project needs a project directory");
      o.project = std::string(args[++i]);
    } else if (a == "--idle-check") {
      if (i + 1 >= args.size() || !parseNumber(args[++i], o.idleCheckSeconds) || !(o.idleCheckSeconds > 0.0) ||
          o.idleCheckSeconds > 3600.0)
        return usageError("--idle-check needs a duration in seconds in (0, 3600]");
    } else {
      return usageError("unknown argument '" + std::string(a) + "'");
    }
  }
  if (o.mode != Mode::Screenshot && o.mode != Mode::Window && (o.width != 0))
    return usageError("--size only applies to --screenshot and the window");
  if (o.mode != Mode::Screenshot && o.mode != Mode::Window && o.scale != 0.0f)
    return usageError("--scale only applies to --screenshot and the window");
  const bool play = o.mode == Mode::Play;
  if (!sineOnly.empty() && o.mode != Mode::Sine && !(play && sineOnly != "--inject-stall"))
    return usageError(std::string(sineOnly) + " only applies to --sine");
  if (!streamOnly.empty() && o.mode != Mode::Sine && o.mode != Mode::LatencyTest && !play)
    return usageError(std::string(streamOnly) + " only applies to --sine, --play and --latency-test");
  if (backendFlag && o.mode != Mode::Sine && o.mode != Mode::ListDevices && !play)
    return usageError("--backend only applies to --sine, --play and --list-devices");
  if (!o.patchName.empty() && !play) return usageError("--patch only applies to --play");
  if (o.stallAfterWake && (o.asio || o.wasapi != WasapiImpl::Own))
    return usageError("--inject-stall needs the own WASAPI backend (--backend wasapi)");
  if (o.asio && o.mode != Mode::Sine && o.mode != Mode::ListDevices && !play)
    return usageError("--asio only applies to --sine, --play and --list-devices");
  // T3.7: the DAW shell. --project picks the shell in the window unless another view scene is named.
  if (!o.project.empty() || o.idleCheckSeconds > 0.0) {
    if (o.mode == Mode::Window && !sceneFlag) o.scene = "shell";
    if (!o.project.empty() && o.mode != Mode::Window && o.mode != Mode::Screenshot)
      return usageError("--project only applies to the window and --screenshot");
    if (o.idleCheckSeconds > 0.0 && o.mode != Mode::Window) return usageError("--idle-check only applies to the window");
    if ((o.mode == Mode::Window || o.mode == Mode::Screenshot) && !pq::views::isViewScene(o.scene))
      return usageError("--project and --idle-check need a view scene (e.g. shell), not '" + o.scene + "'");
  }
  return o;
}

std::string usage() {
  return "usage: pqdaw [options]\n"
         "  (no mode)                                open the window (test pattern for now)\n"
         "      --project <dir>                      open the DAW (scene shell) with this project\n"
         "      --scene <name>                       scene to show (default test-pattern; shell = the DAW with\n"
         "                                           a new project unless --project is given)\n"
         "      --idle-check S                       shell: measure frames and CPU over S idle seconds; exit 0\n"
         "                                           only for 0 frames at < 0.1 % CPU\n"
         "      --size WxH                           initial window size\n"
         "      --scale S                            UI scale override (default: the display's scale)\n"
         "      --animate                            redraw continuously (frame pacing demo); key A toggles\n"
         "  --screenshot <scene> <out.png> [--size WxH] [--scale S] [--project <dir>]\n"
         "                                           render a scene headless to a PNG: WxH logical px\n"
         "                                           (default 800x480; view scenes such as shell 1280x800,\n"
         "                                           over --project or a new project) at UI scale S\n"
         "                                           (default 1), i.e. round(W*S) x round(H*S) device px\n"
         "  --window-selftest                        scripted resize/minimise/restore run (exit 77 = no display)\n"
         "  --list-devices [--backend wasapi|rtaudio] [--asio]\n"
         "                                           list audio devices (WASAPI ids and endpoint ids)\n"
         "  --sine <hz> [options]                    play a test tone and print callback/underrun stats:\n"
         "      --seconds S, --device <id|{endpoint}>, --backend wasapi|rtaudio, --asio, --exclusive,\n"
         "      --rate HZ, --buffer N (device frames), --block N (engine block), --level DBFS (default -24),\n"
         "      --inject-stall W (test: stall 3 periods after device wake-up W; expects 1 underrun)\n"
         "  --play <file.pqp> [--patch NAME] [options]\n"
         "                                           compile a patch (interpreter) and play its global outs on\n"
         "                                           the device at --level DBFS (default -24), clipped to\n"
         "                                           [-1, 1]; same stream options as --sine\n"
         "  --latency-test [--seconds S] [--device ..] [--buffer N] [--level DBFS]\n"
         "                                           shared-mode output latency vs WASAPI loopback capture\n"
         "  --help                                  this text\n";
}

}  // namespace pq::app
