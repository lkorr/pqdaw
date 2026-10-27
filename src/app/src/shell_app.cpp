#include "shell_app.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string_view>
#include <utility>

#include "pq/app/device_player.h"
#include "pq/app/startup.h"
#include "pq/compile/compile.h"
#include "pq/views/circuit/circuit_editor.h"
#include "pq/views/devices/device_view.h"
#include "pq/views/mixer/mixer.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/app/audio_manager.h"
#include "pq/app/user_settings.h"

namespace pq::app {

namespace {

// Codes of the shell's SDL user event (SDL_UserEvent::code).
enum : int32_t { kDialogResult = 1, kIdleStart = 2, kIdleEnd = 3, kAudioChanged = 4 };

// File dialogs run their callback on another thread on some platforms: results are posted to the UI thread as a
// user event carrying the path (owned by the event until delivered) and the request id.
struct Ticket {
  uint32_t eventType;
  uint32_t request;
};

const SDL_DialogFileFilter kProjectFilter[] = {{"pqdaw project (project.pqd)", "pqd"}};
const SDL_DialogFileFilter kScalaFilter[] = {{"Scala scale (*.scl)", "scl"}};
const SDL_DialogFileFilter kPatchFilter[] = {{"pqdaw patch (*.pqp)", "pqp"}};  // T4.2: View > Open Patch in Circuit View

void pushUser(uint32_t type, int32_t code, void* data1 = nullptr, void* data2 = nullptr) {
  SDL_Event ev{};
  ev.type = type;
  ev.user.code = code;
  ev.user.data1 = data1;
  ev.user.data2 = data2;
  ev.user.timestamp = SDL_GetTicksNS();
  if (!SDL_PushEvent(&ev) && code == kDialogResult) delete static_cast<std::string*>(data1);
}

Uint32 SDLCALL idleTimer(void* user, SDL_TimerID, Uint32) {
  const auto packed = reinterpret_cast<uintptr_t>(user);
  pushUser(static_cast<uint32_t>(packed >> 8), static_cast<int32_t>(packed & 0xFF));
  return 0;  // one shot
}

void startTimer(uint32_t eventType, int32_t code, double seconds) {
  const auto packed = (static_cast<uintptr_t>(eventType) << 8) | static_cast<uintptr_t>(code);
  SDL_AddTimer(static_cast<Uint32>(seconds * 1000.0 + 0.5), idleTimer, reinterpret_cast<void*>(packed));
}

}  // namespace

// --- SDL file dialogs -----------------------------------------------------------------------------------

class SdlFileDialogs final : public views::FileDialogs {
public:
  SdlFileDialogs(SDL_Window* window, uint32_t eventType) : window_(window), eventType_(eventType) {}

  void chooseProjectToOpen(PathCallback done) override {
    pending_ = std::move(done);
    SDL_ShowOpenFileDialog(&SdlFileDialogs::onResult, new Ticket{eventType_, ++request_}, window_, kProjectFilter, 1, nullptr,
                           false);
  }
  void chooseProjectToSave(std::string_view suggestedName, PathCallback done) override {
    pending_ = std::move(done);
    const std::string where(suggestedName == "Untitled" ? std::string_view{} : suggestedName);
    SDL_ShowSaveFileDialog(&SdlFileDialogs::onResult, new Ticket{eventType_, ++request_}, window_, kProjectFilter, 1,
                           where.empty() ? nullptr : where.c_str());
  }
  void chooseScalaFile(PathCallback done) override {
    pending_ = std::move(done);
    SDL_ShowOpenFileDialog(&SdlFileDialogs::onResult, new Ticket{eventType_, ++request_}, window_, kScalaFilter, 1, nullptr,
                           false);
  }
  void choosePatchFile(PathCallback done) override {
    pending_ = std::move(done);
    SDL_ShowOpenFileDialog(&SdlFileDialogs::onResult, new Ticket{eventType_, ++request_}, window_, kPatchFilter, 1, nullptr,
                           false);
  }
  views::SaveChoice askSaveChanges(std::string_view projectName) override {
    const std::string msg = "Save changes to \"" + std::string(projectName) + "\" before closing it?";
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Save"},
        {0, 1, "Don't Save"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING;
    box.window = window_;
    box.title = "pqdaw";
    box.message = msg.c_str();
    box.numbuttons = 3;
    box.buttons = buttons;
    int id = 2;
    if (!SDL_ShowMessageBox(&box, &id)) return views::SaveChoice::Cancel;
    return id == 0 ? views::SaveChoice::Save : id == 1 ? views::SaveChoice::Discard : views::SaveChoice::Cancel;
  }

  // UI thread: hands a result to the waiting callback (stale or cancelled requests are dropped).
  void deliver(const SDL_UserEvent& e) {
    std::unique_ptr<std::string> path(static_cast<std::string*>(e.data1));
    const auto request = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e.data2));
    if (request != request_ || !pending_) return;
    PathCallback cb = std::move(pending_);
    pending_ = nullptr;
    cb(path ? *path : std::string());
  }

private:
  static void SDLCALL onResult(void* user, const char* const* files, int /*filter*/) {
    std::unique_ptr<Ticket> t(static_cast<Ticket*>(user));
    auto* path = new std::string(files && files[0] ? files[0] : "");
    if (!files) std::fprintf(stderr, "pqdaw: file dialog failed: %s\n", SDL_GetError());
    pushUser(t->eventType, kDialogResult, path, reinterpret_cast<void*>(static_cast<uintptr_t>(t->request)));
  }

  SDL_Window* window_;
  uint32_t eventType_;
  uint32_t request_ = 0;
  PathCallback pending_;
};

// --- ShellApp -------------------------------------------------------------------------------------------

std::string firstFrameReport() {
  char buf[160];
  std::snprintf(buf, sizeof buf, "first frame %.1f ms after main() (%.1f ms after process creation)", msSinceMainEntry(),
                msSinceProcessCreation());
  return buf;
}

ShellApp::ShellApp(ui::UiTree& tree, SDL_Window* window) : tree_(tree), window_(window) {}

Result<std::unique_ptr<ShellApp>> ShellApp::create(ui::UiTree& tree, SDL_Window* window, const CliOptions& o) {
  std::unique_ptr<ShellApp> app(new ShellApp(tree, window));
  app->userEvent_ = SDL_RegisterEvents(1);
  if (app->userEvent_ == 0) return Error{ErrorCode::Device, std::string("SDL_RegisterEvents: ") + SDL_GetError(), {}};
  app->transport_ = std::make_unique<SimulatedTransport>([] { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; });
  // T3.18: the audio settings (user settings file + this run's device flags) decide the engine's sample rate.
  std::vector<std::string> settingsWarnings;
  const views::AudioSettings audio = AudioDeviceManager::startupSettings(o, userSettingsPath(), &settingsWarnings);
  for (const std::string& w : settingsWarnings) std::fprintf(stderr, "pqdaw: settings: %s (default used)\n", w.c_str());
  {
    // T3.17: playback. The link compiles a default project first (the context's document follows at attach()).
    doc::IdSource ids;
    const double t0 = msSinceMainEntry();
    LinkConfig lc;
    lc.sampleRate = audio.sampleRate;
    // T3.17-fix3 (M3 review M16): chance rolls differently every session (and every playback, every loop repeat).
    lc.chanceSeed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                    reinterpret_cast<uintptr_t>(app.get());
    auto link = ProjectLink::create(lc, doc::makeDefaultProject(ids));
    if (!link && audio.sampleRate != 48000) {  // a rate the engine cannot take: start at 48 kHz
      lc.sampleRate = 48000;
      link = ProjectLink::create(lc, doc::makeDefaultProject(ids));
    }
    if (link) {
      app->link_ = std::move(*link);
      markStartupPhase("playback engine (default project compiled)");
      std::printf("pqdaw: playback engine ready in %.1f ms\n", msSinceMainEntry() - t0);
    } else
      std::fprintf(stderr, "pqdaw: playback unavailable: %s\n", link.error().message.c_str());
  }
  if (app->link_)
    app->ctx_ = std::make_unique<views::ViewContext>(*app->link_);
  else
    app->ctx_ = std::make_unique<views::ViewContext>(*app->transport_);
  // T4.15: view preferences (the circuit editor's Inactive toggle), saved whenever they change.
  app->ctx_->setViewPrefs(loadViewPrefs(userSettingsPath()));
  app->ctx_->setViewPrefsListener([](const views::ViewPrefs& p) {
    if (Status s = saveViewPrefs(userSettingsPath(), p); !s.ok()) std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
  });
  app->dialogs_ = std::make_unique<SdlFileDialogs>(window, app->userEvent_);
  app->ctx_->setFileDialogs(app->dialogs_.get());
  if (!o.project.empty()) {
    if (Status s = app->ctx_->openProject(o.project); !s) return std::move(s).error();
    markStartupPhase("project loaded");
  }
  auto shell = views::Shell::create(tree, *app->ctx_);
  if (!shell) return std::move(shell).error();
  app->shell_ = std::move(*shell);
  markStartupPhase("views built");
  // T4.6: View > Open Device… lists the stock devices (the player is attached below once the playback link exists).
  views::devices::setDeviceLibrary(*app->shell_, stdlibDir() + "/devices", nullptr);
  if (app->link_) {
    const double t1 = msSinceMainEntry();
    app->link_->attach(*app->ctx_);
    app->ctx_->setDeviceLibrary(&app->link_->devices());  // T4.1: the device view's catalog (one load per process)
    app->ctx_->setModulationSource(app->link_.get());  // T4.7: the modulated knobs' live rings
    // T4.4-fix1: the circuit editor on a project device probes and measures it in the project's own unit.
    ProjectLink* link = app->link_.get();
    app->ctx_->setDevicePlayers([link](doc::DeviceId d) -> std::unique_ptr<views::circuit::PatchPlayer> {
      return std::make_unique<DevicePlayer>(*link, d);
    });
    if (!o.project.empty()) std::printf("pqdaw: project sequences and params to the engine in %.1f ms (units compile on the worker)\n", msSinceMainEntry() - t1);
    if (views::MixerView* mixer = views::findMixer(*app->shell_)) mixer->setMeterSource(app->link_.get());
    if (auto* patch = views::findProjectPatch(*app->shell_)) {  // T4.13: wire readouts and the CPU cost labels
      patch->setMeterSource(app->link_.get());
      patch->setCostSource(app->link_.get());
    }
    app->audio_ = std::make_unique<AudioDeviceManager>(*app->link_, audio, ManagerHooks{}, o.device);
    // The output notifies from its threads: post to the UI thread, which polls the manager (follows a device rate
    // change, refreshes the preferences window).
    app->audio_->setUiNotify([](void* self) { pushUser(static_cast<ShellApp*>(self)->userEvent_, kAudioChanged); }, app.get());
    app->ctx_->setAudioPrefs(app->audio_.get());
    // T4.4: the circuit editor's patch preview plays through the same device, attached only while it plays.
    PreviewConfig pc;
    pc.sampleRate = app->link_->engine().config().sampleRate;
    pc.jit = compile::jitUnitBackend(true);  // Q111 C: a voice-group image first, the pair image after it
    app->preview_ = std::make_unique<PatchPreview>(pc);
    app->previewCallback_ = app->preview_->callback();
    ShellApp* raw = app.get();
    app->preview_->onActiveChanged = [raw](bool on) {
      if (raw->audio_) raw->audio_->output().setExtraSource(on ? &raw->previewCallback_ : nullptr);
    };
    views::circuit::setPatchPlayer(*app->shell_, app->preview_.get());
    views::devices::setDeviceLibrary(*app->shell_, stdlibDir() + "/devices", app->preview_.get());  // T4.6
    markStartupPhase("link attached, audio device opening in the background");
  }
  ShellApp* self = app.get();
  app->tempoSub_ = app->ctx_->subscribe([self](const doc::StoreEvent&) { self->onTempoMaybeChanged(); });
  app->onTempoMaybeChanged();
  app->titleSub_ = app->ctx_->observe([self](uint32_t what) {
    if (what & views::view_change::kProject) self->updateTitle();
  });
  app->updateTitle();
  app->idleSeconds_ = o.idleCheckSeconds;
  return app;
}

ShellApp::~ShellApp() {
  // The shell's widgets subscribe to the context: destroy them first.
  tree_.setRoot(std::make_unique<ui::Widget>());
  shell_.reset();
  if (ctx_) ctx_->setAudioPrefs(nullptr);
  if (ctx_) ctx_->setDeviceLibrary(nullptr);  // the link owns it
  if (ctx_) ctx_->setModulationSource(nullptr);
  if (ctx_) ctx_->setDevicePlayers(nullptr);
  if (preview_) preview_->onActiveChanged = nullptr;
  audio_.reset();  // the device stops calling the engine (and the preview)
  if (link_) link_->detach();
}

// T4.4: the project engine was rebuilt at another rate: the preview follows (it stops; the next edit or Play restarts it).
void ShellApp::syncPreviewRate() {
  if (!preview_ || !link_ || !audio_) return;
  const double rate = link_->engine().config().sampleRate;
  if (rate == preview_->sampleRate()) return;
  audio_->output().setExtraSource(nullptr);
  preview_->setSampleRate(rate);
}

void ShellApp::onTempoMaybeChanged() {
  if (link_) return;  // the engine follows the tempo map itself
  const auto& tl = ctx_->doc()->timeline;
  if (tl.get() == tempoTimeline_) return;
  tempoTimeline_ = tl.get();
  transport_->setTempoMap(tl->tempoMap);
}

void ShellApp::updateTitle() {
  const std::string title = "pqdaw \xE2\x80\x94 " + ctx_->projectName() + (ctx_->dirty() ? " *" : "");
  SDL_SetWindowTitle(window_, title.c_str());
}

// SDL delivers TEXT_INPUT only between SDL_StartTextInput / SDL_StopTextInput (and shows the IME then): on while a
// text-entry widget (a text field, a number box being typed into) has focus.
void ShellApp::syncTextInput() {
  const ui::Widget* f = tree_.focus();
  const bool want = f && f->acceptsTextInput();
  if (want == textInput_) return;
  textInput_ = want;
  if (want)
    SDL_StartTextInput(window_);
  else
    SDL_StopTextInput(window_);
}

bool ShellApp::handleEvent(const SDL_Event& e, bool /*consumedByTree*/) {
  ++events_;
  if (e.type == userEvent_) {
    switch (e.user.code) {
      case kDialogResult: dialogs_->deliver(e.user); break;
      case kAudioChanged:
        if (audio_) audio_->poll();
        syncPreviewRate();
        break;
      case kIdleStart:
        // Settle: on a loaded machine the show/expose/focus events of a new window can arrive well after the first
        // frame. Measure only once a whole second passed without a frame or an event (other than this timer's),
        // checked for up to 30 s.
        if ((frames_ != settleFrames_ || events_ != settleEvents_ + 1) && ++settleTries_ < 30) {
          settleFrames_ = frames_;
          settleEvents_ = events_;
          startTimer(userEvent_, kIdleStart, 1.0);
          break;
        }
        threadsAtStart_ = threadCycles();  // first: the snapshot itself costs UI-thread time
        framesAtStart_ = frames_;
        eventsAtStart_ = events_;
        cpuAtStart_ = processCpuMs();
        cyclesAtStart_ = processCycles();
        tscAtStart_ = tscNow();
        uiCyclesAtStart_ = currentThreadCycles();
        wallAtStart_ = static_cast<double>(SDL_GetTicksNS()) * 1e-6;
        startTimer(userEvent_, kIdleEnd, idleSeconds_);
        break;
      case kIdleEnd: {
        const uint32_t frames = frames_ - framesAtStart_;
        const double coarseCpu = processCpuMs() - cpuAtStart_;
        const uint64_t cycles = processCycles() - cyclesAtStart_;
        const uint64_t tsc = tscNow() - tscAtStart_;
        const uint64_t uiCycles = currentThreadCycles() - uiCyclesAtStart_;
        const double wall = static_cast<double>(SDL_GetTicksNS()) * 1e-6 - wallAtStart_;
        // QueryProcessCycleTime counts TSC ticks: CPU ms = cycles × (wall ms / TSC ticks) over this same interval.
        auto ms = [&](uint64_t c) { return tsc ? static_cast<double>(c) * wall / static_cast<double>(tsc) : 0.0; };
        // Attribution (the snapshot comes after the UI thread was read: it costs UI-thread time itself). Threads that
        // started in another module than ours (the executable, ntdll/kernel thread pools) belong to that module,
        // in practice the GPU driver's Vulkan ICD: its idle housekeeping is the same for any Vulkan window.
        std::vector<ThreadCycles> threads = threadCycles();
        for (ThreadCycles& t : threads)
          for (const ThreadCycles& s0 : threadsAtStart_)
            if (s0.threadId == t.threadId) t.cycles -= std::min(t.cycles, s0.cycles);
        const uint32_t self = static_cast<uint32_t>(SDL_GetCurrentThreadID());
        std::erase_if(threads, [self](const ThreadCycles& t) { return t.threadId == self || t.cycles == 0; });
        std::sort(threads.begin(), threads.end(), [](const ThreadCycles& x, const ThreadCycles& y) { return x.cycles > y.cycles; });
        std::wstring exe = processModuleName();
        auto ours = [&](const ThreadCycles& t) {
          return t.module.empty() || _wcsicmp(t.module.c_str(), exe.c_str()) == 0 || _wcsicmp(t.module.c_str(), L"ntdll.dll") == 0 ||
                 _wcsicmp(t.module.c_str(), L"kernel32.dll") == 0 || _wcsicmp(t.module.c_str(), L"KernelBase.dll") == 0 ||
                 _wcsicmp(t.module.c_str(), L"ucrtbase.dll") == 0;
        };
        uint64_t ownCycles = uiCycles, foreignCycles = 0;
        for (const ThreadCycles& t : threads) (ours(t) ? ownCycles : foreignCycles) += t.cycles;
        const double cpu = tsc ? ms(cycles) : coarseCpu;
        const double pct = 100.0 * cpu / wall, ownPct = 100.0 * ms(ownCycles) / wall;
        const bool ok = frames == 0 && ownPct < 0.1;
        std::printf("idle check: %u frames drawn in %.2f s; pqdaw's threads %.3f ms CPU = %.4f %% of one core (UI thread "
                    "%.3f ms); whole process %.3f ms = %.4f %% (GetProcessTimes: %.1f ms) -> %s\n",
                    frames, wall * 1e-3, ms(ownCycles), ownPct, ms(uiCycles), cpu, pct, coarseCpu,
                    ok ? "ok" : "FAILED (want 0 frames and < 0.1 % for pqdaw's threads)");
        for (size_t i = 0; i < threads.size() && i < 6; ++i)
          std::printf("  thread %u [%ls]%s%ls: %.3f ms\n", threads[i].threadId, threads[i].module.c_str(),
                      threads[i].description.empty() ? "" : " ", threads[i].description.c_str(), ms(threads[i].cycles));
        const uint64_t outsideEvents = events_ - eventsAtStart_ - 1;  // minus this timer's
        std::printf("  %llu SDL events reached the shell during the measurement\n",
                    static_cast<unsigned long long>(outsideEvents));
        std::fflush(stdout);
        // An event means something outside pqdaw acted on the window (focus, occlusion, input): what followed was a
        // response, not idling. Measure again (up to 3 windows); the last window is judged whatever happened in it.
        if (outsideEvents != 0 && ++idleAttempts_ < 3) {
          std::printf("  (window disturbed by outside events: measuring again)\n");
          std::fflush(stdout);
          settleFrames_ = frames_;
          settleEvents_ = events_;
          settleTries_ = 0;
          startTimer(userEvent_, kIdleStart, 1.0);
          break;
        }
        idleVerdict_ = ok ? 0 : 1;
        pushUser(SDL_EVENT_QUIT, 0);
        break;
      }
      default: break;
    }
    return true;
  }
  syncTextInput();
  switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
      if (idleVerdict_ >= 0) return false;  // the idle check's own quit
      // Closing the last window queues both events for one click: answer them once.
      if (e.common.timestamp <= quitAskedAt_) return !quitAnswer_;
      quitAnswer_ = ctx_->confirmQuit([this] { pushUser(SDL_EVENT_QUIT, 0); });
      quitAskedAt_ = SDL_GetTicksNS();
      return !quitAnswer_;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (shell_) shell_->cancelInteraction();
      return false;
    case SDL_EVENT_KEY_DOWN:
      return true;  // the shell's keys are all in its keymap; the test scenes' window keys (Esc quits, A animates) are off
    default: return false;
  }
}

void ShellApp::onFramePresented() {
  ++frames_;
  if (frames_ != 1) return;
  std::printf("pqdaw: %s\n", firstFrameReport().c_str());
  if (startupLogEnabled()) std::printf("%s", startupPhaseReport().c_str());
  std::fflush(stdout);
  // T0.7-fix6: tools/bench/startup.ps1 times repeated starts; PQ_STARTUP_EXIT=1 quits after the first frame.
  if (const char* e = std::getenv("PQ_STARTUP_EXIT"); e && *e && *e != '0') {
    SDL_Event q{};
    q.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&q);
  }
  if (idleSeconds_ > 0) {  // settle (show/expose) before measuring
    settleFrames_ = frames_;
    settleEvents_ = events_;
    startTimer(userEvent_, kIdleStart, 1.0);
  }
}

}  // namespace pq::app
