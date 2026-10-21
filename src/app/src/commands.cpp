#include "pq/app/commands.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
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

namespace pq::app {

using namespace pq::ui;

int runScreenshot(const CliOptions& o) {
  const uint32_t w = o.width ? o.width : kDefaultSceneWidth;
  const uint32_t h = o.height ? o.height : kDefaultSceneHeight;
  const UiScale scale(o.scale > 0.0f ? o.scale : 1.0f);
  auto img = renderSceneImage(o.scene, w, h, {}, scale);
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
  // Widget scenes (widgets-core) run a live UiTree: SDL input → translateEvent → the tree; its damage and
  // timers drive frames; layout is in logical px × the display scale.
  std::unique_ptr<UiTree> ui;
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
    if (ui->needsPaint()) damage.addAll();  // swapchain images are not preserved: a damaged frame is drawn whole
  }

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
      if (textures.text) textures.text->beginFrame();  // frame boundary: run cache trim, atlas LRU stamp
      dl.reset(ext.width, ext.height);
      const double t = now();
      if (ui) {
        ui->setViewport(ext.width, ext.height, scale);
        dl.addRectFilled({0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height)}, kSceneClearColor);
        PaintResources res;
        res.text = textures.text.get();
        res.fonts = &textures.fonts;
        ui->paint(dl, res, t);
      } else {
        buildScene(scene, dl, textures, damage.animating() ? t : 0.0);
      }
      if (textures.text && extraFont != kNoFont) {
        const float lh = static_cast<float>(textures.text->metrics(extraFont).lineHeight);
        for (size_t i = 0; i < extraLines.size(); ++i)
          textures.text->draw(dl, extraFont, {8.0f, lh * static_cast<float>(i + 1)}, extraLines[i], rgba(255, 255, 255));
      }
      if (Status s = swapchain->endFrame(*renderer, dl, kSceneClearColor); !s) {
        error = s.error().message;
        ok = false;
      } else {
        ++framesPresented;
        lastFrameScale = textures.scale;
      }
    }
    damage.clear();
    inFrame = false;
    return ok;
  }

  void handle(const SDL_Event& e) {
    if (ui) {
      InputEvent in;
      if (translateEvent(e, window->pixelDensity(), in)) ui->dispatch(in);
    }
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
  app->scaleOverride = o.scale;
  app->scale = UiScale(o.scale > 0.0f ? o.scale : app->window->displayScale());
  auto tex = createSceneTextures(*app->renderer, app->scale);
  if (!tex) return std::move(tex).error();
  app->textures = *tex;
  app->damage.setAnimating(o.animate);
  app->damage.addAll();
  if (o.scene == "widgets-core") {
    app->ui = std::make_unique<UiTree>();
    uint32_t w = 0, h = 0;
    app->window->pixelSize(w, h);
    app->ui->setViewport(w, h, app->scale);
    app->ui->setRoot(makeWidgetsCoreDemo());
  }
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
    if ((!app.damage.needsFrame() && !uiBusy) || app.window->minimized()) {
      SDL_Event e;
      // Sleep until the next event, or the widget tree's next timer.
      const double wake = app.ui ? app.ui->nextWakeTime() : std::numeric_limits<double>::infinity();
      const bool got = std::isfinite(wake)
                           ? SDL_WaitEventTimeout(&e, static_cast<Sint32>(std::clamp(std::ceil((wake - app.now()) * 1000.0), 0.0, 1e6)))
                           : SDL_WaitEvent(&e);
      if (got) app.handle(e);
    }
    pump(app);
    if (app.quit) break;
    app.updateUi();
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

  // T0.8-fix1: 200 glyphs rasterised in the middle of an animated, paced run: no frame interval may exceed
  // 1.5x the run's median (uploads go through the frame's own command buffer; nothing waits for the GPU).
  // FreeType rasterisation itself (~15 us per glyph in release) is CPU work inside the frame. Up to 3 attempts,
  // each with 200 fresh glyphs (a new font size), because the machine may be shared with other work. The
  // timing is a release-build criterion: the dev preset compiles FreeType and HarfBuzz at /Od (~75 us per
  // glyph), so there it is measured and printed but not enforced.
  {
#ifdef NDEBUG
    constexpr bool kEnforceTiming = true;
#else
    constexpr bool kEnforceTiming = false;
#endif
    TextSystem& ts = *app.textures.text;
    bool ok = false;
    double medianMs = 0, worstMs = 0, glyphFrameMs = 0;
    uint64_t rasterised = 0;
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
      intervals.erase(intervals.begin());  // the first interval includes the setup above
      glyphFrameMs = intervals[kGlyphFrame - 1];
      std::vector<double> sorted = intervals;
      std::sort(sorted.begin(), sorted.end());
      medianMs = sorted[sorted.size() / 2];
      worstMs = sorted.back();
      ok = framesOk && rasterised >= 200 && (worstMs <= 1.5 * medianMs || !kEnforceTiming);
      std::printf("  glyph hitch attempt %d: %llu glyphs rasterised in one frame (drawFrame %.2f ms); frame interval median "
                  "%.2f ms, that frame %.2f ms, worst %.2f ms\n",
                  attempt + 1, static_cast<unsigned long long>(rasterised), glyphFrameWorkMs, medianMs, glyphFrameMs, worstMs);
    }
    check(ok, kEnforceTiming ? "200 new glyphs mid-run: no frame interval > 1.5x the median"
                             : "200 new glyphs mid-run present (timing: release builds only)");
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
    if (app.ui) {
      const Widget* header = app.ui->find("header");
      const Widget* browserItem = app.ui->find("body/browser/item[0]");
      check(app.ui->scale() == target && header && header->rect().h == s.px(28) && browserItem &&
                browserItem->rect().h == s.px(22),
            "widget tree relaid out in logical px x new scale");
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
