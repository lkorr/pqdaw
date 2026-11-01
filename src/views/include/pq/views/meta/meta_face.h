#pragma once

// Meta-module faces (TMM.4, docs/plans/MM.md "UI", SPEC §15 2026-11-01 "meta modules"): every meta::Type with a face
// has a MetaFace class here, registered by type id. A face is vector code in its own world units (pq/ui/face_painter.h)
// so it draws — and stays interactive — at every zoom of the circuit canvas; the canvas hosts it on the node of a
// meta call (CircuitScene::faces: the block IS the face) and routes pointer events to it in face-local world units.
//
//   static layers   ≤ 3 value-independent layers, one colour each (body, tracks, frame): rasterised once per zoom
//                   into coverage tiles shared by every face of the type, drawn as one quad each
//   live layer      values, hover, the voice dots: geometry each frame (cheap: a few arcs and sprites)
//   jacks           the host's pins on the face edges, drawn by the face (paintJacks: one look for every face)
//   interaction     hitTest → a control (a part ≥ 0), the frame (kFrame: select / move / double-click opens the
//                   expansion) or nothing; drags are relative and measured in device px with a floor, so a control
//                   travels the same distance per pointer pixel at zoom 0.05 as at 3 (Shift: fine); a double-click on a
//                   control resets it; the wheel goes to a face only where it uses it (phase_acc: never)
//   binding         a MetaTarget (the document, or a read-only default state) and an optional VoiceTapReader
//
// VoiceTapReader is the narrow views-side interface the faces consume (TMM.4 builds against it before TMM.2's
// pq/core/voice_tap.h exists): per voice slot the newest sampled phase plus the block's sampled phases since the last
// read; `live()` says whether anything may still be written (the host polls at ≤ 60 Hz only while a visible face's
// reader is live: idle costs nothing). TMM.2's reader adapts to it.
//
// UI thread only.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/meta/type.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/face_painter.h"
#include "pq/ui/input.h"
#include "pq/ui/theme.h"

namespace pq::views {

// What a face reads and edits: one instance's params (in meta::Type::params order).
class MetaTarget {
public:
  virtual ~MetaTarget() = default;
  virtual double value(size_t param) const = 0;
  virtual bool editable() const noexcept { return true; }
  // A drag: begin, any number of set, then end (one undo step) or cancel (nothing changes).
  virtual void begin(const char* label) = 0;
  virtual void set(size_t param, double value) = 0;
  virtual void end() = 0;
  virtual void cancel() = 0;
  // Back to the type's default (one undo step).
  virtual void reset(size_t param) = 0;
};

// The type's defaults, read-only (a standalone file in the viewer, an instance the host cannot address).
class DefaultTarget final : public MetaTarget {
public:
  explicit DefaultTarget(const meta::Type& t) : type_(&t) {}
  double value(size_t param) const override { return param < type_->params.size() ? type_->params[param].def : 0.0; }
  bool editable() const noexcept override { return false; }
  void begin(const char*) override {}
  void set(size_t, double) override {}
  void end() override {}
  void cancel() override {}
  void reset(size_t) override {}

private:
  const meta::Type* type_;
};

// One sounding voice as a face shows it.
struct FaceVoice {
  static constexpr uint32_t kMaxSamples = 96;  // ≈ 12 blocks of 8 sampled phases: one displayed frame's reads
  uint32_t slot = 0;     // the voice slot (stable while the voice sounds)
  float phase = 0.0f;    // the newest sampled phase, 0..1
  uint32_t sampleCount = 0;
  std::array<float, kMaxSamples> samples{};  // the phases sampled since the last read, oldest first
};

class VoiceTapReader {
public:
  virtual ~VoiceTapReader() = default;
  // The voices sounding now (at most out.size()), newest record per voice slot; returns the count.
  virtual uint32_t read(std::span<FaceVoice> out) = 0;
  // Changes whenever new records arrived.
  virtual uint64_t serial() const noexcept = 0;
  // Whether records may still arrive (a voice sounds, the transport plays): the host keeps polling while true.
  virtual bool live() const noexcept = 0;
};

// A pin of the host on the face's edge (face-local world units).
struct FaceJack {
  ui::Vec2 at{0.0f, 0.0f};
  bool input = true;
  bool connected = false;
  bool visible = true;
  std::string_view name;  // the port (a connected param pin drives that control: drawn as driven)
};

// What a face paints with.
struct FaceEnv {
  const ui::Theme* theme = nullptr;
  float uiScale = 1.0f;
  bool selected = false;
  bool hovered = false;        // the node is the canvas's hovered item
  std::span<const FaceJack> jacks;
};

struct FacePointer {
  enum class Kind : uint8_t { Down, Move, Up, Wheel, Leave } kind = Kind::Move;
  ui::Vec2 local{0.0f, 0.0f};   // face-local world units
  ui::Vec2 device{0.0f, 0.0f};  // device px (drags measure in these)
  float pxPerUnit = 1.0f;       // device px per world unit
  float uiScale = 1.0f;
  ui::Mods mods = ui::mod::kNone;
  uint8_t clicks = 1;
  ui::Vec2 wheel{0.0f, 0.0f};
};

class MetaFace {
public:
  static constexpr int32_t kNone = -1;   // outside the face
  static constexpr int32_t kFrame = -2;  // the face but no control: the block (select, move, double-click: expand)

  explicit MetaFace(const meta::Type& t) : type_(&t) {}
  virtual ~MetaFace() = default;
  MetaFace(const MetaFace&) = delete;
  MetaFace& operator=(const MetaFace&) = delete;

  const meta::Type& type() const noexcept { return *type_; }
  void bind(std::unique_ptr<MetaTarget> target, VoiceTapReader* voices);
  MetaTarget& target() const noexcept { return *target_; }
  VoiceTapReader* voices() const noexcept { return voices_; }

  // ---- geometry ----
  virtual ui::Vec2 worldSize() const noexcept = 0;
  // Where input i of n sits on the left edge / output j of n on the right edge (face-local y), when the host lays
  // its pins out by the face (a nested call's symbol). Default: 24 + 16·i.
  virtual float inputY(size_t i, size_t n) const noexcept;
  virtual float outputY(size_t j, size_t n) const noexcept;

  // ---- painting ----
  virtual uint32_t staticLayerCount() const noexcept = 0;
  virtual ui::Rgba staticLayerColour(const ui::Theme& theme, uint32_t layer) const noexcept = 0;
  // In white (the host tints the tile); value-independent.
  virtual void paintStatic(ui::FacePainter& p, uint32_t layer) const = 0;
  virtual void paintLive(ui::FacePainter& p, const FaceEnv& env) = 0;
  // The host's pins, as sockets on the face's edge (every face shares the look).
  void paintJacks(ui::FacePainter& p, const FaceEnv& env) const;
  // The face-local rects that change while voices sound (the host damages only these, ≤ 60 Hz).
  virtual void liveRects(std::vector<ui::Rect>& out) const = 0;
  // Whether the live layer moves now (a voice sounds): the host keeps its ≤ 60 Hz repaint going.
  virtual bool animating() const noexcept { return voiceCount_ > 0; }
  // Reads the voice reader into the face (the host calls it at most once per displayed frame, never from paint, so a
  // partial repaint of several rects draws one state); true when what the face shows changed.
  bool pollVoices();
  static constexpr uint32_t kMaxVoices = 16;
  std::span<const FaceVoice> sounding() const noexcept { return {voiceBuf_.data(), voiceCount_}; }

  // ---- interaction ----
  virtual int32_t hitTest(ui::Vec2 local, float pxPerUnit) const noexcept = 0;
  // true: consumed (a press on a control captures until Up). Down / Move / Up / Wheel / Leave.
  virtual bool onPointer(const FacePointer& e) = 0;
  virtual bool dragging() const noexcept = 0;
  virtual void cancelDrag() = 0;  // capture lost / Esc: the drag's edits are undone
  virtual ui::Cursor cursor(int32_t part) const noexcept;
  // The info view's text for a part (title, body).
  virtual void describe(int32_t part, std::string& title, std::string& body) const;
  int32_t hoveredPart() const noexcept { return hovered_; }
  // Returns whether it changed (the host repaints the face).
  bool setHoveredPart(int32_t part) noexcept;

protected:
  int32_t hovered_ = kNone;
  std::array<FaceVoice, kMaxVoices> voiceBuf_{};
  uint32_t voiceCount_ = 0;
  uint64_t voiceSerial_ = 0;

private:
  const meta::Type* type_;
  std::unique_ptr<MetaTarget> target_;
  VoiceTapReader* voices_ = nullptr;
};

// ---- registry ----
// The face of type `t` (null: the type has no face; it is drawn as a plain subcircuit block).
std::unique_ptr<MetaFace> makeFace(const meta::Type& t);
bool hasFace(const meta::Type& t) noexcept;
// worldSize of the type's face ({0, 0}: none), without making one.
ui::Vec2 faceSize(const meta::Type& t) noexcept;
// Registers a face factory for a type id (built-in faces register themselves; tests may add one).
using FaceFactory = std::unique_ptr<MetaFace> (*)(const meta::Type& t);
bool registerFace(std::string_view typeId, FaceFactory factory);

}  // namespace pq::views
