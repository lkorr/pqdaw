#include "pq/app/commands.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // QueryThreadCycleTime (self-test CPU measurement)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pq/ui/damage.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/frame_pacer.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
#include "pq/ui/widget.h"
#include "pq/ui/window.h"
#include "pq/views/view_scenes.h"
#include "pq/app/pipeline_cache.h"
#include "pq/app/startup.h"
#include "file_watch.h"
#include "shell_app.h"

namespace pq::app {

using namespace pq::ui;

int runScreenshot(const CliOptions& o) {
  uint32_t w = o.width, h = o.height;
  const bool view = views::isViewScene(o.scene);  // T3.7: the DAW's views over --project
  if (!w || !h) {
    if (view) views::viewSceneDefaultSize(o.scene, w, h);
    else sceneDefaultSize(o.scene, w, h);
  }
  const UiScale scale(o.scale > 0.0f ? o.scale : 1.0f);
  auto img = view ? views::renderViewScene(o.scene, o.project, w, h, scale) : renderSceneImage(o.scene, w, h, {}, scale);
  if (!img) {
    std::fprintf(stderr, "pqdaw: screenshot failed: %s\n", img.error().message.c_str());
    return 1;
  }
  if (Status s = writePng(o.outPath, *img); !s) {
    std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
    return 1;
  }
  std::printf("wrote %s (%ux%u px = %ux%u logical at scale %g, scene %s)\n", o.outPath.c_str(), img->width, img->height, w, h,
              static_cast<double>(scale.factor), o.scene.c_str());
  return 0;
}

namespace {

using Clock = std::chrono::steady_clock;

// CPU cycles this thread has run (excludes time blocked in waits); 0 where unavailable.
uint64_t threadCycles() noexcept {
#ifdef _WIN32
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return c;
#else
  return 0;
#endif
}

// Everything the frame loop needs. Lives on the UI thread only.
struct WindowApp {
  std::unique_ptr<Window> window;
  std::unique_ptr<GpuContext> gpu;
  std::unique_ptr<Swapchain> swapchain;
  std::unique_ptr<Renderer> renderer;
  SceneTextures textures;
  DrawList dl;
  Damage damage;
  FramePacer pacer;
  std::string scene;
  Clock::time_point start = Clock::now();
  bool inFrame = false;  // re-entrancy guard: live-resize expose events draw from inside SDL's modal loop
  bool quit = false;
  uint32_t framesPresented = 0;
  uint32_t framesSkipped = 0;
  std::string error;
  // T0.7-fix6: the Vulkan pipeline cache file and what it held at startup (saved after the first frame if the
  // driver's cache differs).
  std::string pipelineCacheFile;
  std::vector<uint8_t> pipelineCacheAtStart;
  // Widget scenes (widgets-core, widgets) run a live UiTree: SDL input → translateEvent → the tree; its
  // damage and timers drive frames; layout is in logical px × the display scale.
  std::unique_ptr<UiTree> ui;
  PaintResources res;  // what the tree paints and measures with (points into `textures`)
  SdlClipboard clipboard;
  // Dev builds: theme hot reload (T3.2). The loop wakes every 0.5 s for one file-time query; nothing redraws
  // unless the file changed.
  std::unique_ptr<ThemeFileWatcher> themeWatch;
  // Change notifications for the theme's directory (an SDL user event from a blocked helper thread): the theme is
  // re-checked only when the directory changed, so an idle window does no periodic work in dev builds either.
  std::unique_ptr<FileChangeNotifier> themeNotify;
  bool themeCheckDue = true;  // check once at startup, then on each notification
  // The UI scale (pq/ui/ui_scale.h): the one value the tree's layout, the scene fonts and the scenes read.
  // Read from the window's display at startup and on SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED; scaleOverride
  // (> 0: --scale, or the self-test's injected change) replaces the display's value.
  UiScale scale{};
  float scaleOverride = 0.0f;
  UiScale lastFrameScale{};  // the scale the last presented frame was drawn at (self-test)
  uint32_t scaleChanges = 0;
  // Self-test hook: lines of text drawn over the scene (the glyph-upload hitch check rasterises them mid-run).
  FontId extraFont = kNoFont;
  std::vector<std::string> extraLines;
  // T3.7: the DAW shell (view scenes) runs in `ui`; declared last so it is destroyed before the tree.
  std::unique_ptr<ShellApp> shell;

  // T0.7-fix1 partial redraw. A frame redraws only the damage rects of the swapchain's persistent image when
  // the scene can say what changed: widget trees (painted with the damage as clip) and layered scenes (a
  // retained static layer + a transient overlay). Other scenes, resizes and whole-target animation draw in full.
  bool layered = false;            // sceneIsLayered(scene)
  DrawList staticLayer;            // layered scenes: the static part (retained layer 0 on the GPU)
  uint64_t staticVersion = 0;      // bumped on every rebuild
  uint32_t staticW = 0, staticH = 0;
  UiScale staticScale{};
  bool overlayAnimating = false;   // layered scenes: the overlay moves (A toggles, --animate starts it)
  ClipRect lastOverlay{};          // the overlay's pixels in the last presented frame
  bool forceFull = false;          // self-test A/B: every frame a full rebuild + redraw (the pre-T0.7-fix1 path)
  uint32_t partialFrames = 0, fullFrames = 0;
  double lastPartialArea = 0;      // damaged fraction of the target in the last partial frame
  struct FrameTiming {
    double beginMs = 0;  // beginFrame: slot fence wait + acquire
    double buildMs = 0;  // damage, draw-list build
    double endMs = 0;    // endFrame: upload, record, submit, present
  } lastTiming;
  // A frame larger than this fraction of the target is drawn in full (no per-rect clears / scissor passes).
  static constexpr int64_t kPartialMaxPercent = 50;

  bool frameDue() const noexcept { return damage.needsFrame() || overlayAnimating; }
  // The tree's damage → the frame's damage (both kept as disjoint whole-pixel rects).
  void takeUiDamage() {
    if (!ui || !ui->needsPaint()) return;
    if (ui->damage().full()) {
      damage.addAll();
      return;
    }
    for (const ClipRect& r : ui->damage().rects()) damage.addPixels(r);
  }

  // Re-reads the scale; on a change re-resolves the font roles (new sizes are created here, on the event,
  // never inside paint), relayouts the tree and requests a full redraw. Returns false on a hard error.
  bool refreshScale() {
    const UiScale s(scaleOverride > 0.0f ? scaleOverride : window->displayScale());
    if (s == scale) return true;
    if (Status st = setSceneScale(textures, s); !st) {
      error = st.error().message;
      return false;
    }
    scale = s;
    ++scaleChanges;
    if (ui) {
      uint32_t w = 0, h = 0;
      window->pixelSize(w, h);
      ui->setViewport(w ? w : ui->width(), h ? h : ui->height(), scale);
    }
    damage.addAll();
    return true;
  }

  double now() const { return std::chrono::duration<double>(Clock::now() - start).count(); }

  // Runs the tree's timers/frame requests/layout and turns its damage into a frame request.
  void updateUi() {
    if (!ui) return;
    uint32_t w = 0, h = 0;
    window->pixelSize(w, h);
    if (w && h) ui->setViewport(w, h, scale);
    ui->update(now());
    takeUiDamage();
  }

  // Draws and presents one frame if the window has a drawable surface. Returns false on a hard error.
  bool drawFrame() {
    if (inFrame) return true;
    inFrame = true;
    VkExtent2D ext{};
    const auto f0 = Clock::now();
    auto st = swapchain->beginFrame(ext);
    const auto f1 = Clock::now();
    bool ok = true;
    if (!st) {
      error = st.error().message;
      ok = false;
    } else if (*st == Swapchain::FrameStatus::Skipped) {
      ++framesSkipped;
    } else {
      if (textures.text) textures.text->beginFrame();  // frame boundary: run cache trim, atlas LRU stamp
      const uint32_t W = ext.width, H = ext.height;
      const double t = now();
      // --- what changed: the tree's layout/damage, the layered scene's static layer and overlay ---
      if (ui) {
        ui->setViewport(W, H, scale);
        ui->layoutNow();
        takeUiDamage();
      }
      const double overlayT = overlayAnimating ? t : 0.0;
      ClipRect overlay{};
      if (layered) {
        if (!forceFull && (staticW != W || staticH != H || !(staticScale == textures.scale))) {
          staticLayer.reset(W, H);
          buildSceneStatic(scene, staticLayer, textures);
          staticW = W;
          staticH = H;
          staticScale = textures.scale;
          ++staticVersion;
          damage.addAll();
        }
        overlay = playheadRect(W, H, overlayT);
        if (!(overlay == lastOverlay)) {
          damage.addPixels(lastOverlay);
          damage.addPixels(overlay);
        }
      }
      // --- full or partial ---
      ClipRect rects[Damage::kMaxRects];
      uint32_t nRects = 0;
      int64_t area = 0;
      ClipRect box{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
      for (const ClipRect& r : damage.rects()) {
        const ClipRect c{std::max(r.x0, 0), std::max(r.y0, 0), std::min(r.x1, static_cast<int32_t>(W)), std::min(r.y1, static_cast<int32_t>(H))};
        if (c.x1 <= c.x0 || c.y1 <= c.y0) continue;
        rects[nRects++] = c;
        area += Damage::area(c);
        box = Damage::unite(box, c);
      }
      const bool full = forceFull || damage.full() || !swapchain->preserved() || (!ui && !layered) || extraFont != kNoFont ||
                        area * 100 > int64_t(W) * H * kPartialMaxPercent;
      // --- build ---
      DrawLayer layers[2];
      uint32_t nLayers = 0;
      dl.reset(W, H);
      if (ui) {
        res = paintResources(textures);
        if (full) {
          dl.addRectFilled({0, 0, static_cast<float>(W), static_cast<float>(H)}, kSceneClearColor);
          ui->paint(dl, res, t);
        } else {
          // The background is the damage rects' clear; only widgets touching the damage are painted.
          ui->paint(dl, res, t, nRects ? IRect{box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0} : IRect{});
        }
      } else if (layered && !forceFull) {
        layers[nLayers++] = {&staticLayer, 0, staticVersion};
        buildSceneOverlay(scene, dl, textures, overlayT);
      } else {
        buildScene(scene, dl, textures, layered ? overlayT : damage.animating() ? t : 0.0);
      }
      if (textures.text && extraFont != kNoFont) {
        const float lh = static_cast<float>(textures.text->metrics(extraFont).lineHeight);
        for (size_t i = 0; i < extraLines.size(); ++i)
          textures.text->draw(dl, extraFont, {8.0f, lh * static_cast<float>(i + 1)}, extraLines[i], rgba(255, 255, 255));
      }
      layers[nLayers++] = {&dl, kTransientLayer, 0};
      FrameDesc frame;
      frame.layers = std::span<const DrawLayer>(layers, nLayers);
      frame.full = full;
      frame.damage = std::span<const ClipRect>(rects, full ? 0 : nRects);
      frame.clear = kSceneClearColor;
      const auto f2 = Clock::now();
      if (framesPresented == 0) markStartupPhase("first frame: layout + paint");
      Status ended = swapchain->endFrame(*renderer, frame);
      using Ms = std::chrono::duration<double, std::milli>;
      lastTiming = {Ms(f1 - f0).count(), Ms(f2 - f1).count(), Ms(Clock::now() - f2).count()};
      if (Status s = std::move(ended); !s) {
        error = s.error().message;
        ok = false;
      } else {
        if (framesPresented == 0) markStartupPhase("first frame presented");
        ++framesPresented;
        if (framesPresented == 1) savePipelineCacheIfChanged();
        ++(full ? fullFrames : partialFrames);
        if (!full) lastPartialArea = static_cast<double>(area) / (static_cast<double>(W) * H);
        if (shell) shell->onFramePresented();
        lastFrameScale = textures.scale;
        lastOverlay = overlay;
      }
    }
    damage.clear();
    inFrame = false;
    return ok;
  }

  // After the first frame (off the startup path): keep the driver's pipeline cache for the next start, unless it is
  // what was loaded.
  void savePipelineCacheIfChanged() {
    if (pipelineCacheFile.empty() || !renderer) return;
    const std::vector<uint8_t> now = renderer->pipelineCacheData();
    if (now.empty() || now == pipelineCacheAtStart) return;
    if (!savePipelineCache(pipelineCacheFile, now))
      std::fprintf(stderr, "pqdaw: could not save the pipeline cache to %s\n", pipelineCacheFile.c_str());
    pipelineCacheAtStart.clear();
  }

  // Dev theme hot reload: re-resolves the theme's fonts, relayouts and redraws when the file changed.
  void pollTheme() {
    if (!themeWatch || !themeCheckDue) return;
    themeCheckDue = false;
    Theme t;
    std::string err;
    switch (themeWatch->poll(themeWatch->nextCheck(), t, err)) {  // always due: a notification asked for it
      case ThemeFileWatcher::Poll::Reloaded:
        if (Status s = setSceneTheme(textures, std::make_shared<const Theme>(std::move(t))); !s) {
          std::fprintf(stderr, "pqdaw: theme reload failed: %s\n", s.error().message.c_str());
          break;
        }
        res = paintResources(textures);
        if (ui) ui->invalidateAll();
        damage.addAll();
        std::printf("pqdaw: theme reloaded from %s\n", themeWatch->path().c_str());
        break;
      case ThemeFileWatcher::Poll::Failed: std::fprintf(stderr, "pqdaw: theme not reloaded: %s\n", err.c_str()); break;
      default: break;
    }
  }

  void handle(const SDL_Event& e) {
    if (themeNotify && e.type == themeNotify->eventType()) {
      themeNotify->acknowledge();
      themeCheckDue = true;
      return;
    }
    bool consumed = false;
    if (ui) {
      InputEvent in;
      if (translateEvent(e, window->pixelDensity(), in)) consumed = ui->dispatch(in);
    }
    if (shell && shell->handleEvent(e, consumed)) return;
    if (consumed && e.type == SDL_EVENT_KEY_DOWN) return;  // e.g. Esc closed a menu, 'a' typed into a field
    switch (e.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED: quit = true; break;
      case SDL_EVENT_WINDOW_RESIZED:
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        swapchain->invalidate();
        damage.addAll();
        break;
      case SDL_EVENT_WINDOW_EXPOSED:
      case SDL_EVENT_WINDOW_RESTORED:
      case SDL_EVENT_WINDOW_MAXIMIZED:
      case SDL_EVENT_WINDOW_SHOWN: damage.addAll(); break;
      case SDL_EVENT_WINDOW_DISPLAY_CHANGED:  // moved to another monitor
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        if (!refreshScale()) std::fprintf(stderr, "pqdaw: UI scale change failed: %s\n", error.c_str());
        damage.addAll();
        break;
      case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) quit = true;
        if (e.key.key == SDLK_A) {
          if (layered) {
            overlayAnimating = !overlayAnimating;  // the playhead's columns are the only damage
          } else {
            damage.setAnimating(!damage.animating());
            damage.addAll();
          }
        }
        break;
      default: break;
    }
  }

  // Win32 runs a modal loop while the user drags the window border, so the main loop's SDL_WaitEvent does
  // not return until the drag ends. SDL dispatches EXPOSED (data1 = 1 for live resize) to event watchers
  // from inside that loop; drawing there keeps the content live during the drag.
  static bool SDLCALL liveResizeWatch(void* user, SDL_Event* e) {
    auto* app = static_cast<WindowApp*>(user);
    if (e->type == SDL_EVENT_WINDOW_EXPOSED && e->window.data1 == 1 && !app->inFrame) {
      app->swapchain->invalidate();
      app->drawFrame();
    }
    return true;
  }
};

Result<std::unique_ptr<WindowApp>> openWindowApp(const CliOptions& o, bool hidden) {
  auto app = std::make_unique<WindowApp>();
  app->scene = o.scene;
  if (!hasScene(o.scene) && !views::isViewScene(o.scene)) return Error{ErrorCode::NotFound, "unknown scene '" + o.scene + "'", {}};
  WindowOptions wo;
  wo.title = "pqdaw";
  if (o.width) {
    wo.width = o.width;
    wo.height = o.height;
  }
  wo.hidden = hidden;

  // T0.7-fix6: startup in parallel. The Vulkan instance + device (loading the driver) are created on a worker while
  // SDL starts video and creates the window on this thread; the fonts and the theme load on a second worker once the
  // window's display scale is known, while the swapchain and the renderer are made here. Both are joined before
  // anything uses them (the joiners also run on every error return).
  struct Joiner {
    std::thread t;
    ~Joiner() { join(); }
    void join() {
      if (t.joinable()) t.join();
    }
  };
  GpuOptions go;
  go.instanceExtensions = Window::platformInstanceExtensions();
  go.swapchain = true;
  std::optional<Result<std::unique_ptr<GpuContext>>> gpuEarly;
  Joiner gpuThread;
  // PQ_STARTUP_SERIAL=1: the old order (device after the window, fonts after the renderer), for A/B measurement.
  const char* serialEnv = std::getenv("PQ_STARTUP_SERIAL");
  const bool serial = serialEnv && *serialEnv && *serialEnv != '0';
  if (serial) go.instanceExtensions.clear();
  if (!go.instanceExtensions.empty())
    gpuThread.t = std::thread([&] {
      gpuEarly.emplace(GpuContext::create(go));
      markStartupPhase("Vulkan instance + device");
    });
  // SDL video first on its own, only to log the two parts (Window::create's own init is then a reference count).
  const bool videoUp = SDL_InitSubSystem(SDL_INIT_VIDEO);
  if (videoUp) markStartupPhase("SDL video init");
  auto window = Window::create(wo);
  if (videoUp) SDL_QuitSubSystem(SDL_INIT_VIDEO);  // the window holds its own reference
  if (!window) return std::move(window).error();
  app->window = std::move(*window);
  markStartupPhase("window created");
  app->scaleOverride = o.scale;
  app->scale = UiScale(o.scale > 0.0f ? o.scale : app->window->displayScale());
  auto atlas = std::make_shared<Atlas>();
  std::optional<Result<SceneTextures>> text;
  Joiner fontThread;
  auto loadFonts = [&] {
    text.emplace(createSceneText(atlas, app->scale));
    markStartupPhase("fonts + theme");
  };
  if (!serial) fontThread.t = std::thread(loadFonts);

  gpuThread.join();
  {
    // The instance needs every extension the window's surface needs (the early list is SDL's for this platform).
    const std::vector<std::string> need = app->window->requiredInstanceExtensions();
    const bool covered = gpuEarly && std::all_of(need.begin(), need.end(), [&](const std::string& e) {
      return std::find(go.instanceExtensions.begin(), go.instanceExtensions.end(), e) != go.instanceExtensions.end();
    });
    if (!covered) {
      gpuEarly.reset();
      go.instanceExtensions = need;
      gpuEarly.emplace(GpuContext::create(go));
      markStartupPhase("Vulkan instance + device (window's extension list)");
    }
  }
  if (!*gpuEarly) return std::move(*gpuEarly).error();
  app->gpu = std::move(**gpuEarly);
  {
    const GpuContext::CreateTimes& ct = app->gpu->createTimes();
    char note[160];
    std::snprintf(note, sizeof note, "Vulkan: vkCreateInstance %.1f ms, choose device %.1f ms, vkCreateDevice %.1f ms, VMA + pool %.1f ms",
                  ct.instance, ct.choose, ct.device, ct.rest);
    addStartupNote(note);
  }
  auto sc = Swapchain::create(*app->gpu, *app->window, 2);
  if (!sc) return std::move(sc).error();
  app->swapchain = std::move(*sc);
  markStartupPhase("swapchain");
  app->pipelineCacheFile = pipelineCachePath();
  app->pipelineCacheAtStart = loadPipelineCache(app->pipelineCacheFile);
  RendererOptions ro;
  ro.pipelineCache = app->pipelineCacheAtStart;
  ro.atlas = atlas;
  auto renderer = Renderer::create(*app->gpu, app->swapchain->format(), 2, ro);
  if (!renderer) return std::move(renderer).error();
  app->renderer = std::move(*renderer);
  markStartupPhase(app->renderer->pipelineCacheReused() ? "renderer (pipelines from the disk cache)" : "renderer (pipelines compiled)");
  fontThread.join();
  if (serial) loadFonts();
  if (!*text) return std::move(*text).error();
  auto tex = createSceneTextures(*app->renderer, std::move(**text));
  if (!tex) return std::move(tex).error();
  app->textures = *tex;
  markStartupPhase("scene textures");
  app->layered = sceneIsLayered(o.scene);
  app->overlayAnimating = app->layered && o.animate;
  app->damage.setAnimating(!app->layered && o.animate);
  app->damage.addAll();
  app->res = paintResources(app->textures);
  if (isWidgetScene(o.scene)) {
    app->ui = std::make_unique<UiTree>();
    uint32_t w = 0, h = 0;
    app->window->pixelSize(w, h);
    app->ui->setViewport(w, h, app->scale);
    app->ui->setResources(&app->res);
    app->ui->setClipboard(&app->clipboard);
    buildWidgetScene(o.scene, *app->ui);
    SDL_StartTextInput(app->window->sdl());  // text fields get TEXT_INPUT events (IME handling: T3.7)
  } else if (views::isViewScene(o.scene)) {  // T3.7: the DAW shell over --project (text input follows focus)
    app->ui = std::make_unique<UiTree>();
    uint32_t w = 0, h = 0;
    app->window->pixelSize(w, h);
    app->ui->setViewport(w, h, app->scale);
    app->ui->setResources(&app->res);
    app->ui->setClipboard(&app->clipboard);
    auto shell = ShellApp::create(*app->ui, app->window->sdl(), o);
    if (!shell) return std::move(shell).error();
    app->shell = std::move(*shell);
    markStartupPhase("shell app ready");
  }
#ifndef NDEBUG
  if (!hidden) {
    app->themeWatch = std::make_unique<ThemeFileWatcher>(defaultThemePath());
    app->themeNotify = FileChangeNotifier::create(defaultThemePath(), SDL_RegisterEvents(1));
    if (!app->themeNotify) std::fprintf(stderr, "pqdaw: no change notifications for the theme file; hot reload off\n");
  }
#endif
  return app;
}

// Drains pending events into the app (non-blocking).
void pump(WindowApp& app) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) app.handle(e);
}

int interactive(WindowApp& app) {
  SDL_AddEventWatch(&WindowApp::liveResizeWatch, &app);
  while (!app.quit) {
    // Idle = blocked in the OS wait: no frames, no timers, ≈0% CPU. A minimised window never draws, so
    // pending damage or animation must not turn this into a spin either.
    const bool uiBusy = app.ui && app.ui->needsFrame();  // widget animation frames pending
    if ((!app.frameDue() && !uiBusy) || app.window->minimized()) {
      SDL_Event e;
      // Sleep until the next event (theme-file changes arrive as one, dev) or the widget tree's next timer.
      const double wake = app.ui ? app.ui->nextWakeTime() : std::numeric_limits<double>::infinity();
      const bool got = std::isfinite(wake)
                           ? SDL_WaitEventTimeout(&e, static_cast<Sint32>(std::clamp(std::ceil((wake - app.now()) * 1000.0), 0.0, 1e6)))
                           : SDL_WaitEvent(&e);
      if (got) app.handle(e);
    }
    pump(app);
    if (app.quit) break;
    app.pollTheme();
    app.updateUi();
    if (app.frameDue() && !app.window->minimized()) {
      if (!app.drawFrame()) {
        std::fprintf(stderr, "pqdaw: frame failed: %s\n", app.error.c_str());
        SDL_RemoveEventWatch(&WindowApp::liveResizeWatch, &app);
        return 1;
      }
      app.pacer.wait();  // one frame per compositor pass, without a driver vsync spin
    }
  }
  SDL_RemoveEventWatch(&WindowApp::liveResizeWatch, &app);
  return app.shell ? app.shell->exitCode(0) : 0;
}

// Scripted check that resize, minimise and restore are handled (acceptance: they don't crash). Each step
// pumps events and renders through the same path as the interactive loop, then checks the outcome.
int selfTest(WindowApp& app, Clock::time_point processStart) {
  int failures = 0;
  // T0.x-flake: wall-clock criteria that depend on machine load (frame-interval hitch, partial-redraw CPU ratio)
  // are enforced only with PQ_PERF_CHECKS=1 (ctest -L perf: pqdaw_window_selftest_perf); otherwise they are
  // measured and printed, and load-independent counts stand in for them.
  const char* perfEnv = std::getenv("PQ_PERF_CHECKS");
  const bool perfChecks = perfEnv && *perfEnv && *perfEnv != '0';
  auto check = [&](bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
  };
  auto settle = [&] {
    SDL_SyncWindow(app.window->sdl());
    pump(app);
  };
  auto frame = [&](const char* what) {
    const uint32_t before = app.framesPresented;
    const bool ok = app.drawFrame();
    check(ok && app.framesPresented == before + 1, what);
    if (!ok) std::printf("    error: %s\n", app.error.c_str());
  };

  std::printf("window self-test (device: %s, present mode %d)\n", app.gpu->deviceName().c_str(),
              static_cast<int>(app.swapchain->presentMode()));
  SDL_ShowWindow(app.window->sdl());
  settle();
  frame("first frame presents");
  std::printf("  first frame %.1f ms after process start\n",
              std::chrono::duration<double, std::milli>(Clock::now() - processStart).count());

  const struct {
    int w, h;
  } sizes[] = {{640, 400}, {1024, 700}, {320, 200}, {64, 48}, {1400, 900}, {800, 500}};
  for (const auto& s : sizes) {
    SDL_SetWindowSize(app.window->sdl(), s.w, s.h);
    settle();
    char what[96];
    std::snprintf(what, sizeof what, "resize to %dx%d presents", s.w, s.h);
    frame(what);
  }
  check(app.swapchain->recreateCount() >= 2, "swapchain was recreated on resize");

  // Several resizes without pumping in between: the swapchain must notice via surface caps alone.
  for (int i = 0; i < 5; ++i) SDL_SetWindowSize(app.window->sdl(), 500 + 37 * i, 300 + 23 * i);
  frame("resize burst without events presents");
  settle();

  SDL_MinimizeWindow(app.window->sdl());
  settle();
  for (int i = 0; i < 500 && !app.window->minimized(); ++i) {  // minimise animation may lag (up to 5 s under load)
    SDL_Delay(10);
    pump(app);
  }
  check(app.window->minimized(), "window reports minimised");
  {
    const uint32_t presented = app.framesPresented;
    bool ok = true;
    for (int i = 0; i < 5; ++i) ok = app.drawFrame() && ok;
    check(ok && app.framesPresented == presented, "frames while minimised are skipped, not errors");
  }

  SDL_RestoreWindow(app.window->sdl());
  settle();
  for (int i = 0; i < 500 && app.window->minimized(); ++i) {
    SDL_Delay(10);
    pump(app);
  }
  check(!app.window->minimized(), "window restores");
  frame("frame after restore presents");

  // Animated burst through the real pacing path: 60 paced frames should take about one second at 60 Hz.
  const auto t0 = Clock::now();
  const uint32_t timeoutsBefore = app.pacer.timeouts();
  app.damage.setAnimating(true);
  bool burstOk = true;
  for (int i = 0; i < 60; ++i) {
    pump(app);
    burstOk = app.drawFrame() && burstOk;
    app.pacer.wait();
  }
  app.damage.setAnimating(false);
  const double burstMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  check(burstOk, "60 animated paced frames present");
  std::printf("  60 paced frames: %.1f ms (%.2f ms/frame), pacer %s, %u pacer timeouts\n", burstMs, burstMs / 60.0,
              app.pacer.available() ? "DwmFlush" : "unavailable", app.pacer.timeouts() - timeoutsBefore);

  // T0.8-fix1: 200 glyphs rasterised in the middle of an animated, paced run: no frame interval may exceed
  // 1.5x the run's median (uploads go through the frame's own command buffer; nothing waits for the GPU).
  // FreeType rasterisation itself (~15 us per glyph in release) is CPU work inside the frame. Up to 3 attempts,
  // each with 200 fresh glyphs (a new font size), because the machine may be shared with other work. The
  // timing is a release-build criterion: the dev preset compiles FreeType and HarfBuzz at /Od (~75 us per
  // glyph), so there it is measured and printed but not enforced. T0.x-flake: the interval criterion needs the
  // UI thread and the compositor to be scheduled on time, so it is enforced only with PQ_PERF_CHECKS=1; the
  // default run checks the load-independent part: the glyphs were rasterised in that one frame and the frame
  // loop made no blocking GPU submit (the pre-T0.8-fix1 path waited for the device and submitted the upload
  // separately).
  {
#ifdef NDEBUG
    const bool enforceTiming = perfChecks;
#else
    const bool enforceTiming = false;
#endif
    TextSystem& ts = *app.textures.text;
    bool ok = false, functionalOk = false;
    double medianMs = 0, worstMs = 0, glyphFrameMs = 0;
    uint64_t rasterised = 0, blocking = 0;
    int attempt = 0;
    for (; attempt < 3 && !ok; ++attempt) {
      auto font = ts.font(app.textures.regular, 19 + 2 * static_cast<uint32_t>(attempt));  // ASCII warmed now, not in a frame
      if (!font) break;
      // 200 code points from Latin-1 Supplement / Latin Extended-A/B that Inter has (glyph id != 0).
      std::vector<std::string> lines(4);
      uint32_t found = 0;
      for (uint32_t c = 0xC0; c < 0x250 && found < 200; ++c) {
        std::string u;
        u += static_cast<char>(0xC0 | (c >> 6));
        u += static_cast<char>(0x80 | (c & 0x3F));
        const auto g = ts.shapeGlyphs(*font, u);
        if (g.size() != 1 || g[0].glyphId == 0) continue;
        lines[found / 50] += u;
        ++found;
      }
      if (found < 200) break;
      constexpr int kFrames = 61, kGlyphFrame = 30;
      std::vector<double> intervals;
      double glyphFrameWorkMs = 0;
      app.damage.setAnimating(true);
      auto prev = Clock::now();
      const uint64_t before = ts.stats().glyphsRasterised;
      const uint64_t blockingBefore = app.gpu->blockingSubmits();
      bool framesOk = true;
      for (int i = 0; i < kFrames; ++i) {
        if (i == kGlyphFrame) {
          app.extraFont = *font;
          app.extraLines = lines;
        }
        pump(app);
        const auto w0 = Clock::now();
        framesOk = app.drawFrame() && framesOk;
        if (i == kGlyphFrame) glyphFrameWorkMs = std::chrono::duration<double, std::milli>(Clock::now() - w0).count();
        app.pacer.wait();
        const auto now = Clock::now();
        intervals.push_back(std::chrono::duration<double, std::milli>(now - prev).count());
        prev = now;
      }
      app.damage.setAnimating(false);
      app.extraFont = kNoFont;
      app.extraLines.clear();
      rasterised = ts.stats().glyphsRasterised - before;
      blocking = app.gpu->blockingSubmits() - blockingBefore;
      intervals.erase(intervals.begin());  // the first interval includes the setup above
      glyphFrameMs = intervals[kGlyphFrame - 1];
      std::vector<double> sorted = intervals;
      std::sort(sorted.begin(), sorted.end());
      medianMs = sorted[sorted.size() / 2];
      worstMs = sorted.back();
      functionalOk = framesOk && rasterised >= 200 && blocking == 0;
      ok = functionalOk && (worstMs <= 1.5 * medianMs || !enforceTiming);
      std::printf("  glyph hitch attempt %d: %llu glyphs rasterised in one frame (drawFrame %.2f ms), %llu blocking GPU "
                  "submits; frame interval median %.2f ms, that frame %.2f ms, worst %.2f ms\n",
                  attempt + 1, static_cast<unsigned long long>(rasterised), glyphFrameWorkMs,
                  static_cast<unsigned long long>(blocking), medianMs, glyphFrameMs, worstMs);
      if (!functionalOk) break;  // counts do not change with load: no retry
    }
    check(functionalOk, "200 new glyphs mid-run present, no blocking GPU submit");
    if (enforceTiming) check(ok, "200 new glyphs mid-run: no frame interval > 1.5x the median (perf)");
    else std::printf("  (glyph-hitch frame interval: measured, enforced with PQ_PERF_CHECKS=1 in release builds)\n");
  }

  // T0.7-fix3: a display-scale change at runtime re-renders without a restart. The OS event is faked: the
  // override hook stands in for SDL_GetWindowDisplayScale and a DISPLAY_SCALE_CHANGED event goes through the
  // real queue and handler.
  {
    auto injectScale = [&](float override) {
      app.scaleOverride = override;
      app.damage.clear();
      SDL_Event ev{};
      ev.type = SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED;
      ev.window.windowID = app.window->id();
      ev.window.timestamp = SDL_GetTicksNS();
      return SDL_PushEvent(&ev);
    };
    const float initial = app.scale.factor;
    const float target = initial == 1.5f ? 2.0f : 1.5f;
    const uint32_t changesBefore = app.scaleChanges;
    check(injectScale(target), "fake DISPLAY_SCALE_CHANGED event queued");
    pump(app);
    char what[96];
    std::snprintf(what, sizeof what, "UI scale follows the event (%.2f -> %.2f)", static_cast<double>(initial),
                  static_cast<double>(target));
    check(app.scale.factor == target && app.scaleChanges == changesBefore + 1, what);
    const TextSystem& ts = *app.textures.text;
    const UiScale s = app.scale;
    check(ts.metrics(app.textures.fonts.section).pixelSize == s.fontPx(text_size::kSection) &&
              ts.metrics(app.textures.fonts.body).pixelSize == s.fontPx(text_size::kBody) &&
              ts.metrics(app.textures.fonts.title).pixelSize == s.fontPx(text_size::kTitle) &&
              ts.metrics(app.textures.fonts.meter).pixelSize == s.fontPx(text_size::kMeter),
          "font roles re-resolved at round(role x scale) px");
    check(app.damage.needsFrame(), "scale change requests a redraw");
    app.updateUi();
    if (app.ui && app.scene == "widgets-core") {
      const Widget* header = app.ui->find("header");
      const Widget* browserItem = app.ui->find("body/browser/item[0]");
      check(app.ui->scale() == target && header && header->rect().h == s.px(28) && browserItem &&
                browserItem->rect().h == s.px(22),
            "widget tree relaid out in logical px x new scale");
    } else if (app.ui) {  // the T3.2 gallery: header 30 logical px, overlays still open
      const Widget* header = app.ui->find("header");
      check(app.ui->scale() == target && header && header->rect().h == s.px(30) && app.ui->find("overlay/context-menu"),
            "gallery relaid out in logical px x new scale, menus open");
    }
    frame("frame at the new scale presents");
    check(app.lastFrameScale == s, "that frame was drawn at the new scale");
    // Back to the display's own scale (override cleared), as when the window returns to its monitor.
    check(injectScale(0.0f), "second fake DISPLAY_SCALE_CHANGED queued");
    pump(app);
    check(app.scale == UiScale(app.window->displayScale()), "scale returns to the display's value");
    app.updateUi();
    frame("frame back at the display scale presents");
    check(app.lastFrameScale == app.scale && (!app.ui || app.ui->scale() == app.scale.factor),
          "that frame was drawn at the display scale");
  }

  // T0.7-fix1: a 2 px playhead animating over `lines` (scene lines-playhead) redraws only its old and new
  // columns of the persistent image; the same frames through the full-redraw path (rebuild + upload + draw the
  // whole scene, as before T0.7-fix1) for comparison, at the scene's 800×480 design size. UI-thread CPU is
  // measured as the thread's own CPU cycles in drawFrame (QueryThreadCycleTime: blocking excluded) and as wall
  // time; the pacer leaves a compositor interval between frames, so drawFrame does not wait for the GPU. The
  // CPU runs have the GPU profiler off (its query commands would add the same cost to both sides); a separate
  // profiled pass gives the GPU times and fragment counts (read for the slot waited for in beginFrame, i.e. two
  // frames late, so the first frames of each run are skipped).
  if (app.layered) {
    SDL_SetWindowSize(app.window->sdl(), 800, 480);
    settle();
    struct Run {
      std::vector<double> wallMs, cycles, beginMs, buildMs, endMs, drawMs, copyMs, fragments;
      uint64_t vertexBytes = 0;  // draw-list bytes the run uploaded (Renderer::Stats): deterministic CPU work
      uint32_t partial = 0;
      bool ok = true;
    };
    constexpr int kFrames = 90, kSkip = 6;
    auto median = [](std::vector<double> v) {
      if (v.empty()) return 0.0;
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    auto run = [&](bool forceFull) {
      Run r;
      app.forceFull = forceFull;
      app.overlayAnimating = true;
      const uint32_t partialBefore = app.partialFrames;
      const uint64_t bytesBefore = app.renderer->stats().vertexBytesUploaded;
      for (int i = 0; i < kFrames; ++i) {
        pump(app);
        const auto c0 = Clock::now();
        const uint64_t k0 = threadCycles();
        r.ok = app.drawFrame() && r.ok;
        const uint64_t k1 = threadCycles();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
        if (i >= kSkip) {
          r.wallMs.push_back(ms);
          r.cycles.push_back(static_cast<double>(k1 - k0));
          r.beginMs.push_back(app.lastTiming.beginMs);
          r.buildMs.push_back(app.lastTiming.buildMs);
          r.endMs.push_back(app.lastTiming.endMs);
          const GpuFrameStats& g = app.swapchain->lastFrameStats();
          if (g.hasTimes) {
            r.drawMs.push_back(g.drawMs);
            r.copyMs.push_back(g.copyMs);
          }
          if (g.hasFragments) r.fragments.push_back(static_cast<double>(g.fragments));
        }
        app.pacer.wait();
      }
      r.partial = app.partialFrames - partialBefore;
      r.vertexBytes = app.renderer->stats().vertexBytesUploaded - bytesBefore;
      app.overlayAnimating = false;
      app.forceFull = false;
      return r;
    };
    uint32_t w = 0, h = 0;
    app.window->pixelSize(w, h);
    const double pixels = static_cast<double>(w) * h;
    // T0.x-flake: thread CPU cycles exclude preemption but not what load does to the fixed submit/present cost
    // (Q52: 21 % idle, up to 28 % with parallel builds), so the < 25 % ratio is enforced only with
    // PQ_PERF_CHECKS=1 (3 attempts). The default run checks the deterministic part of the same claim: a playhead
    // frame uploads under 25 % of the draw-list bytes a full redraw uploads (it rebuilds only the overlay).
    bool cpuOk = false;
    const int attempts = perfChecks ? 3 : 1;
    for (int attempt = 0; attempt < attempts && !cpuOk; ++attempt) {  // the machine may be shared with other work
      const Run partial = run(false);
      const Run full = run(true);
      const double cycP = median(partial.cycles), cycF = median(full.cycles);
      const double wallP = median(partial.wallMs), wallF = median(full.wallMs);
      cpuOk = partial.ok && full.ok && partial.partial >= kFrames - 2 && cycP < 0.25 * cycF;
      std::printf("  partial redraw attempt %d (%ux%u px): %u/%d frames partial, damage %.3f %% of the target; UI-thread CPU per "
                  "frame (median): %.0fk cycles partial vs %.0fk full (%.1f %%), wall %.3f vs %.3f ms (%.1f %%)\n",
                  attempt + 1, w, h, partial.partial, kFrames, 100.0 * app.lastPartialArea, cycP / 1e3, cycF / 1e3,
                  100.0 * cycP / std::max(1.0, cycF), wallP, wallF, 100.0 * wallP / wallF);
      std::printf("    wall breakdown (median ms) beginFrame / build / endFrame: partial %.3f / %.3f / %.3f, full %.3f / %.3f / "
                  "%.3f\n",
                  median(partial.beginMs), median(partial.buildMs), median(partial.endMs), median(full.beginMs),
                  median(full.buildMs), median(full.endMs));
      if (attempt == 0) {
        check(partial.ok && partial.partial >= kFrames - 2, "playhead frames are partial (damage = its old + new columns)");
        std::printf("    draw-list bytes uploaded per frame: %.0f partial vs %.0f full (%.2f %%)\n",
                    static_cast<double>(partial.vertexBytes) / kFrames, static_cast<double>(full.vertexBytes) / kFrames,
                    100.0 * static_cast<double>(partial.vertexBytes) / std::max<double>(1.0, static_cast<double>(full.vertexBytes)));
        check(full.vertexBytes > 0 && partial.vertexBytes * 4 < full.vertexBytes,
              "playhead frames upload < 25 % of a full redraw's draw-list bytes");
      }
    }
    // The validation layer adds CPU to every Vulkan call, i.e. mostly to the fixed submit/present cost both
    // sides pay: with it on, the ratio is printed but not enforced (it measures the layer, not pqdaw).
    if (app.gpu->validationEnabled())
      check(true, "playhead frame UI-thread CPU (validation on: measured, not enforced)");
    else if (perfChecks)
      check(cpuOk, "playhead frame UI-thread CPU < 25 % of a full redraw (median cycles, perf)");
    else
      std::printf("  (UI-thread CPU ratio: measured, enforced with PQ_PERF_CHECKS=1)\n");

    if (Status s = app.swapchain->setProfiling(true); !s) {
      std::printf("    GPU profiling unavailable: %s\n", s.error().message.c_str());
    } else {
      const Run partial = run(false);
      const Run full = run(true);
      (void)app.swapchain->setProfiling(false);
      if (!partial.drawMs.empty() && !full.drawMs.empty())
        std::printf("    GPU per frame (median): draw %.4f ms partial vs %.4f ms full; copy to the swapchain image %.4f ms\n",
                    median(partial.drawMs), median(full.drawMs), median(partial.copyMs));
      if (!partial.fragments.empty() && !full.fragments.empty()) {
        const double fragP = median(partial.fragments), fragF = median(full.fragments);
        std::printf("    fragment-shader invocations per frame (median): %.0f partial = %.3f %% of the pixels; full %.0f = %.1f %%\n",
                    fragP, 100.0 * fragP / pixels, fragF, 100.0 * fragF / pixels);
        check(fragP <= 0.02 * pixels, "playhead frames shade <= 2 % of the target's pixels");
      } else {
        std::printf("    (no pipeline statistics on this device: fragment count not measured)\n");
      }
    }
  }
  std::printf("  frames: %u partial, %u full\n", app.partialFrames, app.fullFrames);

  if (app.gpu->validationEnabled())
    check(app.gpu->validationErrorCount() == 0, "no Vulkan validation errors");
  else
    std::printf("  (Vulkan validation off; set PQ_VK_VALIDATION=1 to check)\n");
  std::printf("window self-test: %s (%u presented, %u skipped, %u swapchain builds)\n", failures ? "FAILED" : "passed",
              app.framesPresented, app.framesSkipped, app.swapchain->recreateCount());
  return failures ? 1 : 0;
}

}  // namespace

int runWindow(const CliOptions& o, bool selfTestMode) {
  const auto processStart = Clock::now();
  auto app = openWindowApp(o, selfTestMode);
  if (!app) {
    if ((selfTestMode || o.idleCheckSeconds > 0.0) && app.error().code == ErrorCode::Unsupported && app.error().message.rfind("no display", 0) == 0) {
      std::printf("SKIPPED: %s\n", app.error().message.c_str());
      return kExitSkipped;
    }
    std::fprintf(stderr, "pqdaw: %s\n", app.error().message.c_str());
    return 1;
  }
  return selfTestMode ? selfTest(**app, processStart) : interactive(**app);
}

}  // namespace pq::app
