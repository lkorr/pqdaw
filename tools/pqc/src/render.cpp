// pqc render: compile a patch, build a ProgramImage (interpreter or JIT) and render it offline.
#include <chrono>
#include <cmath>

#include "pq/engine/interpreter.h"
#include "pq/jit/compiler.h"
#include "pq/pqc/pqc.h"

namespace pq::pqc {

namespace {

Error backendError(const std::string& m) { return Error{ErrorCode::Unsupported, m, {}}; }

// One LLJIT per process (ARCHITECTURE §6: the JIT compiler is an explicit singleton of the tool).
Result<jit::Compiler*> jitCompiler() {
  static Result<std::unique_ptr<jit::Compiler>> compiler = jit::Compiler::create();
  if (!compiler) return compiler.error();
  return compiler.value().get();
}

}  // namespace

Result<RenderInfo> renderFile(const std::filesystem::path& path, const RenderOptions& options, engine::AudioSink& sink) {
  const auto t0 = std::chrono::steady_clock::now();
  const double sr = options.compile.sampleRate;
  if (!(sr >= 1000.0 && sr <= 768000.0)) return Error{ErrorCode::InvalidArgument, "--rate must be 1000..768000 Hz", {}};
  if (!(options.seconds > 0.0 && options.seconds <= 3600.0))
    return Error{ErrorCode::InvalidArgument, "--seconds must be in (0, 3600]", {}};
  if (options.blockFrames == 0 || options.blockFrames > 8192)
    return Error{ErrorCode::InvalidArgument, "--block must be 1..8192 frames", {}};
  if (options.maxVoices == 0 || options.maxVoices > 256)
    return Error{ErrorCode::InvalidArgument, "--voices must be 1..256", {}};

  auto src = SourceFile::load(path);
  if (!src) return src.error();
  auto index = (*src)->findPatch(options.patch);
  if (!index) return index.error();
  auto compiled = compilePatch(**src, *index, options.compile);
  if (!compiled) return compiled.error();
  const graph::Schedule& sched = compiled->schedule;

  RenderInfo info;
  info.patchName = compiled->patchName;
  info.ops = static_cast<uint32_t>(sched.ops.size());
  const uint32_t groups = engine::voiceGroupsFor(options.maxVoices);
  std::unique_ptr<engine::ProgramImage> image;
  if (options.backend == Backend::Jit) {
    auto jc = jitCompiler();
    if (!jc) return backendError("JIT: " + jc.error().message);
    jit::CompileOptions co;
    co.maxBlockFrames = options.blockFrames;
    co.maxVoiceGroups = groups;
    auto img = (*jc)->compile(sched, co);
    if (!img) return backendError("JIT: " + img.error().message);
    image = std::move(img.value());
  } else {
    engine::InterpretOptions eo;
    eo.maxBlockFrames = options.blockFrames;
    eo.maxVoiceGroups = groups;
    auto img = engine::buildInterpreted(sched, eo);
    if (!img) return backendError("interpreter: " + img.error().message);
    image = std::move(img.value());
  }
  const engine::ImageInterface& io = image->io();

  uint32_t globalOuts = 0, voiceOuts = 0;
  for (const engine::IoSlot& s : io.globalOut) globalOuts += s.isExport ? 0u : 1u;
  for (const engine::IoSlot& s : io.voiceOut) voiceOuts += s.isExport ? 0u : 1u;
  info.voices = io.hasVoiceStages;
  info.instrument = globalOuts == 0;
  if (globalOuts == 0 && voiceOuts == 0)
    return backendError("patch '" + info.patchName + "' has no `out` (global or voice) to render");

  // Params: declared defaults, then --param overrides by name.
  std::vector<float> params;
  for (const graph::ParamSlot& p : io.params) params.push_back(p.defaultValue);
  const graph::Patch& scheduled = compiled->scheduled().patch;
  for (const auto& [name, value] : options.params) {
    bool found = false;
    for (size_t k = 0; k < io.params.size(); ++k) {
      const graph::NodeIndex n = io.params[k].node;
      if (n < scheduled.nodeCount() && scheduled.nodeName(n) == name) {
        params[k] = value;
        found = true;
      }
    }
    if (!found) {
      std::string names;
      for (const graph::ParamSlot& p : io.params)
        if (p.node < scheduled.nodeCount()) names += (names.empty() ? "" : ", ") + std::string(scheduled.nodeName(p.node));
      return Error{ErrorCode::InvalidArgument,
                   "--param: no param '" + name + "' in patch '" + info.patchName + "' (params: " +
                       (names.empty() ? std::string("none") : names) + ")",
                   {}};
    }
  }

  const uint64_t frames = static_cast<uint64_t>(std::llround(options.seconds * sr));
  engine::VoiceEngine voices;
  engine::NoteListSource noteSource;
  engine::RenderRequest rq;
  rq.image = image.get();
  rq.frames = frames;
  rq.blockFrames = options.blockFrames;
  rq.gain = static_cast<float>(std::pow(10.0, options.gainDb / 20.0));
  rq.params = params;
  if (io.hasVoiceStages) {
    auto spec = parseNotes(options.notes.empty() ? kDefaultNotes : std::string_view(options.notes));
    if (!spec) return spec.error();
    const std::vector<engine::TimedNote> notes = timedNotes(*spec, sr, frames);
    info.notes = notes.size();
    noteSource = engine::NoteListSource(notes);
    engine::VoiceConfig vc;
    vc.maxVoices = options.maxVoices;
    vc.doneSlot = engine::findVoiceSlot(io, scheduled, "done");
    if (auto s = voices.prepare(image.get(), vc); !s) return backendError(s.error().message);
    rq.voices = &voices;
    rq.events = noteSource.source();
  } else if (!options.notes.empty()) {
    return Error{ErrorCode::InvalidArgument, "--notes: patch '" + info.patchName + "' has no voice stages", {}};
  }
  info.compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  auto st = engine::renderOffline(rq, sink);
  if (!st) return st.error();
  info.stats = *st;
  if (io.hasVoiceStages) info.voiceStats = voices.stats();
  return info;
}

}  // namespace pq::pqc
