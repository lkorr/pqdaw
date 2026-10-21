#pragma once

// The one authoritative mapping from registry atoms (atoms.h, what the language and inference see) to dsp kernels
// (pq/dsp/atoms.h, what the interpreter and the JIT run), docs/QUESTIONS.md Q14. T1.7 lowers every atom node of a
// TypedPatch through this table; tests/graph/test_kernel_map.cpp checks it against both tables (names, port counts
// and kinds, state sizes, resources, splat requirements).
//
// Conventions shared by both sides:
// - Kernel outputs are the registry outputs, in the same order. Kernel inputs are the registry inputs that ArgUse
//   marks as Input / InputSeconds, in registry order; the other registry inputs are consts consumed when the
//   program is built (state init, resource choice) and never reach the kernel.
// - Durations are seconds in the language and samples in the kernels. The conversion is ordinary const/param-rate
//   math the compiler inserts (`x · sr`), which the optimiser hoists or folds, so kernels never multiply per sample.
// - eq/ne on bools: infer() rewrites them to not(xor(a, b)) / xor(a, b), so the dsp comparators (real-only, a bool
//   mask is a NaN as a float) only see reals.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "pq/dsp/atoms.h"
#include "pq/graph/atoms.h"

namespace pq::graph {

// What becomes of one registry input.
enum class ArgUse : uint8_t {
  Input,                // kernel input `kernelInput`, as is
  InputSeconds,         // kernel input `kernelInput` = value · sr (seconds → samples), inserted by the compiler
  Resource,             // const naming a table / sample buffer → KernelArgs::resource
  InitSeed,             // const → dsp::StateInit::seed (mixed with the NodeId by the builder)
  InitMaxDelaySeconds,  // const seconds → dsp::StateInit::maxDelayFrames = ceil(value · sr)
  InitZ1,               // const → dsp::StateInit::z1Init (bool z1: nonzero → boolMask(true))
};

struct ArgMap {
  ArgUse use = ArgUse::Input;
  uint8_t kernelInput = 0;  // Input / InputSeconds only
};

enum class Lowering : uint8_t {
  Kernel,      // one dsp kernel
  Structural,  // no op: buffer views/copies decided by the scheduler (channel, pack)
  Constant,    // a const after specialisation (sr)
  Engine,      // read or written by the engine runtime, not a kernel: voice.*, transport.*, track/sidechain input,
               // send, clipPlayer, voice reducers (they need the voice table)
};

struct KernelMapping {
  Lowering lowering = Lowering::Engine;
  dsp::Atom kernel = dsp::Atom::Count_;  // Kernel only
  // One entry per registry input spec (a variadic atom's repeated spec maps every repeat 1:1 in order). Empty for
  // non-kernel atoms.
  std::span<const ArgMap> args;
  std::string_view note;  // why a non-kernel atom has no kernel, or what the compiler does around the kernel
};

// The mapping of a registry atom (AtomId of AtomRegistry::builtin()). Built once on first use; immutable.
const KernelMapping& kernelMapping(AtomId atom) noexcept;

// The registry atom a kernel implements, for every dsp atom that is not a split feedback kernel (z1:read …) or the
// block-fill `const` (used for Const nodes that must become buffers); nullopt for those.
std::optional<AtomId> registryAtomOf(dsp::Atom kernel) noexcept;

}  // namespace pq::graph
