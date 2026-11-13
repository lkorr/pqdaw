// The Export Audio window (EX.2): pq/views/export/export_window.h describes it.
#include "pq/views/export/export_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "../common/paint.h"
#include "../prefs/form.h"
#include "../shell/shell_widgets.h"
#include "pq/ui/theme.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/browser/library.h"
#include "pq/views/shell/shell.h"

namespace pq::views {

using namespace pq::ui;
using namespace form;

namespace {

constexpr float kControlWidth = 220.0f;
constexpr const char* kTrackItems[] = {"Master", "All Individual Tracks", "Selected Tracks"};
constexpr const char* kRangeItems[] = {"Song", "Loop", "Time Selection"};
constexpr const char* kBitItems[] = {"16", "24", "32 (float)"};
constexpr const char* kFlacBitItems[] = {"16", "24"};  // EX.3: FLAC is integer only
constexpr const char* kFormatItems[] = {"WAV", "FLAC"};
constexpr const char* kDitherItems[] = {"None", "TPDF", "Shaped"};

template <size_t N>
std::vector<std::string> itemsOf(const char* const (&a)[N]) {
  return {std::begin(a), std::end(a)};
}

// The progress bar: a sunken well filled with the accent up to the fraction done, the percentage centred.
class ProgressBar : public Widget {
public:
  ProgressBar() : Widget("progress") {
    setHeight(Length::fixed(kRowHeight));
    setWidth(Length::flex());
  }
  float value = 0.0f;
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    paint::box(ctx.dl, rect(), 0.0f, th->field.fill, th->field.border);
    const IRect in = paint::inset(rect(), 1);
    const auto w = static_cast<int32_t>(std::lround(static_cast<float>(in.w) * std::clamp(value, 0.0f, 1.0f)));
    if (w > 0) ctx.dl.addRectFilled(IRect{in.x, in.y, w, in.h}.toRect(), th->accent.primary);
    if (const ThemeFonts* f = ctx.res.uiFonts) {
      char b[16];
      std::snprintf(b, sizeof b, "%d %%", static_cast<int>(std::floor(std::clamp(value, 0.0f, 1.0f) * 100.0f)));
      paint::text(ctx, f->numeric, rect(), b, th->text.primary, TextAlign::Center);
    }
  }
};

std::string fileNameOf(const std::string& path) {
  const size_t s = path.find_last_of("/\\");
  return s == std::string::npos ? path : path.substr(s + 1);
}

std::string folderOf(const std::string& path) {
  const size_t s = path.find_last_of("/\\");
  return s == std::string::npos ? std::string() : path.substr(0, s);
}

// The done line for a finished job (also the status line when the window was closed meanwhile).
std::string summary(const ExportStatus& s) {
  switch (s.state) {
    case ExportStatus::State::Idle:
    case ExportStatus::State::Running: return {};
    case ExportStatus::State::Cancelled: return "Export cancelled: no files written.";
    case ExportStatus::State::Failed: return "Export failed: " + s.message;
    case ExportStatus::State::Done:
      if (s.files.empty()) return "Export done: no files written.";
      if (s.files.size() == 1) return "Exported " + fileNameOf(s.files[0]) + " to " + folderOf(s.files[0]);
      return "Exported " + std::to_string(s.files.size()) + " files to " + folderOf(s.files[0]);
  }
  return {};
}

}  // namespace

Ticks exportSongEnd(const doc::Project& p) noexcept {
  Ticks end = 0;
  doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) {
    for (const doc::ClipSlot& c : doc::playedClips(t)) end = std::max(end, c.end());
  });
  return end;
}

ExportWindow::ExportWindow(ExportPort& port, ViewContext& ctx, ExportSelection selection)
    : Popover("export-audio"), port_(port), ctx_(ctx), selection_(std::move(selection)) {
  setSize(Length::fixed(kWidth), Length::fixed(kHeight));
  const ExportPrefs& prefs = ctx_.viewPrefs().exportAudio;

  auto& header = add<Widget>("header");
  header.setLayout(LayoutKind::Row);
  header.setHeight(Length::fixed(22));
  header.add<Label>("title", "Export Audio", FontRoleId::Title, TextTone::Primary);
  auto& close = header.add<Button>("close", "Close");
  close.setWidth(Length::fixed(56));
  close.setInfo("Close", "Closes the window (Esc). A running export goes on; the status line says when it is done.");
  close.onClick = [this] {
    if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*this);
  };

  auto& body = add<Widget>("body");
  body.setLayout(LayoutKind::Column);
  body.setGap(5);
  body.setHeight(Length::flex());

  section(body, "rendering-section", "Rendering");
  {
    auto& r = row(body, "tracks", "Rendered Track");
    tracks_ = &r.add<Dropdown>("chooser", itemsOf(kTrackItems), static_cast<int32_t>(prefs.tracks));
    tracks_->setWidth(Length::fixed(kControlWidth));
    tracks_->setInfo("Rendered Track", "Master: one file of what the master plays. All Individual Tracks: one file per "
                                       "track, after its fader. Selected Tracks: the tracks selected in the arrangement.");
    tracks_->onChange = [this](int32_t i) {
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.tracks = static_cast<ExportTracks>(std::clamp(i, 0, 2));
      choose(p);
    };
  }
  {
    auto& r = row(body, "range", "Range");
    range_ = &r.add<Dropdown>("chooser", itemsOf(kRangeItems), static_cast<int32_t>(prefs.range));
    range_->setWidth(Length::fixed(kControlWidth));
    range_->setInfo("Range", "Song: bar 1 to the end of the last clip, with the release tail (until it is silent, at "
                             "most 10 s). Loop: the loop brace. Time Selection: the arrangement's time selection.");
    range_->onChange = [this](int32_t i) {
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.range = static_cast<ExportRange>(std::clamp(i, 0, 2));
      choose(p);
    };
  }
  {
    auto& r = row(body, "range-readout", "");
    rangeLabel_ = &r.add<Label>("value", "", FontRoleId::Small, TextTone::Primary);
    rangeLabel_->setWidth(Length::flex());
    rangeLabel_->setInfo("Range", "Where the export starts and how long it is (bars. beats. sixteenths).");
  }
  {
    auto& r = row(body, "returns", "Include Return and Master Effects");
    returns_ = &r.add<Toggle>("toggle", "On", ToggleStyle::Accent, prefs.includeReturnsMaster);
    returns_->setWidth(Length::fixed(40));
    returns_->setInfo("Include Return and Master Effects",
                      "Each track's file plays through the returns and the master, as if it were soloed (one render "
                      "per track). Off: the track after its own fader.");
    returns_->onToggle = [this](bool on) {
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.includeReturnsMaster = on;
      choose(p);
    };
  }
  {
    auto& r = row(body, "loop", "Render as Loop");
    loop_ = &r.add<Toggle>("toggle", "On", ToggleStyle::Accent, prefs.renderAsLoop);
    loop_->setWidth(Length::fixed(40));
    loop_->setInfo("Render as Loop", "Renders the range twice and keeps the second time round: the start holds the "
                                     "end's reverb and delay tails, so the file loops seamlessly.");
    loop_->onToggle = [this](bool on) {
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.renderAsLoop = on;
      choose(p);
    };
  }

  section(body, "encoding-section", "Encoding");
  {
    auto& r = row(body, "format", "File Type");
    format_ = &r.add<Dropdown>("chooser", itemsOf(kFormatItems), static_cast<int32_t>(prefs.format));
    format_->setWidth(Length::fixed(120));
    format_->setInfo("File Type", "WAV, or FLAC: lossless and about half the size, 16 or 24-bit only.");
    format_->onChange = [this](int32_t i) {
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.format = i == 1 ? ExportFileFormat::Flac : ExportFileFormat::Wav;
      // FLAC has no 32-bit float: the depth becomes 24-bit (the closest; switching back to WAV keeps it).
      if (p.format == ExportFileFormat::Flac && p.bits == ExportBitDepth::Float32) p.bits = ExportBitDepth::Pcm24;
      choose(p);
    };
  }
  {
    auto& r = row(body, "rate", "Sample Rate");
    rate_ = &r.add<Dropdown>("chooser", std::vector<std::string>{}, 0);
    rate_->setWidth(Length::fixed(120));
    rate_->setInfo("Sample Rate", "The files' sample rate. The project is rendered at this rate (no conversion). The "
                                  "default is the engine's rate.");
    rate_->onChange = [this](int32_t i) {
      if (i < 0 || static_cast<size_t>(i) >= rates_.size()) return;
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.sampleRate = rates_[static_cast<size_t>(i)];
      choose(p);
    };
  }
  {
    auto& r = row(body, "bits", "Bit Depth");
    bits_ = &r.add<Dropdown>("chooser", prefs.format == ExportFileFormat::Flac ? itemsOf(kFlacBitItems) : itemsOf(kBitItems),
                             static_cast<int32_t>(prefs.bits));
    bits_->setWidth(Length::fixed(120));
    bits_->setInfo("Bit Depth", "16 or 24-bit integer samples, or 32-bit float (WAV only: no dither, no clipping above "
                                "0 dB).");
    bits_->onChange = [this](int32_t i) {
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.bits = static_cast<ExportBitDepth>(std::clamp(i, 0, p.format == ExportFileFormat::Flac ? 1 : 2));
      choose(p);
    };
  }
  {
    auto& r = row(body, "dither", "Dither");
    dither_ = &r.add<Dropdown>("chooser", itemsOf(kDitherItems), static_cast<int32_t>(prefs.dither));
    dither_->setWidth(Length::fixed(120));
    dither_->setInfo("Dither", "Noise added before the samples are rounded to 16 or 24 bits. TPDF: plain triangular "
                               "noise. Shaped: the noise moved above ~10 kHz, where it is heard least.");
    dither_->onChange = [this](int32_t i) {
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.dither = static_cast<ExportDither>(std::clamp(i, 0, 2));
      choose(p);
    };
  }
  {
    auto& r = row(body, "normalize", "Normalize");
    normalize_ = &r.add<Toggle>("toggle", "On", ToggleStyle::Accent, prefs.normalize);
    normalize_->setWidth(Length::fixed(40));
    normalize_->setInfo("Normalize", "Each file's loudest sample is brought to −0.3 dBFS.");
    normalize_->onToggle = [this](bool on) {
      ExportPrefs p = ctx_.viewPrefs().exportAudio;
      p.normalize = on;
      choose(p);
    };
  }

  section(body, "output-section", "Output");
  {
    auto& r = row(body, "folder", "Folder");
    folder_ = &r.add<Label>("path", "", FontRoleId::Small, TextTone::Primary);
    folder_->setWidth(Length::flex());
    folder_->setInfo("Folder", "Where the files go (remembered). Existing files of the same name are replaced.");
    browse_ = &r.add<Button>("browse", "Browse\xE2\x80\xA6");
    browse_->setWidth(Length::fixed(88));
    browse_->setInfo("Browse", "Chooses the folder the files go to.");
    browse_->onClick = [this] {
      FileDialogs* d = ctx_.fileDialogs();
      if (!d) return;
      // The dialog answers later (SDL: through the event queue); the window may have closed meanwhile: the choice is
      // remembered either way, an open window shows it.
      d->chooseFolder([c = &ctx_, alive = alive_, this](std::string folder) {
        if (folder.empty()) return;
        ViewPrefs v = c->viewPrefs();
        v.exportAudio.folder = std::move(folder);
        c->setViewPrefs(v);
        if (*alive) updateRows();
      });
    };
  }
  {
    auto& r = row(body, "name", "File Name");
    name_ = &r.add<TextEdit>("field", ctx_.projectName());
    name_->setWidth(Length::fixed(kControlWidth + 100.0f));
    name_->setInfo("File Name", "The master's file name; with tracks, the start of each track's file name "
                                "(\"<name> <track>.wav\" or .flac).");
    name_->onChange = [this](const std::string&) { updateRows(); };
  }

  auto& foot = add<Widget>("footer");
  foot.setLayout(LayoutKind::Row);
  foot.setHeight(Length::fixed(kRowHeight));
  foot.setGap(8);
  done_ = &foot.add<Label>("done", "", FontRoleId::Small, TextTone::Secondary);
  done_->setWidth(Length::flex());
  progress_ = &foot.add<ProgressBar>();
  progress_->setVisible(false);
  export_ = &foot.add<Button>("export", "Export");
  export_->setWidth(Length::fixed(88));
  export_->setInfo("Export", "Renders the files (in the background: playing and editing go on).");
  export_->onClick = [this] { (void)exportNow(); };
  cancel_ = &foot.add<Button>("cancel", "Cancel");
  cancel_->setWidth(Length::fixed(88));
  cancel_->setInfo("Cancel", "Stops the export. Nothing new is left in the folder.");
  cancel_->onClick = [this] { cancelExport(); };
  cancel_->setVisible(false);

  shown_ = port_.status();
  running_ = shown_.state == ExportStatus::State::Running;
  updateRows();
  showStatus(shown_);
}

ExportWindow::~ExportWindow() { *alive_ = false; }

std::string ExportWindow::folder() const {
  const ExportPrefs& p = ctx_.viewPrefs().exportAudio;
  if (!p.folder.empty()) return p.folder;
  if (!ctx_.projectDir().empty()) return ctx_.projectDir();
  return ctx_.library().userLibrary();
}

std::vector<doc::TrackId> ExportWindow::selectedTracks() const {
  std::vector<doc::TrackId> out;
  const doc::Project& p = *ctx_.doc();
  for (doc::TrackId id : selection_.tracks) {
    const doc::Track* t = doc::findTrackPtr(p, id);
    if (t && t->kind != doc::TrackKind::Master && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

std::optional<TickRange> ExportWindow::rangeOf(ExportRange r) const {
  const doc::Project& p = *ctx_.doc();
  switch (r) {
    case ExportRange::Song: {
      const Ticks end = exportSongEnd(p);
      if (end <= 0) return std::nullopt;
      return TickRange{0, end};
    }
    case ExportRange::Loop:
      if (p.loop.end <= p.loop.start) return std::nullopt;
      return TickRange{p.loop.start, p.loop.end};
    case ExportRange::TimeSelection:
      if (!selection_.time || selection_.time->end <= selection_.time->start) return std::nullopt;
      return selection_.time;
  }
  return std::nullopt;
}

std::optional<ExportJob> ExportWindow::job(std::string* why) const {
  auto fail = [&](const char* m) -> std::optional<ExportJob> {
    if (why) *why = m;
    return std::nullopt;
  };
  const ExportPrefs& p = ctx_.viewPrefs().exportAudio;
  const std::optional<TickRange> range = rangeOf(p.range);
  if (!range) {
    switch (p.range) {
      case ExportRange::Song: return fail("Nothing to export: the arrangement has no clips.");
      case ExportRange::Loop: return fail("The loop is empty.");
      case ExportRange::TimeSelection: return fail("No time selection: select a range in the arrangement first.");
    }
  }
  ExportJob j;
  j.snapshot = ctx_.doc();
  j.from = range->start;
  j.to = range->end;
  j.song = p.range == ExportRange::Song;
  j.tracks = p.tracks;
  if (p.tracks == ExportTracks::Selected) {
    j.trackIds = selectedTracks();
    if (j.trackIds.empty()) return fail("No tracks selected: select tracks in the arrangement first.");
  }
  j.includeReturnsMaster = p.tracks != ExportTracks::Master && p.includeReturnsMaster;
  j.renderAsLoop = p.renderAsLoop;
  j.normalize = p.normalize;
  j.sampleRate = p.sampleRate != 0 ? p.sampleRate : port_.engineSampleRate();
  j.bits = p.bits;
  j.dither = p.bits == ExportBitDepth::Float32 ? ExportDither::None : p.dither;
  j.format = p.format;
  j.folder = folder();
  if (j.folder.empty()) return fail("Choose a folder (Browse\xE2\x80\xA6).");
  j.baseName = name_->text();
  if (p.tracks == ExportTracks::Master && j.baseName.empty()) return fail("Type a file name.");
  j.projectDir = ctx_.projectDir();
  return j;
}

bool ExportWindow::exportNow() {
  if (running_) return false;
  std::string why;
  const std::optional<ExportJob> j = job(&why);
  if (!j) {
    done_->setText(why);
    return false;
  }
  if (!port_.start(*j)) {
    done_->setText("Another export is running.");
    return false;
  }
  running_ = true;
  shown_ = port_.status();
  updateRows();
  showStatus(shown_);
  return true;
}

void ExportWindow::cancelExport() {
  if (running_) port_.cancel();
}

void ExportWindow::poll() {
  const ExportStatus s = port_.status();
  if (s == shown_) return;
  shown_ = s;
  const bool was = running_;
  running_ = s.state == ExportStatus::State::Running;
  if (was != running_) updateRows();
  showStatus(s);
}

void ExportWindow::showStatus(const ExportStatus& s) {
  auto* bar = static_cast<ProgressBar*>(progress_);
  if (running_) {
    if (bar->value != s.progress) {
      bar->value = s.progress;
      bar->markDirty();
    }
    if (timer_ == 0) timer_ = requestTimer(kPollSeconds);
  } else {
    if (timer_ != 0) cancelTimer(timer_);
    timer_ = 0;
    const std::string text = summary(s);
    if (!text.empty()) done_->setText(text);
  }
}

void ExportWindow::startPolling() {
  if (running_ && timer_ == 0) timer_ = requestTimer(kPollSeconds);
}

void ExportWindow::onTimer(uint32_t id) {
  if (id != timer_) return;
  timer_ = 0;
  poll();
  if (running_ && timer_ == 0) timer_ = requestTimer(kPollSeconds);
}

void ExportWindow::choose(const ExportPrefs& p) {
  ViewPrefs v = ctx_.viewPrefs();
  v.exportAudio = p;
  ctx_.setViewPrefs(v);
  updateRows();
}

void ExportWindow::updateRows() {
  const ExportPrefs& p = ctx_.viewPrefs().exportAudio;
  const bool idle = !running_;
  // The button pressed is disabled and hidden below (which drops the keyboard focus): the focus moves to the one taking
  // its place, so Esc keeps closing the window and Space / Enter press the new button.
  const Widget* focus = tree() ? tree()->focus() : nullptr;
  const bool moveFocus = tree() && (focus == nullptr || focus == export_ || focus == cancel_);
  tracks_->setIndex(static_cast<int32_t>(p.tracks));
  range_->setIndex(static_cast<int32_t>(p.range));
  returns_->setOn(p.includeReturnsMaster);
  loop_->setOn(p.renderAsLoop);
  format_->setIndex(static_cast<int32_t>(p.format));
  // EX.3: FLAC offers 16 and 24-bit only (choose() never leaves FLAC at 32-bit float).
  if (const size_t n = p.format == ExportFileFormat::Flac ? std::size(kFlacBitItems) : std::size(kBitItems);
      bits_->items().size() != n)
    bits_->setItems(p.format == ExportFileFormat::Flac ? itemsOf(kFlacBitItems) : itemsOf(kBitItems),
                    static_cast<int32_t>(p.bits));
  bits_->setIndex(static_cast<int32_t>(p.bits));
  dither_->setIndex(static_cast<int32_t>(p.dither));
  normalize_->setOn(p.normalize);
  // The rates: the common ones and the engine's.
  const uint32_t engine = port_.engineSampleRate();
  const uint32_t rate = p.sampleRate != 0 ? p.sampleRate : engine;
  std::vector<uint32_t> rates(std::begin(kRates), std::end(kRates));
  for (uint32_t extra : {engine, rate})
    if (extra != 0 && std::find(rates.begin(), rates.end(), extra) == rates.end()) rates.push_back(extra);
  std::sort(rates.begin(), rates.end());
  if (rates != rates_) {
    rates_ = rates;
    std::vector<std::string> items;
    for (uint32_t r : rates_) items.push_back(std::to_string(r));
    rate_->setItems(std::move(items), 0);
  }
  rate_->setIndex(static_cast<int32_t>(std::find(rates_.begin(), rates_.end(), rate) - rates_.begin()));
  // The range readout.
  const doc::Project& doc = *ctx_.doc();
  std::string readout;
  if (const std::optional<TickRange> r = rangeOf(p.range)) {
    const TimeSigMap& sigs = doc.timeline->sigMap;
    readout = "Start " + shell::formatPosition(sigs, r->start) + "  \xC2\xB7  Length " +
              shell::formatDuration(sigs, r->start, r->end - r->start);
    if (p.range == ExportRange::Song && !p.renderAsLoop) readout += "  + tail";
  } else {
    readout = p.range == ExportRange::Song ? "No clips" : p.range == ExportRange::Loop ? "Empty loop" : "No time selection";
  }
  if (p.tracks == ExportTracks::Selected) {
    const size_t n = selectedTracks().size();
    readout += "  \xC2\xB7  " + (n == 1 ? std::string("1 track") : std::to_string(n) + " tracks");
  }
  if (rangeLabel_->text() != readout) rangeLabel_->setText(readout);
  const std::string f = folder().empty() ? std::string("No folder chosen") : folder();
  if (folder_->text() != f) folder_->setText(f);
  // Enabled: everything while idle, except what does not apply.
  tracks_->setEnabled(idle);
  range_->setEnabled(idle);
  returns_->setEnabled(idle && p.tracks != ExportTracks::Master);
  loop_->setEnabled(idle);
  format_->setEnabled(idle);
  rate_->setEnabled(idle);
  bits_->setEnabled(idle);
  dither_->setEnabled(idle && p.bits != ExportBitDepth::Float32);
  normalize_->setEnabled(idle);
  browse_->setEnabled(idle && ctx_.fileDialogs() != nullptr);
  name_->setEnabled(idle);
  std::string why;
  const bool can = job(&why).has_value();
  export_->setEnabled(idle && can);
  export_->setVisible(idle);
  cancel_->setVisible(!idle);
  if (moveFocus) {
    Widget* next = !idle ? static_cast<Widget*>(cancel_) : export_->enabled() ? static_cast<Widget*>(export_) : this;
    tree()->setFocus(next);
  }
  progress_->setVisible(!idle);
  done_->setVisible(idle);
  // The done line says why Export is disabled; else it keeps the last job's result.
  if (idle) done_->setText(can ? summary(shown_) : why);
  markDirty();
}

const std::string& ExportWindow::rangeText() const { return rangeLabel_->text(); }
const std::string& ExportWindow::doneText() const { return done_->text(); }
const std::string& ExportWindow::folderText() const { return folder_->text(); }
float ExportWindow::progressShown() const noexcept { return static_cast<const ProgressBar*>(progress_)->value; }

InfoText ExportWindow::info() const {
  return {"Export Audio", "Renders the master, every track or the selected tracks to audio files, over the song, the "
                          "loop or the time selection. Choices are remembered. Esc closes."};
}

ExportWindow* findExportWindow(const Widget& w) {
  OverlayLayer* layer = OverlayLayer::of(w);
  if (!layer) return nullptr;
  for (size_t i = 0; i < layer->childCount(); ++i)
    if (auto* p = dynamic_cast<ExportWindow*>(&layer->child(i)); p && layer->isOpen(*p)) return p;
  return nullptr;
}

ExportWindow* openExportWindow(Widget& from, ExportPort& port, ViewContext& ctx, ExportSelection selection,
                               std::function<void()> onClosed) {
  OverlayLayer* layer = OverlayLayer::of(from);
  if (!layer) return nullptr;
  if (ExportWindow* open = findExportWindow(from)) return open;
  auto win = std::make_unique<ExportWindow>(port, ctx, std::move(selection));
  ExportWindow* raw = win.get();
  UiTree* tree = layer->tree();
  const UiScale s(tree ? tree->scale() : 1.0f);
  const int32_t W = tree ? static_cast<int32_t>(tree->width()) : 0, H = tree ? static_cast<int32_t>(tree->height()) : 0;
  OverlayOptions o;
  o.anchor = {std::max(0, (W - s.px(ExportWindow::kWidth)) / 2), std::max(0, (H - s.px(ExportWindow::kHeight)) / 2), 0, 0};
  o.placement = Placement::AtPoint;
  o.dismissOnOutsideClick = false;  // a dialog (Live's is modal): Close or Esc
  o.onClosed = std::move(onClosed);
  layer->open(std::move(win), std::move(o));
  raw->startPolling();  // reopened while a job runs
  return raw;
}

namespace {

// Holds the file.exportAudio handler for the shell's lifetime (a hidden child of the clip detail slot, like the
// preferences host). While a window closed during an export, it polls the port (4 Hz) until the job ends and puts the
// result in the status line.
class ExportHost : public Widget {
public:
  explicit ExportHost(Shell& shell) : Widget("export-host"), shell_(shell), ctx_(shell.context()) {
    setVisible(false);
    ctx_.setViewAction(
        "file.exportAudio", [this] { toggle(); }, [this] { return ctx_.exportPort() != nullptr; });
  }
  ~ExportHost() override {
    *alive_ = false;
    ctx_.clearViewAction("file.exportAudio");
  }

  void onTimer(uint32_t id) override {
    if (id != timer_) return;
    timer_ = 0;
    watch();
  }

private:
  void toggle() {
    OverlayLayer& layer = shell_.overlay();
    if (ExportWindow* open = findExportWindow(layer)) {
      layer.close(*open);
      return;
    }
    ExportPort* port = ctx_.exportPort();
    if (!port) return;
    if (timer_ != 0) cancelTimer(timer_);
    timer_ = 0;
    (void)openExportWindow(layer, *port, ctx_, selectionNow(), [this, alive = alive_] {
      if (*alive) watch();
    });
  }

  // After the window closed: the export it started (if one runs) is followed to its end.
  void watch() {
    ExportPort* port = ctx_.exportPort();
    if (!port || findExportWindow(shell_.overlay())) return;
    const ExportStatus s = port->status();
    if (s.state == ExportStatus::State::Running) {
      timer_ = requestTimer(0.25);
      watching_ = true;
      return;
    }
    if (watching_) {
      watching_ = false;
      ctx_.setStatus(summary(s));
    }
  }

  ExportSelection selectionNow() const {
    ExportSelection sel;
    auto* arr = dynamic_cast<ArrangementView*>(shell_.slot(ShellSlot::TrackArea).find("arrangement"));
    if (arr) {
      const ArrangementView::Selection& s = arr->selection();
      sel.tracks = s.tracks;
      if (s.time && s.time->end > s.time->start) sel.time = s.time;
      else if (const auto& t = arr->timeSelection(); t && t->end > t->start) sel.time = t;
    }
    if (sel.tracks.empty() && ctx_.selectedTrack().valid()) sel.tracks.push_back(ctx_.selectedTrack());
    return sel;
  }

  Shell& shell_;
  ViewContext& ctx_;
  uint32_t timer_ = 0;
  bool watching_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

void installExportAudio(Shell& shell) { shell.slot(ShellSlot::ClipDetail).add<ExportHost>(shell); }

}  // namespace pq::views
