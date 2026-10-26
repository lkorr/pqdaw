#pragma once

// Device panel internals shared by device_panel.cpp (frame, body, controls, bindings) and device_displays.cpp
// (transfer curve, EQ display, spectrum). Private to pq_views.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/device_panel.h"

namespace pq::views::devices {

using NameMap = std::map<std::string, std::string, std::less<>>;

class PanelDisplay;

struct DevicePanel::Impl {
  explicit Impl(DevicePanel& p) : panel(p) {}

  DevicePanel& panel;
  std::map<std::string, double, std::less<>> params, consts;
  std::map<std::string, int32_t, std::less<>> selection;  // selector → index
  NameMap selValues;                                        // selector → its current value (for templates)

  // A control bound to a param or const (or a display / tabs / label).
  struct Bound {
    const ControlDesc* desc = nullptr;
    ui::Widget* widget = nullptr;
    PanelDisplay* display = nullptr;
    std::string param, constName;  // resolved now
  };
  std::vector<Bound> bound;
  bool suppress = false;  // set while the panel updates its own controls (their callbacks must not echo)

  // "name" → a param, "#name" → a const; templates resolved.
  double value(std::string_view name) const;
  std::string resolve(std::string_view templ) const;
  const ParamInfo* paramInfo(std::string_view resolved) const { return panel.sig_.findParam(resolved); }

  // User edits from a control (source: that control, which shows the value already).
  void userParamBegin(const std::string& name);
  void userParam(const std::string& name, double v, ui::Widget* source);
  void userParamEnd(const std::string& name);
  void userConst(const std::string& name, double v, ui::Widget* source);
  void userSelect(const std::string& selector, int32_t index);
  void userActive(bool on);
  // Shows a value change on every control and display that reads `name` (except `source`).
  void changed(std::string_view name, bool isConst, ui::Widget* source);
  // Rebinds every control after a selector change; re-evaluates visibility.
  void bindAll();
  void bindOne(Bound& b);
  void refreshVisibility();
  bool conditionsHold(const ControlDesc& d) const;
  SpectrumFeed* spectrum() const { return panel.target_ ? panel.target_->outputSpectrum() : nullptr; }
  double sampleRate() const { return panel.target_ ? panel.target_->sampleRate() : 48000.0; }
};

// A display in the panel body.
class PanelDisplay : public ui::Widget {
public:
  PanelDisplay(std::string name, DevicePanel::Impl& impl, const ControlDesc& desc) : Widget(std::move(name)), impl_(impl), desc_(desc) {}
  // Selectors changed: re-resolve the bindings.
  virtual void rebind() {}
  // `name` (resolved; consts as "#name") changed.
  virtual bool dependsOn(std::string_view name) const = 0;
  virtual void valuesChanged() { markDirty(); }

protected:
  DevicePanel::Impl& impl_;
  const ControlDesc& desc_;
};

std::unique_ptr<PanelDisplay> makeDisplay(DevicePanel::Impl& impl, const ControlDesc& desc);

// True when `w` and every ancestor are visible and it is in a tree.
bool shownInTree(const ui::Widget& w) noexcept;

}  // namespace pq::views::devices
