#pragma once

// Widget-owned RGBA textures (TX.spectrogram): what a widget or face that keeps its own image on the GPU (a scrolling
// spectrogram whose newest columns are written each frame) uses while painting, without holding the Renderer. The
// renderer owns one pool (Renderer::images(); SceneTextures::images; PaintResources::images, null without a GPU) and
// detaches it when it is destroyed, after which every call is a no-op that fails softly, so a widget may outlive the
// renderer it drew with (it keeps the pool alive through share()).
//
//   create    an RGBA8 texture of w × h, every texel transparent black, queued for the next frame (no GPU wait)
//   update    Renderer::updateTexture: `rect` overwritten with rgba (rows `rowPitch` bytes apart; 0 = tight), copied
//             into the staging ring now, on the GPU before the next frame's draws. A one-texel-wide column is a rect
//             of w = 1 with rowPitch 4: no transpose for column-major sources.
//   destroy   Renderer::destroyTexture (deferred until the frames that may sample it are done)
//
// UI thread only (the thread that paints and calls Renderer::upload).

#include <cstdint>
#include <memory>
#include <span>

#include "pq/core/result.h"
#include "pq/ui/draw_list.h"

namespace pq::ui {

class Renderer;
struct TextureRect;
enum class TextureFilter : uint8_t;

class TexturePool : public std::enable_shared_from_this<TexturePool> {
public:
  explicit TexturePool(Renderer* renderer) noexcept : renderer_(renderer) {}
  TexturePool(const TexturePool&) = delete;
  TexturePool& operator=(const TexturePool&) = delete;

  bool attached() const noexcept { return renderer_ != nullptr; }
  std::shared_ptr<TexturePool> share() { return shared_from_this(); }

  Result<TextureId> create(uint32_t w, uint32_t h, TextureFilter filter);
  Status update(TextureId id, const TextureRect& rect, std::span<const uint8_t> rgba, uint32_t rowPitch = 0);
  void destroy(TextureId id) noexcept;

  // Stats (tests, benches): bytes handed to update / create, calls.
  uint64_t bytesUpdated() const noexcept { return bytes_; }
  uint64_t updates() const noexcept { return updates_; }

private:
  friend class Renderer;
  Renderer* renderer_;
  uint64_t bytes_ = 0, updates_ = 0;
};

}  // namespace pq::ui
