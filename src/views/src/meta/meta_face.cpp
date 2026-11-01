// Meta-module faces: the base class's shared parts (jacks, pins, hover) and the registry by type id (TMM.4). See
// pq/views/meta/meta_face.h.

#include "pq/views/meta/meta_face.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>

#include "faces.h"

namespace pq::views {

namespace {

struct Registry {
  std::mutex m;
  std::vector<std::pair<std::string, FaceFactory>> factories{{"phase_acc", &makePhaseAccFace},
                                                                     {"wavetable", &makeWavetableFace},
                                                                     {"envelope", &makeEnvelopeFace}};
  // worldSize per type id, filled on first ask (a face's size never changes).
  std::vector<std::pair<std::string, ui::Vec2>> sizes;
};

Registry& registry() {
  static Registry r;
  return r;
}

FaceFactory factoryOf(std::string_view id) {
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.m);
  for (const auto& [name, f] : r.factories)
    if (name == id) return f;
  return nullptr;
}

double steadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<double (*)()> gClock{nullptr};

}  // namespace

double MetaTarget::setting(size_t i) const {
  const meta::Type* t = targetType();
  return t != nullptr && i < t->settings.size() ? t->settings[i].def : 0.0;
}

meta::DataPtr MetaTarget::data() const {
  // The type's default data, made once per type (a stable pointer: faces cache by it).
  const meta::Type* t = targetType();
  if (t == nullptr || t->defaultData == nullptr) return nullptr;
  static std::mutex m;
  static std::vector<std::pair<const meta::Type*, meta::DataPtr>> made;
  std::lock_guard<std::mutex> lock(m);
  for (const auto& [type, d] : made)
    if (type == t) return d;
  made.emplace_back(t, t->defaultData());
  return made.back().second;
}

void MetaFace::setClock(double (*clock)()) { gClock.store(clock, std::memory_order_relaxed); }
double MetaFace::clockNow() {
  double (*c)() = gClock.load(std::memory_order_relaxed);
  return c != nullptr ? c() : steadySeconds();
}

void paintFace(MetaFace& f, ui::FacePainter& p, const FaceEnv& env) {
  for (uint32_t layer = 0; layer < f.staticLayerCount(); ++layer) {
    ui::FacePainter s(p.drawList(), p.transform());
    s.setTint(f.staticLayerColour(*env.theme, layer));
    f.paintStatic(s, layer);
  }
  f.paintJacks(p, env);
  f.paintLive(p, env);
}

void MetaFace::bind(std::unique_ptr<MetaTarget> target, VoiceTapReader* voices) {
  target_ = target ? std::move(target) : std::make_unique<DefaultTarget>(*type_);
  voices_ = voices;
  voiceCount_ = 0;
  voiceSerial_ = 0;
  justBound_ = true;
  (void)pollVoices();
}

float MetaFace::inputY(size_t i, size_t /*n*/) const noexcept { return 24.0f + 16.0f * static_cast<float>(i); }
float MetaFace::outputY(size_t j, size_t /*n*/) const noexcept { return 24.0f + 16.0f * static_cast<float>(j); }

void MetaFace::paintJacks(ui::FacePainter& p, const FaceEnv& env) const {
  const ui::Theme& th = *env.theme;
  const float w = worldSize().x;
  for (const FaceJack& j : env.jacks) {
    if (!j.visible) continue;
    // A short lead from the edge into a socket ring; a connected socket carries a dot.
    const float edge = j.input ? 0.0f : w, sx = j.input ? 5.0f : w - 5.0f;
    p.line({edge, j.at.y}, {j.input ? sx - 2.75f : sx + 2.75f, j.at.y}, 1.25f, th.face.jackRing);
    p.disc({sx, j.at.y}, 3.4f, th.face.jack);
    p.ring({sx, j.at.y}, 2.75f, 1.1f, th.face.jackRing);
    p.disc({sx, j.at.y}, 1.25f, j.connected ? th.face.signal : ui::mulAlpha(th.face.jackRing, 0.35f));
  }
}

ui::Cursor MetaFace::cursor(int32_t part) const noexcept { return part >= 0 ? ui::Cursor::ResizeNS : ui::Cursor::Inherit; }

void MetaFace::describe(int32_t /*part*/, std::string& title, std::string& body) const {
  title = std::string(type_->name);
  body = "Double-click the frame to open it to its atoms.";
}

bool MetaFace::pollVoices() {
  if (voices_ == nullptr) {
    const bool had = voiceCount_ != 0;
    voiceCount_ = 0;
    return voicesPolled(had);
  }
  const uint64_t serial = voices_->serial();
  if (serial == voiceSerial_) return voicesPolled(false);
  voiceSerial_ = serial;
  const uint32_t before = voiceCount_;
  voiceCount_ = voices_->read(voiceBuf_);
  return voicesPolled(voiceCount_ != 0 || before != 0);
}

bool MetaFace::setHoveredPart(int32_t part) noexcept {
  if (hovered_ == part) return false;
  hovered_ = part;
  return true;
}

std::unique_ptr<MetaFace> makeFace(const meta::Type& t) {
  const FaceFactory f = factoryOf(t.id);
  if (f == nullptr) return nullptr;
  std::unique_ptr<MetaFace> face = f(t);
  if (face) face->bind(nullptr, nullptr);
  return face;
}

bool hasFace(const meta::Type& t) noexcept { return factoryOf(t.id) != nullptr; }

ui::Vec2 faceSize(const meta::Type& t) noexcept {
  Registry& r = registry();
  {
    std::lock_guard<std::mutex> lock(r.m);
    for (const auto& [name, s] : r.sizes)
      if (name == t.id) return s;
  }
  const std::unique_ptr<MetaFace> f = makeFace(t);
  const ui::Vec2 s = f ? f->worldSize() : ui::Vec2{0.0f, 0.0f};
  std::lock_guard<std::mutex> lock(r.m);
  r.sizes.emplace_back(std::string(t.id), s);
  return s;
}

bool registerFace(std::string_view typeId, FaceFactory factory) {
  if (factory == nullptr || typeId.empty()) return false;
  Registry& r = registry();
  std::lock_guard<std::mutex> lock(r.m);
  for (const auto& [name, f] : r.factories)
    if (name == typeId) return false;
  r.factories.emplace_back(std::string(typeId), factory);
  return true;
}

}  // namespace pq::views
