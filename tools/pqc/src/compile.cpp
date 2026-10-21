// pqc: loading a .pqp file with its imports and running the graph pipeline (parse → infer → optimise → schedule).
#include <deque>
#include <fstream>
#include <map>
#include <sstream>

#include "pq/pqc/pqc.h"

namespace pq::pqc {

namespace fs = std::filesystem;

namespace {

Result<std::string> readText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Error{ErrorCode::Io, "cannot read " + path.string(), {}};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string joinDiagnostics(std::string_view text, const std::vector<graph::Diagnostic>& ds) {
  std::string m;
  for (const graph::Diagnostic& d : ds) {
    if (!m.empty()) m += '\n';
    m += graph::formatDiagnostic(text, d);
  }
  return m;
}

// Imports relative to the importing file, recursively; every parsed file stays alive for the PatchLibrary.
// (The same loader as `pqdaw --play`'s, src/app/src/play_command.cpp; QUESTIONS Q?-T1.10-b.)
class FileLoader final : public graph::ImportLoader {
public:
  Result<std::vector<graph::SubcircuitRef>> load(std::string_view path, std::string_view importer) override {
    const fs::path full = fs::weakly_canonical(fs::path(importer).parent_path() / pathFromUtf8(path));
    const std::string key = full.string();
    if (auto it = byPath_.find(key); it != byPath_.end()) return graph::signatures(it->second->file);
    if (active_.count(key)) return Error{ErrorCode::InvalidArgument, "import cycle at " + key, {}};
    auto text = readText(full);
    if (!text) return text.error();
    active_[key] = 1;
    texts_.push_back(std::move(*text));
    graph::ParseOptions po;
    po.imports = this;
    auto parsed = std::make_unique<graph::ParseResult>(graph::parseDetailed(texts_.back(), key, po));
    active_.erase(key);
    if (!parsed->ok()) return Error{ErrorCode::Parse, joinDiagnostics(texts_.back(), parsed->diagnostics), {}};
    auto sigs = graph::signatures(parsed->file);
    byPath_[key] = parsed.get();
    files_.push_back(std::move(parsed));
    return sigs;
  }
  void addTo(graph::PatchLibrary& lib) const {
    for (const auto& f : files_) lib.add(f->file);
  }

private:
  std::deque<std::string> texts_;
  std::vector<std::unique_ptr<graph::ParseResult>> files_;
  std::map<std::string, graph::ParseResult*> byPath_;
  std::map<std::string, int> active_;
};

}  // namespace

fs::path pathFromUtf8(std::string_view s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

struct SourceFile::Impl {
  std::string fileName;
  std::string text;
  FileLoader loader;
  graph::ParseResult parsed;
  graph::PatchLibrary library;
};

SourceFile::SourceFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SourceFile::~SourceFile() = default;

Result<std::unique_ptr<SourceFile>> SourceFile::load(const fs::path& path) {
  std::error_code ec;
  const fs::path full = fs::weakly_canonical(path, ec);
  auto impl = std::make_unique<Impl>();
  impl->fileName = (ec ? path : full).string();
  auto text = readText(ec ? path : full);
  if (!text) return text.error();
  impl->text = std::move(*text);
  graph::ParseOptions po;
  po.imports = &impl->loader;
  impl->parsed = graph::parseDetailed(impl->text, impl->fileName, po);
  if (!impl->parsed.ok()) {
    const graph::Diagnostic& first = impl->parsed.diagnostics.front();
    return Error{first.code, joinDiagnostics(impl->text, impl->parsed.diagnostics), first.location};
  }
  impl->library.add(impl->parsed.file);
  impl->loader.addTo(impl->library);
  return std::unique_ptr<SourceFile>(new SourceFile(std::move(impl)));
}

const graph::PatchFile& SourceFile::file() const noexcept { return impl_->parsed.file; }
const std::string& SourceFile::text() const noexcept { return impl_->text; }
const std::string& SourceFile::fileName() const noexcept { return impl_->fileName; }
const graph::PatchLibrary& SourceFile::library() const noexcept { return impl_->library; }
std::span<const graph::SourceSpan> SourceFile::nodeSpans(size_t index) const noexcept {
  return index < impl_->parsed.nodeSpans.size() ? std::span<const graph::SourceSpan>(impl_->parsed.nodeSpans[index])
                                                 : std::span<const graph::SourceSpan>();
}

Result<size_t> SourceFile::findPatch(std::string_view name) const {
  const auto& patches = file().patches;
  if (patches.empty()) return Error{ErrorCode::NotFound, fileName() + " has no patch", {}};
  if (name.empty()) return size_t{0};
  std::string names;
  for (size_t k = 0; k < patches.size(); ++k) {
    if (patches[k].name() == name) return k;
    names += (k ? ", " : "") + patches[k].name();
  }
  return Error{ErrorCode::NotFound, "no patch '" + std::string(name) + "' in " + fileName() + " (patches: " + names + ")",
               {}};
}

namespace {
// Compile errors are Parse (syntax) or Type (everything after parsing), whatever code the phase used, so callers can
// tell them from I/O and argument errors.
Error compileError(Error e) {
  if (e.code != ErrorCode::Parse) e.code = ErrorCode::Type;
  return e;
}
}  // namespace

Result<Compiled> compilePatch(const SourceFile& src, size_t index, const CompileOptions& options) {
  if (index >= src.file().patches.size()) return Error{ErrorCode::InvalidArgument, "patch index out of range", {}};
  const graph::Patch& root = src.file().patches[index];
  const graph::SourceText source{src.text(), src.fileName(), src.nodeSpans(index)};
  Compiled c;
  c.patchName = root.name();
  graph::InferOptions io;
  io.library = &src.library();
  io.source = source;
  auto typed = graph::infer(root, io);
  if (!typed) return compileError(typed.error());
  c.inferred = typed.value();
  graph::OptimiseOptions oo;
  oo.sampleRate = options.sampleRate;
  auto opt = graph::optimise(std::move(typed.value()), oo, &c.optimiseStats);
  if (!opt) return compileError(opt.error());
  c.optimised = std::move(opt.value());
  graph::ScheduleOptions so;
  so.sampleRate = options.sampleRate;
  so.visualMode = options.visualMode;
  so.source = source;
  graph::ScheduleResult sr = graph::scheduleDetailed(c.optimised, so);
  if (!sr.ok()) {
    const graph::Diagnostic& first = sr.diagnostics.front();
    return compileError(Error{first.code, joinDiagnostics(src.text(), sr.diagnostics), first.location});
  }
  c.schedule = std::move(sr.schedule);
  for (const graph::Diagnostic& w : c.schedule.warnings) {
    std::string m = graph::formatDiagnostic(src.text(), w);
    // formatDiagnostic says "error:"; these are warnings.
    if (const size_t k = m.find(": error: "); k != std::string::npos) m.replace(k, 9, ": warning: ");
    c.warnings.push_back(std::move(m));
  }
  return c;
}

}  // namespace pq::pqc
