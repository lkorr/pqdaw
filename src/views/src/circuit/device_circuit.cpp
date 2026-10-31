// "Open in Circuit View" on a device of the project (T4.4-fix1): pq/views/circuit/device_circuit.h.
#include "pq/views/circuit/device_circuit.h"

#include <memory>
#include <string>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/views/view_context.h"

namespace pq::views::circuit {

namespace {

std::vector<graph::ConstArg> argsOf(const std::vector<doc::DeviceValue>& vs) {
  std::vector<graph::ConstArg> out;
  out.reserve(vs.size());
  for (const doc::DeviceValue& v : vs) out.push_back({v.name, v.value});
  return out;
}

// T4.15: what the editor hides and its tooltips follow the instance (its settings and param values).
void showInstance(CircuitEditor& ed, const doc::Device& d) { ed.setInstance(d.patch, argsOf(d.settings), argsOf(d.params)); }

// The device's text as the project holds it: its own, or the stock file's.
std::string projectText(const doc::DeviceLibrary& lib, const doc::Device& d) {
  return d.source ? d.source->text : lib.stockText(d.patch);
}

class DeviceHost final : public CircuitEditor::Host {
public:
  DeviceHost(CircuitEditor& editor, ViewContext& ctx, doc::DeviceId device, std::unique_ptr<PatchPlayer> player)
      : editor_(editor), ctx_(ctx), device_(device), player_(std::move(player)) {
    if (const doc::Device* d = doc::findDevice(*ctx.doc(), device).device) seen_ = d->source;
    // Runs under the editor: a key per editor run (an edit that coalesced into the previous step keeps the serial).
    keyBase_ = (reinterpret_cast<uintptr_t>(this) * 0x9E3779B97F4A7C15ull) | 1u;
    editor_.document().onCommitted = [this](const std::string& text, const graph::ParseResult& parsed) {
      commit(text, parsed);
    };
    sub_ = ctx.subscribe([this](const doc::StoreEvent& e) { onStore(e); });
  }

  bool undo() override { return ctx_.undo(); }
  bool redo() override { return ctx_.redo(); }
  std::string title() const override { return deviceEditorTitle(ctx_, device_); }

private:
  void commit(const std::string& text, const graph::ParseResult& parsed) {
    const doc::Device* d = doc::findDevice(*ctx_.doc(), device_).device;
    if (d == nullptr) return;
    const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
    if (!d->source && lib && text == lib->stockText(d->patch)) return;  // nothing new
    auto ids = std::make_shared<const graph::PatchFile>(parsed.file);
    // M4 review M8: a text the device library would reject (it renames the device's patch, changes its kind, or its
    // root does not describe a device) is not committed: the last good text keeps playing and is what is saved (a
    // committed rejected text played stale for the session and bypassed the device after a reload). The editor keeps
    // the text and says why; the next text that resolves commits.
    if (lib != nullptr) {
      doc::Device candidate = *d;
      auto src = std::make_shared<doc::UserPatch>();
      src->text = text;
      src->ids = ids;
      candidate.source = std::move(src);
      const std::shared_ptr<const doc::ResolvedDevice> r = lib->resolve(candidate);
      if (r && !r->error.empty()) {
        const std::string why = "Not applied to the device: " + r->error + " (the device keeps its last good text)";
        editor_.showStatus(why);
        ctx_.setStatus(why);
        return;
      }
    }
    auto e = doc::ops::editDevicePatch(*ctx_.doc(), device_, text, std::move(ids));
    if (!e) {
      ctx_.setStatus(e.error().message);
      return;
    }
    if (e->empty()) return;
    const bool fork = !d->edited();
    const uint64_t key = keyBase_ ^ (editor_.document().stepSerial() << 1);
    if (Status s = ctx_.store().dispatchCoalescing(e->command, key); !s) {
      ctx_.setStatus(s.error().message);
      return;
    }
    if (fork) {
      const std::string note = "Edited: this device is now the track's own copy (the stock device and its other "
                               "instances are unchanged; Ctrl+Z, or Revert to Stock in its menu, goes back).";
      ctx_.setStatus(note);
      editor_.showStatus(note);
    }
  }

  void onStore(const doc::StoreEvent& e) {
    const doc::Device* d = doc::findDevice(*e.after, device_).device;
    if (d == nullptr) {  // removed: nothing left to edit
      ctx_.setStatus("The device was removed: its circuit view closed.");
      editor_.closeSoon();
      return;
    }
    showInstance(editor_, *d);  // settings re-hide (no compile), param values show in tooltips
    if (d->source == seen_) return;  // the text did not change (a param, another device, another view)
    seen_ = d->source;
    editor_.refreshTitle();  // edited / stock
    const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
    if (lib == nullptr) return;
    const std::string text = projectText(*lib, *d);
    if (text == editor_.document().text()) return;  // our own edit coming back
    // Undo, redo, Revert to Stock: the editor follows (the ids the project plays, where known).
    const graph::PatchFile* ids = d->source && d->source->ids ? d->source->ids.get() : nullptr;
    if (!d->source) ids = lib->stockFile(d->patch);
    editor_.document().resetText(text, ids);
  }

  CircuitEditor& editor_;
  ViewContext& ctx_;
  doc::DeviceId device_;
  std::unique_ptr<PatchPlayer> player_;  // the project's player for this device (probes, cost), or null
  doc::UserPatchPtr seen_;
  uint64_t keyBase_ = 1;
  ViewContext::Subscription sub_;
};

}  // namespace

void followViewPrefs(CircuitEditor& editor, ViewContext& ctx) {
  editor.setShowInactive(ctx.viewPrefs().circuitShowInactive);
  editor.onShowInactiveChanged = [&ctx](bool on) {
    ViewPrefs p = ctx.viewPrefs();
    p.circuitShowInactive = on;
    ctx.setViewPrefs(p);
  };
}

std::string deviceEditorTitle(const ViewContext& ctx, doc::DeviceId device) {
  const doc::DeviceLoc l = doc::findDevice(*ctx.doc(), device);
  if (l.device == nullptr) return "(removed device)";
  const doc::DeviceLibrary* lib = ctx.deviceLibrary();
  const doc::DeviceInfo* info = lib ? lib->catalog().find(l.device->patch) : nullptr;
  std::string t = info ? info->name : l.device->patch;
  if (l.device->edited()) t += " (edited)";
  if (l.track) t += " \xC2\xB7 " + l.track->name;
  return t;
}

CircuitEditor* openDeviceInCircuitView(ui::Widget& from, ViewContext& ctx, doc::DeviceId device) {
  const doc::Device* d = doc::findDevice(*ctx.doc(), device).device;
  const doc::DeviceLibrary* lib = ctx.deviceLibrary();
  if (d == nullptr || lib == nullptr) {
    ctx.setStatus(d == nullptr ? "No such device" : "No device library: the device cannot be opened");
    return nullptr;
  }
  const std::string path = lib->pathOf(d->patch);
  if (path.empty()) {
    ctx.setStatus("'" + d->patch + "' is not in the device library: it cannot be opened");
    return nullptr;
  }
  // The ids the project plays: the user device's resolution, else the stock parse.
  const graph::PatchFile* ids = lib->stockFile(d->patch);
  std::shared_ptr<const doc::ResolvedDevice> resolved;
  if (d->source) {
    resolved = lib->resolve(*d);
    ids = resolved && resolved->parsedOk ? &resolved->parsed : (d->source->ids ? d->source->ids.get() : nullptr);
  }
  auto editor = PatchEditor::fromText(path, projectText(*lib, *d), ids);
  if (!editor) {
    ctx.setStatus("Cannot open the device: " + editor.error().message);
    return nullptr;
  }
  // Probes and cost through the project unit that plays the device (the app's player), when there is one.
  std::unique_ptr<PatchPlayer> player = ctx.devicePlayers() ? ctx.devicePlayers()(device) : nullptr;
  PatchPlayer* raw = player.get();
  // M4 review M8: a text without the device's patch (an older file, a hand edit) opens on its first patch.
  const bool hasPatch = (*editor)->patchIndex(d->patch) != graph::kNone;
  CircuitEditor* ed = openCircuitEditor(from, std::move(*editor), hasPatch ? std::string_view(d->patch) : std::string_view{}, raw);
  if (ed == nullptr) {
    ctx.setStatus("Cannot open the device: no patch '" + d->patch + "' in its text");
    return nullptr;
  }
  ed->setHost(std::make_unique<DeviceHost>(*ed, ctx, device, std::move(player)));
  followViewPrefs(*ed, ctx);
  showInstance(*ed, *d);
  if (raw != nullptr) (void)ed->togglePlay();  // follow the project: LED trace, hover probes, Cost
  ed->refreshTitle();
  if (!hasPatch) ed->showStatus("This text has no patch '" + d->patch + "' (the device's): its first patch is shown");
  return ed;
}

}  // namespace pq::views::circuit
