// The Device tab and the stand-alone device preview (pq/views/devices/device_view.h, T4.6).

#include "pq/views/devices/device_view.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>

#include "../common/paint.h"
#include "../shell/shell_widgets.h"
#include "pq/graph/parse.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views::devices {

namespace fs = std::filesystem;

namespace {

fs::path utf8Path(std::string_view s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string pathText(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// Shortest text that reads back as the same double.
std::string num(double v) {
  if (!std::isfinite(v)) v = 0.0;
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

// A param declaration with `value` (internal units) as its default, in the declaration's own unit.
graph::Constant literalFor(const graph::ParamDecl& d, double value) {
  if (d.kind == graph::Kind::Bool) return graph::Constant::boolean(value >= 0.5);
  const graph::Unit u = d.defaultValue.unit;
  double lit = value;
  switch (u) {
    case graph::Unit::dB:
      lit = value > 0.0 ? 20.0 * std::log10(value) : (d.hasRange ? d.rangeMin.literal : -120.0);
      break;
    case graph::Unit::percent: lit = value * 100.0; break;
    case graph::Unit::kHz: lit = value / 1000.0; break;
    case graph::Unit::ms: lit = value * 1000.0; break;
    case graph::Unit::st: lit = value / 100.0; break;
    default: break;
  }
  return graph::Constant::real(lit, u);
}

// The arpeggio the preview feeds into an effect: a soft saw-like tone (six partials) under a quick envelope.
constexpr const char* kPreviewSource =
    "// The preview's signal: one voice per arpeggio note (the PatchPreview plays C4 E4 G4 C5).\n"
    "patch preview_source {\n"
    "  out out  : audio voice mono\n"
    "  out done : bool voice mono\n"
    "\n"
    "  g    = voice.gate\n"
    "  env  = z1(env + ((g ? 1 : 0) - env) * (g ? 0.01 : 0.0005))\n"
    "  ph   = phasor(voice.hz)\n"
    "  tone = sine(ph) + 0.5 * sine(frac(ph * 2)) + 0.333 * sine(frac(ph * 3)) + 0.25 * sine(frac(ph * 4)) + 0.2 * sine(frac(ph * 5)) + 0.167 * sine(frac(ph * 6))\n"
    "  out  = 0.3 * tone * env * (0.4 + 0.6 * voice.velocity)\n"
    "  done = !g && env < 0.0001\n"
    "}\n";

constexpr const char* kHostPatch = "device_preview";

}  // namespace

// ---- listing ---------------------------------------------------------------------------------------------------------

std::vector<DeviceEntry> listDevices(const std::string& dir) {
  std::vector<DeviceEntry> out;
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(utf8Path(dir), ec)) return out;
  for (const auto& e : fs::directory_iterator(utf8Path(dir), ec)) {
    if (!e.is_regular_file() || e.path().extension() != ".pqp") continue;
    DeviceEntry d;
    d.path = pathText(e.path());
    d.title = pathText(e.path().stem());
    // The panel's title when there is a description (read without the device: only the "title" key matters here).
    if (auto doc = circuit::loadPatchDocument(d.path); doc && !(*doc)->parsed.file.patches.empty()) {
      const graph::Patch* p = (*doc)->parsed.file.find(d.title);
      if (!p) p = &(*doc)->parsed.file.patches.front();
      if (auto desc = loadPanelDesc(panelPathFor(d.path), signatureOf(*p))) d.title = desc->title;
    }
    out.push_back(std::move(d));
  }
  std::sort(out.begin(), out.end(), [](const DeviceEntry& a, const DeviceEntry& b) { return a.title < b.title; });
  return out;
}

// ---- DevicePreview ---------------------------------------------------------------------------------------------------

Result<std::unique_ptr<DevicePreview>> DevicePreview::open(const std::string& devicePath, circuit::PatchPlayer* player) {
  auto doc = circuit::loadPatchDocument(devicePath);
  if (!doc) return std::move(doc).error();
  std::unique_ptr<DevicePreview> p(new DevicePreview());
  p->path_ = devicePath;
  p->doc_ = *doc;
  const auto& patches = p->doc_->parsed.file.patches;
  if (patches.empty()) return Error{ErrorCode::NotFound, devicePath + ": no patch", {}};
  const std::string stem = pathText(utf8Path(devicePath).stem());
  p->devicePatch_ = p->doc_->parsed.file.find(stem);
  if (!p->devicePatch_) p->devicePatch_ = &patches.front();
  p->sig_ = signatureOf(*p->devicePatch_);
  const std::string panelPath = panelPathFor(devicePath);
  std::error_code ec;
  if (fs::exists(utf8Path(panelPath), ec)) {
    auto desc = loadPanelDesc(panelPath, p->sig_);
    if (!desc) return std::move(desc).error();
    p->desc_ = std::move(*desc);
  } else {
    std::string title = stem;
    if (!title.empty()) title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
    p->desc_ = autoPanel(p->sig_, title);
  }
  for (const ParamInfo& prm : p->sig_.params) p->values_[prm.name] = prm.def;
  for (const ConstInfo& c : p->sig_.consts) p->values_[c.name] = c.def;
  p->player_ = player;
  // The host patch lives next to the device (its import resolves there); it is never written to disk.
  const std::string hostPath = pathText(utf8Path(devicePath).parent_path() / "_device_preview.pqp");
  auto ed = circuit::PatchEditor::fromText(hostPath, p->hostText());
  if (!ed) return std::move(ed).error();
  p->editor_ = std::move(*ed);
  if (!p->editor_->textLowered())
    return Error{ErrorCode::InvalidArgument, "device preview: the host patch does not parse: " +
                                                 (p->editor_->diagnostics().empty() ? std::string() : p->editor_->diagnostics()[0].message), {}};
  p->editor_->setPlayer(player);
  if (player)
    if (TapRing* tap = player->outputTap()) {
      dsp::SpectrumConfig sc;
      sc.sampleRate = player->sampleRate();
      p->feed_ = std::make_unique<SpectrumFeed>(*tap, sc);
    }
  return p;
}

DevicePreview::~DevicePreview() {
  if (playing()) stop();
  feed_.reset();  // joins the worker before its tap goes
  ownTap_.reset();
}

void DevicePreview::setSpectrumSource(std::unique_ptr<TapRing> tap, std::unique_ptr<SpectrumFeed> feed) {
  feed_ = std::move(feed);
  ownTap_ = std::move(tap);
}

std::string DevicePreview::hostText() const {
  const graph::Patch& dev = *devicePatch_;
  // The device's param declarations with the current values as their defaults, printed by the graph printer (exact).
  graph::Patch decls(kHostPatch);
  for (graph::NodeIndex n = 0; n < dev.nodeCount(); ++n) {
    if (dev.node(n).kind != graph::NodeKind::Param) continue;
    graph::ParamDecl d = dev.paramDecl(n);
    const auto it = values_.find(dev.nodeName(n));
    if (it != values_.end()) d.defaultValue = literalFor(d, it->second);
    decls.addParam(dev.nodeName(n), std::move(d));
  }
  std::string printed = graph::print(decls);
  // Keep the body lines (between the braces).
  const size_t open = printed.find('{'), close = printed.rfind('}');
  std::string params = open != std::string::npos && close != std::string::npos && close > open ? printed.substr(open + 2, close - open - 2) : std::string();

  std::string t;
  t += "// Generated by the Device view (T4.6): " + sig_.patch + " with the panel's settings, fed by an arpeggio.\n";
  t += "import \"" + pathText(utf8Path(path_).filename()) + "\"\n\n";
  t += "patch " + std::string(kHostPatch) + " {\n";
  t += params;
  t += "  out out : audio stereo\n";
  t += "  out done : bool voice mono\n\n";
  t += "  pq_v = preview_source()\n";
  t += "  pq_mono = voice_sum(pq_v)\n";
  t += "  done = pq_v.done\n";
  const std::string src = sig_.audioInWidth == 1 ? "pq_mono" : "(pq_mono, pq_mono)";
  if (!active_) {
    t += "  out = " + std::string(sig_.audioInWidth == 1 ? "(pq_mono, pq_mono)" : src) + "\n";
  } else {
    std::string call = sig_.patch + "(";
    bool first = true;
    auto arg = [&](const std::string& s) {
      if (!first) call += ", ";
      call += s;
      first = false;
    };
    if (!sig_.audioIn.empty()) arg(sig_.audioIn + ": " + src);
    for (const ConstInfo& c : sig_.consts) {
      const auto it = values_.find(c.name);
      arg(c.name + ": " + num(it != values_.end() ? it->second : c.def));
    }
    for (const ParamInfo& p : sig_.params) arg(p.name + ": " + p.name);
    call += ")";
    t += "  out = " + call + "\n";
  }
  t += "}\n\n";
  t += kPreviewSource;
  return t;
}

Status DevicePreview::updateHost() {
  const std::string text = hostText();
  if (text == editor_->text()) return Ok{};
  editor_->replaceText(0, static_cast<uint32_t>(editor_->text().size()), text, false);
  ++stats_.recompiles;
  if (!editor_->textLowered())
    return Error{ErrorCode::InvalidArgument, editor_->diagnostics().empty() ? "the host patch does not parse" : editor_->diagnostics()[0].message, {}};
  if (playing()) return editor_->check(editor_->patchIndex(kHostPatch));
  return Ok{};
}

void DevicePreview::setParam(std::string_view param, double value) {
  values_[std::string(param)] = value;
  ++stats_.paramWrites;
  if (playing() && player_) player_->setParam(param, static_cast<float>(value));
}

void DevicePreview::setConst(std::string_view name, double value) {
  values_[std::string(name)] = value;
  (void)updateHost();  // errors show in the editor's diagnostics; the last good program keeps playing
}

void DevicePreview::setActive(bool on) {
  if (active_ == on) return;
  active_ = on;
  (void)updateHost();
}

Status DevicePreview::play() {
  if (!player_) return Error{ErrorCode::Unsupported, "no audio output for the device preview", {}};
  // The host text carries the current values as defaults: bring it up to date before the first image is built.
  const std::string text = hostText();
  if (text != editor_->text()) editor_->replaceText(0, static_cast<uint32_t>(editor_->text().size()), text, false);
  Status s = editor_->play(editor_->patchIndex(kHostPatch));
  if (!s) return s;
  playing_ = true;
  if (feed_) feed_->setSourceLive(true);
  return Ok{};
}

void DevicePreview::stop() {
  if (!playing_) return;
  playing_ = false;
  editor_->stop();
  if (feed_) feed_->setSourceLive(false);
}

bool DevicePreview::playing() const noexcept {
  // Another editor may have taken the player over (one preview output): then this one no longer plays.
  return playing_ && player_ && player_->playing() && editor_->playing();
}

// ---- the Device tab ------------------------------------------------------------------------------------------------

namespace {

// Play / stop for the preview: a triangle or a square, the accent when playing.
class PlayButton final : public ui::Control {
public:
  PlayButton() : Control("play") {
    setFocusable(true);
    setInfo("Preview", "Plays the device with an arpeggio through the audio output (until devices live in tracks, T4.1).");
  }
  std::function<void()> onClick;
  void setPlaying(bool p) {
    if (p == playing_) return;
    playing_ = p;
    markDirty();
  }
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::VisualState v = visualState();
    const ui::IRect r = rect();
    const ui::Rgba fill = playing_ ? th->accent.primary : v.hover ? th->control.fillHover : th->control.fill;
    paint::box(ctx.dl, r, 0.0f, fill, th->control.border);
    const float cx = static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), cy = static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h);
    const float s = ctx.scale.exact(4.0f);
    const ui::Rgba ink = playing_ ? th->text.onAccent : th->text.primary;
    if (playing_) ctx.dl.addRectFilled({cx - s, cy - s, cx + s, cy + s}, ink);
    else ctx.dl.addTriangleFilled({cx - 0.8f * s, cy - s}, {cx + s, cy}, {cx - 0.8f * s, cy + s}, ink);
  }
  bool onPointer(const ui::PointerEvent& e) override {
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left) {
      if (onClick) onClick();
      return true;
    }
    return false;
  }

private:
  bool playing_ = false;
};

}  // namespace

class DeviceView final : public ui::Widget {
public:
  explicit DeviceView(Shell& shell) : Widget("devices"), shell_(shell), ctx_(shell.context()) {
    setLayout(ui::LayoutKind::Row);
    setVisible(false);
    auto& strip = add<ui::Widget>("strip");
    strip.setLayout(ui::LayoutKind::Column);
    strip.setWidth(ui::Length::fixed(26.0f));
    strip.setPadding({4, 4, 4, 4});
    strip.setGap(4);
    play_ = &strip.add<PlayButton>();
    play_->setSize(ui::Length::fixed(18.0f), ui::Length::fixed(18.0f));
    play_->onClick = [this] { togglePlay(); };
    auto& close = strip.add<ui::Button>("close", "x");
    close.setSize(ui::Length::fixed(18.0f), ui::Length::fixed(18.0f));
    close.setTooltip("Close the device");
    close.onClick = [this] { closeDevice(); };
    scroll_ = &add<ui::ScrollView>("scroll", true, false);
    scroll_->setWidth(ui::Length::flex(1.0f));
    row_ = &scroll_->add<ui::Widget>("chain");
    row_->setLayout(ui::LayoutKind::Row);
    row_->setPadding({4, 4, 4, 4});
    ctx_.setViewAction("view.openDevice", [this] { openMenu(); });
  }
  ~DeviceView() override {
    ctx_.clearViewAction("view.openDevice");
    clearDevice();
  }

  void paint(ui::PaintContext& ctx) override {
    if (const ui::Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.panel);
  }
  void onFrame(double) override {
    // Follows the player (another editor may have taken it over).
    const bool p = preview_ && preview_->playing();
    play_->setPlaying(p);
    if (p) requestTimer(0.25);
  }
  void onTimer(uint32_t) override { onFrame(0.0); }

  void setLibrary(std::string dir, circuit::PatchPlayer* player) {
    dir_ = std::move(dir);
    player_ = player;
  }

  DevicePanel* open(const std::string& path, std::string* error) {
    auto pv = DevicePreview::open(path, player_);
    if (!pv) {
      if (error) *error = pv.error().message;
      return nullptr;
    }
    clearDevice();
    preview_ = std::move(*pv);
    auto& panel = row_->add<DevicePanel>("device", preview_->signature(), preview_->panelDesc(), preview_.get(), preview_->values());
    panel_ = &panel;
    panel.setHeight(ui::Length::fixed(panel.logicalHeight()));
    row_->setWidth(ui::Length::fixed(panel.logicalWidth() + 8.0f));
    setVisible(true);
    ctx_.setDetailVisible(true);
    ctx_.setDetailMode(DetailMode::Device);
    invalidateLayout();
    return panel_;
  }
  DevicePreview* preview() const noexcept { return preview_.get(); }
  DevicePanel* panel() const noexcept { return panel_; }

private:
  void togglePlay() {
    if (!preview_) return;
    if (preview_->playing()) preview_->stop();
    else if (Status s = preview_->play(); !s) ctx_.setStatus("Cannot play the device: " + s.error().message.substr(0, s.error().message.find('\n')));
    else if (panel_) panel_->wakeDisplays();
    onFrame(0.0);
  }
  void clearDevice() {
    if (panel_) {
      row_->removeChild(*panel_);
      panel_ = nullptr;
    }
    preview_.reset();
  }
  void closeDevice() {
    clearDevice();
    setVisible(false);
    invalidateLayout();
  }
  void openMenu() {
    std::vector<ui::MenuItem> items;
    for (const DeviceEntry& d : listDevices(dir_)) {
      const std::string path = d.path;
      items.push_back(ui::MenuItem::item(d.title, [this, path] {
        std::string err;
        if (!open(path, &err)) ctx_.setStatus("Cannot open the device: " + err.substr(0, err.find('\n')));
      }));
    }
    if (items.empty()) {
      ctx_.setStatus("No stock devices found");
      return;
    }
    ui::Widget& slot = shell_.slot(ShellSlot::DeviceDetail);
    ctx_.setDetailVisible(true);
    ctx_.setDetailMode(DetailMode::Device);
    const ui::IRect r = slot.rect();
    ui::openContextMenu(slot, {static_cast<float>(r.x + 8), static_cast<float>(r.y + 8)}, std::move(items));
  }

  Shell& shell_;
  ViewContext& ctx_;
  std::string dir_;
  circuit::PatchPlayer* player_ = nullptr;
  PlayButton* play_ = nullptr;
  ui::ScrollView* scroll_ = nullptr;
  ui::Widget* row_ = nullptr;
  std::unique_ptr<DevicePreview> preview_;
  DevicePanel* panel_ = nullptr;
};

namespace {
DeviceView* findView(Shell& shell) noexcept {
  ui::Widget& slot = shell.slot(ShellSlot::DeviceDetail);
  for (size_t i = 0; i < slot.childCount(); ++i)
    if (auto* v = dynamic_cast<DeviceView*>(&slot.child(i))) return v;
  return nullptr;
}
}  // namespace

void installDeviceView(Shell& shell) {
  ui::Widget& slot = shell.slot(ShellSlot::DeviceDetail);
  static_cast<shell::SlotHost&>(slot).setHint("No devices \xC2\xB7 View \xE2\x80\xBA Open Device\xE2\x80\xA6");
  slot.add<DeviceView>(shell);
}

void setDeviceLibrary(Shell& shell, std::string devicesDir, circuit::PatchPlayer* player) {
  if (DeviceView* v = findView(shell)) v->setLibrary(std::move(devicesDir), player);
}

DevicePanel* openDevice(Shell& shell, const std::string& devicePath, std::string* error) {
  DeviceView* v = findView(shell);
  if (!v) {
    if (error) *error = "no Device view installed";
    return nullptr;
  }
  return v->open(devicePath, error);
}

DevicePreview* openPreview(Shell& shell) noexcept {
  DeviceView* v = findView(shell);
  return v ? v->preview() : nullptr;
}

DevicePanel* openPanel(Shell& shell) noexcept {
  DeviceView* v = findView(shell);
  return v ? v->panel() : nullptr;
}

}  // namespace pq::views::devices
