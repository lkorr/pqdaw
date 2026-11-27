// SX.2 / SX.5: the clip menu: see pq/views/audio_clip/clip_menu.h.

#include "pq/views/audio_clip/clip_menu.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "pq/views/audio_clip/audio_clip_view.h"
#include "pq/views/audio_clip/clip_edits.h"

namespace pq::views {

namespace ce = clip_edits;
using ui::RadialBinding;
using ui::RadialChoice;
using ui::RadialItem;
using ui::RadialPlace;

namespace {

// What the bindings share while the menu is open: the clip it edits, its tuning (re-read on every document change),
// the file's tempo tag (probed once), the exact transpose a typed entry parsed (until the gesture ends).
struct MenuState {
  MenuState(ViewContext& c, const doc::ClipRef& r) : ctx(c), ref(r) {}
  ViewContext& ctx;
  doc::ClipRef ref;
  std::optional<tuning::Tuning> tuning;
  double fileBpm = 0.0;
  std::optional<doc::AudioTranspose> typed;
  ui::RadialMenu* menu = nullptr;
  ViewContext::Subscription sub;

  const doc::AudioClip* clip() const { return ce::findAudioClip(*ctx.doc(), ref); }
};
using StatePtr = std::shared_ptr<MenuState>;

// A gesture's begin / end: one store gesture (one undo step) labelled as the panel's.
void gestured(RadialBinding& b, const StatePtr& s, std::string label) {
  b.begin = [s, label = std::move(label)] { (void)s->ctx.store().beginGesture(label); };
  b.end = [s] {
    s->typed.reset();
    if (s->ctx.store().inGesture()) (void)s->ctx.store().commitGesture();
  };
}

// A warp parameter (the mode card's controls): enabled in its mode only.
RadialItem paramSlider(const StatePtr& s, std::string key, std::string label, ui::ValueSpec spec, doc::AudioWarp mode,
                       float doc::WarpParams::*field) {
  RadialBinding b;
  b.get = [s, field] {
    const doc::AudioClip* c = s->clip();
    return c ? static_cast<double>(c->warpParams.*field) : 0.0;
  };
  b.set = [s, field](double v) {
    const doc::AudioClip* c = s->clip();
    if (!c) return;
    doc::WarpParams p = c->warpParams;
    p.*field = static_cast<float>(v);
    (void)ce::setWarpParams(s->ctx, s->ref, p);
  };
  b.enabled = [s, mode] {
    const doc::AudioClip* c = s->clip();
    return c && c->warp == mode;
  };
  gestured(b, s, "Warp Settings");
  return RadialItem::slider(std::move(key), std::move(label), std::move(spec), std::move(b));
}

std::vector<RadialItem> buildItems(const StatePtr& s) {
  std::vector<RadialItem> items;
  items.reserve(9);
  auto enabledIf = [s](bool (*pred)(doc::AudioWarp)) {
    return [s, pred] {
      const doc::AudioClip* c = s->clip();
      return c && pred(c->warp);
    };
  };

  // ---- above: the warp modes in one row (Off, Re-Pitch | the time-stretch modes); the chosen one's settings above it
  auto warp = std::make_shared<RadialBinding>();
  warp->get = [s] {
    const doc::AudioClip* c = s->clip();
    return c ? static_cast<double>(c->warp) : 0.0;
  };
  warp->set = [s](double v) { (void)ce::setWarp(s->ctx, s->ref, static_cast<doc::AudioWarp>(static_cast<int>(v)), s->fileBpm); };
  auto mode = [](doc::AudioWarp w, std::vector<RadialItem> children = {}) {
    const auto i = static_cast<size_t>(w);
    return RadialChoice{i == 6 ? std::string("Pro") : std::string(kWarpModeNames[i]), static_cast<int32_t>(i), std::move(children)};
  };
  {
    std::vector<RadialChoice> tape;
    tape.push_back(mode(doc::AudioWarp::Off));
    tape.push_back(mode(doc::AudioWarp::RePitch));
    items.push_back(RadialItem::chips("warp", std::move(tape), warp, RadialPlace::above()));
  }
  {
    // Beats: Preserve, Transient Loop, Envelope.
    std::vector<RadialItem> beats;
    {
      std::vector<RadialChoice> g;
      for (int32_t i = 0; i < 5; ++i) g.push_back({ce::kGranulationLabels[i], i, {}});
      RadialBinding b;
      b.get = [s] {
        const doc::AudioClip* c = s->clip();
        return c ? static_cast<double>(ce::granulationIndex(c->warpParams.beatsGranulation)) : 0.0;
      };
      b.set = [s](double v) {
        const doc::AudioClip* c = s->clip();
        if (!c) return;
        doc::WarpParams p = c->warpParams;
        p.beatsGranulation = ce::kGranulations[std::clamp(static_cast<int>(v), 0, 4)];
        (void)ce::setWarpParams(s->ctx, s->ref, p);
      };
      b.enabled = [s] {
        const doc::AudioClip* c = s->clip();
        return c && c->warp == doc::AudioWarp::Beats;
      };
      beats.push_back(RadialItem::chips("preserve", std::move(g), std::move(b)));
    }
    {
      std::vector<RadialChoice> g;
      for (int32_t i = 0; i < 3; ++i) g.push_back({i == 0 ? "No Loop" : ce::kBeatsLoopLabels[i], i, {}});  // "Off" sits beside the modes' Off
      RadialBinding b;
      b.get = [s] {
        const doc::AudioClip* c = s->clip();
        return c ? static_cast<double>(c->warpParams.beatsLoop) : 0.0;
      };
      b.set = [s](double v) {
        const doc::AudioClip* c = s->clip();
        if (!c) return;
        doc::WarpParams p = c->warpParams;
        p.beatsLoop = static_cast<doc::BeatsLoop>(std::clamp(static_cast<int>(v), 0, 2));
        (void)ce::setWarpParams(s->ctx, s->ref, p);
      };
      b.enabled = [s] {
        const doc::AudioClip* c = s->clip();
        return c && c->warp == doc::AudioWarp::Beats;
      };
      beats.push_back(RadialItem::chips("transient-loop", std::move(g), std::move(b)));
    }
    beats.push_back(paramSlider(s, "beats-env", "Envelope", ce::beatsEnvelopeSpec(), doc::AudioWarp::Beats, &doc::WarpParams::beatsEnvelope));
    std::vector<RadialItem> tones;
    tones.push_back(paramSlider(s, "tones-grain", "Grain", ce::grainSpec(), doc::AudioWarp::Tones, &doc::WarpParams::grainSizeMs));
    std::vector<RadialItem> texture;
    texture.push_back(paramSlider(s, "texture-grain", "Grain", ce::grainSpec(), doc::AudioWarp::Texture, &doc::WarpParams::grainSizeMs));
    texture.push_back(paramSlider(s, "flux", "Flux", ce::fluxSpec(), doc::AudioWarp::Texture, &doc::WarpParams::flux));
    std::vector<RadialItem> pro;
    pro.push_back(paramSlider(s, "formants", "Formants", ce::formantsSpec(), doc::AudioWarp::ComplexPro, &doc::WarpParams::formants));
    pro.push_back(paramSlider(s, "env-order", "Envelope", ce::envelopeOrderSpec(), doc::AudioWarp::ComplexPro, &doc::WarpParams::envelope));
    std::vector<RadialChoice> stretch;
    stretch.push_back(mode(doc::AudioWarp::Beats, std::move(beats)));
    stretch.push_back(mode(doc::AudioWarp::Tones, std::move(tones)));
    stretch.push_back(mode(doc::AudioWarp::Texture, std::move(texture)));
    stretch.push_back(mode(doc::AudioWarp::Complex));
    stretch.push_back(mode(doc::AudioWarp::ComplexPro, std::move(pro)));
    items.push_back(RadialItem::chips("warp-stretch", std::move(stretch), warp, RadialPlace::above()));
  }

  // ---- below: Transpose and its unit, Detune, Reverse
  {
    RadialBinding b;
    b.get = [s] {
      const doc::AudioClip* c = s->clip();
      return c ? ce::transposeCents(s->tuning, c->transpose) : 0.0;
    };
    b.set = [s](double v) {
      const doc::AudioClip* c = s->clip();
      if (!c) return;
      if (s->typed && std::abs(ce::transposeCents(s->tuning, *s->typed) - v) < 1e-6) {  // the typed entry, exactly
        (void)ce::setTranspose(s->ctx, s->ref, *s->typed);
        return;
      }
      doc::AudioTranspose t;
      switch (c->transpose.unit) {
        case doc::TransposeUnit::Cents: t.cents = std::round(v * 10.0) / 10.0; break;
        case doc::TransposeUnit::Steps: t = ce::nearestStep(s->tuning, v); break;
        case doc::TransposeUnit::Ratio: t = ce::nearestRatio(v, 16); break;
      }
      (void)ce::setTranspose(s->ctx, s->ref, t);
    };
    b.format = [s](double v) {
      const doc::AudioClip* c = s->clip();
      if (c && std::abs(ce::transposeCents(s->tuning, c->transpose) - v) < 1e-6) return transposeReadout(c->transpose, v);
      doc::AudioTranspose t;
      t.cents = v;
      return transposeReadout(t, v);
    };
    b.parse = [s](std::string_view text) -> std::optional<double> {
      const std::optional<doc::AudioTranspose> t = parseTranspose(text);
      if (!t) {
        s->ctx.setStatus("Transpose: type 7 (steps), 386.3c (cents), +7st (semitones) or 5/4 (a ratio)");
        return std::nullopt;
      }
      s->typed = *t;
      return ce::transposeCents(s->tuning, *t);
    };
    b.enabled = enabledIf(ce::transposeApplies);
    gestured(b, s, "Transpose");
    ui::ValueSpec spec = ce::transposeSpec();
    spec.keyStep = ce::meanStepCents(s->tuning);  // the wheel: one step of the clip's tuning
    items.push_back(RadialItem::knob("transpose", "Transpose", std::move(spec), std::move(b), RadialPlace::below()));
  }
  {
    RadialBinding b;
    b.get = [s] {
      const doc::AudioClip* c = s->clip();
      return c ? static_cast<double>(c->transpose.unit) : 0.0;
    };
    b.set = [s](double v) {
      const doc::AudioClip* c = s->clip();
      if (!c) return;
      (void)ce::setTranspose(s->ctx, s->ref, ce::inUnit(s->tuning, c->transpose, static_cast<doc::TransposeUnit>(static_cast<int>(v))));
    };
    b.enabled = enabledIf(ce::transposeApplies);
    std::vector<RadialChoice> g;
    g.push_back({"stp", static_cast<int32_t>(doc::TransposeUnit::Steps), {}});
    g.push_back({"\xC2\xA2", static_cast<int32_t>(doc::TransposeUnit::Cents), {}});
    g.push_back({"n/d", static_cast<int32_t>(doc::TransposeUnit::Ratio), {}});
    items.push_back(RadialItem::chips("unit", std::move(g), std::move(b), RadialPlace::below()));
  }

  {
    RadialBinding b;
    b.get = [s] {
      const doc::AudioClip* c = s->clip();
      return c ? static_cast<double>(c->detuneCents) : 0.0;
    };
    b.set = [s](double v) { (void)ce::setDetune(s->ctx, s->ref, static_cast<float>(v)); };
    b.format = ce::formatDetune;
    b.enabled = enabledIf(ce::transposeApplies);
    gestured(b, s, "Clip Detune");
    items.push_back(RadialItem::knob("detune", "Detune", ce::detuneSpec(), std::move(b), RadialPlace::below()));
  }

  {
    RadialBinding b;
    b.get = [s] {
      const doc::AudioClip* c = s->clip();
      return c && c->reverse ? 1.0 : 0.0;
    };
    b.set = [s](double v) { (void)ce::setReverse(s->ctx, s->ref, v != 0.0); };
    items.push_back(RadialItem::toggle("reverse", "Reverse", std::move(b), RadialPlace::below()));
  }
  // ---- then the tempo: the BPM knob (typed on double-click), :2 / ×2 (momentary: nothing lit), Loop
  {
    RadialBinding b;
    b.get = [s] {
      const doc::AudioClip* c = s->clip();
      return c ? c->sourceBpm : 120.0;
    };
    b.set = [s](double v) { (void)ce::setSourceBpm(s->ctx, s->ref, v); };
    b.enabled = enabledIf(ce::tempoApplies);
    gestured(b, s, "Clip Tempo");
    RadialItem k = RadialItem::knob("bpm", "BPM", ce::bpmSpec(), std::move(b), RadialPlace::below());
    k.resettable = false;
    items.push_back(std::move(k));
  }
  {
    RadialBinding b;
    b.get = [] { return -1.0; };
    b.set = [s](double v) { (void)ce::scaleSourceBpm(s->ctx, s->ref, v == 0.0 ? 0.5 : 2.0); };
    b.enabled = enabledIf(ce::tempoApplies);
    std::vector<RadialChoice> g;
    g.push_back({":2", 0, {}});
    g.push_back({"\xC3\x97" "2", 1, {}});
    items.push_back(RadialItem::chips("tempo", std::move(g), std::move(b), RadialPlace::below()));
  }
  {
    RadialBinding b;
    b.get = [s] {
      const doc::AudioClip* c = s->clip();
      return c && c->loop.enabled ? 1.0 : 0.0;
    };
    b.set = [s](double v) { (void)ce::setLoopEnabled(s->ctx, s->ref, v != 0.0); };
    b.enabled = enabledIf(ce::tempoApplies);
    items.push_back(RadialItem::toggle("loop", "Loop", std::move(b), RadialPlace::below()));
  }
  return items;
}

StatePtr makeState(ViewContext& ctx, const doc::ClipRef& ref) {
  auto s = std::make_shared<MenuState>(ctx, ref);
  s->tuning = ce::clipTuning(*ctx.doc(), ref);
  if (const doc::AudioClip* c = s->clip()) s->fileBpm = ce::fileTempo(ctx, *c);
  return s;
}

}  // namespace

std::vector<RadialItem> clipMenuItems(ViewContext& ctx, const doc::ClipRef& ref) { return buildItems(makeState(ctx, ref)); }

ui::RadialMenu* openClipMenu(ViewContext& ctx, ui::Widget& from, const ui::IRect& anchor, const doc::ClipRef& ref,
                             float atX, std::function<void()> onClosed, bool animate) {
  if (!ce::findAudioClip(*ctx.doc(), ref)) return nullptr;
  StatePtr s = makeState(ctx, ref);
  ui::RadialMenuOptions o;
  o.animate = animate;
  o.layout = ui::RadialLayout::Bands;
  o.bandX = atX;
  o.valid = [s] { return s->clip() != nullptr; };
  o.onClosed = [s, user = std::move(onClosed)] {
    s->menu = nullptr;
    s->sub.reset();  // safe inside a store notification (the store defers the removal)
    if (user) user();
  };
  ui::RadialMenu* m = ui::openRadialMenu(from, anchor, buildItems(s), std::move(o));
  if (!m) return nullptr;
  s->menu = m;
  // The document changes under the menu (its own edits, undo, other views): the tuning and every value re-read; the
  // clip gone closes it (options.valid).
  s->sub = ctx.subscribe([w = std::weak_ptr<MenuState>(s)](const doc::StoreEvent&) {
    const StatePtr st = w.lock();
    if (!st || !st->menu) return;
    st->tuning = ce::clipTuning(*st->ctx.doc(), st->ref);
    st->menu->refresh();
  });
  return m;
}

}  // namespace pq::views
