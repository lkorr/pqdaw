// Q14 reconciliation: every registry atom maps to a dsp kernel with matching ports, kinds and state size, or is
// explicitly handled elsewhere (structure, engine, const); every dsp kernel is reachable from the registry.

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <set>
#include <string>

#include "pq/dsp/atoms.h"
#include "pq/graph/kernel_map.h"

using namespace pq::graph;
namespace dsp = pq::dsp;

TEST_CASE("kernel map: every registry atom is lowered to a matching kernel or explicitly elsewhere") {
  const AtomRegistry& reg = AtomRegistry::builtin();
  uint32_t kernels = 0, elsewhere = 0;
  for (AtomId id = 0; id < reg.size(); ++id) {
    const AtomSignature& sig = reg.get(id);
    const KernelMapping& m = kernelMapping(id);
    CAPTURE(sig.name);
    if (m.lowering != Lowering::Kernel) {
      ++elsewhere;
      CHECK(!m.note.empty());  // says what handles it
      CHECK(m.args.empty());
      const bool structural = sig.category == AtomCategory::Structure || sig.category == AtomCategory::Voice ||
                              sig.category == AtomCategory::Transport || sig.category == AtomCategory::Io;
      CHECK(structural);
      continue;
    }
    ++kernels;
    REQUIRE(static_cast<uint32_t>(m.kernel) < dsp::kAtomCount);
    const dsp::KernelInfo& info = dsp::kernelInfo(m.kernel);
    CHECK(std::string_view(info.name) == sig.name);
    CHECK(registryAtomOf(m.kernel) == id);

    // Inputs: one ArgMap per registry spec; kernel inputs are the Input/InputSeconds ones, numbered in order.
    REQUIRE(m.args.size() == sig.inputs.size());
    uint32_t kin = 0;
    bool usesResource = false;
    for (size_t j = 0; j < sig.inputs.size(); ++j) {
      const PortSpec& p = sig.inputs[j];
      const ArgMap& a = m.args[j];
      CAPTURE(p.name);
      if (a.use == ArgUse::Input || a.use == ArgUse::InputSeconds) {
        CHECK(a.kernelInput == kin);
        const bool kernelBool = (info.boolIn >> kin & 1u) != 0;
        CHECK(kernelBool == (p.type.kind == Kind::Bool));  // any-kind inputs are copied bitwise, as reals
        if (a.use == ArgUse::InputSeconds) CHECK(p.type.kind == Kind::Real);
        // A kernel input that must be a splat (once per block) is bounded to param rate in the language.
        if (info.splRequired >> kin & 1u) CHECK((p.type.rate == Rate::Param || p.type.rate == Rate::Const));
        ++kin;
      } else {
        // Consumed when the program is built: must be a const the language can see.
        CHECK(p.type.rate == Rate::Const);
        CHECK(p.type.kind == Kind::Real);
        usesResource |= a.use == ArgUse::Resource;
      }
    }
    CHECK(kin == info.numIn);
    CHECK(usesResource == (info.resource != dsp::Resource::None));
    CHECK(info.variadicIn == sig.has(atom_flags::kVariadicInputs));
    CHECK(info.variadicOut == sig.has(atom_flags::kVariadicOutputs));

    // Outputs: same count and order; bool outputs are masks.
    REQUIRE(info.numOut == sig.outputs.size());
    for (size_t j = 0; j < sig.outputs.size(); ++j) {
      CAPTURE(sig.outputs[j].name);
      CHECK(((info.boolOut >> j & 1u) != 0) == (sig.outputs[j].type.kind == Kind::Bool));
    }

    // State: the registry's size is the kernel's state block (fixed part for delay lines).
    CHECK(sig.stateBytes == info.stateSize);
    CHECK(sig.has(atom_flags::kStateful) == (info.stateSize != 0));
    dsp::StateInit init;
    init.maxDelayFrames = 100;
    if (sig.has(atom_flags::kVariableState)) CHECK(dsp::stateBytes(m.kernel, init) > info.stateSize);
    else CHECK(dsp::stateBytes(m.kernel, init) == info.stateSize);

    // A kernel exists for both lane counts with the required splats.
    CHECK(dsp::findKernel(m.kernel, 1, info.splRequired) != nullptr);
    CHECK(dsp::findKernel(m.kernel, 8, info.splRequired) != nullptr);
  }
  CHECK(kernels + elsewhere == reg.size());
  CHECK(kernels == dsp::kAtomCount - 7);  // every kernel but the 6 split feedback kernels and the block fill
}

TEST_CASE("kernel map: the atoms without a kernel are exactly the structural, engine and const ones") {
  const AtomRegistry& reg = AtomRegistry::builtin();
  std::set<std::string> elsewhere;
  for (AtomId id = 0; id < reg.size(); ++id)
    if (kernelMapping(id).lowering != Lowering::Kernel) elsewhere.insert(std::string(reg.get(id).name));
  const std::set<std::string> expected = {
      "channel", "pack", "sr", "track.in", "sidechain.in", "send", "clipPlayer",
      "voice.hz", "voice.cents", "voice.gate", "voice.trigger", "voice.velocity", "voice.release", "voice.ratio",
      "voice.harmonic", "voice.unisonCount", "voice.index",
      "transport.bpm", "transport.beat", "transport.barPhase", "transport.playing",
      "voice_sum", "voice_last", "voice_highest", "voice_lowest", "voice_loudest"};
  CHECK(elsewhere == expected);
  CHECK(kernelMapping(*reg.find("sr")).lowering == Lowering::Constant);
  CHECK(kernelMapping(*reg.find("pack")).lowering == Lowering::Structural);
  CHECK(kernelMapping(*reg.find("voice_sum")).lowering == Lowering::Engine);
}

TEST_CASE("kernel map: every dsp kernel except the split feedback kernels and block fill comes from an atom") {
  const std::set<dsp::Atom> internal = {dsp::Atom::Const,           dsp::Atom::Z1Read,          dsp::Atom::Z1Write,
                                        dsp::Atom::DelayReadLinear, dsp::Atom::DelayReadCubic, dsp::Atom::DelayReadAllpass,
                                        dsp::Atom::DelayWrite};
  for (uint32_t k = 0; k < dsp::kAtomCount; ++k) {
    const auto a = static_cast<dsp::Atom>(k);
    CAPTURE(dsp::kernelInfo(a).name);
    const auto id = registryAtomOf(a);
    CHECK(id.has_value() == (internal.count(a) == 0));
    if (id) CHECK(kernelMapping(*id).kernel == a);
  }
}

TEST_CASE("kernel map: the reconciled differences (Q14)") {
  const AtomRegistry& reg = AtomRegistry::builtin();
  auto mapping = [&](std::string_view n) -> const KernelMapping& { return kernelMapping(*reg.find(n)); };
  // Durations: seconds in the language, samples in the kernel.
  CHECK(mapping("delay").args[1].use == ArgUse::InputSeconds);
  CHECK(mapping("delay_cubic").args[2].use == ArgUse::InitMaxDelaySeconds);
  CHECK(mapping("timer").args[1].use == ArgUse::InputSeconds);
  CHECK(mapping("pulse").args[0].use == ArgUse::InputSeconds);
  // pulse has the kernel's optional period (0 = once, the old registry behaviour).
  const AtomSignature& pulse = atomSignature(*reg.find("pulse"));
  REQUIRE(pulse.inputs.size() == 1);
  CHECK(pulse.inputs[0].optional);
  CHECK(pulse.inputs[0].defaultValue == 0.0f);
  // z1 keeps `init`, consumed by the state initialiser.
  CHECK(mapping("z1").args[1].use == ArgUse::InitZ1);
  // Sample playback: the kernels' two forms, the buffer as a resource.
  CHECK(mapping("sample_read_cubic").kernel == dsp::Atom::SampleReadCubic);
  CHECK(mapping("sample_pos_linear").kernel == dsp::Atom::SamplePosLinear);
  CHECK(mapping("sample_read_linear").args[0].use == ArgUse::Resource);
  CHECK_FALSE(reg.find("sampleRead").has_value());
  // wavetable has no `hz`: the kernel picks the mip level from the phase increment.
  CHECK(atomSignature(*reg.find("wavetable")).inputs.size() == 2);
  // Kernels that only existed in dsp are now atoms.
  for (const char* n : {"fast_cents_to_hz", "fast_hz_to_cents", "fast_db_to_gain", "fast_gain_to_db", "muladd"})
    CHECK(reg.find(n).has_value());
  CHECK(mapping("sine").kernel == dsp::Atom::SinePhase);
}
