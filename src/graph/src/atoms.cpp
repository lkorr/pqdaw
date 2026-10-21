#include "pq/graph/atoms.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>

namespace pq::graph {

namespace {

using namespace atom_flags;

// Declared port types used by the table. Infer fields are decided by T1.3 (math follows its inputs).
constexpr PortType kR = portType(Kind::Real);
constexpr PortType kB = portType(Kind::Bool);
constexpr PortType kAny = portType(Kind::Infer);
constexpr PortType kRConst = portType(Kind::Real, Rate::Const, Poly::Global, kWidthMono);  // structural, e.g. max delay
constexpr PortType kRAudio = portType(Kind::Real, Rate::Audio);
constexpr PortType kBAudio = portType(Kind::Bool, Rate::Audio);
constexpr PortType kRMonoOut = portType(Kind::Infer, Rate::Infer, Poly::Infer, kWidthMono);
constexpr PortType kVoiceRA = portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono);
constexpr PortType kVoiceRP = portType(Kind::Real, Rate::Param, Poly::Voice, kWidthMono);
constexpr PortType kVoiceBA = portType(Kind::Bool, Rate::Audio, Poly::Voice, kWidthMono);
constexpr PortType kGlobalRA = portType(Kind::Real, Rate::Audio, Poly::Global, kWidthMono);
constexpr PortType kGlobalRP = portType(Kind::Real, Rate::Param, Poly::Global, kWidthMono);
constexpr PortType kGlobalBP = portType(Kind::Bool, Rate::Param, Poly::Global, kWidthMono);
constexpr PortType kGlobalRC = portType(Kind::Real, Rate::Const, Poly::Global, kWidthMono);
constexpr PortType kGlobalAudioN = portType(Kind::Real, Rate::Audio, Poly::Global, kWidthInfer);

// Reference pitch default (SPEC §15: C3 = 261.626 Hz), used by cents_to_hz / hz_to_cents when `ref` is unconnected.
constexpr float kRefHz = 261.626f;

// ---- port lists -------------------------------------------------------------------------------------------------
constexpr PortSpec pOutR[] = {{"out", kR}};
constexpr PortSpec pOutB[] = {{"out", kB}};
constexpr PortSpec pOutAny[] = {{"out", kAny}};
constexpr PortSpec pOutRAudio[] = {{"out", kRAudio}};
constexpr PortSpec pOutBAudio[] = {{"out", kBAudio}};
constexpr PortSpec pQ[] = {{"q", kBAudio}};

constexpr PortSpec pX[] = {{"x", kR}};
constexpr PortSpec pAB[] = {{"a", kR}, {"b", kR}};
constexpr PortSpec pAnyAB[] = {{"a", kAny}, {"b", kAny}};
constexpr PortSpec pBoolX[] = {{"x", kB}};
constexpr PortSpec pBoolAB[] = {{"a", kB}, {"b", kB}};

constexpr PortSpec pPhasor[] = {{"hz", kR, false, 0.0f, true}, {"reset", kB, true, 0.0f}};
constexpr PortSpec pSine[] = {{"phase", kR}};
constexpr PortSpec pWavetable[] = {{"phase", kR}, {"hz", kR, true, 0.0f}, {"table", kRConst, true, 0.0f}};
constexpr PortSpec pNoise[] = {{"seed", kRConst, true, 0.0f}};

constexpr PortSpec pClip[] = {{"x", kR}, {"lo", kR}, {"hi", kR}};
constexpr PortSpec pLerp[] = {{"a", kR}, {"b", kR}, {"t", kR}};
constexpr PortSpec pSelect[] = {{"cond", kB}, {"a", kAny}, {"b", kAny}};
constexpr PortSpec pCentsToHz[] = {{"cents", kR}, {"ref", kR, true, kRefHz}};
constexpr PortSpec pHzToCents[] = {{"hz", kR}, {"ref", kR, true, kRefHz}};
constexpr PortSpec pDb[] = {{"db", kR}};
constexpr PortSpec pGain[] = {{"gain", kR}};
constexpr PortSpec pGate[] = {{"gate", kB}};

constexpr PortSpec pSchmitt[] = {{"x", kR}, {"lo", kR}, {"hi", kR}};
constexpr PortSpec pSrff[] = {{"set", kB}, {"reset", kB}};
constexpr PortSpec pDff[] = {{"d", kB}, {"clk", kB}};
constexpr PortSpec pTff[] = {{"t", kB}, {"clk", kB}};
constexpr PortSpec pCounter[] = {
    {"up", kB}, {"down", kB, true, 0.0f}, {"reset", kB, true, 0.0f}, {"wrap", kRConst, true, 0.0f}};
constexpr PortSpec pClockdiv[] = {{"clk", kB}, {"div", kR}};
constexpr PortSpec pMux[] = {{"index", kR}, {"in", kAny}};
constexpr PortSpec pDemuxIn[] = {{"index", kR}, {"x", kAny}};
constexpr PortSpec pTimer[] = {{"trig", kB}, {"dur", kR}};

constexpr PortSpec pZ1[] = {{"x", kAny, false, 0.0f, true}, {"init", kRConst, true, 0.0f}};
constexpr PortSpec pDelay[] = {{"x", kR, false, 0.0f, true}, {"time", kR}, {"max", kRConst}};
constexpr PortSpec pSampleHold[] = {{"x", kR}, {"trig", kB}};
constexpr PortSpec pLatch[] = {{"x", kAny}, {"enable", kB}};

constexpr PortSpec pShaper[] = {{"x", kR}, {"table", kRConst, true, 0.0f}};
constexpr PortSpec pChannel[] = {{"x", kAny}, {"index", kRConst}};
constexpr PortSpec pChannelOut[] = {{"out", kRMonoOut}};
constexpr PortSpec pPack[] = {{"in", kAny}};

constexpr PortSpec pVoiceRA[] = {{"out", kVoiceRA}};
constexpr PortSpec pVoiceRP[] = {{"out", kVoiceRP}};
constexpr PortSpec pVoiceBA[] = {{"out", kVoiceBA}};
constexpr PortSpec pGlobalRA[] = {{"out", kGlobalRA}};
constexpr PortSpec pGlobalRP[] = {{"out", kGlobalRP}};
constexpr PortSpec pGlobalBP[] = {{"out", kGlobalBP}};
constexpr PortSpec pGlobalRC[] = {{"out", kGlobalRC}};
constexpr PortSpec pGlobalAudioN[] = {{"out", kGlobalAudioN}};

constexpr PortSpec pSampleRead[] = {
    {"sample", kRConst}, {"pos", kR}, {"loopStart", kR, true, 0.0f}, {"loopEnd", kR, true, 0.0f}};
constexpr PortSpec pSend[] = {{"x", kR}, {"bus", kRConst}};
constexpr PortSpec pClipPlayer[] = {{"clip", kRConst}};

constexpr std::span<const PortSpec> kNoPorts{};

// ---- signatures -------------------------------------------------------------------------------------------------
constexpr uint32_t kMath = kBroadcast;
constexpr uint32_t kMathC = kBroadcast | kCommutative;
constexpr uint32_t kFast = kBroadcast | kFastApprox;

using C = AtomCategory;

// Order is free (lookup goes through the sorted name index); grouped as in SPEC §7.2.
const AtomSignature kAtoms[] = {
    // Generators. phasor keeps a uint32 fixed-point phase (CLAUDE.md §3); its output is the phase before the
    // increment, so `hz` only affects the next sample (delayed) while `reset` acts immediately.
    {"phasor", C::Generator, pPhasor, pOutRAudio, 4, kStateful, 0, {}, "phase 0..1 at hz; reset (sync) restarts at 0"},
    {"sine", C::Generator, pSine, pOutR, 0, kMath, 0, {}, "sin(2π·phase) of a 0..1 phase"},
    {"sine_hz", C::Generator, pPhasor, pOutRAudio, 4, kStateful, 0, {}, "sine oscillator at hz (= sine(phasor(hz)))"},
    {"wavetable", C::Generator, pWavetable, pOutR, 4, kStateful | kBroadcast, 0, {},
     "band-limited table read by phase; mipmap chosen from hz, or from the phase increment when hz = 0"},
    {"noise", C::Generator, pNoise, pOutRAudio, 4, kStateful, 0, {}, "white noise in −1..1 (uint32 generator state)"},

    // Math (exact). Operators: + − * / % unary − map to add sub mul div mod neg.
    {"add", C::Math, pAB, pOutR, 0, kMathC, 0, {}, "a + b"},
    {"sub", C::Math, pAB, pOutR, 0, kMath, 0, {}, "a − b"},
    {"mul", C::Math, pAB, pOutR, 0, kMathC, 0, {}, "a · b"},
    {"div", C::Math, pAB, pOutR, 0, kMath, 0, {}, "a / b"},
    {"mod", C::Math, pAB, pOutR, 0, kMath, 0, {}, "a − b·floor(a/b) (result has the sign of b)"},
    {"neg", C::Math, pX, pOutR, 0, kMath, 0, {}, "−x"},
    {"min", C::Math, pAB, pOutR, 0, kMathC, 0, {}, "min(a, b)"},
    {"max", C::Math, pAB, pOutR, 0, kMathC, 0, {}, "max(a, b)"},
    {"clip", C::Math, pClip, pOutR, 0, kMath, 0, {}, "clamp x to [lo, hi]"},
    {"abs", C::Math, pX, pOutR, 0, kMath, 0, {}, "|x|"},
    {"sign", C::Math, pX, pOutR, 0, kMath, 0, {}, "−1, 0 or 1"},
    {"floor", C::Math, pX, pOutR, 0, kMath, 0, {}, "largest integer ≤ x"},
    {"frac", C::Math, pX, pOutR, 0, kMath, 0, {}, "x − floor(x)"},
    {"tan", C::Math, pX, pOutR, 0, kMath, 0, {}, "tan(x)"},
    {"tanh", C::Math, pX, pOutR, 0, kMath, 0, {}, "tanh(x)"},
    {"exp", C::Math, pX, pOutR, 0, kMath, 0, {}, "e^x"},
    {"log", C::Math, pX, pOutR, 0, kMath, 0, {}, "natural log"},
    {"pow", C::Math, pAB, pOutR, 0, kMath, 0, {}, "a^b"},
    {"sqrt", C::Math, pX, pOutR, 0, kMath, 0, {}, "√x"},
    {"sin", C::Math, pX, pOutR, 0, kMath, 0, {}, "sin(x), x in radians"},
    // Math (fast approximations; error bounds documented with the core fast-math functions, T0.4).
    {"fast_tan", C::Math, pX, pOutR, 0, kFast, 0, "tan", "approximate tan(x)"},
    {"fast_tanh", C::Math, pX, pOutR, 0, kFast, 0, "tanh", "approximate tanh(x)"},
    {"fast_exp", C::Math, pX, pOutR, 0, kFast, 0, "exp", "approximate e^x"},
    {"fast_log", C::Math, pX, pOutR, 0, kFast, 0, "log", "approximate natural log"},
    {"fast_pow", C::Math, pAB, pOutR, 0, kFast, 0, "pow", "approximate a^b"},
    {"fast_sqrt", C::Math, pX, pOutR, 0, kFast, 0, "sqrt", "approximate √x"},
    {"fast_sin", C::Math, pX, pOutR, 0, kFast, 0, "sin", "approximate sin(x)"},
    // Conversions and selection.
    {"cents_to_hz", C::Math, pCentsToHz, pOutR, 0, kMath, 0, {}, "ref · 2^(cents/1200)"},
    {"hz_to_cents", C::Math, pHzToCents, pOutR, 0, kMath, 0, {}, "1200 · log2(hz/ref)"},
    {"db_to_gain", C::Math, pDb, pOutR, 0, kMath, 0, {}, "10^(db/20)"},
    {"gain_to_db", C::Math, pGain, pOutR, 0, kMath, 0, {}, "20 · log10(gain)"},
    {"select", C::Math, pSelect, pOutAny, 0, kMath, 0, {}, "cond ? a : b"},
    {"lerp", C::Math, pLerp, pOutR, 0, kMath, 0, {}, "a + (b − a)·t"},
    {"gate_to_real", C::Math, pGate, pOutR, 0, kMath, 0, {}, "false → 0, true → 1"},

    // Shaping.
    {"shaper", C::Shaping, pShaper, pOutR, 0, kMath, 0, {}, "transfer curve: table read by input value"},

    // Memory. delay*: time in seconds, max (const) sizes the buffer; the read is clamped to ≥ 1 sample, which is
    // what makes x a delayed input.
    {"z1", C::Memory, pZ1, pOutAny, 4, kStateful, 0, {}, "one-sample delay"},
    {"delay", C::Memory, pDelay, pOutR, 8, kStateful | kVariableState | kBroadcast, 0, {}, "delay line, linear read"},
    {"delay_allpass", C::Memory, pDelay, pOutR, 12, kStateful | kVariableState | kBroadcast, 0, {},
     "delay line, allpass-interpolated read"},
    {"delay_cubic", C::Memory, pDelay, pOutR, 8, kStateful | kVariableState | kBroadcast, 0, {},
     "delay line, cubic read"},
    {"samplehold", C::Memory, pSampleHold, pOutR, 4, kStateful | kBroadcast, 0, {}, "x sampled on each true trig"},
    {"latch", C::Memory, pLatch, pOutAny, 4, kStateful, 0, {}, "x while enable, else the last value"},

    // Logic. Comparators (real → bool): < <= > >= == != map to lt le gt ge eq ne; && || ^^ ! map to and or xor not.
    {"lt", C::Logic, pAB, pOutB, 0, kMath, 0, {}, "a < b"},
    {"le", C::Logic, pAB, pOutB, 0, kMath, 0, {}, "a ≤ b"},
    {"gt", C::Logic, pAB, pOutB, 0, kMath, 0, {}, "a > b"},
    {"ge", C::Logic, pAB, pOutB, 0, kMath, 0, {}, "a ≥ b"},
    {"eq", C::Logic, pAnyAB, pOutB, 0, kMathC, 0, {}, "a == b (both real or both bool)"},
    {"ne", C::Logic, pAnyAB, pOutB, 0, kMathC, 0, {}, "a != b (both real or both bool)"},
    {"and", C::Logic, pBoolAB, pOutB, 0, kMathC, 0, {}, "a ∧ b"},
    {"or", C::Logic, pBoolAB, pOutB, 0, kMathC, 0, {}, "a ∨ b"},
    {"xor", C::Logic, pBoolAB, pOutB, 0, kMathC, 0, {}, "a ⊕ b"},
    {"nand", C::Logic, pBoolAB, pOutB, 0, kMathC, 0, {}, "¬(a ∧ b)"},
    {"nor", C::Logic, pBoolAB, pOutB, 0, kMathC, 0, {}, "¬(a ∨ b)"},
    {"not", C::Logic, pBoolX, pOutB, 0, kMath, 0, {}, "¬x"},
    {"schmitt", C::Logic, pSchmitt, pOutBAudio, 4, kStateful | kBroadcast, 0, {},
     "true above hi, false below lo, else unchanged"},
    {"rise", C::Logic, pBoolX, pOutBAudio, 4, kStateful, 0, {}, "true for one sample when x goes false → true"},
    {"fall", C::Logic, pBoolX, pOutBAudio, 4, kStateful, 0, {}, "true for one sample when x goes true → false"},
    {"change", C::Logic, pBoolX, pOutBAudio, 4, kStateful, 0, {}, "true for one sample when x changes"},
    // Flip-flops are srff/dff/tff, not sr/d/t: `sr` is the sample-rate constant (PATCH_LANGUAGE §3).
    {"srff", C::Logic, pSrff, pQ, 4, kStateful, 0, {}, "set/reset flip-flop (reset wins)"},
    {"dff", C::Logic, pDff, pQ, 4, kStateful, 0, {}, "D flip-flop: q = d on each rising clk"},
    {"tff", C::Logic, pTff, pQ, 4, kStateful, 0, {}, "T flip-flop: q toggles on each rising clk while t"},
    {"counter", C::Logic, pCounter, pOutRAudio, 8, kStateful, 0, {},
     "+1 on rising up, −1 on rising down, 0 on reset; wraps to 0 at wrap when wrap > 0"},
    {"clockdiv", C::Logic, pClockdiv, pOutBAudio, 8, kStateful, 0, {}, "one true clk in every div passes"},
    {"mux", C::Logic, pMux, pOutAny, 0, kVariadicInputs, 2, {}, "in[clamp(floor(index))]"},
    {"demux", C::Logic, pDemuxIn, pOutAny, 0, kVariadicOutputs, 2, {}, "x to out[floor(index)], others 0/false"},
    {"pulse", C::Logic, kNoPorts, pOutBAudio, 4, kStateful, 0, {},
     "true on the first sample the instance runs (program start, voice start)"},
    {"timer", C::Logic, pTimer, pOutBAudio, 8, kStateful, 0, {}, "true for dur seconds after each rising trig"},

    // Structure: x[i] is channel(x, i); a tuple (a, b, …) is pack(a, b, …).
    {"channel", C::Structure, pChannel, pChannelOut, 0, 0, 0, {}, "channel i of a multichannel signal"},
    {"pack", C::Structure, pPack, pOutAny, 0, kVariadicInputs, 2, {}, "channels side by side (a, b, …)"},

    // Voice sources (SPEC §7.2 Events and voices).
    {"voice.hz", C::Voice, kNoPorts, pVoiceRA, 0, kNeedsVoice, 0, {}, "voice frequency, float32 Hz"},
    {"voice.cents", C::Voice, kNoPorts, pVoiceRA, 0, kNeedsVoice, 0, {}, "voice pitch in cents from the reference"},
    {"voice.gate", C::Voice, kNoPorts, pVoiceBA, 0, kNeedsVoice, 0, {}, "true while the note is held"},
    {"voice.trigger", C::Voice, kNoPorts, pVoiceBA, 0, kNeedsVoice, 0, {}, "true for one sample at note start"},
    {"voice.velocity", C::Voice, kNoPorts, pVoiceRP, 0, kNeedsVoice, 0, {}, "note-on velocity 0..1"},
    {"voice.release", C::Voice, kNoPorts, pVoiceRP, 0, kNeedsVoice, 0, {}, "note-off velocity 0..1"},
    {"voice.ratio", C::Voice, kNoPorts, pVoiceRP, 0, kNeedsVoice, 0, {}, "pitch as a ratio of its anchor"},
    {"voice.harmonic", C::Voice, kNoPorts, pVoiceRP, 0, kNeedsVoice, 0, {}, "harmonic number for harmonic pitches"},
    {"voice.unisonCount", C::Voice, kNoPorts, pVoiceRP, 0, kNeedsVoice, 0, {}, "number of unison voices"},
    {"voice.index", C::Voice, kNoPorts, pVoiceRP, 0, kNeedsVoice, 0, {}, "index within the unison group"},

    // Transport and sample rate.
    {"transport.bpm", C::Transport, kNoPorts, pGlobalRP, 0, kNeedsTransport, 0, {}, "tempo in BPM"},
    {"transport.beat", C::Transport, kNoPorts, pGlobalRA, 0, kNeedsTransport, 0, {}, "position in beats"},
    {"transport.barPhase", C::Transport, kNoPorts, pGlobalRA, 0, kNeedsTransport, 0, {}, "phase within the bar 0..1"},
    {"transport.playing", C::Transport, kNoPorts, pGlobalBP, 0, kNeedsTransport, 0, {}, "true while playing"},
    {"sr", C::Transport, kNoPorts, pGlobalRC, 0, 0, 0, {}, "sample rate (const after specialisation)"},

    // Samples and I/O.
    {"sampleRead", C::Sample, pSampleRead, pOutAny, 0, 0, 0, {},
     "interpolated read of sample buffer `sample` at frame position pos, looping when loopEnd > loopStart"},
    {"track.in", C::Io, kNoPorts, pGlobalAudioN, 0, 0, 0, {}, "the track's input"},
    {"sidechain.in", C::Io, kNoPorts, pGlobalAudioN, 0, 0, 0, {}, "the device's sidechain input"},
    {"send", C::Io, pSend, kNoPorts, 0, kSideEffect | kBroadcast, 0, {}, "add x into bus"},
    {"clipPlayer", C::Io, pClipPlayer, pGlobalAudioN, 0, kStateful | kVariableState | kNeedsTransport, 0, {},
     "native warp-engine playback of clip (SPEC §9.4)"},
};

constexpr uint32_t kAtomCount = static_cast<uint32_t>(std::size(kAtoms));
static_assert(kAtomCount < 0xFFFF, "AtomId is 16-bit");

std::array<AtomId, kAtomCount> sortedIds() {
  std::array<AtomId, kAtomCount> ids{};
  for (uint32_t i = 0; i < kAtomCount; ++i) ids[i] = static_cast<AtomId>(i);
  std::sort(ids.begin(), ids.end(), [](AtomId a, AtomId b) { return kAtoms[a].name < kAtoms[b].name; });
  return ids;
}

}  // namespace

std::string_view toText(AtomCategory c) noexcept {
  switch (c) {
    case AtomCategory::Generator: return "generator";
    case AtomCategory::Math: return "math";
    case AtomCategory::Shaping: return "shaping";
    case AtomCategory::Memory: return "memory";
    case AtomCategory::Logic: return "logic";
    case AtomCategory::Structure: return "structure";
    case AtomCategory::Voice: return "voice";
    case AtomCategory::Transport: return "transport";
    case AtomCategory::Sample: return "sample";
    case AtomCategory::Io: return "io";
  }
  return "?";
}

uint32_t AtomSignature::fixedInputs() const noexcept {
  const auto n = static_cast<uint32_t>(inputs.size());
  return has(atom_flags::kVariadicInputs) ? n - 1 : n;
}

uint32_t AtomSignature::fixedOutputs() const noexcept {
  const auto n = static_cast<uint32_t>(outputs.size());
  return has(atom_flags::kVariadicOutputs) ? n - 1 : n;
}

AtomRegistry::AtomRegistry() : atoms_(kAtoms) {
  static const std::array<AtomId, kAtomCount> ids = sortedIds();
  byName_ = ids;
#ifndef NDEBUG
  for (uint32_t i = 1; i < kAtomCount; ++i) assert(kAtoms[ids[i - 1]].name != kAtoms[ids[i]].name && "duplicate atom");
#endif
}

const AtomRegistry& AtomRegistry::builtin() {
  static const AtomRegistry registry;
  return registry;
}

std::optional<AtomId> AtomRegistry::find(std::string_view name) const noexcept {
  const auto it = std::lower_bound(byName_.begin(), byName_.end(), name,
                                   [this](AtomId id, std::string_view n) { return atoms_[id].name < n; });
  if (it != byName_.end() && atoms_[*it].name == name) return *it;
  return std::nullopt;
}

AtomId atomId(std::string_view name) noexcept {
  const auto id = AtomRegistry::builtin().find(name);
  // A literal atom name that does not exist is a programming error, never a user error (user text goes through
  // find()), so stop instead of returning some other atom.
  if (!id) std::abort();
  return *id;
}

const AtomSignature& atomSignature(AtomId id) noexcept { return AtomRegistry::builtin().get(id); }

}  // namespace pq::graph
