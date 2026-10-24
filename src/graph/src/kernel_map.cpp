// Registry atom → dsp kernel mapping (Q14). Kernel atoms share their name with the kernel (the registry was
// renamed where they differed: sample_read_* / sample_pos_*), so the table only spells out the exceptions: inputs
// that are not plain kernel inputs, and atoms that have no kernel.

#include "pq/graph/kernel_map.h"

#include <array>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace pq::graph {

namespace {

using U = ArgUse;

// Plain kernel inputs 0..n-1, shared by every atom whose registry inputs are exactly its kernel inputs.
constexpr ArgMap kPlain[] = {{U::Input, 0}, {U::Input, 1}, {U::Input, 2}, {U::Input, 3}, {U::Input, 4}, {U::Input, 5}};

constexpr ArgMap kPulse[] = {{U::InputSeconds, 0}};                                     // period
constexpr ArgMap kTimer[] = {{U::Input, 0}, {U::InputSeconds, 1}};                      // trig, dur
constexpr ArgMap kZ1[] = {{U::Input, 0}, {U::InitZ1, 0}};                               // x, init
constexpr ArgMap kDelay[] = {{U::Input, 0}, {U::InputSeconds, 1}, {U::InitMaxDelaySeconds, 0}};  // x, time, max
constexpr ArgMap kWavetable[] = {{U::Input, 0}, {U::Resource, 0}};                      // phase, table
constexpr ArgMap kShaper[] = {{U::Input, 0}, {U::Resource, 0}};                         // x, table
constexpr ArgMap kNoise[] = {{U::InitSeed, 0}};                                         // seed
constexpr ArgMap kSampleRead[] = {{U::Resource, 0}, {U::Input, 0}, {U::Input, 1},       // sample, rate, reset,
                                  {U::Input, 2},    {U::Input, 3}, {U::Input, 4}};      // start, loopStart, loopEnd
constexpr ArgMap kSamplePos[] = {{U::Resource, 0}, {U::Input, 0}};                      // sample, pos
constexpr ArgMap kTuningHz[] = {{U::Input, 0}, {U::Input, 1}, {U::Input, 2}, {U::Input, 3},  // degree, equave, hz,
                                {U::Input, 4}, {U::Input, 5}, {U::Input, 6}, {U::Input, 7},  // cents, generator, period,
                                {U::Resource, 0}};                                           // stretch, reference, table

struct Special {
  std::string_view atom;
  std::span<const ArgMap> args;
};
constexpr Special kSpecial[] = {
    {"pulse", kPulse},
    {"timer", kTimer},
    {"z1", kZ1},
    {"delay", kDelay},
    {"delay_cubic", kDelay},
    {"delay_allpass", kDelay},
    {"wavetable", kWavetable},
    {"shaper", kShaper},
    {"noise", kNoise},
    {"sample_read_linear", kSampleRead},
    {"sample_read_cubic", kSampleRead},
    {"sample_pos_linear", kSamplePos},
    {"sample_pos_cubic", kSamplePos},
    {"tuning_hz", kTuningHz},
};

struct NoKernel {
  std::string_view atom;  // exact name, or a prefix ending in '.' / '_'
  Lowering lowering;
  std::string_view note;
};
constexpr NoKernel kNoKernel[] = {
    {"channel", Lowering::Structural, "a view of one channel of the input buffer (scheduler)"},
    {"pack", Lowering::Structural, "channels side by side: the inputs are written into one multichannel buffer"},
    {"sr", Lowering::Constant, "the sample rate, a const after specialisation"},
    {"voice.", Lowering::Engine, "read from the voice table (T1.8)"},
    {"tune_voices", Lowering::Structural,
     "consumed by infer(): rebinds voice.hz / voice.cents inside the subcircuits of its patch (T2.6)"},
    {"transport.", Lowering::Engine, "read from the transport snapshot in ProcessContext"},
    {"track.in", Lowering::Engine, "the track's input buffers"},
    {"sidechain.in", Lowering::Engine, "the device's sidechain buffers"},
    {"send", Lowering::Engine, "accumulates into a bus buffer owned by the engine"},
    {"clipPlayer", Lowering::Engine, "native warp-engine playback (SPEC §9.4), engine-owned state"},
    {"voice_", Lowering::Engine, "reduces across all voice groups of the voice table (T1.8)"},
};

bool matches(std::string_view name, std::string_view pattern) noexcept {
  const char last = pattern.back();
  if (last == '.' || last == '_') return name.starts_with(pattern);
  return name == pattern;
}

struct Table {
  std::vector<KernelMapping> byAtom;
  std::array<AtomId, dsp::kAtomCount> byKernel{};
  std::array<bool, dsp::kAtomCount> hasRegistry{};
};

Table build() {
  const AtomRegistry& reg = AtomRegistry::builtin();
  Table t;
  t.byAtom.resize(reg.size());
  for (AtomId id = 0; id < reg.size(); ++id) {
    const AtomSignature& sig = reg.get(id);
    KernelMapping& m = t.byAtom[id];
    bool handled = false;
    for (const NoKernel& nk : kNoKernel) {
      if (matches(sig.name, nk.atom)) {
        m.lowering = nk.lowering;
        m.note = nk.note;
        handled = true;
        break;
      }
    }
    if (handled) continue;
    for (uint32_t k = 0; k < dsp::kAtomCount; ++k) {
      if (sig.name == dsp::kernelInfo(static_cast<dsp::Atom>(k)).name) {
        m.lowering = Lowering::Kernel;
        m.kernel = static_cast<dsp::Atom>(k);
        t.byKernel[k] = id;
        t.hasRegistry[k] = true;
        break;
      }
    }
    // Every atom is either listed above or has a kernel of the same name; anything else is a table bug that the
    // tests catch (lowering stays Engine with an empty note).
    if (m.lowering != Lowering::Kernel) continue;
    m.args = std::span<const ArgMap>(kPlain, sig.inputs.size() <= std::size(kPlain) ? sig.inputs.size() : 0);
    for (const Special& s : kSpecial)
      if (s.atom == sig.name) m.args = s.args;
    if (sig.name == "eq" || sig.name == "ne") m.note = "real operands only; bool eq/ne are rewritten by infer()";
    else if (sig.name == "pulse" || sig.name == "timer" || sig.name.starts_with("delay"))
      m.note = "durations in seconds are multiplied by sr by the compiler (kernels take samples)";
  }
  return t;
}

const Table& table() {
  static const Table t = build();
  return t;
}

}  // namespace

const KernelMapping& kernelMapping(AtomId atom) noexcept {
  const Table& t = table();
  if (atom >= t.byAtom.size()) std::abort();  // a programming error: AtomIds come from the registry
  return t.byAtom[atom];
}

std::optional<AtomId> registryAtomOf(dsp::Atom kernel) noexcept {
  const auto k = static_cast<uint32_t>(kernel);
  if (k >= dsp::kAtomCount || !table().hasRegistry[k]) return std::nullopt;
  return table().byKernel[k];
}

}  // namespace pq::graph
