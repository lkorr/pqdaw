#pragma once

// Device panel internals shared by device_panel.cpp (frame, body, controls, bindings) and face_display.cpp (a native
// engine's face in the body). Private to pq_views.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/device_panel.h"

namespace pq::views::devices {

class PanelDisplay;

struct DevicePanel::Impl {
  explicit Impl(DevicePanel& p) : panel(p) {}

  DevicePanel& panel;
  std::map<std::string, double, std::less<>> params, consts;

  // A control bound to a param or const (or a face).
  struct Bound {
    const ControlLayout* desc = nullptr;
    ui::Widget* widget = nullptr;
    PanelDisplay* display = nullptr;
    std::string param, constName;
  };
  std::vector<Bound> bound;
  bool suppress = false;  // set while the panel updates its own controls (their callbacks must not echo)

  // "name" → a param, "#name" → a const.
  double value(std::string_view name) const;
  const ParamInfo* paramInfo(std::string_view resolved) const { return panel.sig_.findParam(resolved); }

  // User edits from a control (source: that control, which shows the value already).
  void userParamBegin(const std::string& name);
  void userParam(const std::string& name, double v, ui::Widget* source);
  void userParamEnd(const std::string& name);
  void userConst(const std::string& name, double v, ui::Widget* source);
  void userActive(bool on);
  // Shows a value change on every control and display that reads `name` (except `source`).
  void changed(std::string_view name, bool isConst, ui::Widget* source);
  void bindAll();
  void bindOne(Bound& b);
  DeviceTarget* target() const noexcept { return panel.target_; }  // TMM.10
};

// A face in the panel body.
class PanelDisplay : public ui::Widget {
public:
  PanelDisplay(std::string name, DevicePanel::Impl& impl, const ControlLayout& desc) : Widget(std::move(name)), impl_(impl), desc_(desc) {}
  virtual void rebind() {}
  // `name` (resolved; consts as "#name") changed.
  virtual bool dependsOn(std::string_view name) const = 0;
  virtual void valuesChanged() { markDirty(); }
  // TMM.10: the device-px rect of the part that edits `param` (a face's control), or nullopt.
  virtual std::optional<ui::IRect> paramRect(std::string_view /*param*/) const { return std::nullopt; }
  // TMM.10: a face's voices may have become live.
  virtual void wake() {}

protected:
  DevicePanel::Impl& impl_;
  const ControlLayout& desc_;
};

// TMM.10 (face_display.cpp): a meta module's face in the panel ("face" controls; a chain meta device's whole body).
std::unique_ptr<PanelDisplay> makeFaceDisplay(DevicePanel::Impl& impl, const ControlLayout& desc);

// True when `w` and every ancestor are visible and it is in a tree.
bool shownInTree(const ui::Widget& w) noexcept;

}  // namespace pq::views::devices
