// pqc --notes: a compact note list (tools/README.md "Notes").
//   notes    = note { sep note }            sep: spaces, tabs or commas
//   note     = pitch [ "@" time ] [ ":" time ] [ "*" velocity ]
//   pitch    = number                       12-TET key number (69 = A3 = 440 Hz, 60 = C3: Ableton naming, SPEC §3.1; fractions)
//            | number ( "hz" | "Hz" )       frequency
//   time     = number [ "s" | "ms" ]        seconds by default
//   velocity = number in [0, 1]
#include <charconv>
#include <cmath>

#include "pq/pqc/pqc.h"

namespace pq::pqc {

namespace {

Error notesError(const std::string& m) { return Error{ErrorCode::InvalidArgument, "--notes: " + m, {}}; }

bool parseDouble(std::string_view s, double& v) {
  if (s.empty()) return false;
  if (s.front() == '+') s.remove_prefix(1);
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  return ec == std::errc() && p == s.data() + s.size() && std::isfinite(v);
}

bool endsWith(std::string_view s, std::string_view suffix) {
  return s.size() > suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool parseTime(std::string_view s, double& seconds) {
  double scale = 1.0;
  if (endsWith(s, "ms")) {
    s.remove_suffix(2);
    scale = 1e-3;
  } else if (endsWith(s, "s")) {
    s.remove_suffix(1);
  }
  if (!parseDouble(s, seconds)) return false;
  seconds *= scale;
  return true;
}

}  // namespace

Result<NotesSpec> parseNotes(std::string_view text) {
  NotesSpec spec;
  bool timed = false;
  double prevStart = -kArpStepSeconds;
  auto sep = [](char c) { return c == ' ' || c == '\t' || c == ',' || c == '\n' || c == '\r'; };
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && sep(text[i])) ++i;
    if (i >= text.size()) break;
    size_t j = i;
    while (j < text.size() && !sep(text[j])) ++j;
    const std::string_view tok = text.substr(i, j - i);
    i = j;
    // Split into pitch / @start / :length / *velocity (each at most once, in this order).
    const size_t at = tok.find('@'), colon = tok.find(':'), star = tok.find('*');
    const size_t pitchEnd = std::min({at, colon, star, tok.size()});
    auto part = [&](size_t from, size_t a, size_t b) {
      if (from == std::string_view::npos) return std::string_view();
      const size_t end = std::min({a, b, tok.size()});
      return end > from ? tok.substr(from + 1, end - from - 1) : std::string_view();
    };
    if ((at != std::string_view::npos && ((colon != std::string_view::npos && colon < at) || (star != std::string_view::npos && star < at))) ||
        (colon != std::string_view::npos && star != std::string_view::npos && star < colon) ||
        tok.find('@', at == std::string_view::npos ? 0 : at + 1) != std::string_view::npos ||
        tok.find(':', colon == std::string_view::npos ? 0 : colon + 1) != std::string_view::npos ||
        tok.find('*', star == std::string_view::npos ? 0 : star + 1) != std::string_view::npos)
      return notesError("'" + std::string(tok) + "': want pitch[@start][:length][*velocity]");
    std::string_view p = tok.substr(0, pitchEnd);
    const bool isHz = endsWith(p, "hz") || endsWith(p, "Hz");
    if (isHz) p.remove_suffix(2);
    double v = 0.0;
    if (!parseDouble(p, v))
      return notesError("bad pitch '" + std::string(tok.substr(0, pitchEnd)) + "' (want a key number or <f>hz)");
    const double hz = isHz ? v : 440.0 * std::exp2((v - 69.0) / 12.0);
    if (!(hz >= 1.0 && hz <= 20000.0))
      return notesError("pitch '" + std::string(tok.substr(0, pitchEnd)) + "' is outside 1 Hz .. 20 kHz");
    double start = prevStart + kArpStepSeconds, length = kArpGateSeconds, vel = kDefaultVelocity;
    if (at != std::string_view::npos) {
      timed = true;
      if (!parseTime(part(at, colon, star), start) || start < 0.0)
        return notesError("bad start in '" + std::string(tok) + "' (want seconds ≥ 0, e.g. @0.5 or @250ms)");
    }
    if (colon != std::string_view::npos) {
      timed = true;
      if (!parseTime(part(colon, star, std::string_view::npos), length) || !(length > 0.0))
        return notesError("bad length in '" + std::string(tok) + "' (want seconds > 0, e.g. :1 or :120ms)");
    }
    if (star != std::string_view::npos) {
      timed = true;
      if (!parseDouble(part(star, std::string_view::npos, std::string_view::npos), vel) || vel < 0.0 || vel > 1.0)
        return notesError("bad velocity in '" + std::string(tok) + "' (want 0..1)");
    }
    spec.hz.push_back(static_cast<float>(hz));
    spec.start.push_back(start);
    spec.length.push_back(length);
    spec.velocity.push_back(vel);
    prevStart = start;
  }
  if (spec.hz.empty()) return notesError("needs at least one pitch");
  spec.arpeggio = !timed;
  return spec;
}

std::vector<engine::TimedNote> timedNotes(const NotesSpec& spec, double sampleRate, uint64_t frames) {
  std::vector<engine::TimedNote> out;
  auto toFrames = [&](double s) { return static_cast<int64_t>(std::llround(s * sampleRate)); };
  if (spec.arpeggio) {
    // Exactly `pqdaw --play`'s Arpeggio: step = max(2, round(0.25 s · sr)), gate = step / 2.
    const int64_t step = std::max<int64_t>(2, toFrames(kArpStepSeconds));
    const int64_t gate = step / 2;
    for (int64_t k = 0; k * step < static_cast<int64_t>(frames); ++k) {
      engine::TimedNote n;
      n.start = k * step;
      n.length = gate;
      n.hz = spec.hz[static_cast<size_t>(k) % spec.hz.size()];
      n.velocity = kDefaultVelocity;
      out.push_back(n);
    }
    return out;
  }
  for (size_t i = 0; i < spec.hz.size(); ++i) {
    engine::TimedNote n;
    n.start = toFrames(spec.start[i]);
    n.length = std::max<int64_t>(1, toFrames(spec.length[i]));
    n.hz = spec.hz[i];
    n.velocity = static_cast<float>(spec.velocity[i]);
    out.push_back(n);
  }
  return out;
}

}  // namespace pq::pqc
