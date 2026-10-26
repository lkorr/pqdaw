#pragma once

// The detail view's Device tab (T4.6): device panels hosted in ShellSlot::DeviceDetail.
//
// Until T4.1 puts device chains into tracks, the tab shows one stand-alone device preview: View > Open Device…
// (`view.openDevice`) lists the stock devices (stdlib/devices/*.pqp), and the chosen one opens with its panel and plays
// through the circuit editor's PatchPlayer (the app's PatchPreview, T4.4) — fed by a short arpeggio, so an effect has a
// signal to work on:
//
//   generated host patch (DevicePreview::hostText):     preview_source (arpeggio voices) → voice_sum → the device → out
//     · the device's params are the host's params (same names; the panel's current values are their defaults, so a
//       (re)compile starts where the panel is), written live through PatchPlayer::setParam — no recompile;
//     · its structural consts are literal call arguments (QUESTIONS Q121): a change rewrites the host text through the
//       circuit editor's PatchEditor (re-parse with the previous ids → check → hot swap with state transfer and a
//       crossfade, SPEC §7.5) — the same path a text edit in the circuit editor takes;
//     · the activator bypasses the device (structural too: the host plays the source directly).
//
// Seam for T4.1 (device chains in the document): DevicePanel is the panel there too; its DeviceTarget becomes the
// document's device instance (params → param writes through ProjectLink, gestures → undo steps, consts → the
// instance's call arguments → that track's unit recompiles), and the Device tab shows the selected track's chain
// left → right instead of this preview. T4.7 adds the jacks' cables and the knobs' modulation rings through the hooks
// in device_panel.h.
//
// UI thread only.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/views/circuit/patch_editor.h"
#include "pq/views/circuit/patch_player.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/device_panel.h"

namespace pq::views {
class Shell;
}

namespace pq::views::devices {

struct DeviceEntry {
  std::string title;  // the panel's title (or the patch name)
  std::string path;   // the .pqp
};
// The devices in `dir` (stdlib/devices): every .pqp; titles from their panel descriptions. Sorted by title.
std::vector<DeviceEntry> listDevices(const std::string& dir);

class DevicePreview final : public DeviceTarget {
public:
  // Loads the device (the patch named like the file, else the file's first patch) and its panel description (or the
  // generated one). `player` may be null (no sound; the panel still works).
  static Result<std::unique_ptr<DevicePreview>> open(const std::string& devicePath, circuit::PatchPlayer* player);
  ~DevicePreview() override;

  const DeviceSignature& signature() const noexcept { return sig_; }
  const PanelDesc& panelDesc() const noexcept { return desc_; }
  const std::string& devicePath() const noexcept { return path_; }
  const std::map<std::string, double, std::less<>>& values() const noexcept { return values_; }

  // DeviceTarget
  void setParam(std::string_view param, double value) override;
  void setConst(std::string_view name, double value) override;
  void setActive(bool on) override;
  SpectrumFeed* outputSpectrum() override { return feed_.get(); }
  double sampleRate() const override { return player_ ? player_->sampleRate() : 48000.0; }

  // Playback through the player (hot swaps follow structural changes while it plays).
  Status play();
  void stop();
  bool playing() const noexcept;
  // The generated host patch's text (what plays), and the editor that compiles it.
  std::string hostText() const;
  circuit::PatchEditor& editor() noexcept { return *editor_; }
  struct Stats {
    uint64_t paramWrites = 0, recompiles = 0;
  };
  const Stats& stats() const noexcept { return stats_; }
  // Scenes and tests: a spectrum source of their own instead of the player's tap (a manual SpectrumFeed they step).
  void setSpectrumSource(std::unique_ptr<TapRing> tap, std::unique_ptr<SpectrumFeed> feed);

private:
  DevicePreview() = default;
  Status updateHost();  // regenerates the host text into the editor; checks (hot swap) while playing

  std::string path_;
  DeviceSignature sig_;
  PanelDesc desc_;
  const graph::Patch* devicePatch_ = nullptr;
  std::shared_ptr<const circuit::PatchDocument> doc_;
  std::unique_ptr<circuit::PatchEditor> editor_;
  circuit::PatchPlayer* player_ = nullptr;
  std::unique_ptr<SpectrumFeed> feed_;
  std::unique_ptr<TapRing> ownTap_;  // setSpectrumSource's tap (the destructor stops feed_ before it goes)
  std::map<std::string, double, std::less<>> values_;  // params and consts by name (internal units)
  bool active_ = true;
  bool playing_ = false;
  Stats stats_;
};

// The Device tab's content: a play strip and the panel of the open preview.
class DeviceView;

// Installs the Device tab content and the `view.openDevice` action (a menu of listDevices(dir)).
void installDeviceView(Shell& shell);
// Where the stock devices are (the app: stdlibDir()/devices) and what plays them (the app's PatchPreview; null: no
// sound). Call after installDeviceView.
void setDeviceLibrary(Shell& shell, std::string devicesDir, circuit::PatchPlayer* player);
// Opens a device in the Device tab (shows the detail view in Device mode). The panel, or null with `error` set.
DevicePanel* openDevice(Shell& shell, const std::string& devicePath, std::string* error = nullptr);
// The Device tab's open preview / panel (null when none).
DevicePreview* openPreview(Shell& shell) noexcept;
DevicePanel* openPanel(Shell& shell) noexcept;

// The device scenes (view_scenes.cpp: device-utility, device-saturator, device-eq-eight; `file` = the device's .pqp):
// the device opened with a few non-default settings; EQ Eight with a spectrum of a fixed test signal.
Status setupDeviceScene(Shell& shell, std::string_view file);

}  // namespace pq::views::devices
