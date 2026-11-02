// Widget-owned textures (pq/ui/texture_pool.h): forwards to the renderer while it lives.

#include "pq/ui/texture_pool.h"

#include "pq/ui/renderer.h"

namespace pq::ui {

Result<TextureId> TexturePool::create(uint32_t w, uint32_t h, TextureFilter filter) {
  if (renderer_ == nullptr) return Error{ErrorCode::Device, "TexturePool: no renderer", {}};
  Image img;
  img.width = w;
  img.height = h;
  img.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
  bytes_ += img.rgba.size();
  return renderer_->createTexture(img, filter);
}

Status TexturePool::update(TextureId id, const TextureRect& rect, std::span<const uint8_t> rgba, uint32_t rowPitch) {
  if (renderer_ == nullptr) return Error{ErrorCode::Device, "TexturePool: no renderer", {}};
  bytes_ += static_cast<uint64_t>(rect.w) * rect.h * 4;
  ++updates_;
  return renderer_->updateTexture(id, rect, rgba, rowPitch);
}

void TexturePool::destroy(TextureId id) noexcept {
  if (renderer_ != nullptr) (void)renderer_->destroyTexture(id);
}

}  // namespace pq::ui
