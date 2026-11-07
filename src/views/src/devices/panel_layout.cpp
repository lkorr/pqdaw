// A leaf block's generated frame body (GU.11): pq/views/devices/panel_layout.h.
#include "pq/views/devices/panel_layout.h"

#include <string_view>

namespace pq::views::devices {

namespace {

// M4 review L12: a generated panel's labels in title case ("attack" -> "Attack", "lfo_rate" -> "Lfo Rate"); a label
// the patch gave (@label) is kept as written.
std::string titleCase(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool start = true;
  for (const char ch : s) {
    const char c = ch == '_' ? ' ' : ch;
    out += start && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    start = c == ' ';
  }
  return out;
}

}  // namespace

PanelLayout facePanel(const DeviceSignature& sig, std::string title, float faceW, float faceH) {
  PanelLayout p;
  p.patch = sig.patch;
  p.title = std::move(title);
  p.fitFace = true;
  p.faceW = faceW;
  p.faceH = faceH;
  p.width = faceW;
  p.height = faceH;
  ControlLayout d;
  d.kind = ControlKind::Face;
  d.id = "face";
  d.w = faceW;
  d.h = faceH;
  p.controls.push_back(std::move(d));
  return p;
}

PanelLayout autoPanel(const DeviceSignature& sig, std::string title) {
  PanelLayout p;
  p.patch = sig.patch;
  p.title = title.empty() ? sig.patch : std::move(title);
  // Columns 64 px wide; a knob takes a 56-px slot, a toggle or a box a 28-px one, 6 px between rows (a value never sits
  // on the next label: L12); two knob rows per column for up to eight controls (wider, Live-like), else three.
  size_t count = sig.consts.size();
  for (const ParamInfo& prm : sig.params) count += prm.hidden ? 0u : 1u;
  constexpr float kCol = 64.0f, kKnobH = 56.0f, kSmallH = 28.0f, kGap = 6.0f, kPad = 6.0f;
  const float kColH = static_cast<float>(count <= 8 ? 2 : 3) * (kKnobH + kGap) - kGap;
  float x = kPad, y = 0.0f;
  auto place = [&](ControlLayout d, float h) {
    if (y + h > kColH + 0.5f) {
      x += kCol;
      y = 0.0f;
    }
    d.x = x;
    d.y = y;
    d.w = kCol - 8.0f;
    d.h = h - (h == kSmallH ? 6.0f : 0.0f);
    y += h + kGap;
    p.controls.push_back(std::move(d));
  };
  for (const ParamInfo& prm : sig.params) {
    if (prm.hidden) continue;
    ControlLayout d;
    d.param = prm.name;
    d.id = prm.name;
    if (prm.label == prm.name) d.label = titleCase(prm.label);
    if (prm.isBool) {
      d.kind = ControlKind::Toggle;
      d.text = d.label.empty() ? prm.label : d.label;
      d.values = {0.0, 1.0};
      place(std::move(d), kSmallH);
    } else if (prm.steps > 1) {
      d.kind = ControlKind::Box;
      place(std::move(d), kKnobH);
    } else {
      d.kind = ControlKind::Knob;
      place(std::move(d), kKnobH);
    }
  }
  for (const ConstInfo& c : sig.consts) {
    ControlLayout d;
    d.kind = ControlKind::Box;
    d.constName = c.name;
    d.id = c.name;
    d.label = titleCase(c.name);
    place(std::move(d), kKnobH);
  }
  p.width = x + kCol + kPad - 8.0f;
  p.height = kColH;
  return p;
}

}  // namespace pq::views::devices
