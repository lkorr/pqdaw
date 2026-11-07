// GU.3: the document's PanelTarget (pq/views/panel/group_panel_face.h makeDocPanelTarget): what a group panel's
// bindings resolve to and how its elements edit them.
//
//   resolve   once per snapshot the group's track's blocks are indexed by id (one walk of one list; every binding of the
//             panel is inside the group, so in that list); a binding's BoundInfo is cached per binding and redone in
//             place when the snapshot changed (references stay valid); signatures are cached per patch.
//   control   a param of a block (its patch's ParamInfo: label, steps, bool; the device's own curve else the declared
//             one), or of a nested meta module (meta path: the type's ParamSpec and declared curve). Positions are what
//             the document stores (GU.1). A drag is one store gesture of param writes (one undo step, no compile).
//   setting   an instrument group's polyphony (doc::groupSettings), a patch's `real const` input, a meta module's
//             setting. Inside a gesture its positions are a preview; end() commits it once (one Structural step: one
//             recompile).
//   curve     ops::setParamCurve inside the gesture (no compile within the curve's class).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"
#include "pq/views/devices/device_model.h"
#include "pq/document/panel.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/panel/group_panel_face.h"
#include "pq/views/view_context.h"

namespace pq::views::panel {

namespace {

std::string fmt(const char* f, double v) {
  char b[64];
  std::snprintf(b, sizeof b, f, v);
  return b;
}

// A value in a curve's units as text (for bindings without a patch ParamInfo: meta params, settings).
std::string formatUnits(const dsp::Curve& c, double v) {
  auto at = [](double x, double t) { return x >= t * (1.0 - 1e-9); };
  switch (c.unit) {
    case dsp::CurveUnit::Hz:
      if (at(v, 10000.0)) return fmt("%.1f kHz", v / 1000.0);
      if (at(v, 1000.0)) return fmt("%.2f kHz", v / 1000.0);
      return fmt(at(v, 100.0) ? "%.0f Hz" : "%.1f Hz", v);
    case dsp::CurveUnit::Db: return fmt("%.1f dB", v);
    case dsp::CurveUnit::Percent: return fmt("%.0f %%", v * 100.0);
    case dsp::CurveUnit::Seconds: return at(v, 1.0) ? fmt("%.2f s", v) : fmt("%.0f ms", v * 1000.0);
    case dsp::CurveUnit::Cents: return fmt("%.0f c", v);
    case dsp::CurveUnit::None: break;
  }
  if (c.step >= 1.0) return fmt("%.0f", v);
  return fmt(std::fabs(v) >= 10.0 ? "%.1f" : "%.2f", v);
}

dsp::Curve linearOver(double min, double max, double step) {
  dsp::Curve c = dsp::curvePreset(dsp::CurvePreset::Linear, min, max);
  c.step = step;
  return c;
}

double settingValue(const dsp::Curve& c, double position) noexcept { return dsp::evalCurve(c, std::clamp(position, 0.0, 1.0)); }

// GU.7: the display state of an analyser shown on a signal (no engine block holds it): the type's defaults, edited in
// place (not saved, not undoable).
class LocalTarget final : public MetaTarget {
public:
  explicit LocalTarget(const meta::Type& t) : type_(&t), data_(t.defaultData ? t.defaultData() : nullptr) {}
  const meta::Type* targetType() const noexcept override { return type_; }
  double value(size_t param) const override { return param < type_->params.size() ? type_->params[param].def : 0.0; }
  void begin(const char*) override {}
  void set(size_t, double) override {}
  void end() override {}
  void cancel() override {}
  void reset(size_t) override {}
  meta::DataPtr data() const override { return data_; }
  void setData(meta::DataPtr d) override {
    if (d) data_ = std::move(d);
  }

private:
  const meta::Type* type_;
  meta::DataPtr data_;
};

class DocPanelTarget final : public PanelTarget {
public:
  DocPanelTarget(ViewContext& ctx, doc::DeviceId group, std::function<ui::Widget*()> host)
      : ctx_(ctx), group_(group), host_(std::move(host)) {}
  ~DocPanelTarget() override { cancel(); }

  const BoundInfo& info(const doc::Binding& b) const override { return resolved(b).info; }

  double position(const doc::Binding& b) const override {
    for (const Pending& p : pending_)
      if (p.b == b) return p.position;
    const Resolved& r = resolved(b);
    if (!r.info.valid || r.device == nullptr) return 0.0;
    const doc::Device& d = *r.device;
    switch (r.how) {
      case How::Param: return devices::storedPosition(r.param, d);
      case How::MetaParam: return doc::metaParamPosition(d, b.meta, *r.type, r.index);
      case How::GroupSetting:
      case How::Const:
      case How::MetaSetting: {
        double v = r.def;
        if (r.how == How::MetaSetting) {
          if (const doc::MetaInstance* m = d.metaAt(b.meta))
            if (const doc::DeviceValue* x = m->setting(b.name)) v = x->value;
        } else if (const doc::DeviceValue* x = d.setting(b.name)) {
          v = x->value;
        }
        return dsp::curvePosition(r.info.curve, v, 0.0);
      }
      case How::None: break;
    }
    return 0.0;
  }

  std::string format(const doc::Binding& b, double position) const override {
    const Resolved& r = resolved(b);
    if (!r.info.valid) return "—";
    const double p = std::clamp(position, 0.0, 1.0);
    if (r.how == How::Param) {
      devices::ParamInfo pi = r.param;
      pi.curve = r.info.curve;
      // A curve that names its own unit (a value block's curve set to Hz, %) reads in it; else the param's display.
      if (pi.unit == devices::DisplayUnit::None && r.info.curve.unit != dsp::CurveUnit::None && !pi.isBool)
        return formatUnits(r.info.curve, dsp::evalCurveUnits(r.info.curve, p));
      return devices::formatValue(pi, devices::fromPosition(pi, p));
    }
    const double v = r.how == How::MetaParam ? dsp::evalCurveUnits(r.info.curve, p) : settingValue(r.info.curve, p);
    if (!r.info.choices.empty()) {
      const size_t i = static_cast<size_t>(std::clamp<long>(std::lround(v - r.info.curve.min), 0, static_cast<long>(r.info.choices.size()) - 1));
      return r.info.choices[i];
    }
    if (r.info.isSwitch) return v >= 0.5 ? "On" : "Off";
    std::string s = formatUnits(r.info.curve, v);
    if (!r.unit.empty()) s += " " + r.unit;
    return s;
  }

  std::optional<double> parse(const doc::Binding& b, std::string_view text) const override {
    const Resolved& r = resolved(b);
    if (!r.info.valid) return std::nullopt;
    const double hint = position(b);
    if (r.how == How::Param && r.info.curve.min == r.param.declared.min && r.info.curve.max == r.param.declared.max &&
        r.info.curve.unit == r.param.declared.unit) {  // the declared range and unit: the param's own reader
      devices::ParamInfo pi = r.param;
      pi.curve = r.info.curve;
      const std::optional<double> v = devices::parseValue(pi, text);
      if (!v) return std::nullopt;
      return devices::toPosition(pi, *v, hint);
    }
    for (size_t i = 0; i < r.info.choices.size(); ++i)
      if (r.info.choices[i] == text) return dsp::curvePosition(r.info.curve, r.info.curve.min + static_cast<double>(i), hint);
    // A number in the curve's units, with an optional suffix: k (× 1000), ms (seconds), % (a fraction).
    const std::string s(text);
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return std::nullopt;
    std::string_view rest(end);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    if (rest.starts_with("k") || rest.starts_with("K")) v *= 1000.0;
    else if (rest.starts_with("ms")) v /= 1000.0;
    else if (r.info.curve.unit == dsp::CurveUnit::Percent) v /= 100.0;
    // (a value in the curve's own units: a dB curve's output conversion applies after it)
    return dsp::curvePosition(r.info.curve, dsp::curveOutput(r.info.curve, v), hint);
  }

  void begin(const doc::Binding& b, const char* label) override {
    const Resolved& r = resolved(b);
    if (!r.info.valid) return;
    inGesture_ = true;
    if (r.info.setting) return;  // a preview until end()
    if (!storeGesture_ && !ctx_.store().inGesture()) storeGesture_ = ctx_.store().beginGesture(label).ok();
  }

  void setPosition(const doc::Binding& b, double position) override {
    const Resolved& r = resolved(b);
    if (!r.info.valid) return;
    position = std::clamp(position, 0.0, 1.0);
    if (r.info.setting) {
      if (!inGesture_) {
        setOnce(b, position, "Panel Setting");
        return;
      }
      for (Pending& p : pending_)
        if (p.b == b) {
          p.position = position;
          return;
        }
      pending_.push_back({b, position});
      return;
    }
    dispatch(paramEdit(b, r, position));
  }

  void setCurve(const doc::Binding& b, const dsp::Curve& c) override {
    const Resolved& r = resolved(b);
    const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
    if (!r.info.valid || r.how != How::Param || lib == nullptr) {
      notice("Only a block's control has a curve here");
      return;
    }
    dispatch(doc::ops::setParamCurve(*ctx_.doc(), *lib, b.device, b.name, c));
  }

  void end() override {
    inGesture_ = false;
    if (!pending_.empty()) {
      // The settings previewed: one step (inside the store gesture when controls joined it).
      doc::Batch batch{"Panel Setting", {}};
      for (const Pending& p : pending_) {
        const Resolved& r = resolved(p.b);
        if (r.info.valid)
          if (doc::ops::EditResult e = settingEdit(p.b, r, p.position); e && !e->empty()) batch.commands.push_back(e->command);
      }
      pending_.clear();
      if (!batch.commands.empty()) {
        if (Status s = ctx_.store().dispatch(batch.commands.size() == 1 ? batch.commands.front() : doc::Command(std::move(batch))); !s)
          notice(s.error().message);
      }
    }
    if (storeGesture_) {
      storeGesture_ = false;
      if (ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
    }
  }

  void cancel() override {
    inGesture_ = false;
    pending_.clear();
    if (storeGesture_) {
      storeGesture_ = false;
      if (ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
    }
  }

  void setOnce(const doc::Binding& b, double position, const char* /*label*/) override {
    const Resolved& r = resolved(b);
    if (!r.info.valid) return;
    position = std::clamp(position, 0.0, 1.0);
    dispatch(r.info.setting ? settingEdit(b, r, position) : paramEdit(b, r, position));
  }

  void reset(const doc::Binding& b) override {
    const Resolved& r = resolved(b);
    if (!r.info.valid) return;
    const doc::Project& p = *ctx_.doc();
    switch (r.how) {
      case How::Param: dispatch(doc::ops::setDeviceParam(p, b.device, b.name, std::nullopt)); break;
      case How::MetaParam: dispatch(doc::ops::setMetaParam(p, b.device, b.meta, r.type->id, b.name, std::nullopt)); break;
      case How::GroupSetting:
      case How::Const: dispatch(doc::ops::setDeviceSetting(p, b.device, b.name, std::nullopt)); break;
      case How::MetaSetting: dispatch(doc::ops::setMetaSetting(p, b.device, b.meta, r.type->id, b.name, std::nullopt)); break;
      case How::None: break;
    }
  }

  void bindingsChanged() override { cache_.clear(); }  // (keyed by the elements' binding addresses)
  uint64_t revision() const override {
    sync();
    return rev_;
  }
  void notice(std::string_view message) override { ctx_.setStatus(std::string(message)); }
  // GU.6: a child group's panel (the group's track's list, indexed once per snapshot).
  doc::PanelPtr childPanel(const doc::Binding& b) const override {
    sync();
    const doc::Device* d = device(b.device);
    return d != nullptr && doc::isGroupDevice(*d) && d->hasPanel() ? d->panel : nullptr;
  }
  ui::Widget* menuHost() const noexcept override { return host_ ? host_() : nullptr; }

  // GU.7: voices of a Signal binding (the lowering's voice tap while a display shows it), of a Data binding (the
  // engine's own: the Wavetable's frame-table tap, the Envelope's voice tap).
  VoiceTapReader* voices(const doc::Binding& b) override {
    if (b.kind == doc::BindingKind::Signal) return ctx_.faceVoices(b.device, doc::tapPath(doc::ElementTap::Voice, b.name));
    if (b.kind == doc::BindingKind::Data) return ctx_.faceVoices(b.device, b.meta);
    return nullptr;
  }
  devices::SpectrumFeed* signalFeed(const doc::Binding& b) override {
    if (b.kind == doc::BindingKind::Signal) return ctx_.analysisFeed(b.device, doc::tapPath(doc::ElementTap::Signal, b.name));
    if (b.kind == doc::BindingKind::Data && b.meta.empty()) return ctx_.analysisFeed(b.device);
    return nullptr;
  }
  std::shared_ptr<MetaFace> engineFace(const doc::Binding& b, std::string_view typeId, FaceHost* host) override {
    const bool signal = b.kind == doc::BindingKind::Signal;
    if (!signal && b.kind != doc::BindingKind::Data) return nullptr;
    const meta::Type* type = signal ? meta::findType(typeId) : metaTypeAt(ctx_, b.device, b.meta);
    if (type == nullptr || (!typeId.empty() && type->id != typeId)) return nullptr;
    std::string key = std::to_string(b.device.value) + (signal ? "|s|" : "|d|") + b.meta + "|" + b.name + "|" + std::string(type->id);
    if (auto it = faces_.find(key); it != faces_.end()) return it->second;
    std::shared_ptr<MetaFace> f = makeFace(*type);
    if (!f) return nullptr;
    const doc::DeviceId dev = b.device;
    ViewContext* ctx = &ctx_;
    if (signal) {
      const std::string path = doc::tapPath(doc::ElementTap::Signal, b.name);
      f->bind(std::make_unique<LocalTarget>(*type), nullptr);
      if (f->wantsAnalysis())
        f->setAnalysis([ctx, dev, path] { return ctx->analysisFeed(dev, path); }, wakeOf(host, f.get()));
    } else {
      f->bind(std::make_unique<DocTarget>(ctx_, dev, b.meta, *type), ctx_.faceVoices(dev, b.meta));
      if (f->wantsAnalysis()) f->setAnalysis([ctx, dev] { return ctx->analysisFeed(dev); }, wakeOf(host, f.get()));
    }
    faces_.emplace(std::move(key), f);
    return f;
  }

  // GU.5: the controls a wire drives, indexed once per wire list (a persistent document: the list is the same object
  // until a wire changes), so a paint asks a sorted vector.
  bool driven(const doc::Binding& b) const override {
    if (b.kind != doc::BindingKind::Control || !b.meta.empty()) return false;
    const doc::Snapshot& s = ctx_.doc();
    if (!s) return false;
    const doc::WireList* wl = &doc::wiresOf(*s);
    if (wl != drivenOf_ || s != drivenSnap_) {
      if (wl != drivenOf_) {
        drivenOf_ = wl;
        driven_.clear();
        for (const doc::Wire& w : *wl)
          if (!w.param.empty()) driven_.emplace_back(w.to.value, w.param);
        std::sort(driven_.begin(), driven_.end());
      }
      drivenSnap_ = s;
    }
    return std::binary_search(driven_.begin(), driven_.end(), std::pair<uint32_t, std::string>(b.device.value, b.name));
  }

  bool connect(const doc::Binding& from, const doc::Binding& to) override {
    if (from.kind != doc::BindingKind::Signal || to.kind != doc::BindingKind::Control || !from.meta.empty() || !to.meta.empty()) {
      notice("A mod source makes a cable into a control");
      return false;
    }
    doc::Wire w;
    w.from = from.device;
    w.port = from.name;
    w.to = to.device;
    w.param = to.name;
    w = doc::canonicalWire(*ctx_.doc(), std::move(w), ctx_.deviceLibrary());  // (two main ports: a main link)
    const doc::ops::EditResult e = doc::ops::connect(*ctx_.doc(), ctx_.store().ids(), std::move(w));
    if (!e) {
      notice(e.error().message);
      return false;
    }
    if (e->empty()) return false;
    if (Status st = ctx_.store().dispatch(e->command); !st) {
      notice(st.error().message);
      return false;
    }
    return true;
  }

private:
  enum class How : uint8_t { None, Param, MetaParam, GroupSetting, Const, MetaSetting };
  struct Resolved {
    doc::Binding b;
    uint64_t rev = 0;
    BoundInfo info;
    How how = How::None;
    const doc::Device* device = nullptr;
    devices::ParamInfo param;
    const meta::Type* type = nullptr;
    size_t index = 0;
    double def = 0.0;
    std::string unit;
    doc::DevicePtr seenDevice, seenGroup;  // what it was resolved from (held: compared by identity)
  };
  struct Pending {
    doc::Binding b;
    double position = 0.0;
  };

  void sync() const {
    const doc::Snapshot& s = ctx_.doc();
    if (s == seen_) return;
    seen_ = s;
    ++rev_;
    devs_.clear();
    list_ = nullptr;
    const doc::DeviceLoc l = s ? doc::findDevice(*s, group_) : doc::DeviceLoc{};
    if (l.track != nullptr) {
      list_ = &l.track->devices;  // (the snapshot is held: seen_)
      for (const doc::DevicePtr& d : l.track->devices) devs_.emplace(d->id.value, &d);
    }
  }
  const doc::Device* device(doc::DeviceId id) const {
    const auto it = devs_.find(id.value);
    return it == devs_.end() ? nullptr : it->second->get();
  }
  doc::DevicePtr devicePtr(doc::DeviceId id) const {
    const auto it = devs_.find(id.value);
    return it == devs_.end() ? nullptr : *it->second;
  }
  const devices::DeviceSignature* signature(const doc::Device& d) const {
    const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
    if (lib == nullptr) return nullptr;
    const graph::Patch* patch = lib->patchOf(d);
    if (patch == nullptr) return nullptr;
    auto it = sigs_.find(patch);
    if (it == sigs_.end()) {
      it = sigs_.emplace(patch, std::make_unique<devices::DeviceSignature>(devices::signatureOf(*patch))).first;
      if (hold) holds_.push_back(std::move(hold));  // the patch stays alive while its signature is cached
    }
    return it->second.get();
  }

  const Resolved& resolved(const doc::Binding& b) const {
    sync();
    Resolved& r = cache_[&b];
    if (r.rev == rev_ && r.b == b) return r;
    // A new snapshot in which neither the bound block nor the group changed (a persistent document: the same objects,
    // held here, so compared by identity): the resolution holds — a param write elsewhere re-resolves nothing.
    doc::DevicePtr d = devicePtr(b.device), g = devicePtr(group_);
    if (r.rev != 0 && r.b == b && r.seenDevice == d && r.seenGroup == g) {
      r.rev = rev_;
      return r;
    }
    r = Resolved{};
    r.b = b;
    r.rev = rev_;
    r.seenDevice = std::move(d);
    r.seenGroup = std::move(g);
    resolve(b, r);
    r.info.generation = ++generation_;
    return r;
  }

  void resolve(const doc::Binding& b, Resolved& r) const {
    const doc::Device* d = device(b.device);
    const doc::Device* g = device(group_);
    if (d == nullptr || g == nullptr || !doc::bindingAllowed(lastList(), group_, b)) return;
    r.device = d;
    BoundInfo& i = r.info;
    if (b.kind == doc::BindingKind::Control) {
      if (b.meta.empty()) {
        const devices::DeviceSignature* sig = signature(*d);
        const devices::ParamInfo* pi = sig != nullptr ? sig->findParam(b.name) : nullptr;
        if (pi == nullptr) return;
        r.how = How::Param;
        r.param = *pi;
        // A value block is its one control: its own name (when it has one) is the control's.
        i.label = doc::isValueDevice(*d) && !d->name.empty() ? d->name : pi->label;
        i.curve = devices::storedCurve(*pi, *d);
        i.isSwitch = pi->isBool;
        i.steps = pi->isBool ? 2 : pi->steps;
        i.editable = true;
      } else {
        const meta::Type* t = metaTypeAt(ctx_, b.device, b.meta);
        const int32_t k = t != nullptr ? t->paramIndex(b.name) : -1;
        if (k < 0) return;
        r.how = How::MetaParam;
        r.type = t;
        r.index = static_cast<size_t>(k);
        const meta::ParamSpec& ps = t->params[r.index];
        i.label = std::string(ps.label.empty() ? ps.name : ps.label);
        i.curve = meta::paramCurve(ps);
      }
      i.valid = true;
      return;
    }
    if (b.kind == doc::BindingKind::Setting) {
      i.setting = true;
      if (doc::isGroupDevice(*d) && b.meta.empty()) {
        const doc::SettingSpec* s = doc::groupSetting(b.name);
        if (s == nullptr) return;
        r.how = How::GroupSetting;
        r.def = s->def;
        r.unit = std::string(s->unit);
        i.label = std::string(s->label);
        i.curve = linearOver(s->min, s->max, s->step);
        for (const std::string_view c : s->choices) i.choices.emplace_back(c);
        i.isSwitch = s->isSwitch();
        i.steps = s->step > 0.0 ? static_cast<int32_t>(std::lround((s->max - s->min) / s->step)) + 1 : 0;
      } else if (!b.meta.empty()) {
        const meta::Type* t = metaTypeAt(ctx_, b.device, b.meta);
        const int32_t k = t != nullptr ? t->settingIndex(b.name) : -1;
        if (k < 0) return;
        const meta::SettingSpec& ss = t->settings[static_cast<size_t>(k)];
        r.how = How::MetaSetting;
        r.type = t;
        r.def = ss.def;
        i.label = std::string(ss.label.empty() ? ss.name : ss.label);
        const double step = ss.steps >= 2 ? (ss.max - ss.min) / static_cast<double>(ss.steps - 1) : 0.0;
        i.curve = linearOver(ss.min, ss.max, step);
        i.steps = ss.steps >= 2 ? static_cast<int32_t>(ss.steps) : 0;
      } else {
        const devices::DeviceSignature* sig = signature(*d);
        const devices::ConstInfo* ci = sig != nullptr ? sig->findConst(b.name) : nullptr;
        if (ci == nullptr) return;
        r.how = How::Const;
        r.def = ci->def;
        i.label = ci->name;
        // A `real const` declares no range: 0 … max(1, 2·|default|) (a menu's "items" give its choices).
        i.curve = linearOver(std::min(0.0, ci->def), std::max(1.0, 2.0 * std::fabs(ci->def)), 0.0);
      }
      i.valid = true;
      return;
    }
    // Data / Signal / Panel: shown, not edited by the generic kinds (a Panel binding: the child group's name).
    i.label = b.kind == doc::BindingKind::Panel ? d->name : b.name;
    i.editable = false;
    i.valid = true;
  }

  // The group's track's own list (bindingAllowed needs it), of the snapshot sync() indexed.
  const std::vector<doc::DevicePtr>& lastList() const {
    static const std::vector<doc::DevicePtr> kEmpty;
    return list_ != nullptr ? *list_ : kEmpty;
  }

  doc::ops::EditResult paramEdit(const doc::Binding& b, const Resolved& r, double position) const {
    const doc::Project& p = *ctx_.doc();
    if (r.how == How::MetaParam) return doc::ops::setMetaParam(p, b.device, b.meta, r.type->id, b.name, position);
    return doc::ops::setDeviceParam(p, b.device, b.name, position);
  }
  doc::ops::EditResult settingEdit(const doc::Binding& b, const Resolved& r, double position) const {
    const doc::Project& p = *ctx_.doc();
    const double v = settingValue(r.info.curve, position);
    if (r.how == How::MetaSetting) return doc::ops::setMetaSetting(p, b.device, b.meta, r.type->id, b.name, v);
    return doc::ops::setDeviceSetting(p, b.device, b.name, v);
  }
  void dispatch(const doc::ops::EditResult& e) {
    if (!e) {
      notice(e.error().message);
      return;
    }
    if (e->empty()) return;
    if (Status s = ctx_.store().dispatch(e->command); !s) notice(s.error().message);
  }

  ViewContext& ctx_;
  doc::DeviceId group_;
  std::function<ui::Widget*()> host_;
  mutable doc::Snapshot seen_;
  mutable uint64_t rev_ = 0;
  mutable std::unordered_map<uint32_t, const doc::DevicePtr*> devs_;  // into the held snapshot's list
  mutable uint64_t generation_ = 0;
  mutable const std::vector<doc::DevicePtr>* list_ = nullptr;
  mutable std::unordered_map<const graph::Patch*, std::unique_ptr<devices::DeviceSignature>> sigs_;
  mutable std::vector<std::shared_ptr<const doc::ResolvedDevice>> holds_;
  mutable std::unordered_map<const doc::Binding*, Resolved> cache_;
  std::vector<Pending> pending_;
  bool inGesture_ = false, storeGesture_ = false;
  mutable const doc::WireList* drivenOf_ = nullptr;
  mutable doc::Snapshot drivenSnap_;  // (holds the wire list driven_ was built from)
  mutable std::vector<std::pair<uint32_t, std::string>> driven_;
  std::unordered_map<std::string, std::shared_ptr<MetaFace>> faces_;  // GU.7: engine faces by binding and type

  static std::function<void()> wakeOf(FaceHost* host, Face* f) {
    return [host, f] {
      if (host != nullptr) host->faceNeedsUpdate(*f);
    };
  }
};

}  // namespace

std::unique_ptr<PanelTarget> makeDocPanelTarget(ViewContext& ctx, doc::DeviceId group, std::function<ui::Widget*()> menuHost) {
  return std::make_unique<DocPanelTarget>(ctx, group, std::move(menuHost));
}

}  // namespace pq::views::panel
