#include "pq/app/commands.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <memory>

#include "pq/ui/damage.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/frame_pacer.h"
#include "pq/ui/gpu.h"
#include "pq/ui/image.h"
#include "pq/ui/offscreen.h"
#include "pq/ui/renderer.h"
#include "pq/ui/scenes.h"
#include "pq/ui/window.h"

namespace pq::app {

using namespace pq::ui;

int runScreenshot(const CliOptions& o) {
  const uint32_t w = o.width ? o.width : kDefaultSceneWidth;
  const uint32_t h = o.height ? o.height : kDefaultSceneHeight;
  auto img = renderSceneImage(o.scene, w, h);
  if (!img) {
    std::fprintf(stderr, "pqdaw: screenshot failed: %s\n", img.error().message.c_str());
    return 1;
  }
  if (Status s = writePng(o.outPath, *img); !s) {
    std::fprintf(stderr, "pqdaw: %s\n", s.error().message.c_str());
    return 1;
  }
  std::printf("wrote %s (%ux%u, scene %s)\n", o.outPath.c_str(), w, h, o.scene.c_str());
  return 0;
}

namespace {

using Clock = std::chrono::steady_clock;

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

  // Draws and presents one frame if the window has a drawable surface. Returns false on a hard error.
  bool drawFrame() {
    if (inFrame) return true;
    inFrame = true;
    VkExtent2D ext{};
    auto st = swapchain->beginFrame(ext);
    bool ok = true;
    if (!st) {
      error = st.error().message;
      ok = false;
    } else if (*st == Swapchain::FrameStatus::Skipped) {
      ++framesSkipped;
    } else {
      dl.reset(ext.width, ext.height);
      const double t = std::chrono::duration<double>(Clock::now() - start).count();
      buildScene(scene, dl, textures, damage.animating() ? t : 0.0);
      if (Status s = swapchain->endFrame(*renderer, dl, kSceneClearColor); !s) {
        error = s.error().message;
        ok = false;
      } else {
        ++framesPresented;
      }
    }
    damage.clear();
    inFrame = false;
    return ok;
  }

  void handle(const SDL_Event& e) {
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
      case SDL_EVENT_WINDOW_SHOWN:
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: damage.addAll(); break;
      case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) quit = true;
        if (e.key.key == SDLK_A) {
          damage.setAnimating(!damage.animating());
          damage.addAll();
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
  if (!hasScene(o.scene)) return Error{ErrorCode::NotFound, "unknown scene '" + o.scene + "'", {}};
  WindowOptions wo;
  wo.title = "pqdaw";
  if (o.width) {
    wo.width = o.width;
    wo.height = o.height;
  }
  wo.hidden = hidden;
  auto window = Window::create(wo);
  if (!window) return std::move(window).error();
  app->window = std::move(*window);

  GpuOptions go;
  go.instanceExtensions = app->window->requiredInstanceExtensions();
  go.swapchain = true;
  auto gpu = GpuContext::create(go);
  if (!gpu) return std::move(gpu).error();
  app->gpu = std::move(*gpu);
  auto sc = Swapchain::create(*app->gpu, *app->window, 2);
  if (!sc) return std::move(sc).error();
  app->swapchain = std::move(*sc);
  auto renderer = Renderer::create(*app->gpu, app->swapchain->format(), 2);
  if (!renderer) return std::move(renderer).error();
  app->renderer = std::move(*renderer);
  auto tex = createSceneTextures(*app->renderer);
  if (!tex) return std::move(tex).error();
  app->textures = *tex;
  app->damage.setAnimating(o.animate);
  app->damage.addAll();
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
    if (!app.damage.needsFrame() || app.window->minimized()) {
      SDL_Event e;
      if (SDL_WaitEvent(&e)) app.handle(e);
    }
    pump(app);
    if (app.quit) break;
    if (app.damage.needsFrame() && !app.window->minimized()) {
      if (!app.drawFrame()) {
        std::fprintf(stderr, "pqdaw: frame failed: %s\n", app.error.c_str());
        SDL_RemoveEventWatch(&WindowApp::liveResizeWatch, &app);
        return 1;
      }
      app.pacer.wait();  // one frame per compositor pass, without a driver vsync spin
    }
  }
  SDL_RemoveEventWatch(&WindowApp::liveResizeWatch, &app);
  return 0;
}

// Scripted check that resize, minimise and restore are handled (acceptance: they don't crash). Each step
// pumps events and renders through the same path as the interactive loop, then checks the outcome.
int selfTest(WindowApp& app, Clock::time_point processStart) {
  int failures = 0;
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
  for (int i = 0; i < 50 && !app.window->minimized(); ++i) {  // minimise animation may lag
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
  for (int i = 0; i < 50 && app.window->minimized(); ++i) {
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
    if (selfTestMode && app.error().code == ErrorCode::Unsupported && app.error().message.rfind("no display", 0) == 0) {
      std::printf("SKIPPED: %s\n", app.error().message.c_str());
      return kExitSkipped;
    }
    std::fprintf(stderr, "pqdaw: %s\n", app.error().message.c_str());
    return 1;
  }
  return selfTestMode ? selfTest(**app, processStart) : interactive(**app);
}

}  // namespace pq::app
