#pragma once

// GU.3: what the element kinds share (private to pq_views: elements.cpp, curve_element.cpp).

#include <bit>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/views/panel/element.h"

namespace pq::views::devices {
class SpectrumFeed;
}

namespace pq::views::panel {

// GU.7 / DY.3: the newest kFrames frames (50 ms at 48 kHz) of a Signal binding's analysis feed (its tap's signal
// channels), for the meter and the transfer display's live level. The feed is acquired on the first paint (acquire)
// and released when the element goes (or its binding's feed changes): an element that is never painted costs nothing.
// UI thread only.
class SignalWindow {
public:
  static constexpr uint32_t kFrames = 2400;
  SignalWindow() = default;
  SignalWindow(const SignalWindow&) = delete;
  SignalWindow& operator=(const SignalWindow&) = delete;
  ~SignalWindow() { release(); }
  void acquire(PanelTarget& t, const doc::Binding* b);
  void release();
  // Element::pollVoices: reads the newest window when the feed moved (true; channel(0 / 1) then hold frames()
  // frames); false: nothing new.
  bool poll(PanelTarget& t, const doc::Binding* b);
  std::span<const float> channel(int c) const noexcept;
  // Element::voiceReader / animating.
  VoiceTapReader* reader(PanelTarget& t, const doc::Binding* b) const;
  bool live() const noexcept;
  bool acquired() const noexcept { return feed_ != nullptr; }

private:
  struct Reader final : VoiceTapReader {
    devices::SpectrumFeed* feed = nullptr;
    uint32_t read(std::span<FaceVoice>) override { return 0; }
    uint64_t serial() const noexcept override { return 0; }
    bool live() const noexcept override;
  };
  static devices::SpectrumFeed* feedOf(PanelTarget& t, const doc::Binding* b);
  devices::SpectrumFeed* feed_ = nullptr;
  mutable Reader reader_;
  uint64_t end_ = 0;
  uint32_t got_ = 0, left_ = 0, right_ = 1;
  std::vector<float> buf_[4];
};

// "a|b|c" → {"a", "b", "c"} ("" → none).
std::vector<std::string> splitItems(std::string_view s);

// An element bound to values: their positions, a relative drag measured in device px from the press, one gesture.
class ValueElement : public Element {
public:
  using Element::Element;
  static constexpr size_t kSlots = 2;
  bool dragging() const noexcept override { return drag_; }
  void cancelDrag(PanelTarget& t) override;
  void describe(const PanelTarget& t, std::string& title, std::string& body) const override;
  void endWheel(PanelTarget& t) override;

protected:
  double pos(const PanelTarget& t, size_t slot = 0) const;
  const BoundInfo& bound(const PanelTarget& t, size_t slot = 0) const;  // an unbound slot: an invalid one
  bool editable(const PanelTarget& t, size_t slot = 0) const;
  void startDrag(PanelTarget& t, const FacePointer& e, const char* label);
  void endDrag(PanelTarget& t);
  // A stepped binding's position rounded to its steps.
  double quantise(const PanelTarget& t, double p, size_t slot = 0) const;
  // The slot's position after a relative drag (dx, dy: the axis's weight; `axisPx` device px per whole range), clamped.
  double dragged(const PanelTarget& t, const FacePointer& e, size_t slot, float dx, float dy, float axisPx) const;
  // TX.detent (pq/dsp/curve.h "Detents"): the position `px` device px of drag from the press (`axisPx` per whole range;
  // Shift: a tenth, no detents), held at the bound curve's detents; not clamped. detentZone: the sticky zone's half
  // width in position. stepped: a step from → to that crosses a detent lands on it.
  double travelled(const PanelTarget& t, const FacePointer& e, size_t slot, double px, double axisPx) const;
  static double detentZone(const FacePointer& e, double axisPx) noexcept;
  double stepped(const PanelTarget& t, double from, double to, size_t slot = 0) const;
  virtual std::string hint() const { return {}; }
  // GU.13: the DEVICE-PANEL GESTURES (FacePointer::valueGestures; the old device panel's knobs): a wheel notch steps the
  // binding by stepOf (a run of notches is one gesture: endWheel ends it), and after a press the keys — Delete /
  // Backspace: the default, Up / Down: a step (Shift: fine). true: used.
  bool wheelStep(const FacePointer& e, PanelTarget& t);
  bool valueKey(const ui::KeyEvent& e, PanelTarget& t);
  double stepOf(const PanelTarget& t, bool fine) const;
  // Called first by a kind's onPointer: notes whether its host allows the gestures (a press) and ends a wheel run.
  void noteGestures(const FacePointer& e, PanelTarget& t);

  bool drag_ = false;
  bool wheel_ = false, keys_ = false;
  double wheelPos_ = 0.0;
  ui::Vec2 startDev_{0.0f, 0.0f};
  double start_[kSlots] = {0.0, 0.0};
};

// GU.5b: the rich kinds (rich_elements.cpp: dial, arc, orbit), registered by registerGeneric through `reg`.
void registerRichElements(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                      std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// GU.6: the embedded child panel (embed_element.cpp, kind doc::kEmbedKind).
void registerEmbedElement(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                      std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// GU.7: the native engines' faces split into elements (engine_elements.cpp: drawtools, harmonics, wavetable, warpmode,
// envelope, scope, spectrum, spectrogram) and the meter.
void registerEngineElements(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                        std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// EQ.4: the response and phase displays and the band type chooser (response_elements.cpp).
void registerResponseElements(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                          std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// DT.1: the tempo slider (tempo_slider.cpp, kind "temposlider").
void registerTempoSlider(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                     std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// DY.2: the cable source picker (cable_source.cpp, kind "cablesource").
void registerCableSource(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                     std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// DY.3: the transfer curve display (transfer_element.cpp, kind "transfer").
void registerTransferElement(void (*reg)(std::string_view kind, std::string_view label, std::span<const StyleSpec> style,
                                         std::span<const std::string_view> slots, float w, float h, ElementFactory f));

// The curve editor (curve_element.cpp).
std::span<const StyleSpec> curveElementStyle() noexcept;
std::unique_ptr<Element> makeCurveElement(const doc::PanelElement& desc);

}  // namespace pq::views::panel
