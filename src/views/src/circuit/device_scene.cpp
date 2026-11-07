// The scene of a device patch as it plays: pq/views/circuit/device_scene.h.
#include "pq/views/circuit/device_scene.h"

#include <cstdio>

#include "pq/graph/patch_file.h"
#include "pq/graph/structure.h"

namespace pq::views::circuit {

std::shared_ptr<const CircuitScene> buildDeviceScene(const graph::Patch& callee, const std::vector<doc::DeviceValue>& settings) {
  std::vector<graph::ConstArg> args;
  args.reserve(settings.size());
  for (const doc::DeviceValue& v : settings) args.push_back({v.name, v.value});
  const graph::StructuralInfo info = graph::analyseStructure(callee, args);
  SceneOptions so;
  so.inactiveNodes = info.nodeInactive;
  so.inactiveWires = info.wireInactive;
  so.hideInactive = true;
  bool placed = false;
  for (graph::NodeIndex v = 0; v < callee.nodeCount() && !placed; ++v)
    placed = !info.nodeInactive[v] && callee.layout(v).placed;
  so.relayoutVisible = info.inactiveNodes > 0 && !placed;
  so.structure = &info;
  return buildScene(callee, so);
}

std::string deviceSceneKey(const std::string& what, const std::vector<doc::DeviceValue>& settings) {
  std::string k = what;
  for (const doc::DeviceValue& v : settings) {
    char b[48];
    std::snprintf(b, sizeof b, "=%.17g", v.value);
    k += '|';
    k += v.name;
    k += b;
  }
  return k;
}


}  // namespace pq::views::circuit
