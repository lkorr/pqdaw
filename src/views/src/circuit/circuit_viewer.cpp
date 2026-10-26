// The read-only circuit viewer (T4.2/T4.3): .pqp loading with imports, the full-window viewer, View > Open Patch.

#include "pq/views/circuit/circuit_viewer.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "pq/ui/controls.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views::circuit {

namespace fs = std::filesystem;

namespace {

// UTF-8 strings ↔ paths (std::filesystem's narrow strings are the ANSI code page on Windows).
fs::path pathOf(std::string_view utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

Result<std::string> readText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Error{ErrorCode::NotFound, "cannot open " + utf8Of(path), {}};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string formatAll(const graph::ParseResult& r, std::string_view source) {
  std::string s;
  for (const graph::Diagnostic& d : r.diagnostics) {
    if (!s.empty()) s += '\n';
    s += graph::formatDiagnostic(source, d);
  }
  return s;
}

// Parses documents and their imports (relative to the importing file), each file once, with a cycle guard.
class DocLoader final : public graph::ImportLoader {
public:
  Result<std::shared_ptr<PatchDocument>> parseDoc(std::string path, std::string source) {
    auto doc = std::make_shared<PatchDocument>();
    doc->path = std::move(path);
    doc->source = std::move(source);
    active_.insert(doc->path);
    std::vector<std::shared_ptr<const PatchDocument>>* outer = imports_;
    imports_ = &doc->imports;
    graph::ParseOptions o;
    o.imports = this;
    doc->parsed = graph::parseDetailed(doc->source, doc->path, o);
    imports_ = outer;
    active_.erase(doc->path);
    if (!doc->parsed.ok()) return Error{ErrorCode::Parse, formatAll(doc->parsed, doc->source), {}};
    done_[doc->path] = doc;
    return doc;
  }

  Result<std::vector<graph::SubcircuitRef>> load(std::string_view path, std::string_view importer) override {
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(pathOf(importer).parent_path() / pathOf(path), ec);
    const std::string key = utf8Of(full);
    std::shared_ptr<const PatchDocument> doc;
    if (auto it = done_.find(key); it != done_.end()) {
      doc = it->second;
    } else {
      if (active_.count(key)) return Error{ErrorCode::InvalidArgument, "import cycle", {}};
      auto text = readText(full);
      if (!text) return std::move(text).error();
      auto parsed = parseDoc(key, std::move(*text));
      if (!parsed) return Error{ErrorCode::Parse, "the imported file has errors:\n" + parsed.error().message, {}};
      doc = *parsed;
    }
    if (imports_) imports_->push_back(doc);
    return graph::signatures(doc->parsed.file);
  }

private:
  std::map<std::string, std::shared_ptr<PatchDocument>> done_;
  std::set<std::string> active_;
  std::vector<std::shared_ptr<const PatchDocument>>* imports_ = nullptr;
};

std::string fileName(const std::string& path) { return utf8Of(pathOf(path).filename()); }

}  // namespace

const graph::Patch* PatchDocument::find(std::string_view name) const noexcept {
  if (const graph::Patch* p = parsed.file.find(name)) return p;
  for (const auto& d : imports)
    if (const graph::Patch* p = d->find(name)) return p;
  return nullptr;
}

const PatchDocument* PatchDocument::owner(const graph::Patch& patch) const noexcept {
  for (const graph::Patch& p : parsed.file.patches)
    if (&p == &patch) return this;
  for (const auto& d : imports)
    if (const PatchDocument* o = d->owner(patch)) return o;
  return nullptr;
}

Result<std::shared_ptr<const PatchDocument>> loadPatchDocument(std::string path, std::string source) {
  DocLoader loader;
  std::error_code ec;
  const fs::path full = fs::weakly_canonical(pathOf(path), ec);
  auto doc = loader.parseDoc(ec ? path : utf8Of(full), std::move(source));
  if (!doc) return std::move(doc).error();
  return std::shared_ptr<const PatchDocument>(*doc);
}

Result<std::shared_ptr<const PatchDocument>> loadPatchDocument(const std::string& path) {
  auto text = readText(pathOf(path));
  if (!text) return std::move(text).error();
  return loadPatchDocument(path, std::move(*text));
}

// ---- viewer ---------------------------------------------------------------------------------------------------------

CircuitViewer::CircuitViewer(std::shared_ptr<const PatchDocument> doc) : Popover("circuit-viewer"), doc_(std::move(doc)) {
  setPadding(ui::Insets::all(4));
  setGap(4);
  auto& bar = add<ui::Widget>("bar");
  bar.setLayout(ui::LayoutKind::Row);
  bar.setHeight(ui::Length::fixed(20));
  bar.setGap(4);
  back_ = &bar.add<ui::Button>("back", "Back");
  back_->setWidth(ui::Length::fixed(48));
  back_->onClick = [this] { (void)back(); };
  title_ = &bar.add<ui::Label>("title", "", ui::FontRoleId::Small, ui::TextTone::Primary);
  title_->setWidth(ui::Length::flex());
  auto& tidyButton = bar.add<ui::Button>("tidy", "Tidy");
  tidyButton.setWidth(ui::Length::fixed(48));
  tidyButton.onClick = [this] { (void)tidy(); };
  auto& fit = bar.add<ui::Button>("fit", "Fit");
  fit.setWidth(ui::Length::fixed(40));
  fit.onClick = [this] { canvas_->fitAll(); };
  auto& close = bar.add<ui::Button>("close", "Close");
  close.setWidth(ui::Length::fixed(52));
  close.onClick = [this] {
    if (ui::OverlayLayer* layer = ui::OverlayLayer::of(*this)) layer->close(*this);
  };
  canvas_ = &add<CircuitCanvas>("canvas");
  canvas_->setSize(ui::Length::flex(), ui::Length::flex());
  canvas_->onOpen = [this](uint32_t node) { (void)openNode(node); };
}

void CircuitViewer::present(const graph::Patch& p, bool fit) {
  shown_ = &p;
  canvas_->setScene(buildScene(p, {}, &laidOut_), fit);
  const PatchDocument* owner = doc_->owner(p);
  titleText_ = fileName(owner ? owner->path : doc_->path);
  for (const graph::Patch* q : backStack_) titleText_ += " \xE2\x80\xBA " + q->name();
  titleText_ += " \xE2\x80\xBA " + p.name();
  title_->setText(titleText_);
  back_->setEnabled(!backStack_.empty());
}

bool CircuitViewer::show(std::string_view patchName) {
  const graph::Patch* p = patchName.empty() ? (doc_->parsed.file.patches.empty() ? nullptr : &doc_->parsed.file.patches.front())
                                             : doc_->find(patchName);
  if (!p) return false;
  backStack_.clear();
  present(*p, true);
  return true;
}

bool CircuitViewer::openNode(uint32_t node) {
  if (!shown_ || node >= laidOut_.nodeCount() || laidOut_.node(node).kind != graph::NodeKind::Subcircuit) return false;
  const graph::Patch* callee = doc_->find(laidOut_.subcircuit(node).patch);
  if (!callee) return false;
  backStack_.push_back(shown_);
  present(*callee, true);
  return true;
}

bool CircuitViewer::back() {
  if (backStack_.empty()) return false;
  const graph::Patch* p = backStack_.back();
  backStack_.pop_back();
  present(*p, true);
  return true;
}

graph::AutoLayoutStats CircuitViewer::tidy() {
  if (!shown_) return {};
  graph::Patch p = *shown_;
  const graph::AutoLayoutStats st = circuit::tidy(p);
  canvas_->setScene(buildScene(p, {}, &laidOut_), false);
  return st;
}

Result<std::string> CircuitViewer::laidOutText() const {
  if (!shown_) return Error{ErrorCode::InvalidArgument, "no patch shown", {}};
  const PatchDocument* owner = doc_->owner(*shown_);
  if (!owner) return Error{ErrorCode::NotFound, "the shown patch has no file", {}};
  graph::PatchFile file = owner->parsed.file;
  for (graph::Patch& p : file.patches)
    if (p.name() == shown_->name()) p = laidOut_;
  return graph::spliceLayout(owner->source, owner->parsed, file);
}

bool CircuitViewer::onKey(const ui::KeyEvent& e) {
  if (e.down && e.key == ui::key::kBackspace && (e.mods & ui::mod::kChordMask) == 0 && back()) return true;
  return Popover::onKey(e);
}

ui::InfoText CircuitViewer::info() const {
  return {"Circuit View", "The patch as a circuit. Double-click a subcircuit to open it, Backspace goes back, Esc closes."};
}

CircuitViewer* openCircuitViewer(ui::Widget& from, std::shared_ptr<const PatchDocument> doc, std::string_view patch) {
  ui::OverlayLayer* layer = ui::OverlayLayer::of(from);
  if (!layer || !doc) return nullptr;
  ui::UiTree* tree = layer->tree();
  auto viewer = std::make_unique<CircuitViewer>(std::move(doc));
  if (!viewer->show(patch)) return nullptr;
  const float sc = tree ? tree->scale() : 1.0f;
  const float w = tree ? static_cast<float>(tree->width()) / sc : 800.0f, h = tree ? static_cast<float>(tree->height()) / sc : 600.0f;
  viewer->setSize(ui::Length::fixed(w), ui::Length::fixed(h));
  ui::OverlayOptions o;
  o.placement = ui::Placement::AtPoint;
  o.dismissOnOutsideClick = false;
  auto& opened = static_cast<CircuitViewer&>(layer->open(std::move(viewer), std::move(o)));
  if (tree) tree->setFocus(&opened.canvas());
  return &opened;
}

namespace {

// Holds the view.openPatch handler for the shell's lifetime (a hidden child of a slot, like the preferences host).
class CircuitViewerHost : public ui::Widget {
public:
  explicit CircuitViewerHost(Shell& shell) : Widget("circuit-viewer-host"), ctx_(shell.context()), layer_(&shell.overlay()) {
    setVisible(false);
    ctx_.setViewAction("view.openPatch", [this] {
      FileDialogs* d = ctx_.fileDialogs();
      if (!d) return;
      std::weak_ptr<bool> alive = alive_;
      d->choosePatchFile([this, alive](std::string path) {
        if (alive.expired() || path.empty()) return;
        auto doc = loadPatchDocument(path);
        if (!doc) {
          ctx_.setStatus("Cannot open the patch: " + doc.error().message.substr(0, doc.error().message.find('\n')));
          return;
        }
        if (!openCircuitViewer(*layer_, *doc)) ctx_.setStatus("The file has no patch to show");
      });
    });
  }
  ~CircuitViewerHost() override { ctx_.clearViewAction("view.openPatch"); }

private:
  ViewContext& ctx_;
  ui::OverlayLayer* layer_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

void installCircuitViewer(Shell& shell) { shell.slot(ShellSlot::ClipDetail).add<CircuitViewerHost>(shell); }

}  // namespace pq::views::circuit
