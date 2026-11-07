#pragma once

// The frame a LEAF block shows in the Device view (T4.1; GU.11, docs/plans/GU.md "One hierarchy"): its signature and
// its generated body — a native engine's face (TMM.10), else one control per param and setting (an atom or value block).
// Groups are drawn as their own panels (group_frame.cpp), never as this. Private to pq_views.

#include <memory>
#include <string>

#include "pq/document/devices.h"
#include "pq/document/meta.h"
#include "pq/document/model.h"
#include "pq/meta/registry.h"
#include "pq/views/devices/device_model.h"
#include "pq/views/devices/panel_layout.h"
#include "pq/views/meta/meta_face.h"

namespace pq::views {

struct PanelTemplate {
  devices::DeviceSignature sig;
  devices::PanelLayout desc;
};

// The template of the block `d` (null: no library, or not a block). Keyed by the caller (DeviceChainView::Impl::
// templateKey).
inline std::unique_ptr<PanelTemplate> buildPanelTemplate(const doc::DeviceLibrary* lib, const doc::Device& d) {
  const graph::Patch* callee = lib != nullptr ? lib->patchOf(d) : nullptr;
  if (callee == nullptr) return nullptr;
  auto t = std::make_unique<PanelTemplate>();
  t->sig = devices::signatureOf(*callee);
  // TMM.10: a native engine's frame is its face (SPEC §15 2026-11-01), scaled to the Device view's height.
  if (const meta::Type* type = meta::findType(doc::metaDeviceType(d)); type != nullptr && hasFace(*type)) {
    const ui::Vec2 ws = faceSize(*type);
    t->desc = devices::facePanel(t->sig, std::string(type->name), ws.x, ws.y);
    return t;
  }
  const doc::DeviceInfo* info = lib->infoOf(d);
  t->desc = devices::autoPanel(t->sig, info != nullptr ? info->name : d.patch);
  return t;
}

}  // namespace pq::views
