#pragma once

// The panel a device shows (T4.1 Device view; TX.panel-faces: the project patch view's blocks too): its signature and
// its description — the .panel.json next to the device file, else the generated generic panel; a chain meta device's
// face panel (TMM.10). Private to pq_views.

#include <memory>
#include <string>

#include "pq/document/devices.h"
#include "pq/document/meta.h"
#include "pq/document/model.h"
#include "pq/meta/registry.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/devices/panel_desc.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views {

struct PanelTemplate {
  devices::DeviceSignature sig;
  devices::PanelDesc desc;
  bool designed = false;  // TX.panel-faces: from a .panel.json (else generated: autoPanel / facePanel)
};

// The template of what `d` plays (null: nothing plays — no library, a missing patch, an edited text that does not
// resolve). Keyed by the caller (DeviceChainView::Impl::templateKey).
inline std::unique_ptr<PanelTemplate> buildPanelTemplate(const doc::DeviceLibrary* lib, const doc::Device& d) {
  const graph::Patch* callee = lib != nullptr ? lib->patchOf(d) : nullptr;
  if (callee == nullptr) return nullptr;
  auto t = std::make_unique<PanelTemplate>();
  t->sig = devices::signatureOf(*callee);
  // TMM.10: a chain meta device's panel is its face (SPEC §15 2026-11-01), scaled to the Device view's height.
  if (const meta::Type* type = meta::findType(doc::metaDeviceType(d)); type != nullptr && hasFace(*type)) {
    // TMM.10-fix1: the socket shows where the chain is wired (another input when the device chose one; none when it is
    // unplugged: the absence of the wire).
    if (d.chainInput == doc::kChainUnplugged) t->sig.audioIn.clear();
    else if (!d.chainInput.empty() && !t->sig.audioIn.empty()) t->sig.audioIn = d.chainInput;
    const ui::Vec2 ws = faceSize(*type);
    t->desc = devices::facePanel(t->sig, std::string(type->name), ws.x, ws.y);
    return t;
  }
  // The stock panel while its bindings hold for the (edited) signature, else the generic one.
  const std::string file = lib->pathOf(d.patch);
  auto desc = file.empty() ? Result<devices::PanelDesc>(Error{ErrorCode::NotFound, "", {}})
                           : devices::loadPanelDesc(devices::panelPathFor(file), t->sig);
  const doc::DeviceInfo* info = lib->catalog().find(d.patch);
  t->designed = static_cast<bool>(desc);
  t->desc = desc ? std::move(*desc) : devices::autoPanel(t->sig, info != nullptr ? info->name : d.patch);
  return t;
}

}  // namespace pq::views
