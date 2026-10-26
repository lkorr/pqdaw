// The generic device panel (T4.1): one control per param of the device patch, the structural settings first, laid out
// in columns that fill the chain's height. T4.6 replaces it per device with real panels (DevicePanelRegistry).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "../common/paint.h"
#include "internal.h"
#include "pq/ui/controls.h"
#include "pq/ui/theme.h"

namespace pq::views {

namespace {

using device_chain::kPanelPad;
using device_chain::kRowH;

constexpr float kLabelW = 76.0f;  // logical px
constexpr float kBoxW = 60.0f;
constexpr float kColGap = 10.0f;
constexpr float kColW = kLabelW + kBoxW;

// How a param's normalised value maps onto its number box (the box holds display-scale numbers).
struct ValueMap {
  enum class Kind : uint8_t { Linear, Db, Log } kind = Kind::Linear;
  double scale = 1.0;  // display = value · scale (Linear; Log shows exp(u) · scale)
  std::string unit;    // display suffix
  bool integral = false;  // a stepped param with whole-number steps: no decimals

  double toBox(double v) const noexcept {
    switch (kind) {
      case Kind::Db: return v > 1e-12 ? 20.0 * std::log10(v) : -240.0;
      case Kind::Log: return std::log(std::max(v, 1e-12));
      case Kind::Linear: break;
    }
    return v * scale;
  }
  double fromBox(double u) const noexcept {
    switch (kind) {
      case Kind::Db: return std::pow(10.0, u / 20.0);
      case Kind::Log: return std::exp(u);
      case Kind::Linear: break;
    }
    return u / scale;
  }
  // The text a box shows for its value u.
  std::string format(double u, double boxMin) const {
    char buf[48];
    if (kind == Kind::Db) {
      if (u <= boxMin + 1e-9 && boxMin <= -69.99) return "-inf dB";
      std::snprintf(buf, sizeof buf, "%.1f dB", u);
      return buf;
    }
    double d = kind == Kind::Log ? std::exp(u) * scale : u;
    std::string unitText = unit;
    if (unit == "Hz" && std::fabs(d) >= 999.5) {
      d /= 1000.0;
      unitText = "kHz";
    }
    const double a = std::fabs(d);
    const int decimals = integral ? 0 : a >= 99.95 ? 0 : a >= 9.995 ? 1 : 2;
    std::snprintf(buf, sizeof buf, "%.*f", decimals, d);
    std::string s = buf;
    if (!unitText.empty()) s += (unitText == "%" ? "" : " ") + unitText;
    return s;
  }
  // A typed value (display scale, optional unit suffix) → the box value.
  std::optional<double> parse(std::string_view t) const {
    std::string s(t);
    const size_t end = s.find_first_not_of(" \t+-.0123456789eE");
    std::string num = s.substr(0, end);
    std::string suffix = end == std::string::npos ? std::string() : s.substr(end);
    suffix.erase(0, suffix.find_first_not_of(' '));
    if (kind == Kind::Db && (s.find("inf") != std::string::npos)) return -240.0;
    char* stop = nullptr;
    const double x = std::strtod(num.c_str(), &stop);
    if (num.empty() || stop == num.c_str() || !std::isfinite(x)) return std::nullopt;
    double d = x;
    if (suffix == "k" || suffix == "kHz") d *= 1000.0;
    switch (kind) {
      case Kind::Db: return d;
      case Kind::Log: return d / scale > 0.0 ? std::optional<double>(std::log(d / scale)) : std::nullopt;
      case Kind::Linear: break;
    }
    return d;
  }
};

ValueMap mapOf(const doc::DeviceParamInfo& p) {
  ValueMap m;
  using graph::Unit;
  switch (p.unit) {
    case Unit::dB: m.kind = ValueMap::Kind::Db; m.unit = "dB"; break;
    case Unit::percent: m.scale = 100.0; m.unit = "%"; break;
    case Unit::ms: m.scale = 1000.0; m.unit = "ms"; break;
    case Unit::s: m.scale = 1000.0; m.unit = "ms"; break;  // seconds read better as ms at device scales
    case Unit::Hz:
    case Unit::kHz: m.unit = "Hz"; break;
    case Unit::cents: m.unit = "c"; break;
    default: break;
  }
  if (m.kind == ValueMap::Kind::Linear && p.exponential && p.hasRange && p.min > 0.0) m.kind = ValueMap::Kind::Log;
  return m;
}

// One labelled row: a label and its control.
class ParamRow : public ui::Widget {
public:
  ParamRow(std::string name, std::string label, std::string info) : Widget(std::move(name)), label_(std::move(label)), info_(std::move(info)) {
    setLayout(ui::LayoutKind::Row);
  }
  ui::InfoText info() const override { return {label_, info_}; }
  void paint(ui::PaintContext& ctx) override {
    if (!ctx.res.theme || !ctx.res.uiFonts) return;
    const ui::IRect r = rect();
    const ui::IRect lab{r.x, r.y, ctx.scale.px(kLabelW) - ctx.scale.px(4.0f), r.h};
    paint::text(ctx, ctx.res.uiFonts->small, lab, label_, ctx.res.theme->text.secondary, ui::TextAlign::Left);
  }

private:
  std::string label_, info_;
};

class GenericPanel final : public DevicePanel {
public:
  explicit GenericPanel(DeviceHost& host) : DevicePanel("panel"), host_(host) {
    setLayout(ui::LayoutKind::Custom);
    const doc::DeviceInfo* info = host.info();
    if (info == nullptr) return;
    for (const doc::DeviceSettingInfo& s : info->settings) addSetting(s);
    for (const doc::DeviceParamInfo& p : info->params) addParam(p);
    if (const doc::Device* d = host.current()) refresh(*d);
  }

  void refresh(const doc::Device& d) override {
    for (Entry& e : entries_) show(d, e);
  }

  void paramsChanged(const doc::Device& d, std::span<const doc::ParamRef> params) override {
    for (const doc::ParamRef& r : params) {
      if (r.kind != doc::ParamKind::DeviceParam || r.owner != d.id.value) continue;
      for (Entry& e : entries_)
        if (!e.setting && e.hash == r.target) show(d, e);  // only that control repaints (setValue damages it)
    }
  }

  float preferredWidth(float height) const override {
    if (entries_.empty()) return 150.0f;
    const auto cols = static_cast<float>(columns(height));
    return 2.0f * kPanelPad + cols * kColW + (cols - 1.0f) * kColGap;
  }

  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    const float s = pass.scale();
    const uint32_t perCol = rowsPerColumn(static_cast<float>(r.h) / s);
    const int32_t pad = pass.px(kPanelPad), rowH = pass.px(kRowH), colW = pass.px(kColW), gap = pass.px(kColGap);
    for (size_t i = 0; i < rows_.size(); ++i) {
      const auto c = static_cast<int32_t>(i / perCol), k = static_cast<int32_t>(i % perCol);
      pass.place(*rows_[i], {r.x + pad + c * (colW + gap), r.y + pad + k * rowH, colW, rowH});
    }
  }

  void paint(ui::PaintContext& ctx) override {
    if (!entries_.empty() || !ctx.res.theme || !ctx.res.uiFonts) return;
    const std::string msg = host_.current() ? "'" + host_.current()->patch + "' is not available: bypassed" : std::string();
    paint::text(ctx, ctx.res.uiFonts->small, paint::inset(rect(), ctx.scale.px(kPanelPad)), msg,
                ctx.res.theme->text.secondary, ui::TextAlign::Left);
  }

private:
  struct Entry {
    std::string name;
    bool setting = false;
    uint32_t hash = 0;
    const doc::DeviceParamInfo* param = nullptr;
    const doc::DeviceSettingInfo* settingInfo = nullptr;
    ValueMap map;
    ui::ValueControl* box = nullptr;
    ui::Toggle* toggle = nullptr;
  };

  uint32_t rowsPerColumn(float heightLogical) const noexcept {
    return std::max<uint32_t>(1, static_cast<uint32_t>((heightLogical - 2.0f * kPanelPad) / kRowH));
  }
  uint32_t columns(float heightLogical) const noexcept {
    const uint32_t per = rowsPerColumn(heightLogical);
    return static_cast<uint32_t>((rows_.size() + per - 1) / per);
  }

  ParamRow& row(const std::string& name, const std::string& label, const std::string& info) {
    auto& r = add<ParamRow>(name, label, info);
    r.add<ui::Widget>().setWidth(ui::Length::fixed(kLabelW));  // the label's space (painted by the row)
    rows_.push_back(&r);
    return r;
  }

  void addSetting(const doc::DeviceSettingInfo& s) {
    ParamRow& r = row("s:" + s.name, s.name, "A structural setting: a change recompiles the track's device chain (crossfaded).");
    ui::ValueSpec spec;
    spec.min = std::min(0.0, s.defaultValue);
    spec.max = std::max(16.0, s.defaultValue);
    spec.def = s.defaultValue;
    spec.step = 1.0;
    spec.decimals = 0;
    spec.dragPixels = 160.0;
    auto& box = r.add<ui::DragBox>("value", spec);
    box.setWidth(ui::Length::fixed(kBoxW));
    Entry e;
    e.name = s.name;
    e.setting = true;
    e.settingInfo = &s;
    e.box = &box;
    const size_t index = entries_.size();
    // Applied once, when the gesture ends: a setting change compiles, so never one compile per drag step.
    box.onGestureEnd = [this, index] {
      const Entry& en = entries_[index];
      host_.setSetting(en.name, en.box->value() == en.settingInfo->defaultValue && en.settingInfo->hasDefault
                                    ? std::nullopt
                                    : std::optional<double>(en.box->value()));
    };
    box.onGestureCancel = [] {};
    entries_.push_back(std::move(e));
  }

  void addParam(const doc::DeviceParamInfo& p) {
    ParamRow& r = row("p:" + p.name, p.label, "Parameter '" + p.name + "' of the device (a value change is a param write: no compile).");
    Entry e;
    e.name = p.name;
    e.hash = doc::deviceParamHash(p.name);
    e.param = &p;
    const size_t index = entries_.size();
    if (p.kind == graph::Kind::Bool) {
      auto& t = r.add<ui::Toggle>("value", "", ui::ToggleStyle::Accent);
      t.setWidth(ui::Length::fixed(kRowH - 4.0f));
      t.setHeight(ui::Length::fixed(kRowH - 4.0f));
      r.setAlign(ui::Align::Start, ui::Align::Center);
      t.onToggle = [this, index](bool on) { host_.setParam(entries_[index].name, on ? 1.0 : 0.0); };
      e.toggle = &t;
    } else {
      e.map = mapOf(p);
      ui::ValueSpec spec;
      const double lo = p.hasRange ? p.min : p.defaultValue - 1e6, hi = p.hasRange ? p.max : p.defaultValue + 1e6;
      spec.min = e.map.toBox(lo);
      spec.max = e.map.toBox(hi);
      if (e.map.kind == ValueMap::Kind::Db) spec.min = std::max(spec.min, -70.0);
      spec.def = std::clamp(e.map.toBox(p.defaultValue), spec.min, spec.max);
      if (p.steps >= 2 && e.map.kind == ValueMap::Kind::Linear) {
        spec.step = (spec.max - spec.min) / (p.steps - 1);
        e.map.integral = spec.step == std::floor(spec.step) && spec.min == std::floor(spec.min);
      }
      spec.decimals = 2;
      auto& box = r.add<ui::DragBox>("value", spec);
      box.setWidth(ui::Length::fixed(kBoxW));
      const ValueMap map = e.map;
      const double boxMin = spec.min;
      box.formatter = [map, boxMin](double u) { return map.format(u, boxMin); };
      box.parser = [map](std::string_view t) { return map.parse(t); };
      box.onGestureBegin = [this, index] { host_.beginParam(entries_[index].name); };
      box.onChange = [this, index](double u) {
        const Entry& en = entries_[index];
        const bool atFloor = en.map.kind == ValueMap::Kind::Db && u <= en.box->spec().min + 1e-9 && en.param->hasRange;
        host_.setParam(en.name, atFloor ? en.param->min : en.map.fromBox(u));
      };
      box.onGestureEnd = [this] { host_.endParam(); };
      box.onGestureCancel = [this] { host_.cancelParam(); };
      box.setSpec(box.spec());  // re-formats the shown text with the formatter
      e.box = &box;
    }
    entries_.push_back(std::move(e));
  }

  void show(const doc::Device& d, Entry& e) {
    if (e.setting) {
      if (!e.box->inGesture()) e.box->setValue(doc::deviceSettingValue(d, *e.settingInfo));
      return;
    }
    const double v = doc::deviceParamValue(d, *e.param);
    if (e.toggle != nullptr) {
      e.toggle->setOn(v >= 0.5);
    } else if (!e.box->inGesture()) {
      e.box->setValue(e.map.toBox(v));
    }
  }

  DeviceHost& host_;
  std::vector<Entry> entries_;
  std::vector<ui::Widget*> rows_;
};

}  // namespace

std::unique_ptr<DevicePanel> makeGenericPanel(DeviceHost& host) { return std::make_unique<GenericPanel>(host); }

}  // namespace pq::views
