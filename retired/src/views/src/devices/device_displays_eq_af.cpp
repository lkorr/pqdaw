// RETIRED (GU.10, no Filter module yet): the EQ Eight and Auto Filter panel displays, cut from
// src/views/src/devices/device_displays.cpp as they were at the retirement (inside its anonymous namespace; they used
// device_math.h's EQ / Auto Filter math, retired/src/views/src/devices/device_math_eq_af.h, and AnalysedDisplay /
// FreqAxis / panelText of device_displays.cpp). Not built: kept to read when EQ Eight and Auto Filter are rebuilt from
// modules. Registered by makeDisplay as: "eq" -> EqDisplay, "filter" -> FilterDisplay, "lfo" -> LfoDisplay.

// ---- EQ Eight: response, band dots, spectrum ------------------------------------------------------------------------------
class EqDisplay final : public AnalysedDisplay {
public:
  EqDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : AnalysedDisplay(d.id, impl, d) {
    withChip_ = true;
    if (const auto it = d.bind.find("@bands"); it != d.bind.end()) bandSel_ = it->second;
    if (const auto it = d.bind.find("@range"); it != d.bind.end()) rangeDb_ = std::max(1.0f, std::stof(it->second));
    rebind();
  }

  void rebind() override {
    const SelectorDesc* s = impl_.panel.desc().findSelector(bandSel_);
    bands_.clear();
    deps_.clear();
    if (!s) return;
    for (const std::string& v : s->values) {
      NameMap m = impl_.selValues;
      m[bandSel_] = v;
      Band b;
      b.label = v;
      b.freq = role("freq", m);
      b.gain = role("gain", m);
      b.q = role("q", m);
      b.type = role("type", m);
      b.on = role("on", m);
      for (const std::string* n : {&b.freq, &b.gain, &b.q, &b.type, &b.on})
        if (!n->empty()) deps_.insert(*n);
      bands_.push_back(std::move(b));
    }
    for (const char* r : {"scale", "adaptive", "output", "hq", "mode"}) {
      std::string n = role(r, impl_.selValues);
      if (!n.empty()) deps_.insert(n);
      globals_[r] = std::move(n);
    }
    dirty_ = true;
  }
  bool dependsOn(std::string_view name) const override { return deps_.count(std::string(name)) != 0; }
  void valuesChanged() override {
    dirty_ = true;
    markDirty();
  }
  ui::InfoText info() const override {
    return {"EQ Display", "Drag a band's dot to change its frequency and gain (Shift: fine; Alt or the wheel: Q). Click selects a "
                          "band; a double-click switches it on or off. The output spectrum is drawn behind the curve; "
                          "\"Measured\" overlays the response measured from the device's input and output (violet)."};
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    sync();
    spec_.wake();  // shown: hold the feed (and show what it has at once)
    {  // the measured response on this display's gain axis
      SpectrumPlotStyle s = spec_.style();
      s.glow = 0.6f;
      s.fill = 0.35f;
      s.bands = false;
      s.dbLabels = false;
      s.respTopDb = rangeDb_;
      s.respBottomDb = -rangeDb_;
      s.respInset = 6.0f;
      spec_.setStyle(s);
    }
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const ui::Rect f = insetRect(r, 1).toRect();
    axis_ = {f.x0, f.x1};
    plot_ = f;
    ctx.dl.pushClip(f);
    // Grid: frequencies, then gain lines (0 dB stronger).
    spec_.paintGrid(ctx.dl, f, axis_, *th, ctx.scale.factor, false);
    for (float db : {-12.0f, -6.0f, 6.0f, 12.0f}) {
      if (std::fabs(db) > rangeDb_) continue;
      const float y = std::round(yOf(db));
      ctx.dl.addRectFilled({f.x0, y, f.x1, y + 1.0f}, th->device.displayGrid);
    }
    ctx.dl.addRectFilled({f.x0, std::round(yOf(0.0f)), f.x1, std::round(yOf(0.0f)) + 1.0f}, th->device.displayGridMajor);
    // Spectrum behind everything else (subdued: the curve is the point of this display).
    spec_.paintSpectrum(ctx.dl, f, axis_, ctx.scale.factor);
    const PlotText text = panelText(ctx);
    spec_.paintLabels(ctx.dl, f, axis_, *th, ctx.scale.factor, text);
    // Response.
    const uint32_t n = static_cast<uint32_t>(std::max(16.0f, f.width() / ctx.scale.exact(1.5f)));
    if (dirty_ || resp_.size() != n) compute(n);
    pts_.resize(n);
    const float y0 = yOf(0.0f);
    for (uint32_t i = 0; i < n; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(n - 1);
      pts_[i] = {f.x0 + u * f.width(), std::clamp(yOf(resp_[i]), f.y0 - 2.0f, f.y1 + 2.0f)};
    }
    // The selected band's own response, filled faintly toward 0 dB (Live shows the band being edited).
    const int32_t sel = impl_.panel.selected(bandSel_);
    if (sel >= 0 && sel < static_cast<int32_t>(bands_.size()) && selResp_.size() == n) {
      for (uint32_t i = 0; i + 1 < n; ++i) {
        const float xa = pts_[i].x, xb = pts_[i + 1].x;
        const float ya = std::clamp(yOf(selResp_[i]), f.y0, f.y1), yb = std::clamp(yOf(selResp_[i + 1]), f.y0, f.y1);
        const ui::Vec2 q[4] = {{xa, y0}, {xa, ya}, {xb, yb}, {xb, y0}};
        if (std::fabs(ya - y0) + std::fabs(yb - y0) > 0.25f) ctx.dl.addConvexPolyFilled(q, th->device.curveFill);
      }
    }
    ui::smoothPolyline(ctx.dl, pts_.data(), n, th->device.curve, ctx.scale.exact(1.6f));
    // The measured response over the exact one (on demand: the chip).
    spec_.paintResponse(ctx.dl, f, axis_, ctx.scale.factor);
    // Band dots.
    const float rad = ctx.scale.exact(kDotRadius);
    for (size_t b = 0; b < bands_.size(); ++b) {
      const ui::Vec2 c = dotPos(b);
      const bool on = bands_[b].on.empty() || impl_.value(bands_[b].on) >= 0.5;
      const bool selected = static_cast<int32_t>(b) == sel;
      const bool hot = static_cast<int32_t>(b) == hover_ || static_cast<int32_t>(b) == drag_;
      if (selected || hot) ctx.dl.addCircleFilled(c, rad + ctx.scale.exact(1.5f), th->device.bandSelected);
      ctx.dl.addCircleFilled(c, rad, on ? th->device.band : th->device.bandOff);
      if (ctx.res.uiFonts) {
        const int32_t s = static_cast<int32_t>(std::lround(2.0f * rad));
        const ui::IRect box{static_cast<int32_t>(std::lround(c.x - rad)), static_cast<int32_t>(std::lround(c.y - rad)), s, s};
        paint::text(ctx, ctx.res.uiFonts->small, box, bands_[b].label, th->device.bandText, ui::TextAlign::Center);
      }
    }
    paintChip(ctx, *th, f);
    if (hover_ < 0 && drag_ < 0) spec_.paintHover(ctx.dl, f, axis_, hoverX_, ctx.scale.factor, text);
    ctx.dl.popClip();
  }

  bool onPointer(const ui::PointerEvent& e) override {
    if (drag_ < 0 && chipPointer(e)) return true;
    switch (e.type) {
      case ui::PointerType::Move: {
        if (drag_ >= 0) {
          dragTo(e.pos, e.mods);
          return true;
        }
        const int32_t h = dotAt(e.pos);
        if (h != hover_) {
          hover_ = h;
          markDirty();
        }
        trackHover(e.pos, h < 0);
        return false;
      }
      case ui::PointerType::Down: {
        if (e.button != ui::PointerButton::Left) return false;
        const int32_t b = dotAt(e.pos);
        if (b < 0) return false;
        impl_.userSelect(bandSel_, b);
        if (e.clickCount == 2 && !bands_[static_cast<size_t>(b)].on.empty()) {
          const std::string& on = bands_[static_cast<size_t>(b)].on;
          impl_.userConst(bare(on), impl_.value(on) >= 0.5 ? 0.0 : 1.0, this);
          return true;
        }
        drag_ = b;
        last_ = e.pos;
        const Band& bd = bands_[static_cast<size_t>(b)];
        if (!bd.freq.empty()) impl_.userParamBegin(bd.freq);
        if (!bd.gain.empty()) impl_.userParamBegin(bd.gain);
        if (!bd.q.empty()) impl_.userParamBegin(bd.q);
        markDirty();
        return true;
      }
      case ui::PointerType::Up: {
        if (drag_ < 0 || e.button != ui::PointerButton::Left) return false;
        endDrag();
        return true;
      }
      case ui::PointerType::Wheel: {
        const int32_t b = dotAt(e.pos) >= 0 ? dotAt(e.pos) : impl_.panel.selected(bandSel_);
        if (b < 0 || b >= static_cast<int32_t>(bands_.size())) return false;
        const Band& bd = bands_[static_cast<size_t>(b)];
        if (const ParamInfo* p = impl_.paramInfo(bd.q)) {
          const double pos = std::clamp(toPosition(*p, impl_.value(bd.q)) + 0.02 * e.wheel.y, 0.0, 1.0);
          impl_.userParamBegin(bd.q);
          impl_.userParam(bd.q, fromPosition(*p, pos), this);
          impl_.userParamEnd(bd.q);
          return true;
        }
        return false;
      }
      default: return false;
    }
  }
  void onCaptureLost() override {
    if (drag_ >= 0) endDrag();
  }
  // T4.16c: a band's dot moves (frequency sideways, gain up / down).
  ui::Cursor cursorAt(ui::Vec2) const override { return drag_ >= 0 || hover_ >= 0 ? ui::Cursor::Move : ui::Cursor::Inherit; }
  void onHoverChanged(bool h) override {
    AnalysedDisplay::onHoverChanged(h);
    if (!h && hover_ >= 0) {
      hover_ = -1;
      markDirty();
    }
  }

  // Tests: where band b's dot is drawn (device px), after a paint.
  ui::Vec2 dotPos(size_t b) const {
    const Band& bd = bands_[b];
    const float x = axis_.x(static_cast<float>(impl_.value(bd.freq)));
    float db = 0.0f;
    const int type = static_cast<int>(std::lround(impl_.value(bd.type)));
    if (type == math::kLowShelf || type == math::kBell || type == math::kHighShelf) {
      const double g = impl_.value(bd.gain);
      db = static_cast<float>(20.0 * std::log10(std::max(g, 1e-9)) * globalValue("scale", 1.0));
    }
    return {x, std::clamp(yOf(db), plot_.y0 + 2.0f, plot_.y1 - 2.0f)};
  }

private:
  static constexpr float kDotRadius = 6.5f;
  struct Band {
    std::string label, freq, gain, q, type, on;  // resolved names (consts with their '#')
  };
  // The resolved name bound to a role ("#name" for a const, as Impl::value and the change keys take it), or "".
  std::string role(const char* r, const NameMap& m) const {
    const auto it = desc_.bind.find(r);
    if (it == desc_.bind.end() || it->second.empty()) return {};
    const std::string& t = it->second;
    if (t[0] == '#') return "#" + resolveTemplate(std::string_view(t).substr(1), m);
    return resolveTemplate(t, m);
  }
  double globalValue(const char* r, double def) const {
    const auto it = globals_.find(r);
    if (it == globals_.end() || it->second.empty()) return def;
    return impl_.value(it->second);
  }
  static std::string bare(const std::string& n) { return !n.empty() && n[0] == '#' ? n.substr(1) : n; }
  float yOf(float db) const noexcept {
    const float mid = 0.5f * (plot_.y0 + plot_.y1);
    return mid - db / rangeDb_ * (0.5f * plot_.height() - 6.0f);
  }
  int32_t dotAt(ui::Vec2 p) const {
    const float s = tree() ? tree()->scale() : 1.0f;
    const float r2 = (kDotRadius + 3.0f) * s * (kDotRadius + 3.0f) * s;
    int32_t best = -1;
    float bestD = r2;
    for (size_t b = 0; b < bands_.size(); ++b) {
      const ui::Vec2 c = dotPos(b);
      const float d = (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y);
      if (d <= bestD) {
        bestD = d;
        best = static_cast<int32_t>(b);
      }
    }
    return best;
  }
  void dragTo(ui::Vec2 p, ui::Mods mods) {
    const Band& bd = bands_[static_cast<size_t>(drag_)];
    const float k = (mods & ui::mod::kShift) ? 0.1f : 1.0f;
    const float dx = (p.x - last_.x) * k, dy = (p.y - last_.y) * k;
    last_ = p;
    if (mods & ui::mod::kAlt) {  // Q
      if (const ParamInfo* q = impl_.paramInfo(bd.q)) {
        const double pos = std::clamp(toPosition(*q, impl_.value(bd.q)) - dy / 150.0, 0.0, 1.0);
        impl_.userParam(bd.q, fromPosition(*q, pos), this);
      }
      return;
    }
    if (const ParamInfo* fp = impl_.paramInfo(bd.freq)) {
      const double cur = toPosition(*fp, impl_.value(bd.freq));
      // The axis spans 10 Hz … 22 kHz; the param's own range may differ (position is log in both).
      const double perPx = std::log(2200.0) / std::log(fp->max / fp->min) / std::max(1.0f, plot_.width());
      impl_.userParam(bd.freq, fromPosition(*fp, std::clamp(cur + dx * perPx, 0.0, 1.0)), this);
    }
    const int type = static_cast<int>(std::lround(impl_.value(bd.type)));
    if (type == math::kLowShelf || type == math::kBell || type == math::kHighShelf)
      if (const ParamInfo* gp = impl_.paramInfo(bd.gain)) {
        const double scale = globalValue("scale", 1.0);
        const double dbPerPx = rangeDb_ / std::max(1.0f, 0.5f * plot_.height() - 6.0f) / std::max(0.05, std::fabs(scale));
        const double db = toDisplay(*gp, impl_.value(bd.gain)) - dy * dbPerPx * (scale < 0 ? -1.0 : 1.0);
        impl_.userParam(bd.gain, std::clamp(fromDisplay(*gp, db), gp->min, gp->max), this);
      }
  }
  void endDrag() {
    const Band& bd = bands_[static_cast<size_t>(drag_)];
    drag_ = -1;
    if (!bd.freq.empty()) impl_.userParamEnd(bd.freq);
    if (!bd.gain.empty()) impl_.userParamEnd(bd.gain);
    if (!bd.q.empty()) impl_.userParamEnd(bd.q);
    markDirty();
  }
  void compute(uint32_t n) {
    resp_.assign(n, 0.0f);
    selResp_.assign(n, 0.0f);
    const double sr = impl_.sampleRate();
    const double os = globalValue("hq", 1.0) >= 1.5 ? 2.0 : 1.0;
    const double scale = globalValue("scale", 1.0);
    const bool adaptive = globalValue("adaptive", 0.0) >= 0.5;
    const double outDb = 20.0 * std::log10(std::max(globalValue("output", 1.0), 1e-9));
    const int32_t sel = impl_.panel.selected(bandSel_);
    std::vector<math::EqBand> bs(bands_.size());
    std::vector<math::EqCoef> cs(bands_.size());
    for (size_t b = 0; b < bands_.size(); ++b) {
      const Band& bd = bands_[b];
      bs[b].on = bd.on.empty() || impl_.value(bd.on) >= 0.5;
      bs[b].type = static_cast<int>(std::lround(impl_.value(bd.type)));
      bs[b].freq = impl_.value(bd.freq);
      bs[b].gain = impl_.value(bd.gain);
      bs[b].q = impl_.value(bd.q);
      cs[b] = math::eqCoef(bs[b], adaptive, scale, sr, os);
    }
    for (uint32_t i = 0; i < n; ++i) {
      const double hz = axis_.hz(plot_.x0 + plot_.width() * static_cast<float>(i) / static_cast<float>(n - 1));
      std::complex<double> h = 1.0;
      const double wf = math::eqWarp(hz, sr, os);
      for (size_t b = 0; b < bs.size(); ++b) {
        const std::complex<double> hb = math::eqBandResponseWarped(bs[b], cs[b], wf);
        h *= hb;
        if (static_cast<int32_t>(b) == sel) selResp_[i] = static_cast<float>(20.0 * std::log10(std::max(std::abs(hb), 1e-9)));
      }
      resp_[i] = static_cast<float>(20.0 * std::log10(std::max(std::abs(h), 1e-9)) + outDb);
    }
    dirty_ = false;
  }


  std::string bandSel_ = "band";
  float rangeDb_ = 15.0f;
  std::vector<Band> bands_;
  std::map<std::string, std::string, std::less<>> globals_;
  std::set<std::string, std::less<>> deps_;
  std::vector<float> resp_, selResp_;
  std::vector<ui::Vec2> pts_;
  FreqAxis axis_;
  ui::Rect plot_{0, 0, 1, 1};
  bool dirty_ = true;
  int32_t hover_ = -1, drag_ = -1;
  ui::Vec2 last_{0, 0};
};

// ---- Auto Filter: the filter response with a frequency / resonance dot, and the LFO shape ---------------------------
// Both read their values through role bindings ("freq", "res", "#type" …); a role that is not bound uses a default.
class RoleBinding {
public:
  RoleBinding(DevicePanel::Impl& impl, const ControlDesc& d) : impl_(impl), desc_(d) {}
  void rebind() {
    names_.clear();
    deps_.clear();
    for (const auto& [role, name] : desc_.bind) {
      if (role.empty() || role[0] == '@' || name.empty()) continue;
      const std::string n = name[0] == '#' ? "#" + impl_.resolve(name.substr(1)) : impl_.resolve(name);
      names_[role] = n;
      deps_.insert(n);
    }
  }
  bool dependsOn(std::string_view name) const { return deps_.count(std::string(name)) != 0; }
  double get(const char* role, double def) const {
    const auto it = names_.find(role);
    return it == names_.end() ? def : impl_.value(it->second);
  }
  // The param a role is bound to ("" for a const or an unbound role).
  std::string param(const char* role) const {
    const auto it = names_.find(role);
    return it == names_.end() || it->second.empty() || it->second[0] == '#' ? std::string() : it->second;
  }

private:
  DevicePanel::Impl& impl_;
  const ControlDesc& desc_;
  std::map<std::string, std::string, std::less<>> names_;
  std::set<std::string, std::less<>> deps_;
};

class FilterDisplay final : public AnalysedDisplay {
public:
  FilterDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : AnalysedDisplay(d.id, impl, d), roles_(impl, d) {
    withChip_ = true;
    rebind();
  }
  void rebind() override {
    roles_.rebind();
    dirty_ = true;
  }
  bool dependsOn(std::string_view name) const override { return roles_.dependsOn(name); }
  void valuesChanged() override {
    dirty_ = true;
    markDirty();
  }
  ui::InfoText info() const override {
    return {"Filter Display", "The filter's response at the current settings (without the LFO and envelope), over the output "
                              "spectrum. Drag the dot: left / right sets the frequency (DJ: the DJ knob), up / down the "
                              "resonance. Shift: fine. \"Measured\" overlays the response measured from the device's input and "
                              "output (violet): with the LFO or envelope moving the cutoff, what it does now."};
  }

  math::AutoFilterState state() const {
    math::AutoFilterState s;
    s.type = static_cast<int>(std::lround(roles_.get("type", 0)));
    s.circuit = static_cast<int>(std::lround(roles_.get("circuit", 0)));
    s.slope = static_cast<int>(std::lround(roles_.get("slope", 0)));
    s.freq = roles_.get("freq", 1000.0);
    s.res = roles_.get("res", 0.3);
    s.morph = roles_.get("morph", 0.0);
    s.dj = roles_.get("dj", 0.0);
    return s;
  }
  // Tests: where the dot is drawn (device px), after a paint.
  ui::Vec2 dotPos() const {
    const math::AutoFilterState s = state();
    const float x = axis_.x(static_cast<float>(math::autoFilterCutoff(s, impl_.sampleRate())));
    const float u = static_cast<float>(std::clamp(s.res / kResMax, 0.0, 1.0));
    return {x, plot_.y1 - 4.0f - u * (plot_.height() - 8.0f)};
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    sync();
    spec_.wake();  // shown: hold the feed (and show what it has at once)
    {  // the spectrum behind, subdued; the measured response on this display's dB axis
      SpectrumPlotStyle s = spec_.style();
      s.glow = 0.6f;
      s.fill = 0.3f;
      s.bands = false;
      s.dbLabels = false;
      s.respTopDb = kTopDb;
      s.respBottomDb = kBottomDb;
      s.respInset = 2.0f;
      spec_.setStyle(s);
    }
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const ui::Rect f = insetRect(r, 1).toRect();
    axis_ = {f.x0, f.x1};
    plot_ = f;
    ctx.dl.pushClip(f);
    spec_.paintGrid(ctx.dl, f, axis_, *th, ctx.scale.factor, false);
    for (float db : {kTopDb - 12.0f, -12.0f, -24.0f}) {
      const float y = std::round(yOf(db));
      ctx.dl.addRectFilled({f.x0, y, f.x1, y + 1.0f}, th->device.displayGrid);
    }
    ctx.dl.addRectFilled({f.x0, std::round(yOf(0.0f)), f.x1, std::round(yOf(0.0f)) + 1.0f}, th->device.displayGridMajor);
    // TX.spectrum: the output spectrum behind the curve (as EQ Eight's).
    spec_.paintSpectrum(ctx.dl, f, axis_, ctx.scale.factor);
    const PlotText text = panelText(ctx);
    spec_.paintLabels(ctx.dl, f, axis_, *th, ctx.scale.factor, text);
    const uint32_t n = static_cast<uint32_t>(std::max(16.0f, f.width() / ctx.scale.exact(1.5f)));
    if (dirty_ || resp_.size() != n) compute(n);
    pts_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(n - 1);
      pts_[i] = {f.x0 + u * f.width(), std::clamp(yOf(resp_[i]), f.y0 - 2.0f, f.y1 + 2.0f)};
    }
    // The response filled down to the floor (Live draws its filter as a filled shape), then the curve.
    for (uint32_t i = 0; i + 1 < n; ++i) {
      const ui::Vec2 q[4] = {{pts_[i].x, f.y1}, pts_[i], pts_[i + 1], {pts_[i + 1].x, f.y1}};
      ctx.dl.addConvexPolyFilled(q, th->device.curveFill);
    }
    ui::smoothPolyline(ctx.dl, pts_.data(), n, th->device.curve, ctx.scale.exact(1.6f));
    // The measured response (on demand): with the LFO or the envelope moving the cutoff, what it does now.
    spec_.paintResponse(ctx.dl, f, axis_, ctx.scale.factor);
    // The frequency / resonance dot.
    const ui::Vec2 c = dotPos();
    const float rad = ctx.scale.exact(kDotRadius);
    if (hover_ || drag_) ctx.dl.addCircleFilled(c, rad + ctx.scale.exact(1.5f), th->device.bandSelected);
    ctx.dl.addCircleFilled(c, rad, th->device.band);
    paintChip(ctx, *th, f);
    if (!hover_ && !drag_) spec_.paintHover(ctx.dl, f, axis_, hoverX_, ctx.scale.factor, text);
    ctx.dl.popClip();
  }

  bool onPointer(const ui::PointerEvent& e) override {
    if (!drag_ && chipPointer(e)) return true;
    switch (e.type) {
      case ui::PointerType::Move: {
        if (drag_) {
          dragTo(e.pos, e.mods);
          return true;
        }
        const bool h = nearDot(e.pos);
        if (h != hover_) {
          hover_ = h;
          markDirty();
        }
        trackHover(e.pos, !h);
        return false;
      }
      case ui::PointerType::Down: {
        if (e.button != ui::PointerButton::Left || !nearDot(e.pos)) return false;
        drag_ = true;
        last_ = e.pos;
        xParam_ = state().type == math::kAfDj ? roles_.param("dj") : roles_.param("freq");
        resParam_ = roles_.param("res");
        if (!xParam_.empty()) impl_.userParamBegin(xParam_);
        if (!resParam_.empty()) impl_.userParamBegin(resParam_);
        markDirty();
        return true;
      }
      case ui::PointerType::Up: {
        if (!drag_ || e.button != ui::PointerButton::Left) return false;
        endDrag();
        return true;
      }
      default: return false;
    }
  }
  void onCaptureLost() override {
    if (drag_) endDrag();
  }
  // T4.16c: the dot moves (cutoff sideways, resonance up / down).
  ui::Cursor cursorAt(ui::Vec2) const override { return drag_ || hover_ ? ui::Cursor::Move : ui::Cursor::Inherit; }
  void onHoverChanged(bool h) override {
    AnalysedDisplay::onHoverChanged(h);
    if (!h && hover_) {
      hover_ = false;
      markDirty();
    }
  }

private:
  static constexpr float kDotRadius = 6.0f, kTopDb = 24.0f, kBottomDb = -48.0f;
  static constexpr double kResMax = 1.25;
  float yOf(float db) const noexcept { return plot_.y0 + 2.0f + (kTopDb - db) / (kTopDb - kBottomDb) * (plot_.height() - 4.0f); }
  bool nearDot(ui::Vec2 p) const {
    const float s = tree() ? tree()->scale() : 1.0f;
    const ui::Vec2 c = dotPos();
    const float r = (kDotRadius + 4.0f) * s;
    return (c.x - p.x) * (c.x - p.x) + (c.y - p.y) * (c.y - p.y) <= r * r;
  }
  void dragTo(ui::Vec2 p, ui::Mods mods) {
    const float k = (mods & ui::mod::kShift) ? 0.1f : 1.0f;
    const float dx = (p.x - last_.x) * k, dy = (p.y - last_.y) * k;
    last_ = p;
    if (const ParamInfo* xp = impl_.paramInfo(xParam_)) {
      double pos = toPosition(*xp, impl_.value(xParam_));
      if (xp->exp)  // the axis spans 10 Hz … 22 kHz; the param's own range may differ (position is log in both)
        pos += dx * std::log(2200.0) / std::log(xp->max / xp->min) / std::max(1.0f, plot_.width());
      else  // DJ: the axis' width spans the knob's travel
        pos += dx / std::max(1.0f, plot_.width());
      impl_.userParam(xParam_, fromPosition(*xp, std::clamp(pos, 0.0, 1.0)), this);
    }
    if (const ParamInfo* rp = impl_.paramInfo(resParam_)) {
      const double pos = std::clamp(toPosition(*rp, impl_.value(resParam_)) - dy / std::max(1.0f, plot_.height() - 8.0f), 0.0, 1.0);
      impl_.userParam(resParam_, fromPosition(*rp, pos), this);
    }
  }
  void endDrag() {
    drag_ = false;
    if (!xParam_.empty()) impl_.userParamEnd(xParam_);
    if (!resParam_.empty()) impl_.userParamEnd(resParam_);
    markDirty();
  }
  void compute(uint32_t n) {
    resp_.assign(n, 0.0f);
    const math::AutoFilterState s = state();
    const double sr = impl_.sampleRate();
    for (uint32_t i = 0; i < n; ++i) {
      const double hz = axis_.hz(plot_.x0 + plot_.width() * static_cast<float>(i) / static_cast<float>(n - 1));
      resp_[i] = static_cast<float>(20.0 * std::log10(std::max(std::abs(math::autoFilterResponse(s, hz, sr)), 1e-9)));
    }
    dirty_ = false;
  }

  RoleBinding roles_;
  std::vector<float> resp_;
  std::vector<ui::Vec2> pts_;
  FreqAxis axis_;
  ui::Rect plot_{0, 0, 1, 1};
  bool dirty_ = true, hover_ = false, drag_ = false;
  ui::Vec2 last_{0, 0};
  std::string xParam_, resParam_;
};

class LfoDisplay final : public PanelDisplay {
public:
  LfoDisplay(DevicePanel::Impl& impl, const ControlDesc& d) : PanelDisplay(d.id, impl, d), roles_(impl, d) { rebind(); }
  void rebind() override { roles_.rebind(); }
  bool dependsOn(std::string_view name) const override { return roles_.dependsOn(name); }
  ui::InfoText info() const override {
    return {"LFO Shape", "Two cycles of the LFO: the left channel bright, the right channel (Phase, Spin) dim."};
  }
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, ctx.scale.exact(2.0f), th->device.display, th->device.border);
    const ui::Rect f = insetRect(r, ctx.scale.px(3.0f)).toRect();
    ctx.dl.pushClip(r.toRect());
    const float cy = std::round(0.5f * (f.y0 + f.y1));
    ctx.dl.addRectFilled({f.x0, cy, f.x1, cy + 1.0f}, th->device.displayGridMajor);
    const float mx = std::round(0.5f * (f.x0 + f.x1));
    ctx.dl.addRectFilled({mx, f.y0, mx + 1.0f, f.y1}, th->device.displayGrid);
    const bool on = roles_.get("on", 1.0) >= 0.5;
    const int shape = static_cast<int>(std::lround(roles_.get("shape", 0)));
    const double offset = roles_.get("offset", 0.0), phase = roles_.get("phase", 0.0), spin = roles_.get("spin", 0.0);
    const uint32_t n = static_cast<uint32_t>(std::max(32.0f, f.width()));
    for (int ch = 1; ch >= 0; --ch) {
      pts_.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        const double u = 2.0 * i / (n - 1);  // two cycles
        const double t = ch ? u * (1.0 + spin) + offset + phase : u + offset;
        const double ph = t - std::floor(t);
        const double v = math::autoFilterLfo(shape, ph, static_cast<int>(std::floor(t)) + 3 * ch);
        pts_[i] = {f.x0 + static_cast<float>(u * 0.5) * f.width(), cy - static_cast<float>(v) * 0.5f * (f.height() - 2.0f)};
      }
      const ui::Rgba col = !on ? th->device.displayGrid : ch ? ui::withAlpha(th->device.curve, 110) : th->device.curve;
      ui::smoothPolyline(ctx.dl, pts_.data(), n, col, ctx.scale.exact(ch ? 1.2f : 1.6f));
    }
    ctx.dl.popClip();
  }

private:
  RoleBinding roles_;
  std::vector<ui::Vec2> pts_;
};

