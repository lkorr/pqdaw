// The circuit editor window (T4.4): canvas + text pane over one PatchEditor. See pq/views/circuit/circuit_editor.h.

#include "pq/views/circuit/circuit_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "pq/ui/controls.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/views/circuit/circuit_scene.h"
#include "probe_paint.h"

namespace pq::views::circuit {

using graph::NodeIndex;

// ---- syntax colours ----------------------------------------------------------------------------------------------------

namespace {

bool identStart(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool identChar(char c) noexcept { return identStart(c) || (c >= '0' && c <= '9'); }
bool digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool isKeywordWord(std::string_view w) noexcept {
  static constexpr std::string_view kWords[] = {
      "patch", "import", "param", "in", "out", "export", "solve", "iterations", "layout", "true", "false", "unknown",
      "node", "at", "wire", "via", "comment", "rot", "flip", "real", "bool", "audio", "const", "global", "voice",
      "mono", "stereo", "ch"};
  for (const std::string_view k : kWords)
    if (k == w) return true;
  return false;
}

}  // namespace

uint32_t highlightPatchLine(std::string_view line, uint32_t state, std::vector<ui::CodeSpan>& out) {
  const uint32_t n = static_cast<uint32_t>(line.size());
  uint32_t i = 0;
  auto span = [&](uint32_t b, uint32_t e, ui::CodeStyle s) {
    if (e > b) out.push_back({b, e, s});
  };
  if (state == 1) {
    const size_t close = line.find("*/");
    if (close == std::string_view::npos) {
      span(0, n, ui::CodeStyle::Comment);
      return 1;
    }
    i = static_cast<uint32_t>(close + 2);
    span(0, i, ui::CodeStyle::Comment);
  }
  while (i < n) {
    const char c = line[i];
    if (c == '/' && i + 1 < n && line[i + 1] == '/') {
      span(i, n, ui::CodeStyle::Comment);
      return 0;
    }
    if (c == '/' && i + 1 < n && line[i + 1] == '*') {
      const size_t close = line.find("*/", i + 2);
      if (close == std::string_view::npos) {
        span(i, n, ui::CodeStyle::Comment);
        return 1;
      }
      span(i, static_cast<uint32_t>(close + 2), ui::CodeStyle::Comment);
      i = static_cast<uint32_t>(close + 2);
      continue;
    }
    if (c == '"') {
      uint32_t j = i + 1;
      while (j < n && line[j] != '"') j += line[j] == '\\' ? 2 : 1;
      j = std::min(n, j + 1);
      span(i, j, ui::CodeStyle::String);
      i = j;
      continue;
    }
    if (digit(c) || (c == '.' && i + 1 < n && digit(line[i + 1]))) {
      uint32_t j = i;
      while (j < n && (digit(line[j]) || line[j] == '.')) ++j;
      if (j < n && (line[j] == 'e' || line[j] == 'E') && j + 1 < n &&
          (digit(line[j + 1]) || ((line[j + 1] == '-' || line[j + 1] == '+') && j + 2 < n && digit(line[j + 2])))) {
        j += 2;
        while (j < n && digit(line[j])) ++j;
      }
      while (j < n && identChar(line[j])) ++j;  // a unit suffix
      if (j < n && line[j] == '%') ++j;
      span(i, j, ui::CodeStyle::Number);
      i = j;
      continue;
    }
    if (identStart(c) || (c == '@' && i + 1 < n && identStart(line[i + 1]))) {
      uint32_t j = i + 1;
      while (j < n && identChar(line[j])) ++j;
      const std::string_view w = line.substr(i, j - i);
      uint32_t k = j;
      while (k < n && line[k] == ' ') ++k;
      if (c == '@') span(i, j, ui::CodeStyle::Name);
      else if (isKeywordWord(w)) span(i, j, ui::CodeStyle::Keyword);
      else if (k < n && line[k] == '(') span(i, j, ui::CodeStyle::Name);
      i = j;
      continue;
    }
    if (std::string_view("+-*/%<>=!&|^?:()[]{},.;").find(c) != std::string_view::npos) {
      span(i, i + 1, ui::CodeStyle::Punctuation);
      ++i;
      continue;
    }
    ++i;
  }
  return 0;
}

// ---- the editing canvas ------------------------------------------------------------------------------------------------

EditCanvas::EditCanvas(std::string name) : CircuitCanvas(std::move(name)) {}

void EditCanvas::setStale(bool s) {
  if (s == stale_) return;
  stale_ = s;
  markDirty();
}

uint32_t EditCanvas::pinNear(ui::Vec2 d, bool wantInput) const {
  const auto& scp = scene();
  if (!scp) return UINT32_MAX;
  const CircuitScene& sc = *scp;
  const graph::Point2 w = toWorld(d);
  const float us = tree() ? tree()->scale() : 1.0f;
  const float radius = 8.0f * us / std::max(deviceScale(), 1e-3f);  // 8 logical px in world units
  // The node under the pointer: any of its pins of the wanted direction, nearest in y.
  const CanvasHit hit = hitTest(d);
  uint32_t node = UINT32_MAX;
  if (hit.kind == CanvasHit::Kind::Node) node = hit.index;
  else if (hit.kind == CanvasHit::Kind::Pin) node = sc.pins[hit.index].node;
  uint32_t best = UINT32_MAX;
  float bestD = 1e30f;
  auto consider = [&](uint32_t v, bool anyDistance) {
    const SceneNode& n = sc.nodes[v];
    for (uint32_t k = 0; k < n.pinCount; ++k) {
      const ScenePin& p = sc.pins[n.firstPin + k];
      if (p.input != wantInput) continue;
      const float dd = anyDistance ? std::fabs(p.y - w.y) : std::max(std::fabs(p.x - w.x), std::fabs(p.y - w.y));
      if ((anyDistance || dd <= radius) && dd < bestD) {
        bestD = dd;
        best = n.firstPin + k;
      }
    }
  };
  if (node != UINT32_MAX) {
    consider(node, true);
    return best;
  }
  for (uint32_t v = 0; v < sc.nodes.size(); ++v) {
    const SceneNode& n = sc.nodes[v];
    if (w.x < n.x - radius || w.x > n.x + n.w + radius || w.y < n.y - radius || w.y > n.y + n.h + radius) continue;
    consider(v, false);
  }
  return best;
}

bool EditCanvas::onPointer(const ui::PointerEvent& e) {
  if (e.type == ui::PointerType::Move || e.type == ui::PointerType::Down) {
    pointer_ = e.pos;
    if (overlay_ && overlay_->readout) markDirty();  // the readout follows the pointer
  }
  if (!scene() || !editable_ || stale_) {
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && e.clickCount >= 2 && scene()) {
      const CanvasHit hit = hitTest(e.pos);
      if (hit.kind == CanvasHit::Kind::Node) {
        const Symbol sym = scene()->specs[scene()->nodes[hit.index].spec].symbol;
        if (sym != Symbol::Subcircuit && sym != Symbol::Filter) return true;
      }
    }
    return CircuitCanvas::onPointer(e);
  }
  const CircuitScene& sc = *scene();
  const float us = tree() ? tree()->scale() : 1.0f;
  switch (e.type) {
    case ui::PointerType::Down: {
      if (e.button != ui::PointerButton::Left || (e.mods & ui::mod::kAlt)) return CircuitCanvas::onPointer(e);
      const CanvasHit hit = hitTest(e.pos);
      if (e.clickCount >= 2) {
        if (hit.kind == CanvasHit::Kind::None) {
          if (tree()) tree()->setFocus(this);
          if (onAddAt) onAddAt(toWorld(e.pos));
          return true;
        }
        if (hit.kind == CanvasHit::Kind::Node) {
          const Symbol sym = sc.specs[sc.nodes[hit.index].spec].symbol;
          if (sym != Symbol::Subcircuit && sym != Symbol::Filter) {
            if (onRename) onRename(hit.index);
            return true;
          }
        }
        return CircuitCanvas::onPointer(e);
      }
      if (hit.kind == CanvasHit::Kind::Pin) {
        if (tree()) tree()->setFocus(this);
        drag_ = Drag::Wire;
        fromPin_ = hit.index;
        targetPin_ = UINT32_MAX;
        press_ = last_ = e.pos;
        markDirty();
        return true;
      }
      if (hit.kind == CanvasHit::Kind::Node && (e.mods & (ui::mod::kShift | ui::mod::kCtrl)) == 0) {
        if (!nodeSelected(hit.index)) (void)CircuitCanvas::onPointer(e);  // select it
        else if (tree()) tree()->setFocus(this);
        drag_ = Drag::MaybeMove;
        press_ = last_ = e.pos;
        fromPin_ = hit.index;  // the pressed node (a click without a move selects only it)
        return true;
      }
      return CircuitCanvas::onPointer(e);
    }
    case ui::PointerType::Move: {
      if (drag_ == Drag::Wire) {
        last_ = e.pos;
        targetPin_ = pinNear(e.pos, !sc.pins[fromPin_].input);
        markDirty();
        return true;
      }
      if (drag_ == Drag::MaybeMove || drag_ == Drag::Move) {
        last_ = e.pos;
        if (drag_ == Drag::MaybeMove &&
            std::max(std::fabs(e.pos.x - press_.x), std::fabs(e.pos.y - press_.y)) >= 3.0f * us)
          drag_ = Drag::Move;
        if (drag_ == Drag::Move) markDirty();
        return true;
      }
      return CircuitCanvas::onPointer(e);
    }
    case ui::PointerType::Up: {
      if (drag_ == Drag::Wire) {
        drag_ = Drag::None;
        const uint32_t to = pinNear(e.pos, !sc.pins[fromPin_].input);
        markDirty();
        if (to != UINT32_MAX && onConnect) {
          const bool fromInput = sc.pins[fromPin_].input;
          onConnect(fromInput ? to : fromPin_, fromInput ? fromPin_ : to);
        }
        return true;
      }
      if (drag_ == Drag::Move) {
        drag_ = Drag::None;
        const float s = deviceScale();
        const float dx = std::round((last_.x - press_.x) / s / 4.0f) * 4.0f;
        const float dy = std::round((last_.y - press_.y) / s / 4.0f) * 4.0f;
        (void)CircuitCanvas::onPointer(e);
        markDirty();
        if ((dx != 0.0f || dy != 0.0f) && onMove) onMove(selection().nodes, dx, dy);
        return true;
      }
      if (drag_ == Drag::MaybeMove) {
        drag_ = Drag::None;
        (void)CircuitCanvas::onPointer(e);
        const uint32_t one[1] = {fromPin_};
        select(one, {});
        return true;
      }
      return CircuitCanvas::onPointer(e);
    }
    default: return CircuitCanvas::onPointer(e);
  }
}

void EditCanvas::onCaptureLost() {
  if (drag_ != Drag::None) markDirty();
  drag_ = Drag::None;
  CircuitCanvas::onCaptureLost();
}

void EditCanvas::paintOverlay(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th || !scene()) return;
  const CircuitScene& sc = *scene();
  const ui::IRect r = rect();
  const float s = deviceScale();
  const float t = std::max(1.0f, std::round(ctx.scale.exact(1.5f)));
  if (drag_ == Drag::Move) {
    const float dx = std::round((last_.x - press_.x) / s / 4.0f) * 4.0f, dy = std::round((last_.y - press_.y) / s / 4.0f) * 4.0f;
    for (const uint32_t v : selection().nodes) {
      const SceneNode& n = sc.nodes[v];
      const ui::Vec2 a = toDevice({n.x + dx, n.y + dy}), b = toDevice({n.x + n.w + dx, n.y + n.h + dy});
      ctx.dl.addRectStroke({std::round(a.x), std::round(a.y), std::round(b.x), std::round(b.y)}, th->circuit.selected, t);
    }
  }
  if (drag_ == Drag::Wire && fromPin_ < sc.pins.size()) {
    const ScenePin& from = sc.pins[fromPin_];
    const ui::Vec2 a = toDevice({from.x, from.y});
    ui::Vec2 b = last_;
    if (targetPin_ != UINT32_MAX) b = toDevice({sc.pins[targetPin_].x, sc.pins[targetPin_].y});
    const float mx = std::round((a.x + b.x) * 0.5f);
    auto h = [&](float x0, float x1, float y) {
      ctx.dl.addRectFilled({std::min(x0, x1), std::round(y - t * 0.5f), std::max(x0, x1) + t, std::round(y - t * 0.5f) + t},
                           th->circuit.selected);
    };
    auto v = [&](float y0, float y1, float x) {
      ctx.dl.addRectFilled({std::round(x - t * 0.5f), std::min(y0, y1), std::round(x - t * 0.5f) + t, std::max(y0, y1) + t},
                           th->circuit.selected);
    };
    h(a.x, mx, a.y);
    v(a.y, b.y, mx);
    h(mx, b.x, b.y);
    // Every pin of the node under the pointer (hidden optional inputs too), the target highlighted.
    const CanvasHit hit = hitTest(last_);
    uint32_t node = UINT32_MAX;
    if (hit.kind == CanvasHit::Kind::Node) node = hit.index;
    else if (hit.kind == CanvasHit::Kind::Pin) node = sc.pins[hit.index].node;
    if (node != UINT32_MAX) {
      const SceneNode& n = sc.nodes[node];
      const float d = std::max(3.0f, std::round(ctx.scale.exact(3.0f)));
      for (uint32_t k = 0; k < n.pinCount; ++k) {
        const ScenePin& p = sc.pins[n.firstPin + k];
        if (p.input == from.input) continue;
        const ui::Vec2 q = toDevice({p.x, p.y});
        const bool target = n.firstPin + k == targetPin_;
        const float rr = target ? d * 1.6f : d;
        ctx.dl.addRectFilled({std::round(q.x - rr), std::round(q.y - rr), std::round(q.x + rr), std::round(q.y + rr)},
                             target ? th->circuit.selected : th->circuit.pinText);
      }
    }
  }
  if (overlay_) {  // T4.5: cost labels, then the probe readout on top
    if (lod() != Lod::Far) {
      for (const ProbeOverlay::Label& l : overlay_->nodeLabels) {
        if (l.item >= sc.nodes.size()) continue;
        const SceneNode& n = sc.nodes[l.item];
        paintCostLabel(ctx, toDevice({n.x + n.w * 0.5f, n.y}), l.text, l.heat, true);
      }
      for (const ProbeOverlay::Label& l : overlay_->wireLabels) {
        if (l.item >= sc.wires.size()) continue;
        const SceneWire& w = sc.wires[l.item];
        if (w.pointCount < 2) continue;
        const uint32_t k = w.firstPoint + (w.pointCount - 1) / 2;
        const graph::Point2 a = sc.points[k], b = sc.points[k + 1];
        paintCostLabel(ctx, toDevice({(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f}), l.text, l.heat, false);
      }
    }
    if (overlay_->readout) paintProbeReadout(ctx, *overlay_, pointer_, r);
  }
  if (stale_) {
    ctx.dl.addRectFilled(r.toRect(), th->code.stale);
    if (ctx.res.text && ctx.res.uiFonts) {
      const std::string_view msg = "The text has errors: this is the last version that parsed.";
      const int32_t w = ctx.res.text->measure(ctx.res.uiFonts->small, msg);
      (void)ctx.res.text->draw(ctx.dl, ctx.res.uiFonts->small,
                               {static_cast<float>(r.x + (r.w - w) / 2), static_cast<float>(r.y + ctx.scale.px(18))}, msg,
                               th->code.errorText);
    }
  }
}

// ---- the editor window --------------------------------------------------------------------------------------------------

namespace {

// The add / rename type-in: a one-field popover.
class TypeIn : public ui::Popover {
public:
  TypeIn(std::string label, std::string text) : Popover("circuit-typein") {
    setPadding(ui::Insets::all(4));
    setGap(4);
    setLayout(ui::LayoutKind::Row);
    auto& l = add<ui::Label>("label", std::move(label), ui::FontRoleId::Small, ui::TextTone::Secondary);
    l.setWidth(ui::Length::fixed(52));
    field = &add<ui::TextEdit>("field", std::move(text));
    field->setWidth(ui::Length::flex());
    setSize(ui::Length::fixed(300), ui::Length::fixed(26));
  }
  ui::TextEdit* field = nullptr;
};

}  // namespace

CircuitEditor::CircuitEditor(std::unique_ptr<PatchEditor> document) : Popover("circuit-editor"), doc_(std::move(document)) {
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
  play_ = &bar.add<ui::Toggle>("play", "Play", ui::ToggleStyle::Accent, false);
  play_->setWidth(ui::Length::fixed(48));
  play_->onToggle = [this](bool) { (void)togglePlay(); };
  cost_ = &bar.add<ui::Toggle>("cost", "Cost", ui::ToggleStyle::Accent, false);  // T4.5: ns per sample per part
  cost_->setWidth(ui::Length::fixed(44));
  cost_->onToggle = [this](bool on) { setCostShown(on); };
  auto& tidyButton = bar.add<ui::Button>("tidy", "Tidy");
  tidyButton.setWidth(ui::Length::fixed(44));
  tidyButton.onClick = [this] { (void)tidy(); };
  auto& fit = bar.add<ui::Button>("fit", "Fit");
  fit.setWidth(ui::Length::fixed(36));
  fit.onClick = [this] { canvas_->fitAll(); };
  auto& saveButton = bar.add<ui::Button>("save", "Save");
  saveButton.setWidth(ui::Length::fixed(44));
  saveButton.onClick = [this] { (void)save(); };
  auto& close = bar.add<ui::Button>("close", "Close");
  close.setWidth(ui::Length::fixed(48));
  close.onClick = [this] {
    if (doc_->dirty() && !closeArmed_) {
      closeArmed_ = true;
      setStatus("Unsaved changes: Save keeps them; Close again closes without saving.");
      return;
    }
    if (doc_->playing()) doc_->stop();
    if (ui::OverlayLayer* layer = ui::OverlayLayer::of(*this)) layer->close(*this);
  };

  auto& body = add<ui::Splitter>("body", ui::Splitter::Axis::Horizontal, 420.0f, ui::Splitter::Anchor::Second);
  body.setHeight(ui::Length::flex());
  body.setLimits(200, ui::kUnbounded, 240);
  body.setHandleThickness(4);
  canvas_ = &body.add<EditCanvas>("canvas");
  text_ = &body.add<ui::CodeEditor>("text");
  status_ = &add<ui::Label>("status", "", ui::FontRoleId::Small, ui::TextTone::Secondary);
  status_->setHeight(ui::Length::fixed(16));

  text_->setHighlighter(highlightPatchLine);
  text_->setText(doc_->text());
  text_->onEdit = [this](uint32_t offset, uint32_t removed, std::string_view inserted, bool typing) {
    if (readOnly()) return;
    doc_->replaceText(offset, removed, inserted, typing);
  };
  doc_->onChange = [this](const EditorChange& c) { documentChanged(c); };
  canvas_->onOpen = [this](uint32_t node) { (void)openNode(node); };
  canvas_->onMove = [this](std::vector<uint32_t> nodes, float dx, float dy) {
    const size_t idx = shownIndex();
    if (idx == graph::kNone) return;
    if (Status s = doc_->moveNodes(idx, nodes, dx, dy); !s) setStatus(s.error().message);
  };
  canvas_->onConnect = [this](uint32_t fromPin, uint32_t toPin) {
    const size_t idx = shownIndex();
    const auto& sc = canvas_->scene();
    if (idx == graph::kNone || !sc) return;
    const ScenePin& a = sc->pins[fromPin];
    const ScenePin& b = sc->pins[toPin];
    if (Status s = doc_->connect(idx, {a.node, a.port}, {b.node, b.port}); !s) setStatus(s.error().message);
  };
  canvas_->onAddAt = [this](graph::Point2 w) {
    const ui::Vec2 d = canvas_->toDevice(w);
    beginAdd(w, d);
  };
  canvas_->onRename = [this](uint32_t node) { beginRename(node); };
  canvas_->onHoverChanged = [this](const CanvasHit& h) { hoverChanged(h); };
  canvas_->setProbeOverlay(&overlay_);
}

CircuitEditor::~CircuitEditor() {
  *alive_ = false;
  if (doc_) {
    doc_->onChange = nullptr;
    if (doc_->playing()) doc_->stop();  // a closed editor leaves nothing playing
    if (PatchPlayer* pl = doc_->player()) pl->setMetering(false);
  }
}

size_t CircuitEditor::shownIndex() const noexcept { return doc_->patchIndex(shown_); }

const graph::Patch* CircuitEditor::shownGraph() {
  const size_t idx = shownIndex();
  if (idx != graph::kNone) return &doc_->drawn(idx);
  if (!shown_.empty() && doc_->findCallee(shown_)) return &importedShown_;
  return nullptr;
}

void CircuitEditor::setStatus(std::string s) {
  statusText_ = std::move(s);
  status_->setText(statusText_);
}

void CircuitEditor::updateTitle() {
  std::string file = doc_->path();
  if (const size_t k = file.find_last_of("/\\"); k != std::string::npos) file = file.substr(k + 1);
  titleText_ = file + (doc_->dirty() ? " \xE2\x80\xA2" : "");
  for (const std::string& q : backStack_) titleText_ += " \xE2\x80\xBA " + q;
  titleText_ += " \xE2\x80\xBA " + shown_;
  if (readOnly()) titleText_ += "  (imported: read-only)";
  title_->setText(titleText_);
  back_->setEnabled(!backStack_.empty());
  play_->setOn(doc_->playing());
}

void CircuitEditor::present(const std::string& patch, bool fit) {
  const bool wasLocal = shownIndex() != graph::kNone || shown_.empty();
  shown_ = patch;
  const bool local = shownIndex() != graph::kNone;
  canvas_->setEditable(local);
  if (!local) {
    // An imported patch: shown laid out, with its own file's text (read-only).
    importedShown_ = *doc_->findCallee(patch);
    std::vector<SymbolSpec> specs = describeAll(importedShown_);
    graph::completeLayout(importedShown_, geometryOf(importedShown_, specs));
    for (const auto& d : doc_->imports())
      if (const PatchDocument* owner = d->owner(*doc_->findCallee(patch))) {
        text_->setText(owner->source);
        break;
      }
    text_->setReadOnly(true);
  } else if (!wasLocal) {
    text_->setText(doc_->text());
    text_->setReadOnly(false);
  }
  // T4.5: another patch: its probes (ids are per shown patch) and the hover state start afresh.
  probeIdOf_.clear();
  hoverWire_ = probeWire_ = hoverProbe_ = UINT32_MAX;
  rebuildScene(fit);
  refreshProbes(true);
  refreshMarks();
  updateTitle();
  if (local && doc_->textLowered() && static_cast<size_t>(shownIndex()) < doc_->parsed().patchSpans.size())
    text_->reveal(doc_->parsed().patchSpans[shownIndex()].begin);
}

bool CircuitEditor::show(std::string_view patch) {
  std::string name(patch);
  if (name.empty()) {
    if (doc_->parsed().file.patches.empty()) {
      // The file's text never parsed: the text pane shows it with its errors; the canvas fills in once it parses.
      backStack_.clear();
      shown_.clear();
      canvas_->setEditable(false);
      canvas_->setStale(!doc_->textLowered());
      refreshMarks();
      updateTitle();
      return true;
    }
    name = doc_->parsed().file.patches.front().name();
  }
  if (doc_->patchIndex(name) == graph::kNone && !doc_->findCallee(name)) return false;
  backStack_.clear();
  callStack_.clear();
  present(name, true);
  return true;
}

bool CircuitEditor::openNode(uint32_t node) {
  const graph::Patch* p = shownGraph();
  if (!p || node >= p->nodeCount() || p->node(node).kind != graph::NodeKind::Subcircuit) return false;
  const std::string callee = p->subcircuit(node).patch;
  if (doc_->patchIndex(callee) == graph::kNone && !doc_->findCallee(callee)) return false;
  backStack_.push_back(shown_);
  callStack_.push_back(p->node(node).id);
  present(callee, true);
  return true;
}

bool CircuitEditor::back() {
  if (backStack_.empty()) return false;
  const std::string p = backStack_.back();
  backStack_.pop_back();
  if (!callStack_.empty()) callStack_.pop_back();
  present(p, true);
  return true;
}

void CircuitEditor::rebuildScene(bool fit) {
  // Keep the selection across the rebuild by NodeId (dense indices follow the text order).
  std::vector<graph::NodeId> ids = std::move(keepSelection_);
  keepSelection_.clear();
  const graph::Patch* p = shownGraph();
  if (!p) {
    if (!doc_->parsed().file.patches.empty()) {
      backStack_.clear();
      shown_ = doc_->parsed().file.patches.front().name();
      p = shownGraph();
      canvas_->setEditable(true);
      updateTitle();
      fit = true;
    }
    if (!p) return;
  }
  canvas_->setScene(buildScene(*p), fit, true);
  // T4.5: wire indices change with the scene: the hover probe follows its source (by NodeId), the LEDs are re-read.
  lit_.clear();
  overlay_.nodeLabels.clear();
  overlay_.wireLabels.clear();
  hoverWire_ = UINT32_MAX;
  overlay_.readout = false;
  if (probeWire_ != UINT32_MAX) probeWire_ = UINT32_MAX - 1;  // re-found by refreshProbes from hoverProbe_'s source
  refreshProbes(false);  // an edit's own check (50 ms later) compiles them in
  lastIds_.clear();
  for (NodeIndex v = 0; v < p->nodeCount(); ++v) lastIds_.push_back(p->node(v).id);
  std::vector<uint32_t> sel;
  for (const graph::NodeId id : ids)
    if (auto v = p->indexOf(id)) sel.push_back(*v);
  if (!sel.empty()) canvas_->select(sel, {});
}

void CircuitEditor::refreshMarks() {
  if (readOnly()) {
    text_->setMarks({});
    return;
  }
  std::vector<ui::CodeMark> marks;
  for (const EditorDiagnostic& d : doc_->diagnostics()) {
    ui::CodeMark m;
    m.begin = d.offset;
    m.end = d.offset + d.length;
    m.warning = d.phase == EditorDiagnostic::Phase::Warning;
    m.message = d.message;
    marks.push_back(std::move(m));
  }
  text_->setMarks(std::move(marks));
}

void CircuitEditor::documentChanged(const EditorChange& c) {
  if (c.text && c.source != EditSource::TextPane && !readOnly()) {
    const bool jump = c.source == EditSource::Undo || c.source == EditSource::Redo;
    text_->replace(c.offset, c.removed, std::string_view(doc_->text()).substr(c.offset, c.inserted),
                   jump ? c.caret : UINT32_MAX);
    if (jump) text_->reveal(c.caret);
  }
  if (c.graph) {
    // The selection survives by NodeId: remember it before the scene goes.
    const auto& sc = canvas_->scene();
    keepSelection_.clear();
    if (sc && lastIds_.size() == sc->nodes.size())
      for (const uint32_t v : canvas_->selection().nodes)
        if (v < lastIds_.size()) keepSelection_.push_back(lastIds_[v]);
    rebuildScene(false);
  }
  canvas_->setStale(!doc_->textLowered());
  refreshMarks();
  if (c.text) {
    closeArmed_ = false;
    scheduleCheck();
  }
  if (!doc_->note().empty()) setStatus(doc_->note());
  else if (c.text) setStatus({});
  updateTitle();
}

void CircuitEditor::scheduleCheck() {
  if (checkTimer_) cancelTimer(checkTimer_);
  checkTimer_ = requestTimer(0.05);
}

void CircuitEditor::onTimer(uint32_t id) {
  if (id == dwellTimer_) {  // T4.5: the pointer rested on a wire: probe it
    dwellTimer_ = 0;
    if (hoverWire_ != UINT32_MAX) probeWire(hoverWire_);
    return;
  }
  if (id == graceTimer_) {  // left the probed wire a while ago: drop its probe
    graceTimer_ = 0;
    if (hoverWire_ == UINT32_MAX) probeWire(UINT32_MAX);
    return;
  }
  if (id == pollTimer_) {
    pollTimer_ = 0;
    pollProbes();
    ensurePoll();
    return;
  }
  if (id == costTimer_) {
    costTimer_ = 0;
    pollCost();
    if (costShown()) costTimer_ = requestTimer(0.25);
    return;
  }
  if (id != checkTimer_) {
    Popover::onTimer(id);
    return;
  }
  checkTimer_ = 0;
  checkNow();
}

void CircuitEditor::checkNow() {
  if (!doc_->checkPending()) return;
  size_t idx = doc_->playing() ? graph::kNone : shownIndex();
  if (!doc_->playing() && idx == graph::kNone) idx = 0;
  const Status s = doc_->check(idx);
  if (!s && doc_->playing()) setStatus("Not swapped (the last good version keeps playing): " + s.error().message);
  else if (s && doc_->playing()) setStatus("Playing " + doc_->playingPatch() + ": swapped in the edit");
}

bool CircuitEditor::addAt(graph::Point2 world, std::string_view spec) {
  const size_t idx = shownIndex();
  if (idx == graph::kNone) {
    setStatus("An imported patch is read-only.");
    return false;
  }
  auto id = doc_->addNode(idx, spec, world);
  if (!id) {
    setStatus(id.error().message);
    return false;
  }
  const graph::Patch& p = doc_->drawn(idx);
  if (auto v = p.indexOf(*id)) {
    const uint32_t one[1] = {*v};
    canvas_->select(one, {});
  }
  return true;
}

bool CircuitEditor::deleteSelection() {
  const size_t idx = shownIndex();
  if (idx == graph::kNone) return false;
  const auto sel = canvas_->selection();
  if (sel.nodes.empty() && sel.wires.empty()) return false;
  if (Status s = doc_->erase(idx, sel.nodes, sel.wires); !s) {
    setStatus(s.error().message);
    return false;
  }
  canvas_->clearSelection();
  return true;
}

bool CircuitEditor::renameNode(uint32_t node, std::string name) {
  const size_t idx = shownIndex();
  if (idx == graph::kNone) return false;
  if (Status s = doc_->rename(idx, node, std::move(name)); !s) {
    setStatus(s.error().message);
    return false;
  }
  return true;
}

bool CircuitEditor::makeSubcircuit(std::string name) {
  const size_t idx = shownIndex();
  if (idx == graph::kNone) return false;
  const auto sel = canvas_->selection();
  auto r = doc_->makeSubcircuit(idx, sel.nodes, std::move(name));
  if (!r) {
    setStatus(r.error().message);
    return false;
  }
  setStatus("Made subcircuit " + doc_->parsed().file.patches[*r].name() + " (double-click it to open it).");
  return true;
}

bool CircuitEditor::makeParam() {
  const size_t idx = shownIndex();
  if (idx == graph::kNone) return false;
  const auto sel = canvas_->selection();
  if (sel.nodes.size() != 1) {
    setStatus("Select one constant to turn it into a param pin.");
    return false;
  }
  auto k = doc_->makeParam(idx, sel.nodes.front());
  if (!k) {
    setStatus(k.error().message);
    return false;
  }
  const graph::Patch& p = doc_->drawn(idx);
  if (auto v = p.indexOf(*k)) {
    const uint32_t one[1] = {*v};
    canvas_->select(one, {});
    setStatus("Param pin '" + p.nodeName(*v) + "': a control on the device panel (rename it with Ctrl+R).");
  }
  return true;
}

bool CircuitEditor::undo() { return doc_->undo(); }
bool CircuitEditor::redo() { return doc_->redo(); }

bool CircuitEditor::save() {
  if (Status s = doc_->save(); !s) {
    setStatus("Not saved: " + s.error().message);
    return false;
  }
  setStatus("Saved " + doc_->path());
  updateTitle();
  return true;
}

bool CircuitEditor::togglePlay() {
  if (doc_->playing()) {
    doc_->stop();
    refreshProbes(false);  // nothing plays: no probes
    clearProbeDisplay();
    setStatus("Stopped.");
    updateTitle();
    return true;
  }
  const size_t idx = shownIndex();
  if (idx == graph::kNone) {
    setStatus("Open a patch of this file to play it.");
    updateTitle();
    return false;
  }
  if (Status s = doc_->play(idx); !s) {
    setStatus("Cannot play: " + s.error().message);
    updateTitle();
    return false;
  }
  setStatus("Playing " + doc_->playingPatch() + ": edits swap in as you make them.");
  refreshProbes(true);  // T4.5: the LED trace of the bool wires (a second, bit-identical program)
  updateTitle();
  return true;
}

bool CircuitEditor::tidy() {
  const size_t idx = shownIndex();
  if (idx == graph::kNone) {
    (void)circuit::tidy(importedShown_);
    canvas_->setScene(buildScene(importedShown_), false, true);
    lastIds_.clear();
    for (NodeIndex v = 0; v < importedShown_.nodeCount(); ++v) lastIds_.push_back(importedShown_.node(v).id);
    return true;
  }
  if (Status s = doc_->tidy(idx); !s) {
    setStatus(s.error().message);
    return false;
  }
  return true;
}

void CircuitEditor::beginAdd(graph::Point2 world, ui::Vec2 device) {
  if (readOnly()) {
    setStatus("An imported patch is read-only.");
    return;
  }
  ui::OverlayLayer* layer = ui::OverlayLayer::of(*this);
  if (!layer) return;
  auto pop = std::make_unique<TypeIn>("Add", "");
  pop->field->setPlaceholder("tanh, 0.5, 440Hz, *, svf, param x = 1, out y");
  TypeIn* raw = pop.get();
  std::weak_ptr<bool> alive = alive_;
  raw->field->onCommit = [this, alive, raw, layer, world](const std::string& spec) {
    if (alive.expired()) return;
    const std::string s = spec;
    layer->close(*raw);
    if (!s.empty()) (void)addAt(world, s);
  };
  raw->field->onCancel = [layer, raw] { layer->close(*raw); };
  ui::OverlayOptions o;
  o.placement = ui::Placement::AtPoint;
  o.anchor = {static_cast<int32_t>(device.x), static_cast<int32_t>(device.y), 1, 1};
  ui::Widget& opened = layer->open(std::move(pop), std::move(o));
  if (ui::UiTree* t = layer->tree()) t->setFocus(static_cast<TypeIn&>(opened).field);
}

void CircuitEditor::beginRename(uint32_t node) {
  const graph::Patch* p = shownGraph();
  if (!p || node >= p->nodeCount() || readOnly()) return;
  ui::OverlayLayer* layer = ui::OverlayLayer::of(*this);
  if (!layer || !canvas_->scene()) return;
  const SceneNode& n = canvas_->scene()->nodes[node];
  const ui::Vec2 d = canvas_->toDevice({n.x, n.y});
  std::string current = p->nodeName(node);
  if (graph::edit::isGeneratedName(current)) current.clear();
  auto pop = std::make_unique<TypeIn>("Name", current);
  TypeIn* raw = pop.get();
  std::weak_ptr<bool> alive = alive_;
  const graph::NodeId id = p->node(node).id;
  raw->field->onCommit = [this, alive, raw, layer, id](const std::string& name) {
    if (alive.expired()) return;
    const std::string s = name;
    layer->close(*raw);
    if (const graph::Patch* q = shownGraph())
      if (auto v = q->indexOf(id)) (void)renameNode(*v, s);
  };
  raw->field->onCancel = [layer, raw] { layer->close(*raw); };
  ui::OverlayOptions o;
  o.placement = ui::Placement::AtPoint;
  o.anchor = {static_cast<int32_t>(d.x), static_cast<int32_t>(d.y) - 30, 1, 1};
  ui::Widget& opened = layer->open(std::move(pop), std::move(o));
  if (ui::UiTree* t = layer->tree()) {
    t->setFocus(static_cast<TypeIn&>(opened).field);
    static_cast<TypeIn&>(opened).field->buffer().selectAll();
  }
}

bool CircuitEditor::onKey(const ui::KeyEvent& e) {
  if (!e.down) return Popover::onKey(e);
  const ui::Mods m = e.mods & ui::mod::kChordMask;
  const bool ctrl = m == ui::mod::kCtrl, ctrlShift = m == (ui::mod::kCtrl | ui::mod::kShift);
  if ((ctrl && e.key == 'z')) return undo() || true;
  if ((ctrlShift && e.key == 'z') || (ctrl && e.key == 'y')) return redo() || true;
  if (ctrl && e.key == 's') return save() || true;
  if (ctrl && e.key == ui::key::kReturn) return togglePlay() || true;
  const bool onCanvas = canvas_->focused();
  const auto& sel = canvas_->selection();
  if (onCanvas && m == ui::mod::kNone) {
    if (e.key == ui::key::kDelete || e.key == ui::key::kBackspace) {
      if (!sel.nodes.empty() || !sel.wires.empty()) return deleteSelection() || true;
      if (e.key == ui::key::kBackspace) return back() || true;
    }
    if (e.key == 'n') {
      const ui::IRect r = canvas_->rect();
      const ui::Vec2 d{static_cast<float>(r.x + r.w / 2), static_cast<float>(r.y + r.h / 2)};
      beginAdd(canvas_->toWorld(d), d);
      return true;
    }
    if (e.key == 'p') return makeParam() || true;
    if (e.key == ui::key::kReturn && sel.nodes.size() == 1) return openNode(sel.nodes.front()) || true;
  }
  if (onCanvas && ctrl && e.key == 'g') return makeSubcircuit() || true;
  if (onCanvas && ctrl && e.key == 'r' && sel.nodes.size() == 1) {
    beginRename(sel.nodes.front());
    return true;
  }
  if (e.key == ui::key::kEscape && m == ui::mod::kNone) {
    if (text_->focused()) {
      if (tree()) tree()->setFocus(canvas_);
      return true;
    }
    if (doc_->dirty() && !closeArmed_) {
      closeArmed_ = true;
      setStatus("Unsaved changes: Ctrl+S saves; Esc again closes without saving.");
      return true;
    }
    if (doc_->playing()) doc_->stop();
  }
  return Popover::onKey(e);
}

ui::InfoText CircuitEditor::info() const {
  return {"Circuit Editor",
          "Drag pins to wire, drag nodes to move, double-click to add or rename. The text on the right is the same patch: "
          "edit either. Ctrl+Z undoes both, Ctrl+S saves, Ctrl+Enter plays."};
}

// ---- T4.5: live probes and cost meters -------------------------------------------------------------------------------

namespace {
constexpr double kDwellSeconds = 0.15;  // the pointer rests on a wire this long before it is probed
constexpr double kGraceSeconds = 1.5;   // a probe stays compiled this long after the pointer left its wire
constexpr double kPollSeconds = 1.0 / 30.0;
constexpr uint32_t kMaxLedProbes = 64;
constexpr uint32_t kScopeFrames = 1024;  // the readout's mini-scope: ≈ 21 ms at 48 kHz
}  // namespace

bool CircuitEditor::shownPath(std::vector<graph::NodeId>& path) const {
  path.clear();
  if (!doc_->playing()) return false;
  const std::string& playing = doc_->playingPatch();
  if (backStack_.empty()) return shown_ == playing;
  if (backStack_.front() != playing || callStack_.size() != backStack_.size()) return false;
  path = callStack_;
  return true;
}

bool CircuitEditor::wireSource(uint32_t wire, graph::NodeId& node, uint16_t& port) const {
  const auto& sc = canvas_->scene();
  const graph::Patch* p = const_cast<CircuitEditor*>(this)->shownGraph();
  if (!sc || !p || wire >= sc->wires.size()) return false;
  const ScenePin& from = sc->pins[sc->wires[wire].fromPin];
  if (from.node >= p->nodeCount()) return false;
  node = p->node(from.node).id;
  port = from.port;
  return true;
}

uint32_t CircuitEditor::probeIdFor(graph::NodeId node, uint16_t port) {
  const uint64_t key = (static_cast<uint64_t>(node.value) << 16) | port;
  for (const auto& [k, id] : probeIdOf_)
    if (k == key) return id;
  probeIdOf_.emplace_back(key, nextProbeId_);
  return nextProbeId_++;
}

void CircuitEditor::hoverChanged(const CanvasHit& hit) {
  const uint32_t wire = hit.kind == CanvasHit::Kind::Wire ? hit.index : UINT32_MAX;
  if (wire == hoverWire_) return;
  hoverWire_ = wire;
  if (dwellTimer_) cancelTimer(dwellTimer_);
  dwellTimer_ = 0;
  if (wire == UINT32_MAX) {
    if (overlay_.readout) {
      overlay_.readout = false;
      canvas_->markDirty();
    }
    if (hoverProbe_ != UINT32_MAX) {
      if (graceTimer_) cancelTimer(graceTimer_);
      graceTimer_ = requestTimer(kGraceSeconds);
    }
    return;
  }
  if (graceTimer_) cancelTimer(graceTimer_);
  graceTimer_ = 0;
  std::vector<graph::NodeId> path;
  if (!shownPath(path)) return;  // nothing to probe: the shown patch is not playing
  // Back on the probed source (another wire of its net too): the readout at once; else after the dwell.
  graph::NodeId n, m;
  uint16_t a = 0, b = 0;
  if (probeWire_ != UINT32_MAX && wireSource(wire, n, a) && wireSource(probeWire_, m, b) && n == m && a == b) {
    probeWire_ = wire;
    overlay_.readout = true;
    overlay_.wire = wire;
    pollProbes();
    ensurePoll();
    return;
  }
  dwellTimer_ = requestTimer(kDwellSeconds);
}

void CircuitEditor::probeWire(uint32_t wire) {
  graph::NodeId node;
  uint16_t port = 0;
  if (wire == UINT32_MAX || !wireSource(wire, node, port)) {
    probeWire_ = hoverProbe_ = UINT32_MAX;
    overlay_.readout = false;
    canvas_->markDirty();
    refreshProbes(true);
    return;
  }
  probeWire_ = wire;
  hoverProbe_ = probeIdFor(node, port);
  const auto& sc = canvas_->scene();
  overlay_.readout = true;
  overlay_.wire = wire;
  overlay_.isBool = sc->wires[wire].style.boolean;
  overlay_.hasData = false;
  overlay_.samples.clear();
  canvas_->markDirty();
  refreshProbes(true);
  ensurePoll();
}

void CircuitEditor::refreshProbes(bool recheck) {
  std::vector<graph::ProbeTarget> targets;
  std::vector<uint32_t> ids, signals;
  ledWires_.clear();
  std::vector<graph::NodeId> path;
  const auto& sc = canvas_->scene();
  const graph::Patch* p = shownGraph();
  if (shownPath(path) && sc && p) {
    auto add = [&](uint32_t wire) -> uint32_t {
      graph::NodeId node;
      uint16_t port = 0;
      if (!wireSource(wire, node, port)) return UINT32_MAX;
      const uint32_t id = probeIdFor(node, port);
      if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        graph::ProbeTarget t;
        t.path = path;
        t.node = node;
        t.output = port;
        targets.push_back(std::move(t));
        ids.push_back(id);
      }
      return id;
    };
    // The LED trace: every bool net (one probe per source, level only).
    for (uint32_t w = 0; w < sc->wires.size(); ++w) {
      if (!sc->wires[w].style.boolean) continue;
      if (ids.size() >= kMaxLedProbes) break;
      if (const uint32_t id = add(w); id != UINT32_MAX) ledWires_.emplace_back(w, id);
    }
    // The hover probe (its full signal), found again by its source after a scene rebuild.
    if (hoverProbe_ != UINT32_MAX) {
      if (probeWire_ == UINT32_MAX - 1 || probeWire_ >= sc->wires.size()) {
        probeWire_ = UINT32_MAX;
        for (uint32_t w = 0; w < sc->wires.size() && probeWire_ == UINT32_MAX; ++w) {
          graph::NodeId node;
          uint16_t port = 0;
          if (wireSource(w, node, port) && probeIdFor(node, port) == hoverProbe_) probeWire_ = w;
        }
        if (probeWire_ == UINT32_MAX) hoverProbe_ = UINT32_MAX;  // its source is gone
      }
      if (probeWire_ != UINT32_MAX && add(probeWire_) == hoverProbe_) signals.push_back(hoverProbe_);
    }
  } else {
    hoverProbe_ = UINT32_MAX;
    probeWire_ = UINT32_MAX;
  }
  lit_.assign(sc ? sc->wires.size() : 0, 0);
  for (uint32_t w = 0; sc && w < sc->wires.size(); ++w) canvas_->setWireLit(w, false);
  if (ids == compiledIds_ && signals == compiledSignals_) {
    ensurePoll();
    return;
  }
  compiledTargets_ = targets;
  compiledIds_ = ids;
  compiledSignals_ = signals;
  doc_->setProbes(std::move(targets), std::move(ids));
  if (PatchPlayer* pl = doc_->player()) pl->setProbeSignals(compiledSignals_);
  if (recheck && doc_->playing()) {
    const Status st = doc_->check();
    if (!st) setStatus("Probes not compiled in: " + st.error().message);
  }
  ensurePoll();
}

void CircuitEditor::clearProbeDisplay() {
  overlay_.readout = false;
  overlay_.samples.clear();
  overlay_.nodeLabels.clear();
  overlay_.wireLabels.clear();
  if (const auto& sc = canvas_->scene())
    for (uint32_t w = 0; w < sc->wires.size(); ++w) canvas_->setWireLit(w, false);
  lit_.clear();
  canvas_->markDirty();
}

void CircuitEditor::ensurePoll() {
  const bool want = doc_->playing() && (!ledWires_.empty() || overlay_.readout);
  if (want && !pollTimer_) pollTimer_ = requestTimer(kPollSeconds);
}

void CircuitEditor::pollProbes() {
  PatchPlayer* pl = doc_->player();
  if (!pl || !doc_->playing()) return;
  // LED trace: lit in a block where the value was high.
  const auto& sc = canvas_->scene();
  if (sc && lit_.size() == sc->wires.size())
    for (const auto& [w, id] : ledWires_) {
      const ProbeLevel* lv = pl->probeLevel(id);
      const bool on = lv && lv->hi.load(std::memory_order_relaxed) > 0.5f;
      if ((lit_[w] != 0) != on) {
        lit_[w] = on ? 1 : 0;
        canvas_->setWireLit(w, on);
      }
    }
  // The readout: the value and the last kScopeFrames frames.
  if (!overlay_.readout || hoverProbe_ == UINT32_MAX) return;
  TapRing* ring = pl->probeSignal(hoverProbe_);
  if (!ring) return;
  const uint64_t written = ring->written();
  const uint64_t from = std::max<uint64_t>(ring->oldestReadable(), written > kScopeFrames ? written - kScopeFrames : 0);
  const uint32_t n = static_cast<uint32_t>(written - from);
  overlay_.samples.resize(n);
  const uint32_t got = n ? ring->read(0, from, n, overlay_.samples.data()) : 0;
  overlay_.samples.resize(got);
  overlay_.hasData = got > 0;
  if (got > 0) {
    const float last = overlay_.samples.back();
    overlay_.value = overlay_.isBool ? (std::signbit(last) ? "HIGH" : "LOW") : formatProbeValue(last);
  }
  canvas_->markDirty();
}

bool CircuitEditor::costShown() const noexcept { return cost_ && cost_->on(); }

void CircuitEditor::setCostShown(bool on) {
  if (cost_->on() != on) cost_->setOn(on);
  if (PatchPlayer* pl = doc_->player()) pl->setMetering(on);
  costSmooth_.clear();
  costTotal_ = 0.0;
  if (!on) {
    if (costTimer_) cancelTimer(costTimer_);
    costTimer_ = 0;
    overlay_.nodeLabels.clear();
    overlay_.wireLabels.clear();
    canvas_->markDirty();
    setStatus({});
    return;
  }
  setStatus(doc_->playing() ? "Cost: measuringâ¦" : "Cost: play the patch to measure it.");
  if (!costTimer_) costTimer_ = requestTimer(0.25);
}

void CircuitEditor::pollCost() {
  PatchPlayer* pl = doc_->player();
  const PatchEditor::PlayedProgram* played = doc_->played();
  if (!costShown() || !pl || !played || !doc_->playing()) return;
  PatchPlayer::CostReading rd;
  if (!pl->readCost(rd)) return;
  std::vector<graph::NodeId> path;
  CostMap cm;
  if (!shownPath(path) || !attributeCost(*played, rd, path, cm)) {
    if (rd.generation == played->generation) {
      costTotal_ = costTotal_ > 0.0 ? 0.7 * costTotal_ + 0.3 * rd.totalNsPerFrame : rd.totalNsPerFrame;
      setStatus("Cost " + formatCostNs(costTotal_) + " per sample (the shown patch is not what plays)");
    }
    return;
  }
  // Smoothed (one-pole over the 4 Hz readings) per node and wire.
  auto smooth = [&](uint64_t key, double ns) {
    for (auto& [k, v] : costSmooth_)
      if (k == key) {
        v = 0.7 * v + 0.3 * ns;
        return v;
      }
    costSmooth_.emplace_back(key, ns);
    return ns;
  };
  costTotal_ = costTotal_ > 0.0 ? 0.7 * costTotal_ + 0.3 * cm.totalNs : cm.totalNs;
  const graph::Patch* p = shownGraph();
  const auto& sc = canvas_->scene();
  if (!p || !sc) return;
  overlay_.nodeLabels.clear();
  overlay_.wireLabels.clear();
  const double top = std::max(cm.shownNs, 1e-9);
  for (const CostMap::Entry& e : cm.nodes) {
    const auto v = p->indexOf(e.node);
    if (!v || *v >= sc->nodes.size()) continue;
    const double ns = smooth(e.node.value, e.ns);
    if (ns < 0.005) continue;
    overlay_.nodeLabels.push_back({*v, formatCostNs(ns), static_cast<float>(e.ns / top)});
  }
  auto wireInto = [&](graph::NodeIndex v) {  // a per-voice wire into v (the one a voice → global sum belongs to)
    uint32_t any = UINT32_MAX;
    for (uint32_t w = 0; w < sc->wires.size(); ++w)
      if (sc->pins[sc->wires[w].toPin].node == v) {
        if (sc->wires[w].style.poly) return w;
        if (any == UINT32_MAX) any = w;
      }
    return any;
  };
  auto wireOutOf = [&](graph::NodeIndex v) {
    for (uint32_t w = 0; w < sc->wires.size(); ++w)
      if (sc->pins[sc->wires[w].fromPin].node == v) return w;
    return UINT32_MAX;
  };
  for (const CostMap::Entry& e : cm.sumsInto)
    if (const auto v = p->indexOf(e.node))
      if (const uint32_t w = wireInto(*v); w != UINT32_MAX) {
        const double ns = smooth(static_cast<uint64_t>(e.node.value) | (uint64_t{1} << 63), e.ns);
        if (ns >= 0.005) overlay_.wireLabels.push_back({w, formatCostNs(ns), static_cast<float>(e.ns / top)});
      }
  for (const CostMap::Entry& e : cm.copiesFrom)
    if (const auto v = p->indexOf(e.node))
      if (const uint32_t w = wireOutOf(*v); w != UINT32_MAX) {
        const double ns = smooth(static_cast<uint64_t>(e.node.value) | (uint64_t{1} << 62), e.ns);
        if (ns >= 0.005) overlay_.wireLabels.push_back({w, formatCostNs(ns), static_cast<float>(e.ns / top)});
      }
  canvas_->markDirty();
  const double sr = pl->sampleRate();
  char pct[48];
  std::snprintf(pct, sizeof pct, "%.2f %% of a core at %.0f kHz", costTotal_ * sr * 1e-7, sr / 1000.0);
  setStatus("Cost " + formatCostNs(costTotal_) + " per sample (" + pct + ")");
}

CircuitEditor* openCircuitEditor(ui::Widget& from, std::unique_ptr<PatchEditor> document, std::string_view patch,
                                 PatchPlayer* player) {
  ui::OverlayLayer* layer = ui::OverlayLayer::of(from);
  if (!layer || !document) return nullptr;
  ui::UiTree* tree = layer->tree();
  document->setPlayer(player);
  auto editor = std::make_unique<CircuitEditor>(std::move(document));
  if (!editor->show(patch)) return nullptr;
  const float sc = tree ? tree->scale() : 1.0f;
  const float w = tree ? static_cast<float>(tree->width()) / sc : 1200.0f, h = tree ? static_cast<float>(tree->height()) / sc : 800.0f;
  editor->setSize(ui::Length::fixed(w), ui::Length::fixed(h));
  ui::OverlayOptions o;
  o.placement = ui::Placement::AtPoint;
  o.dismissOnOutsideClick = false;
  auto& opened = static_cast<CircuitEditor&>(layer->open(std::move(editor), std::move(o)));
  if (tree) tree->setFocus(&opened.canvas());
  return &opened;
}

}  // namespace pq::views::circuit
