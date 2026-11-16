// Meta-module faces: the base class's shared parts (jacks, pins, hover) and the registry by type id (TMM.4). See
// pq/views/meta/meta_face.h.

#include "pq/views/meta/meta_face.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

#include "faces.h"
#include "pq/document/lower.h"
#include "pq/views/devices/device_model.h"

namespace pq::views {

namespace {

struct Registry {
  std::mutex m;
  // (GU.4: the Phase accumulator's and the VCA's faces are their presets' panels now.)
  std::vector<std::pair<std::string, FaceFactory>> factories{{"wavetable", &makeWavetableFace},
                                                                     {"envelope", &makeEnvelopeFace},
                                                                     {"spectrum", &makeSpectrumFace},  // TX.spectrum
                                                                     {"oscilloscope", &makeScopeFace},  // TX.scope
                                                                     {"spectrogram", &makeSpectrogramFace},  // TX.spectrogram
                                                                     {"noise", &makeNoiseFace},  // NZ.1
                                                                     {"level_meter", &makeLevelMeterFace},  // VZ.4
                                                                     {"lufs_meter", &makeLufsMeterFace},    // VZ.4
                                                                     {"loudness_history", &makeLoudnessHistoryFace},  // VZ.4
                                                                     {"stereo_field", &makeStereoFieldFace},  // VZ.5
                                                                     {"stereo_image", &makeStereoImageFace},  // VZ.5
                                                                     {"phase_scope", &makePhaseScopeFace},  // VZ.5
                                                                     {"band_levels", &makeBandLevelsFace},  // VZ.6
                                                                     {"waveform", &makeWaveformFace},      // VZ.6
                                                                     {"waterfall", &makeWaterfallFace},  // VZ.7
                                                                     {"stereo_spectrogram", &makeStereoSpectrogramFace}};  // VZ.7
  // worldSize per type id, filled on first ask (a face's size never changes).
  std::vector<std::pair<std::string, ui::Vec2>> sizes;
  std::vector<std::pair<std::string, ui::Vec2>> captions;  // TMM.13-fix2: captionAt per type id
  std::vector<std::pair<std::string, FaceShape>> shapes;   // TX.orbit-ports: shape() per type id
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
  f.paintGhosts(p, env);  // bug-mod-ghost: under the live layer
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

float Face::inputY(size_t i, size_t /*n*/) const noexcept { return 24.0f + 16.0f * static_cast<float>(i); }
float Face::outputY(size_t j, size_t /*n*/) const noexcept { return 24.0f + 16.0f * static_cast<float>(j); }
double Face::nextWake() const noexcept { return std::numeric_limits<double>::infinity(); }

void Face::describe(int32_t /*part*/, std::string& title, std::string& body) const {
  title.clear();
  body.clear();
}

void Face::paintJacks(ui::FacePainter& p, const FaceEnv& env) const {
  const ui::Theme& th = *env.theme;
  const float w = worldSize().x;
  for (const FaceJack& j : env.jacks) {
    if (!j.visible) continue;
    // A short lead from the edge into a socket ring; a connected socket carries a dot.
    const float in = jackInset();
    const float edge = j.input ? 0.0f : w, sx = j.input ? in : w - in;
    if (in > 2.75f) p.line({edge, j.at.y}, {j.input ? sx - 2.75f : sx + 2.75f, j.at.y}, 1.25f, th.face.jackRing);
    p.disc({sx, j.at.y}, 3.4f, th.face.jack);
    p.ring({sx, j.at.y}, 2.75f, 1.1f, th.face.jackRing);
    p.disc({sx, j.at.y}, 1.25f, j.connected ? th.face.signal : ui::mulAlpha(th.face.jackRing, 0.35f));
  }
}

// ---- TX.knob-curves / GU.1 ----
namespace {
// The switched curve of param i (false: the declared one, meta::travelToValue's).
bool switchedCurve(const MetaTarget& target, const meta::Type& type, size_t i, dsp::Curve& out) noexcept {
  if (i >= type.params.size()) return false;
  const dsp::Curve* c = target.curve(i);
  if (c == nullptr || *c == meta::paramCurve(type.params[i])) return false;
  out = *c;
  return true;
}
}  // namespace

double faceTravelToValue(const MetaTarget& target, const meta::Type& type, size_t param, double t) noexcept {
  if (dsp::Curve c; switchedCurve(target, type, param, c)) return dsp::evalCurve(c, std::clamp(t, 0.0, 1.0));
  return meta::travelToValue(type.params[param], t);
}

double faceValueToTravel(const MetaTarget& target, const meta::Type& type, size_t param, double v) noexcept {
  if (dsp::Curve c; switchedCurve(target, type, param, c)) return dsp::curvePosition(c, v, 0.0);
  return meta::valueToTravel(type.params[param], v);
}

bool faceCurveSwitched(const MetaTarget& target, const meta::Type& type, size_t param) noexcept {
  dsp::Curve c;
  return switchedCurve(target, type, param, c);
}

void MetaFace::paintJacks(ui::FacePainter& p, const FaceEnv& env) const {
  Face::paintJacks(p, env);
  if (target_ == nullptr || env.theme == nullptr) return;
  for (size_t k = 0; k < type_->params.size(); ++k) {
    dsp::Curve tr;
    if (!switchedCurve(*target_, *type_, k, tr)) continue;
    const std::optional<ui::Rect> r = paramRect(k);
    if (!r) continue;
    // A 6 × 4 unit glyph of the curve inside the control's top-right corner, in the face's tick colour.
    std::array<ui::Vec2, devices::kCurveGlyphPoints> pts;
    devices::curveGlyphPoints(tr, {r->x1 - 7.0f, r->y0 + 1.0f, r->x1 - 1.0f, r->y0 + 5.0f}, pts);
    p.polyline(pts, 0.8f, ui::mulAlpha(env.theme->face.jackRing, 0.9f));
  }
}

ui::Cursor Face::cursor(int32_t part) const noexcept { return part >= 0 ? ui::Cursor::ResizeNS : ui::Cursor::Inherit; }

void MetaFace::describe(int32_t /*part*/, std::string& title, std::string& body) const {
  title = std::string(type_->name);
  body = "Its menu's Open Group opens it to its atoms.";
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

// ---- bug-mod-ghost: the ghosts ------------------------------------------------------------------------------------

namespace {
// The ghost pass's target: param i at its ghost position (through the inner target's curve), everything else the
// inner target's; it edits nothing.
class GhostMetaTarget final : public MetaTarget {
public:
  GhostMetaTarget(const MetaTarget& inner, const meta::Type& type) : in_(inner), type_(type) {}
  double value(size_t i) const override {
    if (const std::optional<double> g = in_.ghostPosition(i); g && i < type_.params.size()) return faceTravelToValue(in_, type_, i, *g);
    return in_.value(i);
  }
  bool editable() const noexcept override { return in_.editable(); }
  void begin(const char*) override {}
  void set(size_t, double) override {}
  void end() override {}
  void cancel() override {}
  void reset(size_t) override {}
  double setting(size_t i) const override { return in_.setting(i); }
  meta::DataPtr data() const override { return in_.data(); }
  const meta::Type* targetType() const noexcept override { return in_.targetType(); }
  void pitchGuides(float lo, float hi, std::vector<PitchGuide>& out) const override { in_.pitchGuides(lo, hi, out); }
  uint64_t guidesKey() const override { return in_.guidesKey(); }
  const dsp::Curve* curve(size_t i) const override { return in_.curve(i); }
  std::optional<double> ghostPosition(size_t i) const override { return in_.ghostPosition(i); }
  bool modulated(size_t i) const override { return in_.modulated(i); }

private:
  const MetaTarget& in_;
  const meta::Type& type_;
};
}  // namespace

bool MetaFace::hasGhost() const {
  if (target_ == nullptr) return false;
  for (size_t i = 0; i < type_->params.size(); ++i)
    if (target_->ghostPosition(i)) return true;
  return false;
}

bool MetaFace::ghostsLive() const {
  if (target_ == nullptr || !target_->modulationLive()) return false;
  for (size_t i = 0; i < type_->params.size(); ++i)
    if (target_->modulated(i)) return true;
  return false;
}

bool MetaFace::pollGhosts() {
  if (target_ == nullptr) return false;
  bool any = false;
  for (size_t i = 0; i < type_->params.size() && !any; ++i) any = target_->modulated(i);
  if (!any) return false;
  const uint64_t s = target_->modulationSerial();
  if (s == ghostSerial_) return false;
  ghostSerial_ = s;
  return true;
}

void MetaFace::paintGhosts(ui::FacePainter& p, const FaceEnv& env) {
  if (ghost_ != nullptr || !hasGhost()) return;
  GhostMetaTarget g(*target_, *type_);
  ui::FacePainter q = p.at(p.transform());
  q.setGhost(kGhostOpacity);
  FaceEnv ge = env;
  ge.ghost = true;
  ge.hovered = ge.selected = false;
  const int32_t hovered = hovered_;
  hovered_ = kNone;  // (no hover highlight on a ghost)
  ghost_ = &g;
  paintLive(q, ge);
  ghost_ = nullptr;
  hovered_ = hovered;
}

std::string MetaFace::paramAt(ui::Vec2 local) const {
  for (size_t k = 0; k < type_->params.size(); ++k)
    if (const std::optional<ui::Rect> r = paramRect(k))
      if (local.x >= r->x0 && local.x <= r->x1 && local.y >= r->y0 && local.y <= r->y1) return std::string(type_->params[k].name);
  return {};
}

std::optional<ControlAnchor> MetaFace::controlAnchor(std::string_view param) const {
  const int32_t k = type_->paramIndex(param);
  if (k < 0) return std::nullopt;
  const std::optional<ui::Rect> r = paramRect(static_cast<size_t>(k));
  if (!r) return std::nullopt;
  return ControlAnchor{{(r->x0 + r->x1) * 0.5f, (r->y0 + r->y1) * 0.5f}, 0.5f * std::min(r->x1 - r->x0, r->y1 - r->y0)};
}

bool Face::setHoveredPart(int32_t part) noexcept {
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

ui::Vec2 faceCaptionAt(const meta::Type& t) noexcept {
  Registry& r = registry();
  {
    std::lock_guard<std::mutex> lock(r.m);
    for (const auto& [name, c] : r.captions)
      if (name == t.id) return c;
  }
  const std::unique_ptr<MetaFace> f = makeFace(t);
  const ui::Vec2 c = f ? f->captionAt() : ui::Vec2{2.0f, -6.0f};
  std::lock_guard<std::mutex> lock(r.m);
  r.captions.emplace_back(std::string(t.id), c);
  return c;
}

FaceShape faceShape(const meta::Type& t) noexcept {
  Registry& r = registry();
  {
    std::lock_guard<std::mutex> lock(r.m);
    for (const auto& [name, c] : r.shapes)
      if (name == t.id) return c;
  }
  const std::unique_ptr<MetaFace> f = makeFace(t);
  const FaceShape c = f ? f->shape() : FaceShape{};
  std::lock_guard<std::mutex> lock(r.m);
  r.shapes.emplace_back(std::string(t.id), c);
  return c;
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
