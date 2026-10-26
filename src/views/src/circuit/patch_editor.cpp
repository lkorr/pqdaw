// The circuit editor's document (T4.4): text as the one source of truth, re-parsed with the previous parse after every
// edit from either pane; one undo history; check + hot swap. See pq/views/circuit/patch_editor.h.

#include "pq/views/circuit/patch_editor.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>

#include "pq/graph/inspect.h"
#include "pq/graph/layout.h"
#include "pq/graph/optimise.h"
#include "pq/graph/schedule.h"
#include "pq/views/circuit/circuit_scene.h"
#include "pq/views/circuit/symbols.h"

namespace pq::views::circuit {

namespace fs = std::filesystem;
using graph::NodeIndex;
using graph::PatchFile;

namespace {

fs::path pathOf(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

Result<std::string> readFile(const std::string& path) {
  std::ifstream in(pathOf(path), std::ios::binary);
  if (!in) return Error{ErrorCode::NotFound, "cannot open " + path, {}};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Error refuse(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m), {}}; }

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Imports relative to the edited file, each file loaded once per editor (an edit re-parses the text many times; the
// imported files are read-only here, so their parse is kept).
class ImportCache final : public graph::ImportLoader {
public:
  Result<std::vector<graph::SubcircuitRef>> load(std::string_view path, std::string_view importer) override {
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(pathOf(importer).parent_path() / pathOf(path), ec);
    const std::string key = utf8Of(full);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      auto doc = loadPatchDocument(key);
      if (!doc) {
        failures_[key] = doc.error().message;
        return Error{ErrorCode::Parse, doc.error().message.substr(0, doc.error().message.find('\n')), {}};
      }
      it = cache_.emplace(key, *doc).first;
    }
    if (std::find(used_.begin(), used_.end(), it->second) == used_.end()) used_.push_back(it->second);
    return graph::signatures(it->second->parsed.file);
  }
  void beginParse() { used_.clear(); }
  std::vector<std::shared_ptr<const PatchDocument>> used_;  // direct imports of the last parse, in order

private:
  std::map<std::string, std::shared_ptr<const PatchDocument>> cache_;
  std::map<std::string, std::string> failures_;
};

// The same graph, node for node in the same dense order with the same NodeIds, wires, payloads, names and layout (a
// re-parse after an edit of a comment or of whitespace): O(nodes + wires), no hashing. The canvas then keeps its scene.
bool sameGraph(const graph::Patch& a, const graph::Patch& b) {
  if (a.name() != b.name() || a.nodeCount() != b.nodeCount() || a.wireCount() != b.wireCount() ||
      a.portCount() != b.portCount())
    return false;
  for (NodeIndex v = 0; v < a.nodeCount(); ++v) {
    const graph::Node &x = a.node(v), &y = b.node(v);
    if (x.id != y.id || x.kind != y.kind || x.atom != y.atom || x.numInputs != y.numInputs || x.numOutputs != y.numOutputs ||
        x.nameOutput != y.nameOutput || x.shared != y.shared || a.nodeName(v) != b.nodeName(v) || !(a.layout(v) == b.layout(v)))
      return false;
    switch (x.kind) {
      case graph::NodeKind::Const:
        if (!(a.constant(v) == b.constant(v))) return false;
        break;
      case graph::NodeKind::Param:
        if (!(a.paramDecl(v) == b.paramDecl(v))) return false;
        break;
      case graph::NodeKind::Subcircuit:
        if (!(a.subcircuit(v) == b.subcircuit(v))) return false;
        break;
      case graph::NodeKind::Unknown:
      case graph::NodeKind::Equation:
        if (x.payload != y.payload) return false;
        break;
      default: break;
    }
  }
  const auto pa = a.portTypes(), pb = b.portTypes();
  if (!std::equal(pa.begin(), pa.end(), pb.begin())) return false;
  for (graph::WireIndex w = 0; w < a.wireCount(); ++w) {
    if (!(a.wire(w) == b.wire(w))) return false;
    const auto va = a.wireVia(w), vb = b.wireVia(w);
    if (!std::equal(va.begin(), va.end(), vb.begin(), vb.end())) return false;
  }
  const auto sa = a.solves(), sb = b.solves();
  const auto ca = a.comments(), cb = b.comments();
  return std::equal(sa.begin(), sa.end(), sb.begin(), sb.end()) && std::equal(ca.begin(), ca.end(), cb.begin(), cb.end());
}

bool sameGraph(const PatchFile& a, const PatchFile& b) {
  if (a.imports != b.imports || a.patches.size() != b.patches.size()) return false;
  for (size_t i = 0; i < a.patches.size(); ++i)
    if (!sameGraph(a.patches[i], b.patches[i])) return false;
  return true;
}

struct Step {
  uint32_t offset = 0;
  std::string removed, inserted;
  bool typing = false;
  std::chrono::steady_clock::time_point time;
};

constexpr size_t kMaxHistory = 4096;
constexpr double kCoalesceSeconds = 1.5;

}  // namespace

struct PatchEditor::Impl {
  PatchEditor* self = nullptr;
  std::string path, text, fileText;
  ImportCache loader;
  std::vector<std::shared_ptr<const PatchDocument>> imports;
  graph::ParseResult good;  // the last parse that lowered
  bool lowered = false;
  bool graphChanged = false;  // the last re-parse changed the graph (else only text spans moved)
  std::vector<std::optional<graph::Patch>> drawn;  // parallel to good.file.patches; computed when asked
  std::vector<graph::Diagnostic> parseDiags;       // of the current text
  std::vector<EditorDiagnostic> checkDiags, all;
  std::vector<Step> steps;
  size_t cursor = 0;  // steps[0, cursor) can be undone
  std::string note;
  PatchPlayer* player = nullptr;
  std::string playingName;
  bool checkPending = true;
  CheckStats stats;
  // T4.5: probes for the playing patch's next checks; what the player plays.
  std::vector<graph::ProbeTarget> probeTargets;
  std::vector<uint32_t> probeIds;
  std::unique_ptr<PlayedProgram> played;

  // ---- parsing -------------------------------------------------------------------------------------------------------
  void reparse(const PatchFile* idsFrom) {
    const auto t0 = std::chrono::steady_clock::now();
    graph::ParseOptions o;
    o.imports = &loader;
    o.previous = idsFrom ? idsFrom : (good.file.patches.empty() ? nullptr : &good.file);
    o.lenientLayout = true;
    o.deferMissingInputs = true;
    loader.beginParse();
    graph::ParseResult r = graph::parseDetailed(text, path, o);
    parseDiags = r.diagnostics;
    const bool wasLowered = lowered;
    lowered = r.lowered;
    graphChanged = false;
    if (r.lowered && wasLowered && sameGraph(r.file, good.file)) {
      // A comment, whitespace or formatting edit: the same graph (and so the same drawn patches); only the text spans
      // moved.
      stats.lastIdMatch = r.idMatch;
      good = std::move(r);
      imports = loader.used_;
    } else if (r.lowered) {
      graphChanged = true;
      // Carry the drawn positions by NodeId (a text edit names no position for a new or moved-key node).
      std::vector<std::optional<graph::Patch>> next(r.file.patches.size());
      for (size_t i = 0; i < r.file.patches.size(); ++i) {
        const graph::Patch* old = nullptr;
        for (size_t k = 0; k < drawn.size(); ++k)
          if (drawn[k] && drawn[k]->name() == r.file.patches[i].name()) old = &*drawn[k];
        if (!old) continue;
        graph::Patch p = r.file.patches[i];
        graph::carryLayout(p, *old);
        complete(p);
        next[i] = std::move(p);
      }
      drawn = std::move(next);
      stats.lastIdMatch = r.idMatch;
      good = std::move(r);
      imports = loader.used_;
    }
    checkDiags.clear();
    checkPending = true;
    stats.lastParseMs = msSince(t0);
    rebuildDiagnostics();
  }

  static void complete(graph::Patch& p) {
    const std::vector<SymbolSpec> specs = describeAll(p);
    graph::completeLayout(p, geometryOf(p, specs));
  }

  const graph::Patch& drawnPatch(size_t i) {
    if (i >= drawn.size()) drawn.resize(good.file.patches.size());
    if (!drawn[i]) {
      graph::Patch p = good.file.patches[i];
      complete(p);
      drawn[i] = std::move(p);
    }
    return *drawn[i];
  }

  EditorDiagnostic convert(const graph::Diagnostic& d, EditorDiagnostic::Phase phase) const {
    EditorDiagnostic e;
    e.phase = phase;
    e.offset = std::min<uint32_t>(d.offset, static_cast<uint32_t>(text.size()));
    e.length = d.length;
    e.line = d.location.line;
    e.col = d.location.col;
    e.message = d.message;
    return e;
  }

  void rebuildDiagnostics() {
    all.clear();
    for (const graph::Diagnostic& d : parseDiags)
      all.push_back(convert(d, lowered ? EditorDiagnostic::Phase::Validation : EditorDiagnostic::Phase::Syntax));
    for (const EditorDiagnostic& d : checkDiags) all.push_back(d);
  }

  // ---- text changes --------------------------------------------------------------------------------------------------
  bool external = false;  // T4.4-fix1: resetText: the change came from the host, not from an edit here
  uint64_t serial = 0;    // history steps recorded (stepSerial)
  void emit(const EditorChange& c) {
    if (self->onChange) self->onChange(c);
    if (lowered && !external && self->onCommitted) self->onCommitted(text, good);
  }

  // Replaces [offset, offset + removed) by `inserted`, re-parses (ids from idsFrom, else the last good parse) and tells
  // the listeners. `record`: push a history step.
  void apply(uint32_t offset, uint32_t removed, std::string_view inserted, EditSource source, const PatchFile* idsFrom,
             bool record, bool typing) {
    offset = std::min<uint32_t>(offset, static_cast<uint32_t>(text.size()));
    removed = std::min<uint32_t>(removed, static_cast<uint32_t>(text.size()) - offset);
    if (record) push(offset, text.substr(offset, removed), std::string(inserted), typing);
    text.replace(offset, removed, inserted);
    reparse(idsFrom);
    EditorChange c;
    c.source = source;
    c.text = true;
    c.offset = offset;
    c.removed = removed;
    c.inserted = static_cast<uint32_t>(inserted.size());
    c.graph = lowered && graphChanged;
    c.diagnostics = true;
    c.caret = offset + c.inserted;
    emit(c);
  }

  // The whole text becomes `next` (a circuit edit): recorded as the one replacement between the common prefix and
  // suffix, so undo restores exactly the old bytes.
  void replaceAll(const std::string& next, const PatchFile* idsFrom) {
    size_t a = 0;
    const size_t n = std::min(text.size(), next.size());
    while (a < n && text[a] == next[a]) ++a;
    size_t b = 0;
    while (b < n - a && text[text.size() - 1 - b] == next[next.size() - 1 - b]) ++b;
    apply(static_cast<uint32_t>(a), static_cast<uint32_t>(text.size() - a - b),
          std::string_view(next).substr(a, next.size() - a - b), EditSource::Circuit, idsFrom, true, false);
  }

  void push(uint32_t offset, std::string removed, std::string inserted, bool typing) {
    const auto now = std::chrono::steady_clock::now();
    steps.resize(cursor);  // a new step drops what could be redone
    const bool lineBreak = removed.find('\n') != std::string::npos || inserted.find('\n') != std::string::npos;
    if (typing && !lineBreak && !steps.empty() && steps.back().typing &&
        std::chrono::duration<double>(now - steps.back().time).count() < kCoalesceSeconds) {
      Step& p = steps.back();
      const uint32_t pend = p.offset + static_cast<uint32_t>(p.inserted.size());
      if (removed.empty() && offset == pend) {  // typing on
        p.inserted += inserted;
        p.time = now;
        return;
      }
      if (inserted.empty() && offset + removed.size() == pend) {  // backspacing over what was typed (and before)
        if (removed.size() <= p.inserted.size()) {
          p.inserted.resize(p.inserted.size() - removed.size());
        } else {
          const size_t k = removed.size() - p.inserted.size();
          p.removed = removed.substr(0, k) + p.removed;
          p.offset -= static_cast<uint32_t>(k);
          p.inserted.clear();
        }
        p.time = now;
        return;
      }
    }
    Step s;
    s.offset = offset;
    s.removed = std::move(removed);
    s.inserted = std::move(inserted);
    s.typing = typing && !lineBreak;
    s.time = now;
    ++serial;
    steps.push_back(std::move(s));
    if (steps.size() > kMaxHistory) steps.erase(steps.begin());
    cursor = steps.size();
  }

  // ---- circuit edits -------------------------------------------------------------------------------------------------
  Status needLowered() const {
    if (!lowered) return refuse("the text has errors: fix them (or undo) before editing the circuit");
    return Ok{};
  }

  // The file as drawn: every computed drawn patch with its completed layout (the base of circuit edits).
  PatchFile drawnFile() {
    PatchFile f = good.file;
    for (size_t i = 0; i < f.patches.size() && i < drawn.size(); ++i)
      if (drawn[i]) f.patches[i] = *drawn[i];
    return f;
  }

  std::string lineEnding() const { return text.find("\r\n") != std::string::npos ? "\r\n" : "\n"; }

  std::string printed(const graph::Patch& p) const {
    std::string s = graph::print(p);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    if (lineEnding() == "\r\n") {
      std::string o;
      o.reserve(s.size() + s.size() / 16);
      for (const char c : s) {
        if (c == '\n') o += '\r';
        o += c;
      }
      return o;
    }
    return s;
  }

  // A structural edit: `work` (drawnFile() after the edit; `inserted` = index of a patch the edit added, or kNone)
  // reprints the `changed` patches over their text spans, everything else keeps its bytes.
  Status commitStructural(PatchFile& work, std::span<const size_t> changed, size_t inserted) {
    std::vector<uint8_t> isChanged(work.patches.size(), 0);
    for (const size_t k : changed)
      if (k < isChanged.size()) isChanged[k] = 1;
    for (size_t k = 0; k < work.patches.size(); ++k)
      if (isChanged[k]) graph::edit::anonymize(work.patches[k]);
    std::string next = text;
    const size_t originals = good.file.patches.size();
    const std::string nl = lineEnding();
    for (size_t i = originals; i-- > 0;) {
      const graph::PatchTextSpans& ps = good.patchSpans[i];
      if (ps.begin == graph::kNone || ps.close == graph::kNone) return refuse("cannot locate the patch in the text");
      const size_t j = (inserted != graph::kNone && i >= inserted) ? i + 1 : i;
      if (inserted != graph::kNone && j + 1 == inserted)
        next.insert(ps.close + 1, nl + nl + printed(work.patches[inserted]));
      if (isChanged[j]) next.replace(ps.begin, ps.close + 1 - ps.begin, printed(work.patches[j]));
    }
    replaceAll(next, &work);
    return Ok{};
  }

  Status commitLayout(PatchFile& laid) {
    auto spliced = graph::spliceLayout(text, good, laid);
    if (!spliced) return std::move(spliced).error();
    if (*spliced != text) replaceAll(*spliced, &laid);
    return Ok{};
  }

  // ---- check -------------------------------------------------------------------------------------------------------
  void addCheckDiags(const std::vector<graph::Diagnostic>& ds) {
    for (const graph::Diagnostic& d : ds) checkDiags.push_back(convert(d, EditorDiagnostic::Phase::Check));
  }

  void collectLibrary(graph::PatchLibrary& lib, const PatchDocument& d) {
    lib.add(d.parsed.file);
    for (const auto& i : d.imports) collectLibrary(lib, *i);
  }

  Status runCheck(size_t idx) {
    const auto t0 = std::chrono::steady_clock::now();
    ++stats.checks;
    checkDiags.clear();
    checkPending = false;
    Status result = Ok{};
    if (!lowered || !parseDiags.empty()) {
      result = refuse("the text has errors");
    } else if (idx >= good.file.patches.size()) {
      result = refuse("no such patch");
    } else {
      graph::PatchLibrary lib;
      lib.add(good.file);
      for (const auto& d : imports) collectLibrary(lib, *d);
      const graph::SourceText source{text, path, idx < good.nodeSpans.size() ? std::span<const graph::SourceSpan>(good.nodeSpans[idx])
                                                                             : std::span<const graph::SourceSpan>()};
      const bool live = player && !playingName.empty() && playingName == good.file.patches[idx].name();
      graph::InferOptions io;
      io.library = &lib;
      io.source = source;
      if (live) io.probes = probeTargets;  // T4.5: only the playing patch carries probes
      graph::InferResult ir = graph::inferDetailed(good.file.patches[idx], io);
      const double rate = player ? player->sampleRate() : 48000.0;
      if (!ir.ok()) {
        addCheckDiags(ir.diagnostics);
        result = refuse("the patch does not check");
      } else {
        graph::OptimiseOptions oo;
        oo.sampleRate = rate;
        std::vector<uint32_t> valueMap;
        if (!ir.probeSources.empty()) oo.valueMap = &valueMap;
        // The instance tree as NodeIds while the parse's patches are at hand (PlayedProgram).
        std::vector<uint32_t> instParent;
        std::vector<graph::NodeId> instCall;
        for (const graph::InstanceInfo& in : ir.typed.instances) {
          instParent.push_back(in.parent);
          instCall.push_back(in.parent != graph::kNone && in.parent < ir.typed.instances.size() &&
                                     ir.typed.instances[in.parent].patch != nullptr
                                 ? ir.typed.instances[in.parent].patch->node(in.callNode).id
                                 : graph::NodeId{});
        }
        std::vector<const graph::Patch*> instPatch;
        for (const graph::InstanceInfo& in : ir.typed.instances) instPatch.push_back(in.patch);
        auto opt = graph::optimise(std::move(ir.typed), oo);
        if (!opt) {
          const uint32_t at = good.patchSpans[idx].begin == graph::kNone ? 0 : good.patchSpans[idx].begin;
          addCheckDiags({graph::makeDiagnostic(text, path, {at, 5}, opt.error().message, ErrorCode::Type)});
          result = refuse("the patch does not check");
        } else {
          graph::ScheduleOptions so;
          so.sampleRate = rate;
          so.source = source;
          so.previous = live ? player->stateLayout() : nullptr;
          std::vector<graph::ProbePoint> points;
          if (!ir.probeSources.empty()) points = graph::probePoints(opt->patch, ir.probeSources, valueMap, probeIds);
          so.probes = points;
          graph::ScheduleResult sr = graph::scheduleDetailed(*opt, so);
          if (!sr.ok()) {
            addCheckDiags(sr.diagnostics);
            result = refuse("the patch does not schedule");
          } else if (live && player->external()) {
            // T4.4-fix1: the patch plays elsewhere (a project device): the check is the editor's; the program, its
            // probes and its cost are the player's.
            ++stats.swaps;
          } else if (live) {
            const graph::TypedPatch& scheduled = sr.schedule.rewritten ? *sr.schedule.rewritten : *opt;
            if (Status s = player->play(sr.schedule, scheduled); s) {
              ++stats.swaps;
              // T4.5: what plays, in NodeIds (cost attribution; the instances' patches are this parse's).
              auto pp = std::make_unique<PlayedProgram>();
              pp->generation = player->generation();
              pp->schedule = sr.schedule;
              pp->instanceParent = instParent;
              pp->instanceCall = instCall;
              const size_t nn = scheduled.patch.nodeCount();
              pp->nodeInstance.assign(nn, graph::kNone);
              pp->nodeSource.assign(nn, graph::NodeId{});
              pp->nodeWhat.assign(nn, graph::NodeOrigin::What::Node);
              for (size_t n = 0; n < nn && n < scheduled.origin.size(); ++n) {
                const graph::NodeOrigin& o = scheduled.origin[n];
                pp->nodeInstance[n] = o.instance;
                pp->nodeWhat[n] = o.what;
                if (o.instance < instPatch.size() && instPatch[o.instance] && o.node < instPatch[o.instance]->nodeCount())
                  pp->nodeSource[n] = instPatch[o.instance]->node(o.node).id;
              }
              for (const graph::ProbeRef& pr : sr.schedule.probes) pp->probeIds.push_back(pr.id);
              played = std::move(pp);
            } else {
              ++stats.refusedSwaps;
              note = "not played: " + s.error().message;
              result = std::move(s);
            }
          }
        }
      }
    }
    stats.lastCheckMs = msSince(t0);
    rebuildDiagnostics();
    EditorChange c;
    c.source = EditSource::Load;
    c.diagnostics = true;
    if (self->onChange) self->onChange(c);
    return result;
  }
};

// ---- PatchEditor -----------------------------------------------------------------------------------------------------

PatchEditor::PatchEditor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) { impl_->self = this; }
PatchEditor::~PatchEditor() = default;

Result<std::unique_ptr<PatchEditor>> PatchEditor::fromText(std::string path, std::string text,
                                                           const graph::PatchFile* idsFrom) {
  auto impl = std::make_unique<Impl>();
  std::error_code ec;
  const fs::path full = fs::weakly_canonical(pathOf(path), ec);
  impl->path = ec ? path : utf8Of(full);
  impl->text = std::move(text);
  impl->fileText = impl->text;
  impl->reparse(idsFrom);
  if (impl->good.file.patches.empty()) {
    // Nothing lowered: an editor over broken text still opens (the text pane shows the errors), but a file whose
    // text never lowered has no circuit; refuse only an empty file.
    if (impl->text.find_first_not_of(" \t\r\n") == std::string::npos)
      return Error{ErrorCode::InvalidArgument, "the file has no patch", {}};
  }
  return std::unique_ptr<PatchEditor>(new PatchEditor(std::move(impl)));
}

Result<std::unique_ptr<PatchEditor>> PatchEditor::open(const std::string& path) {
  auto text = readFile(path);
  if (!text) return std::move(text).error();
  return fromText(path, std::move(*text));
}

const std::string& PatchEditor::path() const noexcept { return impl_->path; }
const std::string& PatchEditor::text() const noexcept { return impl_->text; }
bool PatchEditor::dirty() const noexcept { return impl_->text != impl_->fileText; }
bool PatchEditor::textLowered() const noexcept { return impl_->lowered; }
const graph::ParseResult& PatchEditor::parsed() const noexcept { return impl_->good; }
const graph::Patch& PatchEditor::drawn(size_t patch) { return impl_->drawnPatch(patch); }
std::span<const EditorDiagnostic> PatchEditor::diagnostics() const noexcept { return impl_->all; }
const std::vector<std::shared_ptr<const PatchDocument>>& PatchEditor::imports() const noexcept { return impl_->imports; }
const std::string& PatchEditor::note() const noexcept { return impl_->note; }
bool PatchEditor::checkPending() const noexcept { return impl_->checkPending; }
void PatchEditor::setPlayer(PatchPlayer* player) noexcept { impl_->player = player; }
PatchPlayer* PatchEditor::player() const noexcept { return impl_->player; }
bool PatchEditor::playing() const noexcept { return !impl_->playingName.empty(); }
const std::string& PatchEditor::playingPatch() const noexcept { return impl_->playingName; }
const PatchEditor::CheckStats& PatchEditor::stats() const noexcept { return impl_->stats; }
bool PatchEditor::canUndo() const noexcept { return impl_->cursor > 0; }
bool PatchEditor::canRedo() const noexcept { return impl_->cursor < impl_->steps.size(); }
size_t PatchEditor::historySize() const noexcept { return impl_->cursor; }
uint64_t PatchEditor::stepSerial() const noexcept { return impl_->serial; }

void PatchEditor::resetText(std::string text, const graph::PatchFile* idsFrom) {
  Impl& m = *impl_;
  if (text == m.text) return;
  m.note.clear();
  // The one replacement between the common prefix and suffix (the text pane keeps everything else).
  size_t a = 0;
  const size_t n = std::min(m.text.size(), text.size());
  while (a < n && m.text[a] == text[a]) ++a;
  size_t b = 0;
  while (b < n - a && m.text[m.text.size() - 1 - b] == text[text.size() - 1 - b]) ++b;
  m.external = true;
  m.apply(static_cast<uint32_t>(a), static_cast<uint32_t>(m.text.size() - a - b),
          std::string_view(text).substr(a, text.size() - a - b), EditSource::Undo, idsFrom, false, false);
  m.external = false;
}

size_t PatchEditor::patchIndex(std::string_view name) const noexcept {
  const auto& ps = impl_->good.file.patches;
  for (size_t i = 0; i < ps.size(); ++i)
    if (ps[i].name() == name) return i;
  return graph::kNone;
}

const graph::Patch* PatchEditor::findCallee(std::string_view name) const noexcept {
  if (const graph::Patch* p = impl_->good.file.find(name)) return p;
  for (const auto& d : impl_->imports)
    if (const graph::Patch* p = d->find(name)) return p;
  return nullptr;
}

std::vector<graph::SubcircuitRef> PatchEditor::callees() const {
  std::vector<graph::SubcircuitRef> out = graph::signatures(impl_->good.file);
  for (const auto& d : impl_->imports)
    for (graph::SubcircuitRef& r : graph::signatures(d->parsed.file)) out.push_back(std::move(r));
  return out;
}

void PatchEditor::replaceText(uint32_t offset, uint32_t length, std::string_view with, bool typing) {
  impl_->note.clear();
  if (length == 0 && with.empty()) return;
  impl_->apply(offset, length, with, EditSource::TextPane, nullptr, true, typing);
}

Result<graph::NodeId> PatchEditor::addNode(size_t patch, std::string_view spec, graph::Point2 at) {
  Impl& m = *impl_;
  m.note.clear();
  if (Status s = m.needLowered(); !s) return std::move(s).error();
  if (patch >= m.good.file.patches.size()) return refuse("no such patch");
  (void)m.drawnPatch(patch);
  PatchFile work = m.drawnFile();
  graph::Patch& p = work.patches[patch];
  const std::vector<graph::SubcircuitRef> sigs = callees();
  std::vector<graph::SubcircuitRef> usable;
  for (const graph::SubcircuitRef& r : sigs)
    if (r.patch != p.name()) usable.push_back(r);  // a patch cannot call itself
  auto n = graph::edit::addNode(p, spec, usable);
  if (!n) return std::move(n).error();
  graph::NodeLayout l;
  l.placed = true;
  l.x = std::round(at.x / 4.0f) * 4.0f;
  l.y = std::round(at.y / 4.0f) * 4.0f;
  p.setLayout(*n, l);
  const graph::NodeId id = p.node(*n).id;
  std::vector<size_t> changed{patch};
  for (const size_t q : graph::edit::refreshCallers(work, patch)) changed.push_back(q);
  if (Status s = m.commitStructural(work, changed, graph::kNone); !s) return std::move(s).error();
  return id;
}

Status PatchEditor::connect(size_t patch, graph::PortRef from, graph::PortRef to) {
  Impl& m = *impl_;
  m.note.clear();
  if (Status s = m.needLowered(); !s) return s;
  if (patch >= m.good.file.patches.size()) return refuse("no such patch");
  (void)m.drawnPatch(patch);
  PatchFile work = m.drawnFile();
  auto r = graph::edit::connect(work.patches[patch], from, to);
  if (!r) return std::move(r).error();
  const size_t changed[1] = {patch};
  if (Status s = m.commitStructural(work, changed, graph::kNone); !s) return s;
  if (r->delay != graph::kNone)
    m.note = "inserted a z\xE2\x81\xBB\xC2\xB9 (one-sample delay): the wire closed a loop that had no delay";
  return Ok{};
}

Status PatchEditor::erase(size_t patch, std::span<const NodeIndex> nodes, std::span<const graph::WireIndex> wires) {
  Impl& m = *impl_;
  m.note.clear();
  if (Status s = m.needLowered(); !s) return s;
  if (patch >= m.good.file.patches.size()) return refuse("no such patch");
  if (nodes.empty() && wires.empty()) return refuse("nothing selected");
  (void)m.drawnPatch(patch);
  PatchFile work = m.drawnFile();
  graph::Patch& p = work.patches[patch];
  std::vector<graph::WireIndex> ws(wires.begin(), wires.end());
  std::sort(ws.begin(), ws.end(), std::greater<>());
  ws.erase(std::unique(ws.begin(), ws.end()), ws.end());
  for (const graph::WireIndex w : ws)
    if (w < p.wireCount()) p.disconnect(w);  // descending: the wire moved into w is never one still to remove
  std::vector<size_t> changed = graph::edit::eraseNodes(work, patch, nodes);
  return m.commitStructural(work, changed, graph::kNone);
}

Status PatchEditor::rename(size_t patch, NodeIndex node, std::string name) {
  Impl& m = *impl_;
  m.note.clear();
  if (Status s = m.needLowered(); !s) return s;
  (void)m.drawnPatch(patch);
  PatchFile work = m.drawnFile();
  auto changed = graph::edit::rename(work, patch, node, std::move(name));
  if (!changed) return std::move(changed).error();
  if (changed->empty()) return Ok{};
  return m.commitStructural(work, *changed, graph::kNone);
}

Result<graph::NodeId> PatchEditor::makeParam(size_t patch, NodeIndex constNode) {
  Impl& m = *impl_;
  m.note.clear();
  if (Status s = m.needLowered(); !s) return std::move(s).error();
  if (patch >= m.good.file.patches.size()) return refuse("no such patch");
  (void)m.drawnPatch(patch);
  PatchFile work = m.drawnFile();
  auto k = graph::edit::constToParam(work.patches[patch], constNode);
  if (!k) return std::move(k).error();
  const graph::NodeId id = work.patches[patch].node(*k).id;
  std::vector<size_t> changed{patch};
  for (const size_t q : graph::edit::refreshCallers(work, patch)) changed.push_back(q);
  if (Status s = m.commitStructural(work, changed, graph::kNone); !s) return std::move(s).error();
  return id;
}

Result<size_t> PatchEditor::makeSubcircuit(size_t patch, std::span<const NodeIndex> nodes, std::string name) {
  Impl& m = *impl_;
  m.note.clear();
  if (Status s = m.needLowered(); !s) return std::move(s).error();
  if (patch >= m.good.file.patches.size()) return refuse("no such patch");
  (void)m.drawnPatch(patch);
  PatchFile work = m.drawnFile();
  auto r = graph::edit::makeSubcircuit(work, patch, nodes, std::move(name));
  if (!r) return std::move(r).error();
  // Lay the new patch out (its own drawing has no stored routes yet).
  Impl::complete(work.patches[r->patch]);
  const size_t changed[2] = {patch, r->patch};
  if (Status s = m.commitStructural(work, changed, r->patch); !s) return std::move(s).error();
  return r->patch;
}

Status PatchEditor::moveNodes(size_t patch, std::span<const NodeIndex> nodes, float dx, float dy) {
  Impl& m = *impl_;
  m.note.clear();
  if (Status s = m.needLowered(); !s) return s;
  if (patch >= m.good.file.patches.size()) return refuse("no such patch");
  if (nodes.empty() || (dx == 0.0f && dy == 0.0f)) return Ok{};
  (void)m.drawnPatch(patch);
  PatchFile laid = m.drawnFile();
  graph::Patch& p = laid.patches[patch];
  std::vector<uint8_t> moved(p.nodeCount(), 0);
  for (const NodeIndex v : nodes) {
    if (v >= p.nodeCount()) continue;
    graph::NodeLayout l = p.layout(v);
    l.placed = true;
    l.x = std::round((l.x + dx) / 4.0f) * 4.0f;
    l.y = std::round((l.y + dy) / 4.0f) * 4.0f;
    p.setLayout(v, l);
    moved[v] = 1;
  }
  // Routes of the wires that touch a moved node are re-routed (a Z) when drawn; wires between two moved nodes move
  // with them.
  for (graph::WireIndex w = 0; w < p.wireCount(); ++w) {
    const graph::Wire& wr = p.wire(w);
    const bool a = moved[wr.from.node] != 0, b = moved[wr.to.node] != 0;
    if (a && b) {
      std::vector<graph::Point2> via(p.wireVia(w).begin(), p.wireVia(w).end());
      for (graph::Point2& q : via) q = {q.x + dx, q.y + dy};
      p.setWireVia(w, std::move(via));
    } else if (a || b) {
      p.setWireVia(w, {});
    }
  }
  Impl::complete(p);
  return m.commitLayout(laid);
}

Status PatchEditor::tidy(size_t patch) {
  Impl& m = *impl_;
  m.note.clear();
  if (Status s = m.needLowered(); !s) return s;
  if (patch >= m.good.file.patches.size()) return refuse("no such patch");
  (void)m.drawnPatch(patch);
  PatchFile laid = m.drawnFile();
  (void)circuit::tidy(laid.patches[patch]);
  return m.commitLayout(laid);
}

bool PatchEditor::undo() {
  Impl& m = *impl_;
  if (m.cursor == 0) return false;
  m.note.clear();
  const Step s = m.steps[--m.cursor];
  m.apply(s.offset, static_cast<uint32_t>(s.inserted.size()), s.removed, EditSource::Undo, nullptr, false, false);
  return true;
}

bool PatchEditor::redo() {
  Impl& m = *impl_;
  if (m.cursor >= m.steps.size()) return false;
  m.note.clear();
  const Step s = m.steps[m.cursor++];
  m.apply(s.offset, static_cast<uint32_t>(s.removed.size()), s.inserted, EditSource::Redo, nullptr, false, false);
  return true;
}

Status PatchEditor::save() {
  Impl& m = *impl_;
  if (m.lowered) {
    PatchFile laid = m.drawnFile();
    if (Status s = m.commitLayout(laid); !s) return s;
  }
  const fs::path target = pathOf(m.path);
  fs::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return Error{ErrorCode::Io, "cannot write " + m.path, {}};
    out.write(m.text.data(), static_cast<std::streamsize>(m.text.size()));
    out.flush();
    if (!out) return Error{ErrorCode::Io, "cannot write " + m.path, {}};
  }
  std::error_code ec;
  fs::rename(tmp, target, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return Error{ErrorCode::Io, "cannot replace " + m.path, {}};
  }
  m.fileText = m.text;
  EditorChange c;
  c.source = EditSource::Load;
  if (onChange) onChange(c);
  return Ok{};
}

void PatchEditor::setProbes(std::vector<graph::ProbeTarget> targets, std::vector<uint32_t> ids) {
  impl_->probeTargets = std::move(targets);
  impl_->probeIds = std::move(ids);
  impl_->probeIds.resize(impl_->probeTargets.size(), 0);
  // T4.4-fix1: an external player compiles them into its own program (now; the editor's check has no program).
  if (impl_->player && impl_->player->external() && !impl_->playingName.empty())
    impl_->player->setProbeTargets(impl_->probeTargets, impl_->probeIds);
}

const PatchEditor::PlayedProgram* PatchEditor::played() const noexcept {
  if (impl_->player && impl_->player->external()) return impl_->playingName.empty() ? nullptr : impl_->player->playedProgram();
  return impl_->played.get();
}

Status PatchEditor::check(size_t patch) {
  Impl& m = *impl_;
  size_t idx = patch;
  if (idx == graph::kNone) idx = m.playingName.empty() ? 0 : patchIndex(m.playingName);
  return m.runCheck(idx);
}

Status PatchEditor::play(size_t patch) {
  Impl& m = *impl_;
  if (!m.player) return refuse("no audio output for the editor");
  if (patch >= m.good.file.patches.size()) return refuse("no such patch");
  const std::string was = m.playingName;
  m.playingName = m.good.file.patches[patch].name();
  Status s = m.runCheck(patch);
  if (m.player->external()) {  // T4.4-fix1: it plays anyway (the project keeps its last good text): follow it
    m.player->setProbeTargets(m.probeTargets, m.probeIds);
    return Ok{};
  }
  if (!s) m.playingName = was;
  return s;
}

void PatchEditor::stop() {
  Impl& m = *impl_;
  if (m.player) m.player->stop();
  m.playingName.clear();
  m.played.reset();
  EditorChange c;
  if (onChange) onChange(c);
}

}  // namespace pq::views::circuit
