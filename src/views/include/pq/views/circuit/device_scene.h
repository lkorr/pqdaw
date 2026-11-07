#pragma once

// T4.15 / TMM.12: the scene of a device's patch as it plays (what its structural settings switch off hidden, the visible
// rest laid out on its own unless the patch's stored layout places it), as the Device-level editor shows it. (GU.9: kept
// from the retired chain circuit.) UI thread.

#include <memory>
#include <string>
#include <vector>

#include "pq/document/model.h"
#include "pq/views/circuit/circuit_scene.h"

namespace pq::views::circuit {

std::shared_ptr<const CircuitScene> buildDeviceScene(const graph::Patch& callee, const std::vector<doc::DeviceValue>& settings);
// The key a device's scene is cached under: what it plays (`what`) and its settings.
std::string deviceSceneKey(const std::string& what, const std::vector<doc::DeviceValue>& settings);

}  // namespace pq::views::circuit
