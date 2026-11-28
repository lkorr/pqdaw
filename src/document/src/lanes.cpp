// FD.2: the estimated width of every wire (pq/document/lanes.h).
#include "pq/document/lanes.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>

#include "pq/document/model.h"
#include "pq/graph/infer.h"
#include "pq/meta/registry.h"
#include "pq/meta/type.h"

namespace pq::doc {

uint16_t LaneWidths::of(WireId w) const noexcept {
  const auto it = std::lower_bound(wide.begin(), wide.end(), w, [](const auto& a, WireId b) { return a.first < b; });
  return it != wide.end() && it->first == w ? it->second : uint16_t{0};
}
uint16_t LaneWidths::boundOf(DeviceId group) const noexcept {
  const auto it = std::lower_bound(bound.begin(), bound.end(), group, [](const auto& a, DeviceId b) { return a.first < b; });
  return it != bound.end() && it->first == group ? it->second : uint16_t{0};
}

namespace {

// A width as the compiler writes it at the start of `s` ("mono", "stereo", "ch(8)"): its channel count and how many
// characters it took; 0: none.
uint32_t parseWidth(std::string_view s, size_t& used) {
  if (s.starts_with("mono")) return used = 4, 1u;
  if (s.starts_with("stereo")) return used = 6, 2u;
  if (!s.starts_with("ch(")) return 0;
  uint32_t n = 0;
  size_t i = 3;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) n = n * 10 + static_cast<uint32_t>(s[i] - '0');
  if (i == 3 || i >= s.size() || s[i] != ')') return 0;
  used = i + 1;
  return n;
}
// "8 lanes", "a stereo signal", "a mono signal".
std::string cableText(uint32_t w) {
  return w == 1 ? "a mono signal" : w == 2 ? "a stereo signal" : std::to_string(w) + " lanes";
}

}  // namespace

std::string laneRefusal(std::string_view message) {
  constexpr std::string_view kHead = "width mismatch: ";
  const size_t at = message.find(kHead);
  if (at == std::string_view::npos) return {};
  std::string_view s = message.substr(at + kHead.size());
  size_t used = 0;
  const uint32_t a = parseWidth(s, used);
  if (a == 0) return {};
  s.remove_prefix(used);
  const bool into = s.starts_with(" into "), meet = s.starts_with(" and ");
  if (!into && !meet) return {};
  s.remove_prefix(into ? 6 : 5);
  const uint32_t b = parseWidth(s, used);
  if (b == 0 || (a <= 2 && b <= 2)) return {};  // (mono / stereo only: the compiler's own words)
  if (meet) {
    return cableText(std::max(a, b)) + " and " + cableText(std::min(a, b)) +
           " meet here: only a mono signal joins a lane cable (fan the other out with Lanes, or merge the lanes first)";
  }
  const std::string head = a > 2 ? cableText(a) : std::string(a == 2 ? "A stereo signal" : "A mono signal");
  if (b == 2) return head + " into a stereo input: merge the lanes first (a Merge module)";
  if (b == 1) return head + " into a mono input: merge the lanes first (Merge), or take one lane (Unpack)";
  return head + " into a module set to " + std::to_string(b) + " lanes: " +
         (a > 2 ? "set its lane count to " + std::to_string(a) : std::string("fan the signal out first (Lanes), or set its lane count to 2"));
}

namespace {

enum class Rule : uint8_t { Join, Pack, Group, Fixed };

struct Dev {
  Rule rule = Rule::Join;
  bool active = true;
  // What a cable out of a group's In terminal finds here: a `channel` block (its index + 1), a native engine with a
  // declared output width (kEngine), anything else (0: it takes any width).
  static constexpr uint16_t kEngine = 0xFFFF;
  uint16_t takes = 0;
  DeviceId id;
  bool control = false;  // a value block: the wires into it drive its control (lower.h "Lane cables": the rule below)
  uint32_t track = 0;    // its track's id
  uint16_t in = 1, out = 1;  // Group: what reaches its main input / its Out terminal; the others: `out`
  uint32_t firstWire = 0, wireCount = 0;  // into `into`: the wires into it
};

}  // namespace

LaneWidths laneWidths(const Project& p) {
  LaneWidths result;
  const WireList& wires = wiresOf(p);
  if (wires.empty()) return result;
  // Without a pack block nothing is wider than stereo, and without a channel block past the second channel no group
  // takes more than stereo: nothing to find.
  auto channelIndex = [](const Device& d) {
    const DeviceValue* v = d.setting("index");
    return v != nullptr && v->value >= 0.0 && v->value < graph::kMaxWidth ? static_cast<uint16_t>(v->value) : uint16_t{0};
  };
  bool any = false;
  forEachDevice(p, [&](const Device& d, const Track&) {
    if (any || !isAtomDevice(d)) return;
    const std::string_view atom = atomDeviceName(d);
    any = atom == "pack" || (atom == "channel" && channelIndex(d) >= 2);
  });
  if (!any) return result;

  std::vector<Dev> devs;
  std::unordered_map<uint32_t, uint32_t> indexOf;  // device id → devs
  std::unordered_map<uint32_t, bool> midi;         // track id → a MIDI track
  forEachTrack(p, [&](const Track& t, const Track*) {
    midi.emplace(t.id.value, t.kind == TrackKind::Midi);
    for (const DevicePtr& dp : t.devices) {
      const Device& d = *dp;
      Dev x;
      x.active = d.active;
      x.id = d.id;
      x.track = t.id.value;
      if (isGroupDevice(d)) {
        x.rule = Rule::Group;
      } else if (isAtomDevice(d)) {
        const std::string_view atom = atomDeviceName(d);
        if (atom == "pack") {
          x.rule = Rule::Pack;
        } else if (atom == "channel") {
          x.rule = Rule::Fixed;  // (1)
          x.takes = static_cast<uint16_t>(channelIndex(d) + 1);
        }
      } else if (isValueDevice(d)) {
        x.rule = Rule::Join;
        x.control = true;
      } else if (isMetaDevice(d)) {
        // A native engine: its declared outputs, or (every output's width inferred: an analyser) what reaches it.
        const meta::Type* type = meta::findType(metaDeviceType(d));
        uint16_t w = 0;
        bool inferred = type != nullptr && !type->outputs.empty();
        if (type != nullptr)
          for (const meta::OutputSpec& o : type->outputs) {
            inferred = inferred && o.width == graph::kWidthInfer;
            w = std::max(w, o.width);
          }
        if (!inferred) {
          x.rule = Rule::Fixed;
          x.out = std::max<uint16_t>(w, 1);
          x.takes = Dev::kEngine;
        }
      } else {
        x.rule = Rule::Fixed;
        x.out = graph::kWidthStereo;
      }
      indexOf.emplace(d.id.value, static_cast<uint32_t>(devs.size()));
      devs.push_back(x);
    }
  });
  auto devOf = [&](DeviceId id) -> uint32_t {
    const auto it = id.valid() ? indexOf.find(id.value) : indexOf.end();
    return it != indexOf.end() ? it->second : graph::kNone;
  };
  // The wires by target device (CSR), and by source device (who to look at again when a width grows).
  const uint32_t nw = static_cast<uint32_t>(wires.size());
  std::vector<uint32_t> to(nw, graph::kNone), from(nw, graph::kNone);
  for (uint32_t i = 0; i < nw; ++i) {
    to[i] = devOf(wires[i].to);
    from[i] = wires[i].fromNoteSource() || wires[i].fromMixerOut() ? graph::kNone : devOf(wires[i].from);
    if (to[i] != graph::kNone) ++devs[to[i]].wireCount;
  }
  std::vector<uint32_t> into(nw), outBegin(devs.size() + 1, 0), outOf(nw);
  {
    uint32_t at = 0;
    for (Dev& d : devs) {
      d.firstWire = at;
      at += d.wireCount;
      d.wireCount = 0;
    }
    for (uint32_t i = 0; i < nw; ++i)
      if (to[i] != graph::kNone) into[devs[to[i]].firstWire + devs[to[i]].wireCount++] = i;
    for (uint32_t i = 0; i < nw; ++i)
      if (from[i] != graph::kNone) ++outBegin[from[i] + 1];
    for (size_t k = 1; k < outBegin.size(); ++k) outBegin[k] += outBegin[k - 1];
    std::vector<uint32_t> fill(outBegin.begin(), outBegin.end() - 1);
    for (uint32_t i = 0; i < nw; ++i)
      if (from[i] != graph::kNone) outOf[fill[from[i]]++] = i;
  }
  // What wire i carries now.
  auto carried = [&](uint32_t i) -> uint16_t {
    const Wire& w = wires[i];
    if (w.fromMixerOut()) return graph::kWidthStereo;
    if (w.fromNoteSource()) {
      const auto it = midi.find(w.noteSource.value);
      return it != midi.end() && it->second ? graph::kWidthMono : graph::kWidthStereo;
    }
    if (from[i] == graph::kNone) return graph::kWidthMono;
    const Dev& d = devs[from[i]];
    return d.rule == Rule::Group && w.port == kGroupInPort ? d.in : d.out;
  };
  // What wire i brings to device `d`. Into a CONTROL (a value block's) the lowering gives a lane cable's every lane, but
  // of a mono or stereo signal the first channel, and across tracks always the first (lower.h "Lane cables"): so a
  // stereo signal fed back into a control of the block that made it (a Utility's output into its Gain) stays what it is.
  auto arriving = [&](uint32_t i, const Dev& d) -> uint16_t {
    const uint16_t w = carried(i);
    if (!d.control) return w;
    const bool cross = from[i] != graph::kNone && devs[from[i]].track != d.track;
    return w > graph::kWidthStereo && !cross ? w : graph::kWidthMono;
  };
  auto cap = [](uint32_t w) { return static_cast<uint16_t>(std::min<uint32_t>(w, graph::kMaxWidth)); };
  // One device's widths from the wires into it; true when one grew.
  auto update = [&](Dev& d) {
    uint16_t in = d.in, out = d.out;
    switch (d.rule) {
      case Rule::Fixed: return false;
      case Rule::Join:
        for (uint32_t k = 0; k < d.wireCount; ++k) out = std::max(out, arriving(into[d.firstWire + k], d));
        break;
      case Rule::Group:
        for (uint32_t k = 0; k < d.wireCount; ++k) {
          const uint32_t i = into[d.firstWire + k];
          if (wires[i].param == kGroupOutPort) out = std::max(out, carried(i));
          else in = std::max(in, carried(i));
        }
        if (!d.active) out = std::max(out, in);  // switched off: what reaches it passes on
        break;
      case Rule::Pack: {
        // Its inputs by name (two on the block; a wire into each is usual, several add: the widest).
        std::string_view names[2];
        uint16_t width[2] = {1, 1};
        uint32_t n = 0;
        for (uint32_t k = 0; k < d.wireCount; ++k) {
          const uint32_t i = into[d.firstWire + k];
          uint32_t s = 0;
          while (s < n && names[s] != wires[i].param) ++s;
          if (s == n) {
            if (n == 2) continue;  // (no such input: the lowering leaves the wire out)
            names[n++] = wires[i].param;
          }
          width[s] = std::max(width[s], carried(i));
        }
        out = std::max(out, cap(static_cast<uint32_t>(width[0]) + width[1]));
        break;
      }
    }
    const bool grew = in != d.in || out != d.out;
    d.in = in, d.out = out;
    return grew;
  };
  std::vector<uint32_t> todo;
  std::vector<uint8_t> queued(devs.size(), 1);
  todo.reserve(devs.size());
  for (uint32_t k = static_cast<uint32_t>(devs.size()); k-- > 0;) todo.push_back(k);
  while (!todo.empty()) {
    const uint32_t k = todo.back();
    todo.pop_back();
    queued[k] = 0;
    if (!update(devs[k])) continue;
    for (uint32_t j = outBegin[k]; j < outBegin[k + 1]; ++j)
      if (const uint32_t next = to[outOf[j]]; next != graph::kNone && queued[next] == 0) {
        queued[next] = 1;
        todo.push_back(next);
      }
  }
  for (uint32_t i = 0; i < nw; ++i)
    if (const uint16_t w = carried(i); w > graph::kWidthStereo) result.wide.emplace_back(wires[i].id, w);
  // (the wire list is sorted by id)
  // The bound groups: every cable out of the In terminal ends on a channel block or a declared-width engine, and what
  // reaches the main input is neither mono nor that wide.
  for (uint32_t k = 0; k < devs.size(); ++k) {
    const Dev& g = devs[k];
    if (g.rule != Rule::Group || !g.active || g.in <= graph::kWidthMono) continue;
    uint16_t n = 0;
    bool all = true, anyOut = false;
    for (uint32_t j = outBegin[k]; j < outBegin[k + 1] && all; ++j) {
      const Wire& w = wires[outOf[j]];
      if (w.port != kGroupInPort) continue;
      const uint16_t takes = to[outOf[j]] != graph::kNone ? devs[to[outOf[j]]].takes : uint16_t{0};
      anyOut = true;
      if (takes == 0) all = false;
      else n = std::max<uint16_t>(n, takes == Dev::kEngine ? graph::kWidthStereo : takes);
    }
    if (all && anyOut && n >= graph::kWidthStereo && g.in != n) result.bound.emplace_back(g.id, n);
  }
  std::sort(result.bound.begin(), result.bound.end());
  return result;
}

}  // namespace pq::doc
