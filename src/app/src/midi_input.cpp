// MIDI input ports (T6.4): pq/app/midi_input.h describes them.
#include "pq/app/midi_input.h"

#include <RtMidi.h>

#include <algorithm>
#include <exception>
#include <utility>

#include "pq/app/project_link.h"
#include "pq/engine/audio_block.h"

namespace pq::app {

// ---- RtMidi (WinMM) ------------------------------------------------------------------------------------------------

namespace {

// Port names as RtMidi gives them; a second device with the same name gets " 2" (and so on).
std::vector<std::string> uniqueNames(const std::vector<std::string>& names) {
  std::vector<std::string> out;
  for (size_t i = 0; i < names.size(); ++i) {
    const auto n = std::count(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(i), names[i]);
    out.push_back(n == 0 ? names[i] : names[i] + " " + std::to_string(n + 1));
  }
  return out;
}

class RtMidiPort final : public MidiInPort {
public:
  RtMidiPort(std::unique_ptr<RtMidiIn> in, MidiCallback cb, void* user) : in_(std::move(in)), cb_(cb), user_(user) {}
  RtMidiIn& in() noexcept { return *in_; }
  ~RtMidiPort() override {
    try {
      in_->cancelCallback();
      in_->closePort();
    } catch (...) {  // RtMidi reports by exception; a port that is already gone has nothing left to close
    }
  }
  // RtMidi's callback (its WinMM thread): one complete message.
  static void onRt(double, std::vector<unsigned char>* msg, void* self) {
    auto& p = *static_cast<RtMidiPort*>(self);
    if (msg != nullptr && !msg->empty()) p.cb_(p.user_, msg->data(), msg->size());
  }

private:
  std::unique_ptr<RtMidiIn> in_;
  MidiCallback cb_;
  void* user_;
};

class WinMmSource final : public MidiSource {
public:
  std::vector<std::string> portNames() override {
    try {
      RtMidiIn probe(RtMidi::WINDOWS_MM);
      std::vector<std::string> names;
      const unsigned n = probe.getPortCount();
      for (unsigned i = 0; i < n; ++i) names.push_back(probe.getPortName(i));
      return uniqueNames(names);
    } catch (const std::exception&) {
      return {};
    }
  }
  Result<std::unique_ptr<MidiInPort>> open(const std::string& name, MidiCallback cb, void* user) override {
    try {
      auto in = std::make_unique<RtMidiIn>(RtMidi::WINDOWS_MM, "pqdaw", 1024);
      const std::vector<std::string> names = [&] {
        std::vector<std::string> v;
        const unsigned n = in->getPortCount();
        for (unsigned i = 0; i < n; ++i) v.push_back(in->getPortName(i));
        return uniqueNames(v);
      }();
      const auto it = std::find(names.begin(), names.end(), name);
      if (it == names.end()) return Error{ErrorCode::NotFound, "MIDI port '" + name + "' is not connected", {}};
      in->ignoreTypes(true, true, true);  // SysEx, MIDI time code, clock / active sensing: never delivered
      auto port = std::make_unique<RtMidiPort>(std::move(in), cb, user);
      port->in().setCallback(&RtMidiPort::onRt, port.get());
      port->in().openPort(static_cast<unsigned>(it - names.begin()), "pqdaw");
      return std::unique_ptr<MidiInPort>(std::move(port));
    } catch (const std::exception& e) {
      return Error{ErrorCode::Device, std::string("MIDI port '") + name + "': " + e.what(), {}};
    }
  }
};

}  // namespace

std::unique_ptr<MidiSource> makeWinMmSource() { return std::make_unique<WinMmSource>(); }

// ---- MidiInputService ----------------------------------------------------------------------------------------------

MidiInputService::MidiInputService(ProjectLink& link, std::unique_ptr<MidiSource> source,
                                   std::vector<MidiPortSetting> settings,
                                   std::function<void(const std::vector<MidiPortSetting>&)> save)
    : link_(link), source_(std::move(source)), settings_(std::move(settings)), save_(std::move(save)) {}

MidiInputService::~MidiInputService() {
  for (Open& o : open_) close(o);
  open_.clear();
  link_.setLivePorts({}, {});
  if (anyOpen_ && onOpen_) onOpen_(false);
}

// The port's callback thread: stamp, push, tell the link (KB.3). Wait-free but for the link's one post per idle edge.
void MidiInputService::onMessage(void* user, const uint8_t* bytes, size_t size) noexcept {
  const auto& c = *static_cast<const Open::Ctx*>(user);
  const uint8_t status = bytes[0];
  if (status < 0x80 || status >= 0xF0) return;  // running status never comes from RtMidi; system messages: ignored
  engine::RawMidi m;
  m.stamp = static_cast<int64_t>(engine::qpcNow100ns());
  m.port = c.index;
  m.status = status;
  m.d1 = size > 1 ? static_cast<uint8_t>(bytes[1] & 0x7F) : 0;
  m.d2 = size > 2 ? static_cast<uint8_t>(bytes[2] & 0x7F) : 0;
  (void)c.rings->push(c.index, m);  // a full ring: counted, and the engine releases this port's notes
  c.link->signalActivity();          // KB.3: meters stopped while the engine slept poll again
  c.link->feedMidiNote(c.index, m);  // TU.2b: the views' note feed (nothing while no view listens)
}

MidiPortSetting& MidiInputService::settingFor(const std::string& name) {
  for (MidiPortSetting& s : settings_)
    if (s.name == name) return s;
  settings_.push_back(MidiPortSetting{name, true, 2.0f});  // a port seen for the first time: on (as Live does)
  if (save_) save_(settings_);
  return settings_.back();
}

void MidiInputService::close(Open& o) {
  o.port.reset();  // no callback runs after this: the ring has no other producer now
  if (!o.ctx) return;
  engine::RawMidi m;
  m.stamp = static_cast<int64_t>(engine::qpcNow100ns());
  m.port = static_cast<uint8_t>(o.index);
  for (uint8_t ch = 0; ch < 16; ++ch) {  // All Notes Off on every channel: nothing of this port stays held
    m.status = static_cast<uint8_t>(0xB0 | ch);
    m.d1 = 123;
    (void)link_.livePorts()->push(o.index, m);
    m.d1 = 121;  // M6 review L1: and its wheel centred (a device reusing the index starts clean)
    (void)link_.livePorts()->push(o.index, m);
    m.d1 = 123;  // TU.2b: the views' note feed lets go of its notes too (the port's thread is gone: this is its producer)
    link_.feedMidiNote(o.index, m);
  }
}

void MidiInputService::refresh() {
  present_ = source_->portNames();
  // Close what went away or was switched off.
  for (auto it = open_.begin(); it != open_.end();) {
    const bool present = std::find(present_.begin(), present_.end(), it->name) != present_.end();
    if (!present || !settingFor(it->name).enabled) {
      close(*it);
      it = open_.erase(it);
    } else {
      ++it;
    }
  }
  // Open what is enabled and present, into free ring indices.
  for (const std::string& name : present_) {
    if (!settingFor(name).enabled) continue;
    if (std::any_of(open_.begin(), open_.end(), [&](const Open& o) { return o.name == name; })) continue;
    uint32_t index = 0;
    while (index < engine::kLivePorts &&
           std::any_of(open_.begin(), open_.end(), [&](const Open& o) { return o.index == index; }))
      ++index;
    if (index == engine::kLivePorts) {
      lastError_ = "MIDI: more than 16 input ports are on; '" + name + "' is not opened";
      continue;
    }
    Open o;
    o.name = name;
    o.index = index;
    o.ctx = std::make_unique<Open::Ctx>(Open::Ctx{link_.livePorts().get(), static_cast<uint8_t>(index), &link_});
    auto port = source_->open(name, &MidiInputService::onMessage, o.ctx.get());
    if (!port.ok()) {
      lastError_ = port.error().message;
      continue;
    }
    o.port = std::move(port.value());
    open_.push_back(std::move(o));
  }
  publish();
}

void MidiInputService::publish() {
  std::vector<std::string> names(engine::kLivePorts);
  std::vector<float> bends(engine::kLivePorts, 0.0f);
  for (const Open& o : open_) {
    names[o.index] = o.name;
    bends[o.index] = settingFor(o.name).bendSemitones;
  }
  link_.setLivePorts(std::move(names), std::move(bends));
  const bool any = !open_.empty();
  if (any != anyOpen_) {
    anyOpen_ = any;
    if (onOpen_) onOpen_(any);
  }
}

std::vector<views::MidiPortEntry> MidiInputService::ports() {
  refresh();
  std::vector<views::MidiPortEntry> out;
  for (const std::string& name : present_) {
    const MidiPortSetting& s = settingFor(name);
    out.push_back({name, true, s.enabled, s.bendSemitones});
  }
  for (const MidiPortSetting& s : settings_)
    if (std::find(present_.begin(), present_.end(), s.name) == present_.end())
      out.push_back({s.name, false, s.enabled, s.bendSemitones});
  return out;
}

void MidiInputService::setPort(const std::string& name, bool enabled, float bendSemitones) {
  MidiPortSetting& s = settingFor(name);
  if (s.enabled == enabled && s.bendSemitones == bendSemitones) return;
  s.enabled = enabled;
  s.bendSemitones = bendSemitones > 0.0f ? bendSemitones : 2.0f;
  if (save_) save_(settings_);
  refresh();
}

std::vector<std::string> MidiInputService::openPorts() const {
  std::vector<std::string> v(engine::kLivePorts);
  for (const Open& o : open_) v[o.index] = o.name;
  return v;
}

}  // namespace pq::app
