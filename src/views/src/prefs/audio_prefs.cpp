// The preferences window, Audio tab (T3.18): pq/views/prefs/audio_prefs.h describes it.
#include "pq/views/prefs/audio_prefs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "../common/paint.h"
#include "pq/ui/theme.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views {

using namespace pq::ui;

const char* audioDriverTypeName(AudioDriverType t) noexcept {
  switch (t) {
    case AudioDriverType::Asio: return "ASIO";
    case AudioDriverType::WasapiShared: return "WASAPI (shared)";
    case AudioDriverType::WasapiExclusive: return "WASAPI (exclusive)";
  }
  return "?";
}

const char* audioDriverTypeKey(AudioDriverType t) noexcept {
  switch (t) {
    case AudioDriverType::Asio: return "asio";
    case AudioDriverType::WasapiShared: return "wasapi-shared";
    case AudioDriverType::WasapiExclusive: return "wasapi-exclusive";
  }
  return "?";
}

bool parseAudioDriverType(std::string_view key, AudioDriverType& out) noexcept {
  for (AudioDriverType t : {AudioDriverType::Asio, AudioDriverType::WasapiShared, AudioDriverType::WasapiExclusive})
    if (key == audioDriverTypeKey(t)) {
      out = t;
      return true;
    }
  return false;
}

FakeAudioPrefsPort FakeAudioPrefsPort::studio() {
  FakeAudioPrefsPort p;
  Driver asio{AudioDriverType::Asio, {}, {}, {}};
  asio.outputs = {{"Focusrite USB ASIO", "Focusrite USB ASIO", 2, 2, false},
                  {"ASIO4ALL v2", "ASIO4ALL v2", 2, 2, false},
                  {"Voicemeeter Virtual ASIO", "Voicemeeter Virtual ASIO", 8, 8, false}};
  asio.caps.sampleRates = {44100, 48000, 88200, 96000, 176400, 192000};
  asio.caps.bufferSizes = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024};
  asio.caps.maxInputChannels = 2;
  asio.caps.maxOutputChannels = 2;
  asio.caps.hasControlPanel = true;
  Driver shared{AudioDriverType::WasapiShared, {}, {}, {}};
  shared.outputs = {{"", "System default (Speakers (Focusrite USB Audio))", 0, 2, true},
                    {"{0.0.0.00000000}.{a}", "Speakers (Focusrite USB Audio)", 0, 2, true},
                    {"{0.0.0.00000000}.{b}", "Headphones (Realtek(R) Audio)", 0, 2, false}};
  shared.inputs = {{"", "No input", 0, 0, false}, {"{0.0.1.00000000}.{c}", "Analogue 1 + 2 (Focusrite USB Audio)", 2, 0, true}};
  shared.caps.sampleRates = {48000};
  shared.caps.bufferSizes = {128, 256, 480, 512, 1024};
  shared.caps.maxOutputChannels = 2;
  shared.caps.inputSupported = false;
  Driver exclusive = shared;
  exclusive.type = AudioDriverType::WasapiExclusive;
  exclusive.caps.sampleRates = {44100, 48000, 88200, 96000};
  exclusive.caps.bufferSizes = {144, 192, 288, 480, 960};
  p.drivers = {asio, shared, exclusive};
  p.current.driver = AudioDriverType::Asio;
  p.current.outputDevice = "Focusrite USB ASIO";
  p.current.outputName = "Focusrite USB ASIO";
  p.current.sampleRate = 48000;
  p.current.bufferFrames = 64;
  p.current.inputChannels = 2;
  p.current.outputChannels = 2;
  p.running.state = AudioStatus::State::Running;
  p.running.deviceName = "Focusrite USB ASIO";
  p.running.sampleRate = 48000;
  p.running.bufferFrames = 64;
  p.running.inputChannels = 2;
  p.running.outputChannels = 2;
  p.running.inputLatencyMs = 1.646;
  p.running.outputLatencyMs = 3.938;
  p.running.revision = 1;
  p.load = 2.1;
  return p;
}

namespace {

constexpr float kLabelWidth = 190.0f;
constexpr float kControlWidth = 320.0f;
constexpr float kRowHeight = 22.0f;
constexpr uint32_t kCommonRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
constexpr uint32_t kCommonBuffers[] = {32, 64, 128, 256, 512, 1024, 2048};

// The tab column at the left (Live's Preferences tabs; only Audio exists so far): a sunken strip with the selected
// tab highlighted.
class TabColumn : public Widget {
public:
  TabColumn() : Widget("tabs") {
    setLayout(LayoutKind::Column);
    setWidth(Length::fixed(96));
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    ctx.dl.addRectFilled(rect().toRect(), th->surface.sunken);
    const IRect sel{rect().x, rect().y, rect().w, ctx.scale.px(kRowHeight + 4.0f)};
    ctx.dl.addRectFilled(sel.toRect(), th->surface.highlight);
  }
};

// A 1-device-px line under a section title.
class Divider : public Widget {
public:
  Divider() : Widget() { setHeight(Length::fixed(1)); }
  void paint(PaintContext& ctx) override {
    if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(IRect{rect().x, rect().y, rect().w, 1}.toRect(), th->surface.divider);
  }
};

Widget& section(Widget& parent, const char* name, const char* title) {
  auto& w = parent.add<Widget>(name);
  w.setLayout(LayoutKind::Column);
  w.setHeight(Length::fixed(21));
  w.setGap(0);
  auto& l = w.add<Label>("title", title, FontRoleId::BodyStrong, TextTone::Primary);
  l.setHeight(Length::fixed(20));
  w.add<Divider>();
  return w;
}

Widget& row(Widget& parent, const char* name, const char* label, Label** labelOut = nullptr) {
  auto& r = parent.add<Widget>(name);
  r.setLayout(LayoutKind::Row);
  r.setHeight(Length::fixed(kRowHeight));
  r.setGap(8);
  auto& l = r.add<Label>("label", label, FontRoleId::Small, TextTone::Secondary);
  l.setWidth(Length::fixed(kLabelWidth));
  if (labelOut) *labelOut = &l;
  return r;
}

std::string fmtMs(double ms) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f ms", ms);
  return b;
}

// After a driver or device change: keep the rate and buffer size when the new device can do them, else take the
// nearest it can (48 kHz preferred; the next larger buffer).
void fitToCaps(AudioPrefsPort& port, AudioSettings& s) {
  AudioDeviceCaps c = port.capabilities(s);
  std::sort(c.sampleRates.begin(), c.sampleRates.end());
  std::sort(c.bufferSizes.begin(), c.bufferSizes.end());
  if (!c.sampleRates.empty() && !std::binary_search(c.sampleRates.begin(), c.sampleRates.end(), s.sampleRate))
    s.sampleRate = std::binary_search(c.sampleRates.begin(), c.sampleRates.end(), 48000u) ? 48000u : c.sampleRates.front();
  if (!c.bufferSizes.empty() && !std::binary_search(c.bufferSizes.begin(), c.bufferSizes.end(), s.bufferFrames)) {
    const auto up = std::lower_bound(c.bufferSizes.begin(), c.bufferSizes.end(), s.bufferFrames);
    s.bufferFrames = up != c.bufferSizes.end() ? *up : c.bufferSizes.back();
  }
  if (!c.inputSupported) s.inputChannels = 0;
  if (c.maxOutputChannels > 0) s.outputChannels = std::min(s.outputChannels, std::max(c.maxOutputChannels, 1u));
}

int32_t indexOf(const std::vector<uint32_t>& v, uint32_t x) {
  const auto it = std::find(v.begin(), v.end(), x);
  return it == v.end() ? 0 : static_cast<int32_t>(it - v.begin());
}

}  // namespace

PreferencesWindow::PreferencesWindow(AudioPrefsPort& port) : Popover("preferences"), port_(port) {
  setSize(Length::fixed(kWidth), Length::fixed(kHeight));
  shown_ = port_.settings();

  auto& header = add<Widget>("header");
  header.setLayout(LayoutKind::Row);
  header.setHeight(Length::fixed(22));
  header.add<Label>("title", "Preferences", FontRoleId::Title, TextTone::Primary);
  auto& close = header.add<Button>("close", "Close");
  close.setWidth(Length::fixed(56));
  close.setInfo("Close", "Closes the preferences (Esc). Settings apply as soon as they are chosen.");
  close.onClick = [this] {
    if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*this);
  };

  auto& body = add<Widget>("body");
  body.setLayout(LayoutKind::Row);
  body.setGap(12);
  auto& tabs = body.add<TabColumn>();
  tabs.setPadding({8, 4, 4, 4});
  auto& audioTab = tabs.add<Label>("audio", "Audio", FontRoleId::BodyStrong, TextTone::Primary);
  audioTab.setHeight(Length::fixed(kRowHeight));
  audioTab.setInfo("Audio", "Audio device, sample rate, buffer size, latency and the test tone.");

  auto& page = body.add<Widget>("audio");
  page.setLayout(LayoutKind::Column);
  page.setGap(5);

  section(page, "device-section", "Audio Device");
  {
    auto& r = row(page, "driver", "Driver Type");
    driver_ = &r.add<Dropdown>("chooser", std::vector<std::string>{}, 0);
    driver_->setWidth(Length::fixed(kControlWidth));
    driver_->setInfo("Driver Type", "ASIO talks to the interface's own driver (lowest latency). WASAPI shared mixes "
                                    "with other programs; WASAPI exclusive takes the device for itself.");
    driver_->onChange = [this](int32_t i) {
      if (i < 0 || static_cast<size_t>(i) >= drivers_.size()) return;
      AudioSettings s = shown_;
      if (s.driver == drivers_[static_cast<size_t>(i)]) return;
      s.driver = drivers_[static_cast<size_t>(i)];
      // Another driver type has other devices: start from its default (the first entry).
      const auto outs = port_.outputDevices(s.driver);
      s.outputDevice = outs.empty() ? std::string() : outs.front().id;
      s.outputName = outs.empty() ? std::string() : outs.front().name;
      s.inputDevice.clear();
      s.inputName.clear();
      fitToCaps(port_, s);
      choose(std::move(s));
    };
  }
  {
    auto& r = row(page, "output", "Audio Output Device", &outputLabel_);
    outputRow_ = &r;
    output_ = &r.add<Dropdown>("chooser", std::vector<std::string>{}, 0);
    output_->setWidth(Length::fixed(kControlWidth));
    output_->setInfo("Audio Device", "The device the master plays on (ASIO: the driver, for input and output).");
    output_->onChange = [this](int32_t i) {
      if (i < 0 || static_cast<size_t>(i) >= outputs_.size()) return;
      AudioSettings s = shown_;
      s.outputDevice = outputs_[static_cast<size_t>(i)].id;
      s.outputName = outputs_[static_cast<size_t>(i)].name;
      fitToCaps(port_, s);
      choose(std::move(s));
    };
  }
  {
    auto& r = row(page, "input", "Audio Input Device");
    inputRow_ = &r;
    input_ = &r.add<Dropdown>("chooser", std::vector<std::string>{}, 0);
    input_->setWidth(Length::fixed(kControlWidth));
    input_->setInfo("Audio Input Device", "The device recording reads from (WASAPI). Inputs are opened from M6 "
                                          "(recording); until then the choice is kept for later.");
    input_->onChange = [this](int32_t i) {
      if (i < 0 || static_cast<size_t>(i) >= inputs_.size()) return;
      AudioSettings s = shown_;
      s.inputDevice = inputs_[static_cast<size_t>(i)].id;
      s.inputName = inputs_[static_cast<size_t>(i)].name;
      choose(std::move(s));
    };
  }
  {
    auto& r = row(page, "hardware", "Hardware Setup");
    panelRow_ = &r;
    panel_ = &r.add<Button>("button", "Hardware Setup");
    panel_->setWidth(Length::fixed(140));
    panel_->setInfo("Hardware Setup", "Opens the ASIO driver's control panel (its buffer size, clock source, routing).");
    panel_->onClick = [this] { (void)port_.openControlPanel(); };
  }
  {
    auto& r = row(page, "channels", "Input / Output Channels");
    inChannels_ = &r.add<Dropdown>("inputs", std::vector<std::string>{}, 0);
    inChannels_->setWidth(Length::fixed(156));
    inChannels_->setInfo("Input Channels", "How many of the device's inputs the stream opens (from input 1).");
    inChannels_->onChange = [this](int32_t i) {
      if (i < 0 || static_cast<size_t>(i) >= inCounts_.size()) return;
      AudioSettings s = shown_;
      s.inputChannels = inCounts_[static_cast<size_t>(i)];
      choose(std::move(s));
    };
    outChannels_ = &r.add<Dropdown>("outputs", std::vector<std::string>{}, 0);
    outChannels_->setWidth(Length::fixed(156));
    outChannels_->setInfo("Output Channels", "How many of the device's outputs the stream opens. The master plays on "
                                             "outputs 1/2; the others get silence.");
    outChannels_->onChange = [this](int32_t i) {
      if (i < 0 || static_cast<size_t>(i) >= outCounts_.size()) return;
      AudioSettings s = shown_;
      s.outputChannels = outCounts_[static_cast<size_t>(i)];
      choose(std::move(s));
    };
  }
  {
    auto& r = row(page, "cue", "Cue Out");
    cue_ = &r.add<Dropdown>("chooser", std::vector<std::string>{}, 0);
    cue_->setWidth(Length::fixed(156));
    cue_->setInfo("Cue Out", "Where the browser's preview plays: with the master on outputs 1/2, or on another output "
                             "pair (headphones on a second pair: open enough Output Channels first).");
    cue_->onChange = [this](int32_t i) {
      if (i < 0) return;
      AudioSettings s = shown_;
      s.cueOutput = static_cast<uint32_t>(i);
      choose(std::move(s));
    };
  }

  section(page, "rate-section", "Sample Rate");
  {
    auto& r = row(page, "rate", "In/Out Sample Rate");
    rate_ = &r.add<Dropdown>("chooser", std::vector<std::string>{}, 0);
    rate_->setWidth(Length::fixed(156));
    rate_->setInfo("Sample Rate", "The rate the device and the engine run at. Changing it recompiles the project for "
                                  "the new rate (a short pause; playback resumes where it was).");
    rate_->onChange = [this](int32_t i) {
      if (i < 0 || static_cast<size_t>(i) >= rates_.size()) return;
      AudioSettings s = shown_;
      s.sampleRate = rates_[static_cast<size_t>(i)];
      choose(std::move(s));
    };
  }

  section(page, "latency-section", "Latency");
  {
    auto& r = row(page, "buffer", "Buffer Size");
    buffer_ = &r.add<Dropdown>("chooser", std::vector<std::string>{}, 0);
    buffer_->setWidth(Length::fixed(156));
    buffer_->setInfo("Buffer Size", "Frames per device callback: smaller = lower latency, more CPU and a higher risk "
                                    "of dropouts.");
    buffer_->onChange = [this](int32_t i) {
      if (i < 0 || static_cast<size_t>(i) >= buffers_.size()) return;
      AudioSettings s = shown_;
      s.bufferFrames = buffers_[static_cast<size_t>(i)];
      choose(std::move(s));
    };
  }
  {
    auto& r = row(page, "latency", "Input / Output / Overall");
    latency_ = &r.add<Label>("value", "", FontRoleId::Small, TextTone::Primary);
    latency_->setInfo("Latency", "As the driver reports it, plus the engine's block FIFO when the buffer size is not a "
                                 "multiple of the engine block.");
  }

  section(page, "test-section", "Test");
  {
    auto& r = row(page, "tone", "Test Tone");
    tone_ = &r.add<Toggle>("toggle", "On", ToggleStyle::Accent, port_.testTone());
    tone_->setWidth(Length::fixed(40));
    tone_->setInfo("Test Tone", "Plays a sine on outputs 1/2 (at the volume and frequency to the right).");
    tone_->onToggle = [this](bool) { sendTone(); };
    ValueSpec vs;
    vs.min = -60.0;
    vs.max = -12.0;
    vs.def = -36.0;
    vs.step = 1.0;
    vs.decimals = 0;
    vs.unit = "dB";
    toneDb_ = &r.add<DragBox>("volume", vs);
    toneDb_->setWidth(Length::fixed(72));
    toneDb_->setValue(-36.0);
    toneDb_->setInfo("Tone Volume", "The test tone's level in dBFS (−60 … −12).");
    toneDb_->onChange = [this](double) { sendTone(); };
    ValueSpec fs;
    fs.min = 20.0;
    fs.max = 20000.0;
    fs.def = 440.0;
    fs.step = 1.0;
    fs.decimals = 0;
    fs.unit = "Hz";
    fs.dragPixels = 600.0;
    toneHz_ = &r.add<DragBox>("frequency", fs);
    toneHz_->setWidth(Length::fixed(84));
    toneHz_->setValue(440.0);
    toneHz_->setInfo("Tone Frequency", "The test tone's frequency (20 Hz … 20 kHz).");
    toneHz_->onChange = [this](double) { sendTone(); };
  }
  {
    auto& r = row(page, "cpu", "CPU Usage");
    cpu_ = &r.add<Label>("value", "", FontRoleId::Small, TextTone::Primary);
    cpu_->setInfo("CPU Usage", "Time spent in the audio callback per buffer period, averaged over the last half "
                               "second. 0 while the stream is stopped (an idle DAW runs no audio).");
  }
  auto& spacer = page.add<Widget>("spacer");
  spacer.setHeight(Length::flex());
  {
    auto& r = page.add<Widget>("status");
    r.setLayout(LayoutKind::Row);
    r.setHeight(Length::fixed(kRowHeight));
    statusLabel_ = &r.add<Label>("value", "", FontRoleId::Small, TextTone::Secondary);
  }

  port_.setListener([alive = alive_, this] {
    if (*alive) refresh();
  });
  refresh();
}

PreferencesWindow::~PreferencesWindow() {
  *alive_ = false;
  port_.setListener(nullptr);
}

void PreferencesWindow::startPolling() {
  if (timer_ == 0) timer_ = requestTimer(kPollSeconds);
}

void PreferencesWindow::onTimer(uint32_t id) {
  if (id != timer_) return;
  timer_ = 0;
  updateReadouts(false);
  startPolling();
}

void PreferencesWindow::refresh() {
  shown_ = port_.settings();
  rebuildChoosers();
  updateReadouts(true);
}

void PreferencesWindow::choose(AudioSettings s) {
  shown_ = std::move(s);
  port_.apply(shown_);
  shown_ = port_.settings();
  rebuildChoosers();
  updateReadouts(true);
}

void PreferencesWindow::sendTone() {
  port_.setTestTone(tone_->on(), static_cast<float>(toneDb_->value()), static_cast<float>(toneHz_->value()));
}

void PreferencesWindow::rebuildChoosers() {
  const bool asio = shown_.driver == AudioDriverType::Asio;
  drivers_ = port_.driverTypes();
  {
    std::vector<std::string> items;
    int32_t sel = 0;
    for (size_t i = 0; i < drivers_.size(); ++i) {
      items.emplace_back(audioDriverTypeName(drivers_[i]));
      if (drivers_[i] == shown_.driver) sel = static_cast<int32_t>(i);
    }
    driver_->setItems(std::move(items), sel);
  }
  outputs_ = port_.outputDevices(shown_.driver);
  {
    int32_t sel = -1;
    for (size_t i = 0; i < outputs_.size(); ++i)
      if (outputs_[i].id == shown_.outputDevice) sel = static_cast<int32_t>(i);
    if (sel < 0) {  // the chosen device is not connected: keep showing it
      outputs_.push_back({shown_.outputDevice, (shown_.outputName.empty() ? shown_.outputDevice : shown_.outputName) +
                                                   " (not connected)", 0, 0, false});
      sel = static_cast<int32_t>(outputs_.size()) - 1;
    }
    std::vector<std::string> items;
    for (const AudioDeviceEntry& e : outputs_) items.push_back(e.name);
    output_->setItems(std::move(items), sel);
  }
  outputLabel_->setText(asio ? "Audio Device" : "Audio Output Device");
  inputRow_->setVisible(!asio);
  panelRow_->setVisible(asio);
  inputs_ = port_.inputDevices(shown_.driver);
  {
    int32_t sel = -1;
    for (size_t i = 0; i < inputs_.size(); ++i)
      if (inputs_[i].id == shown_.inputDevice) sel = static_cast<int32_t>(i);
    if (sel < 0 && !shown_.inputDevice.empty()) {
      inputs_.push_back({shown_.inputDevice, (shown_.inputName.empty() ? shown_.inputDevice : shown_.inputName) +
                                                 " (not connected)", 0, 0, false});
      sel = static_cast<int32_t>(inputs_.size()) - 1;
    }
    std::vector<std::string> items;
    for (const AudioDeviceEntry& e : inputs_) items.push_back(e.name);
    input_->setItems(std::move(items), std::max(sel, 0));
  }
  caps_ = port_.capabilities(shown_);
  panel_->setEnabled(caps_.hasControlPanel);
  rates_ = caps_.sampleRates;
  if (rates_.empty()) rates_.assign(std::begin(kCommonRates), std::end(kCommonRates));
  if (std::find(rates_.begin(), rates_.end(), shown_.sampleRate) == rates_.end()) rates_.push_back(shown_.sampleRate);
  std::sort(rates_.begin(), rates_.end());
  {
    std::vector<std::string> items;
    for (uint32_t r : rates_) items.push_back(std::to_string(r));
    rate_->setItems(std::move(items), indexOf(rates_, shown_.sampleRate));
  }
  buffers_ = caps_.bufferSizes;
  if (buffers_.empty()) buffers_.assign(std::begin(kCommonBuffers), std::end(kCommonBuffers));
  if (std::find(buffers_.begin(), buffers_.end(), shown_.bufferFrames) == buffers_.end()) buffers_.push_back(shown_.bufferFrames);
  std::sort(buffers_.begin(), buffers_.end());
  {
    std::vector<std::string> items;
    for (uint32_t b : buffers_) items.push_back(std::to_string(b) + " Samples");
    buffer_->setItems(std::move(items), indexOf(buffers_, shown_.bufferFrames));
  }
  inCounts_.assign(1, 0);
  if (caps_.inputSupported)
    for (uint32_t n = 1; n <= caps_.maxInputChannels; ++n) inCounts_.push_back(n);
  if (std::find(inCounts_.begin(), inCounts_.end(), shown_.inputChannels) == inCounts_.end()) inCounts_.push_back(shown_.inputChannels);
  {
    std::vector<std::string> items;
    for (uint32_t n : inCounts_) items.push_back(n == 0 ? std::string("No inputs") : std::to_string(n) + (n == 1 ? " input" : " inputs"));
    inChannels_->setItems(std::move(items), indexOf(inCounts_, shown_.inputChannels));
  }
  inChannels_->setEnabled(caps_.inputSupported);
  outCounts_.clear();
  const uint32_t maxOut = std::max<uint32_t>(caps_.maxOutputChannels, 1);
  outCounts_.push_back(std::min<uint32_t>(2, maxOut));
  for (uint32_t n = 4; n <= maxOut; n += 2) outCounts_.push_back(n);
  if (maxOut > 2 && (maxOut & 1)) outCounts_.push_back(maxOut);
  if (std::find(outCounts_.begin(), outCounts_.end(), shown_.outputChannels) == outCounts_.end()) outCounts_.push_back(shown_.outputChannels);
  std::sort(outCounts_.begin(), outCounts_.end());
  {
    std::vector<std::string> items;
    for (uint32_t n : outCounts_) items.push_back(std::to_string(n) + (n == 1 ? " output" : " outputs"));
    outChannels_->setItems(std::move(items), indexOf(outCounts_, shown_.outputChannels));
  }
  {  // T5.10: the cue pair among the opened outputs
    std::vector<std::string> items{"1/2 (Master)"};
    for (uint32_t k = 1; 2 * k + 2 <= shown_.outputChannels; ++k)
      items.push_back(std::to_string(2 * k + 1) + "/" + std::to_string(2 * k + 2));
    const auto idx = static_cast<int32_t>(shown_.cueOutput < items.size() ? shown_.cueOutput : 0);
    cue_->setItems(std::move(items), idx);
  }
}

void PreferencesWindow::updateReadouts(bool force) {
  const AudioStatus st = port_.status();
  if (force || st.revision != status_.revision || !(st == status_)) {
    status_ = st;
    std::string s;
    char b[160];
    switch (st.state) {
      case AudioStatus::State::Opening: s = "Opening the audio device\xE2\x80\xA6"; break;
      case AudioStatus::State::Running:
      case AudioStatus::State::Suspended:
        std::snprintf(b, sizeof b, "%s \xC2\xB7 %s \xC2\xB7 %u Hz \xC2\xB7 %u samples \xC2\xB7 %u in / %u out",
                      st.state == AudioStatus::State::Running ? "Running" : "Idle (stream stopped)",
                      st.deviceName.c_str(), st.sampleRate, st.bufferFrames, st.inputChannels, st.outputChannels);
        s = b;
        break;
      case AudioStatus::State::NoDevice: s = "No audio device: the engine runs silently"; break;
    }
    if (!st.message.empty()) s += " \xE2\x80\x94 " + st.message;
    statusLabel_->setText(std::move(s));
    const bool known = st.state == AudioStatus::State::Running || st.state == AudioStatus::State::Suspended;
    if (!known) {
      latency_->setText("\xE2\x80\x94");
    } else {
      const std::string in = st.inputChannels > 0 ? fmtMs(st.inputLatencyMs) : std::string("\xE2\x80\x94");
      latency_->setText(in + "  \xC2\xB7  " + fmtMs(st.outputLatencyMs) + "  \xC2\xB7  " +
                        fmtMs((st.inputChannels > 0 ? st.inputLatencyMs : 0.0) + st.outputLatencyMs));
    }
  }
  const double load = port_.cpuLoad();
  char b[32];
  std::snprintf(b, sizeof b, "%.1f %%", std::max(0.0, load));
  if (cpu_->text() != b) cpu_->setText(b);
}

const std::string& PreferencesWindow::statusText() const { return statusLabel_->text(); }
const std::string& PreferencesWindow::latencyText() const { return latency_->text(); }
const std::string& PreferencesWindow::cpuText() const { return cpu_->text(); }

bool PreferencesWindow::onKey(const KeyEvent& e) { return Popover::onKey(e); }

InfoText PreferencesWindow::info() const {
  return {"Preferences", "Audio device, sample rate, buffer size and latency. Changes apply at once and are kept for "
                         "the next start. Esc closes."};
}

PreferencesWindow* openPreferencesWindow(const Widget& w) {
  OverlayLayer* layer = OverlayLayer::of(w);
  if (!layer) return nullptr;
  for (size_t i = 0; i < layer->childCount(); ++i)
    if (auto* p = dynamic_cast<PreferencesWindow*>(&layer->child(i)); p && layer->isOpen(*p)) return p;
  return nullptr;
}

PreferencesWindow* openPreferences(Widget& from, AudioPrefsPort& port) {
  OverlayLayer* layer = OverlayLayer::of(from);
  if (!layer) return nullptr;
  if (PreferencesWindow* open = openPreferencesWindow(from)) return open;
  auto win = std::make_unique<PreferencesWindow>(port);
  PreferencesWindow* raw = win.get();
  UiTree* tree = layer->tree();
  const UiScale s(tree ? tree->scale() : 1.0f);
  const int32_t W = tree ? static_cast<int32_t>(tree->width()) : 0, H = tree ? static_cast<int32_t>(tree->height()) : 0;
  OverlayOptions o;
  o.anchor = {std::max(0, (W - s.px(PreferencesWindow::kWidth)) / 2), std::max(0, (H - s.px(PreferencesWindow::kHeight)) / 2), 0, 0};
  o.placement = Placement::AtPoint;
  layer->open(std::move(win), std::move(o));
  raw->startPolling();
  return raw;
}

namespace {

// Holds the app.preferences handler for the shell's lifetime (a hidden child of the clip detail slot, like the MOS wheel host).
class PreferencesHost : public Widget {
public:
  explicit PreferencesHost(Shell& shell) : Widget("preferences-host"), ctx_(shell.context()) {
    setVisible(false);
    OverlayLayer* layer = &shell.overlay();
    ctx_.setViewAction(
        "app.preferences",
        [this, layer] {
          if (PreferencesWindow* open = openPreferencesWindow(*layer)) {  // the hotkey toggles the window
            layer->close(*open);
            return;
          }
          if (AudioPrefsPort* port = ctx_.audioPrefs()) (void)openPreferences(*layer, *port);
        },
        [this] { return ctx_.audioPrefs() != nullptr; });
  }
  ~PreferencesHost() override { ctx_.clearViewAction("app.preferences"); }

private:
  ViewContext& ctx_;
};

}  // namespace

void installPreferences(Shell& shell) { shell.slot(ShellSlot::ClipDetail).add<PreferencesHost>(shell); }

}  // namespace pq::views
