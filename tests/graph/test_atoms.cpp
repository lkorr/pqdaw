#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <set>
#include <string>
#include <string_view>

#include "pq/dsp/atom_state.h"
#include "pq/graph/atoms.h"

using namespace pq::graph;

namespace {
const AtomRegistry& reg() { return AtomRegistry::builtin(); }

const AtomSignature& sig(std::string_view name) {
  const auto id = reg().find(name);
  REQUIRE_MESSAGE(id.has_value(), "missing atom ", std::string(name));
  return reg().get(*id);
}
}  // namespace

TEST_CASE("atoms: every SPEC §7.2 leaf atom and every language operator is registered") {
  // SPEC §7.2 (flip-flops are srff/dff/tff because `sr` is the sample-rate constant), plus the operator atoms of
  // PATCH_LANGUAGE §2 and the voice/transport namespaces of §3. Q14 (T1.3) added the kernels' fast_ conversions and
  // muladd, split sampleRead into the kernels' sample_read_* / sample_pos_*, and added the voice reducers (§7.4).
  const char* names[] = {
      "phasor", "sine", "sine_hz", "wavetable", "noise",
      "add", "sub", "mul", "div", "mod", "neg", "min", "max", "clip", "abs", "sign", "floor", "frac",
      "tan", "tanh", "exp", "log", "pow", "sqrt", "sin",
      "fast_tan", "fast_tanh", "fast_exp", "fast_log", "fast_pow", "fast_sqrt", "fast_sin",
      "cents_to_hz", "hz_to_cents", "db_to_gain", "gain_to_db", "select", "lerp", "gate_to_real",
      "fast_cents_to_hz", "fast_hz_to_cents", "fast_db_to_gain", "fast_gain_to_db", "muladd",
      "shaper",
      "z1", "delay", "delay_allpass", "delay_cubic", "samplehold", "latch",
      "lt", "le", "gt", "ge", "eq", "ne", "and", "or", "xor", "nand", "nor", "not",
      "schmitt", "rise", "fall", "change", "srff", "dff", "tff", "counter", "clockdiv", "mux", "demux",
      "pulse", "timer",
      "channel", "pack", "voice_sum", "voice_last", "voice_highest", "voice_lowest", "voice_loudest",
      "voice.hz", "voice.cents", "voice.gate", "voice.trigger", "voice.velocity", "voice.release",
      "voice.ratio", "voice.harmonic", "voice.unisonCount", "voice.index",
      "transport.bpm", "transport.beat", "transport.barPhase", "transport.playing", "sr",
      "sample_read_linear", "sample_read_cubic", "sample_pos_linear", "sample_pos_cubic", "track.in", "sidechain.in", "send", "clipPlayer",
      // T2.6 (SPEC §3.3 tuning as a signal): the tuning node, its voice binding and the Degree voice signals.
      "tuning_hz", "tune_voices", "voice.degree", "voice.equave",
      // T2.6-fix3: which of a track patch's tuning nodes a note follows.
      "voice.tuning",
      // T3.17-fix2 (the mixer): smoothed gains.
      "gain", "gain_add",
  };
  for (const char* n : names) {
    CAPTURE(n);
    CHECK(reg().find(n).has_value());
  }
  CHECK(reg().size() == std::size(names));  // nothing registered that this list (and SPEC) does not name
}

TEST_CASE("atoms: names are unique and lookup round-trips every id") {
  std::set<std::string_view> seen;
  for (AtomId id = 0; id < reg().size(); ++id) {
    const AtomSignature& s = reg().get(id);
    CAPTURE(s.name);
    CHECK(seen.insert(s.name).second);
    REQUIRE(reg().find(s.name).has_value());
    CHECK(*reg().find(s.name) == id);
    CHECK(&atomSignature(atomId(s.name)) == &s);
  }
  CHECK_FALSE(reg().find("").has_value());
  CHECK_FALSE(reg().find("Add").has_value());
  CHECK_FALSE(reg().find("voice").has_value());
  CHECK_FALSE(reg().find("zzz").has_value());
}

TEST_CASE("atoms: signatures are well-formed") {
  for (const AtomSignature& s : reg().all()) {
    CAPTURE(s.name);
    CHECK(!s.summary.empty());
    // Port names unique per direction.
    std::set<std::string_view> in, out;
    for (const PortSpec& p : s.inputs) CHECK(in.insert(p.name).second);
    for (const PortSpec& p : s.outputs) CHECK(out.insert(p.name).second);
    // Defaults: bool inputs default to 0/1.
    for (const PortSpec& p : s.inputs)
      if (p.optional && p.type.kind == Kind::Bool) CHECK((p.defaultValue == 0.0f || p.defaultValue == 1.0f));
    if (s.has(atom_flags::kVariadicInputs)) {
      CHECK(!s.inputs.empty());
      CHECK(s.minRepeat >= 1);
    }
    if (s.has(atom_flags::kVariadicOutputs)) {
      CHECK(!s.outputs.empty());
      CHECK(s.minRepeat >= 1);
    }
    CHECK_FALSE((s.has(atom_flags::kVariadicInputs) && s.has(atom_flags::kVariadicOutputs)));
    // Only stateful atoms delay an input or need state; variable state implies stateful.
    bool anyDelayed = false;
    for (const PortSpec& p : s.inputs) anyDelayed |= p.delayed;
    if (anyDelayed || s.stateBytes > 0 || s.has(atom_flags::kVariableState)) CHECK(s.has(atom_flags::kStateful));
    if (s.has(atom_flags::kFastApprox)) {
      CHECK(s.name.starts_with("fast_"));
      const auto exact = reg().find(s.exactVariant);
      REQUIRE(exact.has_value());
      const AtomSignature& e = reg().get(*exact);
      CHECK(e.inputs.size() == s.inputs.size());
      CHECK(!e.has(atom_flags::kFastApprox));
    }
    if (s.has(atom_flags::kCommutative)) {
      REQUIRE(s.inputs.size() >= 2);
      CHECK(s.inputs[0].type == s.inputs[1].type);
    }
    if (s.category == AtomCategory::Voice) {
      CHECK(s.inputs.empty());
      CHECK(s.has(atom_flags::kNeedsVoice));
      CHECK(s.outputs[0].type.poly == Poly::Voice);
    }
    if (s.has(atom_flags::kSideEffect)) CHECK(s.outputs.empty());
    else CHECK(!s.outputs.empty());
  }
}

TEST_CASE("atoms: selected signatures (data the compiler relies on)") {
  const AtomSignature& phasor = sig("phasor");
  REQUIRE(phasor.inputs.size() == 2);
  CHECK(phasor.inputs[0].name == "hz");
  CHECK(phasor.inputs[0].delayed);
  CHECK(phasor.inputs[1].name == "reset");
  CHECK(phasor.inputs[1].optional);
  CHECK(phasor.inputs[1].type.kind == Kind::Bool);
  CHECK(phasor.stateBytes == sizeof(pq::dsp::PhasorState));  // the kernel's state block (uint32 phase per lane)
  CHECK(phasor.outputs[0].type.rate == Rate::Audio);

  const AtomSignature& z1 = sig("z1");
  CHECK(z1.inputs[0].delayed);
  CHECK(z1.inputs[0].type.kind == Kind::Infer);  // any kind: z1 of a bool is a bool
  CHECK(z1.inputs[1].type.rate == Rate::Const);
  CHECK(z1.stateBytes == sizeof(pq::dsp::Z1State));

  const AtomSignature& latch = sig("latch");
  CHECK(latch.has(atom_flags::kStateful));
  CHECK_FALSE(latch.inputs[0].delayed);  // feedthrough: a cycle through latch is still delay-free

  const AtomSignature& delay = sig("delay");
  CHECK(delay.has(atom_flags::kVariableState));
  CHECK(delay.inputs[2].name == "max");
  CHECK(delay.inputs[2].type.rate == Rate::Const);

  const AtomSignature& add = sig("add");
  CHECK(add.stateBytes == 0);
  CHECK(add.has(atom_flags::kCommutative));
  CHECK(add.has(atom_flags::kBroadcast));
  CHECK_FALSE(sig("sub").has(atom_flags::kCommutative));
  CHECK(add.outputs[0].type == portType(Kind::Real));  // rate/poly/width inferred

  const AtomSignature& gt = sig("gt");
  CHECK(gt.inputs[0].type.kind == Kind::Real);
  CHECK(gt.outputs[0].type.kind == Kind::Bool);

  const AtomSignature& c2h = sig("cents_to_hz");
  CHECK(c2h.inputs[1].optional);
  CHECK(c2h.inputs[1].defaultValue == doctest::Approx(261.626));  // SPEC §15 reference C3

  const AtomSignature& hz = sig("voice.hz");
  CHECK(hz.outputs[0].type == portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono));
  CHECK(sig("voice.gate").outputs[0].type.kind == Kind::Bool);
  CHECK(sig("sr").outputs[0].type.rate == Rate::Const);
  CHECK(sig("transport.bpm").outputs[0].type.rate == Rate::Param);

  const AtomSignature& mux = sig("mux");
  CHECK(mux.has(atom_flags::kVariadicInputs));
  CHECK(mux.fixedInputs() == 1);
  CHECK(mux.minRepeat == 2);
  const AtomSignature& demux = sig("demux");
  CHECK(demux.fixedOutputs() == 0);
  CHECK(demux.fixedInputs() == 2);

  CHECK(sig("fast_tan").exactVariant == "tan");
  CHECK(sig("send").has(atom_flags::kSideEffect));
  CHECK(sig("channel").inputs[1].type.rate == Rate::Const);
  CHECK(sig("channel").outputs[0].type.width == kWidthMono);
}
