#pragma once

// What faces bind to (TMM.4; pq/views/meta/meta_face.h):
//   DocTarget        one meta instance of the document, addressed by (device, name path; "" = a chain meta device
//                    itself): values from the snapshot (doc::metaParamValue), edits through doc::ops::setMetaParam in
//                    one store gesture per drag (one undo step; a drag never recompiles: params are runtime values)
//   SyntheticVoices  a VoiceTapReader that plays voices from a list (goldens, tests, benchmarks): fixed phases, or
//                    phases advanced by advance(seconds) at each voice's frequency with 8 sampled phases per 64-frame
//                    block at 48 kHz (what TMM.2's tap records). TMM.2's real reader replaces it in the app.
//
// UI thread only.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "pq/document/model.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views {

class ViewContext;

class DocTarget final : public MetaTarget {
public:
  DocTarget(ViewContext& ctx, doc::DeviceId device, std::string path, const meta::Type& type)
      : ctx_(ctx), device_(device), path_(std::move(path)), type_(type) {}
  ~DocTarget() override;
  double value(size_t param) const override;
  bool editable() const noexcept override;
  void begin(const char* label) override;
  void set(size_t param, double value) override;
  void end() override;
  void cancel() override;
  void reset(size_t param) override;
  // TMM.7: settings (one step each), data (doc::ops::setMetaData: inside a drag's gesture, else its own step), the
  // status line.
  double setting(size_t i) const override;
  void setSetting(size_t i, double value) override;
  meta::DataPtr data() const override;
  void setData(meta::DataPtr data) override;
  void notice(std::string_view message) override;
  const meta::Type* targetType() const noexcept override { return &type_; }
  doc::DeviceId device() const noexcept { return device_; }
  const std::string& path() const noexcept { return path_; }

private:
  ViewContext& ctx_;
  doc::DeviceId device_;
  std::string path_;
  const meta::Type& type_;
  bool gesture_ = false;
};

class SyntheticVoices final : public VoiceTapReader {
public:
  struct Voice {
    uint32_t slot = 0;
    double hz = 0.0;     // phase increment per second (0: fixed)
    double phase = 0.0;  // the current phase, 0..1
    double position = 0.0;  // TMM.7: the position reported with it (a wavetable's frame position)
    double increment = 0.0; // TMM.17: the increment reported with it (cycles per sample; kept by freeze())
  };
  void setVoices(std::vector<Voice> v);
  // TMM.17: the rate FaceVoice::rate reports (samples per second the increments are per; 0, the default: unknown).
  void setRate(double rate) noexcept { rate_ = rate; }
  const std::vector<Voice>& voicesNow() const noexcept { return voices_; }
  // Moves every voice on by `seconds` (records 8 phases per 64-frame block at 48 kHz, as the tap would).
  void advance(double seconds);
  // Stops every voice where it is (hz = 0), keeping the phases sampled so far for the next read (a still frame:
  // goldens).
  void freeze();
  uint32_t read(std::span<FaceVoice> out) override;
  uint64_t serial() const noexcept override { return serial_; }
  // Live (the host keeps polling) while a voice moves (hz != 0): fixed voices are read once and stay shown.
  bool live() const noexcept override;

private:
  std::vector<Voice> voices_;
  std::vector<std::vector<float>> pending_;  // per voice: the phases sampled since the last read
  uint64_t serial_ = 1;
  double rate_ = 0.0;
};

}  // namespace pq::views
