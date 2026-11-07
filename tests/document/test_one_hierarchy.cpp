// GU.11 (docs/plans/GU.md "One hierarchy", accept): the model has exactly blocks, groups and native engines. Every
// device of every project fixture under tests/ (each must load) and of every built-in module's group preset (and of
// any .pqpreset file under tests/) is one of the four kinds of block (model.h Device): a group, an atom block of a
// known atom, a value block, or a native engine of a registered meta type.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/library_files.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/document/serialize.h"
#include "pq/meta/registry.h"

using namespace pq::doc;
namespace fs = std::filesystem;

namespace {

// Project fixtures that are invalid on purpose (an error test loads them expecting a refusal). None today:
// test_serialize_errors.cpp builds its broken files from fixtures/minimal in memory.
const std::vector<std::string> kDeliberatelyInvalid = {};

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

std::string u8(const fs::path& p) {
  const std::u8string s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}
fs::path testsDir() {
  const std::string s = PQ_TEST_DATA_DIR;
  return fs::path(std::u8string(s.begin(), s.end()));
}

// Why `d` is not a block, or "" when it is one.
std::string notABlock(const Device& d) {
  if (isGroupDevice(d) || isValueDevice(d)) return {};
  if (isAtomDevice(d)) return atomBlockPatch(atomDeviceName(d)) != nullptr ? std::string() : "an unknown atom";
  if (isMetaDevice(d)) return pq::meta::findType(metaDeviceType(d)) != nullptr ? std::string() : "an unknown native engine";
  return "not a block";
}

void checkDevices(const std::string& where, const std::vector<DevicePtr>& devices, std::vector<std::string>& problems) {
  for (const DevicePtr& d : devices) {
    REQUIRE(d != nullptr);
    if (const std::string why = notABlock(*d); !why.empty())
      problems.push_back(where + ": device " + std::to_string(d->id.value) + " '" + d->patch + "' is " + why);
  }
}

}  // namespace

TEST_CASE("one hierarchy: every project fixture under tests/ loads and holds only blocks, groups and native engines") {
  const fs::path root = testsDir();
  REQUIRE(fs::is_directory(root));
  std::vector<std::string> problems;
  size_t projects = 0, devices = 0;
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file() || e.path().filename() != "project.pqd") continue;
    const fs::path dir = e.path().parent_path();
    const std::string rel = u8(fs::relative(dir, root));
    if (std::find(kDeliberatelyInvalid.begin(), kDeliberatelyInvalid.end(), rel) != kDeliberatelyInvalid.end()) continue;
    ++projects;
    LoadOptions lo;
    lo.devices = &library();
    const pq::Result<LoadedProject> r = loadProject(u8(dir), lo);
    if (!r.ok()) {
      problems.push_back(rel + ": does not load: " + r.error().message);
      continue;
    }
    forEachTrack(*r->project, [&](const Track& t, const Track*) {
      devices += t.devices.size();
      checkDevices(rel + " track '" + t.name + "'", t.devices, problems);
    });
  }
  MESSAGE(projects << " project fixtures, " << devices << " devices");
  CHECK(projects >= 4);
  std::string all;
  for (const std::string& p : problems) all += "\n  " + p;
  CHECK_MESSAGE(problems.empty(), problems.size() << " problem(s):" << all);
}

TEST_CASE("one hierarchy: every built-in module's group preset and every preset file under tests/ holds only blocks") {
  std::vector<std::string> problems;
  for (const ModuleInfo& m : moduleInfos()) {
    const BlockSet set = moduleBlocks(m.kind);
    CAPTURE(m.key);
    REQUIRE_FALSE(set.devices.empty());
    CHECK(isGroupDevice(*set.devices.front()));  // the group first
    checkDevices("module '" + std::string(m.key) + "'", set.devices, problems);
  }
  const fs::path root = testsDir();
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file() || e.path().extension() != ".pqpreset") continue;
    const std::string rel = u8(fs::relative(e.path(), root));
    std::ifstream f(e.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const pq::Result<PresetFile> p = parsePreset(text, u8(e.path().filename()));
    if (!p.ok()) {
      problems.push_back(rel + ": does not load: " + p.error().message);
      continue;
    }
    if (p->groupPreset()) checkDevices(rel, p->blocks.devices, problems);
    else if (p->device) checkDevices(rel, std::vector<DevicePtr>{p->device}, problems);
  }
  std::string all;
  for (const std::string& p : problems) all += "\n  " + p;
  CHECK_MESSAGE(problems.empty(), problems.size() << " problem(s):" << all);
}
