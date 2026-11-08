// F12 bug reports in the window (T5.x-bugreport): pq/app/bug_reporter.h.
#include "pq/app/bug_reporter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include "build_info.h"
#include "pq/app/audio_manager.h"
#include "pq/app/pipeline_cache.h"
#include "pq/app/project_link.h"
#include "pq/app/startup.h"
#include "pq/document/samples.h"
#include "pq/document/serialize.h"
#include "pq/engine/callback_stats.h"
#include "pq/engine/stream_trace.h"
#include "pq/views/shell/shell.h"

namespace pq::app {

namespace fs = std::filesystem;
namespace br = views::bugreport;

namespace {

fs::path pathOfUtf8(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8Of(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

bool isCheckout(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p / "SPEC.md", ec) && fs::is_directory(p / "src" / "app", ec);
}

std::string osVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  RTL_OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof v;
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion"))); fn && fn(&v) == 0) {
      char b[64];
      std::snprintf(b, sizeof b, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
      return b;
    }
  return "Windows (version unknown)";
}

std::string mb(double bytes) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f MB", bytes / (1024.0 * 1024.0));
  return b;
}

std::string indent(const std::string& lines, const char* prefix) {
  std::string out;
  size_t p = 0;
  while (p < lines.size()) {
    size_t e = lines.find('\n', p);
    if (e == std::string::npos) e = lines.size();
    out += prefix;
    out.append(lines, p, e - p);
    out += '\n';
    p = e + 1;
  }
  return out;
}

const char* driverName(views::AudioDriverType t) { return views::audioDriverTypeName(t); }

// AU.stream: the stream's flight recorder (pq/engine/stream_trace.h) over the last `seconds`: starts, stops, driver
// messages, pre-rolls, fades, and the buffers where something happened, as a code block (the last maxLines lines).
std::string streamTraceText(double seconds, size_t maxLines) {
  engine::StreamTrace& tr = engine::streamTrace();
  const uint64_t head = tr.head();
  std::vector<engine::TraceEvent> all;
  (void)tr.read(head > engine::StreamTrace::kCapacity ? head - engine::StreamTrace::kCapacity : 0, all);
  const uint64_t now = engine::readTsc();
  const auto window = static_cast<uint64_t>(seconds * engine::tscFrequencyHz());
  std::vector<engine::TraceEvent> recent;
  engine::TraceFormat fmt;
  for (const engine::TraceEvent& e : all) {
    if (now - e.tsc <= window) {
      recent.push_back(e);
    } else if (e.kind == engine::TraceKind::Open || e.kind == engine::TraceKind::Reopened) {
      fmt.frames = static_cast<uint32_t>(e.a);  // the stream's shape from before the window
      fmt.rate = static_cast<uint32_t>(e.b);
    }
  }
  if (recent.empty()) return "- audio stream trace: no events in the last " + std::to_string(static_cast<int>(seconds)) + " s\n";
  const std::string text = engine::describeStreamTrace(recent, now, fmt);  // times: ms before this report (negative)
  std::vector<std::string_view> lines;
  for (size_t at = 0; at < text.size();) {
    const size_t nl = text.find('\n', at);
    const size_t end = nl == std::string::npos ? text.size() : nl;
    lines.emplace_back(text.data() + at, end - at);
    at = end + 1;
  }
  std::string s = "- audio stream trace (last " + std::to_string(static_cast<int>(seconds)) +
                  " s; ms relative to this report; notable buffers only):\n\n```\n";
  if (lines.size() > maxLines) s += "(" + std::to_string(lines.size() - maxLines) + " earlier lines left out)\n";
  for (size_t i = lines.size() > maxLines ? lines.size() - maxLines : 0; i < lines.size(); ++i) {
    s += lines[i];
    s += '\n';
  }
  s += "```\n\n";
  return s;
}

const char* stateName(views::AudioStatus::State s) {
  switch (s) {
    case views::AudioStatus::State::Opening: return "opening";
    case views::AudioStatus::State::Running: return "running";
    case views::AudioStatus::State::Suspended: return "suspended (idle in the background)";
    case views::AudioStatus::State::NoDevice: return "no device";
  }
  return "?";
}

}  // namespace

fs::path bugReportRoot() {
  if (const char* env = std::getenv("PQ_BUG_REPORTS_DIR"); env && *env) return pathOfUtf8(env);
  wchar_t buf[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n > 0 && n < std::size(buf)) {
    fs::path p = fs::path(buf).parent_path();
    for (int i = 0; i < 8 && !p.empty(); ++i, p = p.parent_path()) {
      if (isCheckout(p)) return p / "bug_reports";
      if (p == p.parent_path()) break;
    }
  }
#ifdef PQ_SOURCE_DIR
  if (const fs::path src = pathOfUtf8(PQ_SOURCE_DIR); isCheckout(src)) return src / "bug_reports";
#endif
  const std::string local = localCacheDir();
  return local.empty() ? fs::path("bug_reports") : pathOfUtf8(local) / "bug_reports";
}

BugReporter::BugReporter(views::Shell& shell, ProjectLink* link, AudioDeviceManager* audio, fs::path root)
    : shell_(shell), ctx_(shell.context()), link_(link), audio_(audio), root_(std::move(root)),
      alive_(std::make_shared<BugReporter*>(this)) {
  // The status line's recent messages (one append per change; nothing else).
  statusSub_ = ctx_.observe([this](uint32_t bits) {
    if (!(bits & views::view_change::kStatus)) return;
    const std::string& s = ctx_.status();
    if (s.empty() || (!statuses_.empty() && statuses_.back().text == s)) return;
    statuses_.push_back({now(), s});
    while (statuses_.size() > 8) statuses_.pop_front();
  });
}

BugReporter::~BugReporter() {
  *alive_ = nullptr;
  if (worker_.joinable()) worker_.join();
}

double BugReporter::now() const noexcept {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

void BugReporter::recordInput(const ui::InputEvent& e) noexcept { input_.record(e, shell_.tree(), now()); }

views::bugreport::BugReportPanel* BugReporter::panel() const { return br::findBugReportPanel(shell_.overlay()); }

bool BugReporter::panelOpen() const { return panel() != nullptr; }

bool BugReporter::wantsShot(bool shift) const {
  if (panelOpen()) return false;
  return !(shift && collecting());
}

std::string BugReporter::shotContext(const BugWindowInfo& info) {
  std::string s;
  char b[200];
  std::snprintf(b, sizeof b, "- window: %ux%u device px, UI scale %.3g (pixel density %.3g)\n", info.width, info.height,
                static_cast<double>(info.uiScale), static_cast<double>(info.pixelDensity));
  s += b;
  s += "- build: " + buildInfoLine() + "\n";
  s += br::describeShot(shell_, info.pointer, info.pointerInside);
  if (!draft_.shots.empty()) {  // a later shot of this report: what happened since the previous one
    const std::string since = input_.format(now(), 15, inputAtLastShot_);
    s += "- input since the previous shot:" + std::string(since.empty() ? " none\n" : "\n" + indent(since, "  "));
  }
  return s;
}

std::string BugReporter::firstShotContext(const BugWindowInfo& info) {
  std::string s = "## Context (at the first shot)\n\n";
  s += "- build: " + buildInfoLine() + "\n";
  char b[320];
  const double up = msSinceProcessCreation() / 1000.0;
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  std::snprintf(b, sizeof b, "- uptime: %d min %02d s; %s; %lu logical CPUs\n", static_cast<int>(up / 60), static_cast<int>(up) % 60,
                osVersion().c_str(), si.dwNumberOfProcessors);
  s += b;
  std::snprintf(b, sizeof b, "; present mode %s; last frame: wait + acquire %.2f ms, build %.2f ms, submit + present %.2f ms; %u frames presented; readback %.1f ms\n",
                info.presentMode.c_str(), info.frameBeginMs, info.frameBuildMs, info.frameEndMs, info.framesPresented, info.readbackMs);
  s += "- GPU: " + info.gpu + b;
  PROCESS_MEMORY_COUNTERS_EX pm{};
  pm.cb = sizeof pm;
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm))
    s += "- memory: working set " + mb(static_cast<double>(pm.WorkingSetSize)) + ", private " + mb(static_cast<double>(pm.PrivateUsage)) +
         ", peak working set " + mb(static_cast<double>(pm.PeakWorkingSetSize)) + "\n";
  std::snprintf(b, sizeof b, "- window: %ux%u device px, UI scale %.3g\n", info.width, info.height, static_cast<double>(info.uiScale));
  s += b;
  s += br::describeProject(shell_);
  if (audio_) {
    const views::AudioSettings st = audio_->settings();
    const views::AudioStatus as = audio_->status();
    std::snprintf(b, sizeof b, "- audio: %s, \"%s\", %u Hz, %u frames (asked %u Hz / %u), %s; in/out latency %.2f / %.2f ms; control bar %s; disk underruns %llu\n",
                  driverName(st.driver), as.deviceName.empty() ? st.outputName.c_str() : as.deviceName.c_str(), as.sampleRate,
                  as.bufferFrames, st.sampleRate, st.bufferFrames, stateName(as.state), as.inputLatencyMs, as.outputLatencyMs,
                  shell_.cpuText().empty() ? "?" : shell_.cpuText().c_str(), static_cast<unsigned long long>(audio_->diskUnderruns()));
    s += b;
    if (!as.message.empty()) s += "- audio message: " + as.message + "\n";
    if (!audio_->lastError().empty()) s += "- audio last error: " + audio_->lastError() + "\n";
    s += streamTraceText(30.0, 160);
  } else {
    s += "- audio: no device manager (playback engine unavailable)\n";
  }
  if (link_) {
    const JitStats j = link_->jitStats();
    std::snprintf(b, sizeof b, "- engine: %.0f Hz; JIT images wanted %llu, built %llu, published %llu, stale %llu, errors %llu\n",
                  link_->engine().config().sampleRate, static_cast<unsigned long long>(j.wanted), static_cast<unsigned long long>(j.built),
                  static_cast<unsigned long long>(j.published), static_cast<unsigned long long>(j.stale),
                  static_cast<unsigned long long>(j.errors));
    s += b;
    if (const std::string e = link_->lastError(); !e.empty()) s += "- engine last error: " + e + "\n";
  }
  s += "\n## Recent input\n\n(oldest first; seconds before the first shot. Pointer moves are not logged.)\n\n";
  firstInput_ = input_.format(now(), 40);
  s += firstInput_.empty() ? std::string("- (none)\n") : firstInput_;
  s += "\n## Recent status messages\n\n";
  if (statuses_.empty()) s += "- (none)\n";
  for (const Status& st : statuses_) {
    std::snprintf(b, sizeof b, "- %+.1f s  ", st.t - now());
    s += b + st.text + "\n";
  }
  return s;
}

void BugReporter::onKey(bool shift, Result<ui::Image> frame, const BugWindowInfo& info) {
  if (panelOpen()) {  // F12 on the panel: hide it, keep the draft
    close();
    return;
  }
  if (shift && collecting()) {
    openPanel();
    return;
  }
  std::string problem;
  if (!frame) {
    problem = "No picture: " + frame.error().message;
  } else {
    br::Shot shot;
    shot.image = std::move(*frame);
    if (info.pointerInside) shot.crop = br::cursorCrop(shot.image, info.pointer.x, info.pointer.y);
    shot.context = shotContext(info);
    const bool first = draft_.shots.empty() && draft_.trailer.empty();
    if (first) {
      draft_.trailer = firstShotContext(info);
      // The document as it is now (unsaved edits included), samples' relative paths made absolute so the copy opens
      // from the report folder with every sample where it is.
      doc::Project copy = *ctx_.doc();
      if (copy.samples && !copy.samples->empty()) {
        auto list = std::make_shared<doc::SampleList>(*copy.samples);
        for (doc::SampleRef& r : *list)
          if (!doc::isAbsoluteSamplePath(r.path)) r.path = doc::absoluteSamplePath(r.path, ctx_.projectDir());
        copy.samples = std::move(list);
      }
      if (auto text = doc::serializeProject(copy, ctx_.store().view())) {
        draft_.project = std::move(*text);
        draft_.attachProject = draft_.project.size() <= br::kAttachProjectDefaultLimit;
        draft_.trailer.insert(draft_.trailer.find("\n## Recent input"),
                              "- project copy: `project.pqd` when attached (the document at this shot; sample paths made absolute); "
                              "reopen with `pqdaw --project <this folder>`\n");
      } else {
        problem = "The project could not be serialized: " + text.error().message;
      }
    }
    inputAtLastShot_ = input_.total();
    draft_.shots.push_back(std::move(shot));
  }
  openPanel();
  if (!problem.empty())
    if (auto* p = panel()) p->setMessage(problem);
}

void BugReporter::openPanel() {
  br::PanelHooks h;
  h.submit = [this] { submit(); };
  h.addAnother = [this] { addAnother(); };
  h.discard = [this] { discard(); };
  h.close = [this] { close(); };
  h.post = ctx_.uiPoster();
  br::BugReportPanel* p = br::openBugReportPanel(shell_.overlay(), draft_, root_, std::move(h));
  if (p) p->shotsChanged();
}

void BugReporter::close() {
  if (auto* p = panel()) shell_.overlay().close(*p);
}

void BugReporter::addAnother() {
  close();
  ctx_.setStatus("Bug report: press F12 for the next shot (Shift+F12: back to the report)");
}

void BugReporter::discard() {
  close();
  draft_ = {};
  firstInput_.clear();
  ctx_.setStatus("Bug report discarded");
}

void BugReporter::waitForWrite() {
  if (worker_.joinable()) worker_.join();
}

void BugReporter::submit() {
  // The writer's own checks, before the panel goes (it then shows why).
  std::string problem;
  if (draft_.target.empty() && draft_.shots.empty() && !draft_.suggestion) problem = "A new bug report needs a picture: Add another, then F12.";
  if (draft_.target.empty() && draft_.suggestion && draft_.shots.empty() && draft_.text.find_first_not_of(" \t\r\n") == std::string::npos)
    problem = "A suggestion needs a picture or some text.";
  if (!draft_.target.empty() && draft_.shots.empty() && draft_.text.empty()) problem = "Nothing to add to the ticket.";
  if (!problem.empty()) {
    if (auto* p = panel()) p->setMessage(problem);
    return;
  }
  br::Draft d = std::move(draft_);
  draft_ = {};
  if (!d.target.empty() && !d.shots.empty() && !firstInput_.empty())  // appended: the steps go with the first new shot
    d.shots.front().context += "- recent input (before this report's first shot):\n" + indent(firstInput_, "  ");
  firstInput_.clear();
  close();
  waitForWrite();
  written_ = false;
  const std::string folder = br::localStamp(true), human = br::localStamp(false);
  const views::ViewContext::UiPoster& post = ctx_.uiPoster();
  if (!post) {  // tests: synchronously
    onWritten(br::writeReport(root_, d, folder, human));
    return;
  }
  ctx_.setStatus("Bug report: writing…");
  std::weak_ptr<BugReporter*> alive = alive_;
  worker_ = std::thread([alive, post, d = std::move(d), root = root_, folder, human]() mutable {
    auto r = br::writeReport(root, d, folder, human);
    auto shared = std::make_shared<Result<br::WriteResult>>(std::move(r));
    post([alive, shared] {
      if (auto a = alive.lock(); a && *a) (*a)->onWritten(std::move(*shared));
    });
  });
}

void BugReporter::onWritten(Result<br::WriteResult> r) {
  written_ = true;
  if (!r) {
    lastError_ = r.error().message;
    ctx_.setStatus("Bug report NOT written: " + lastError_);
    std::fprintf(stderr, "pqdaw: bug report not written: %s\n", lastError_.c_str());
    return;
  }
  lastWritten_ = utf8Of(r->dir);
  char b[64];
  std::snprintf(b, sizeof b, " (%.0f KB)", static_cast<double>(r->bytes) / 1024.0);
  ctx_.setStatus("Bug report written: " + lastWritten_ + b);
  std::printf("pqdaw: bug report written: %s%s\n", lastWritten_.c_str(), b);
}

}  // namespace pq::app
