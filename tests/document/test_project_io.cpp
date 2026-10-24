// Project directories (T3.5): atomic save (a failed write leaves the previous file intact and no temp file), non-ASCII
// project paths (UTF-8 → UTF-16), DocumentStore open/save.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "pq/document/serialize.h"
#include "project_gen.h"

using namespace pq::doc;
using pq::Result;
namespace fs = std::filesystem;

namespace {

std::string toUtf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
fs::path fromUtf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string readFile(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A fresh directory under the temp dir whose name has non-ASCII characters (é, 日本, a 4-byte emoji).
struct TempDir {
  std::string utf8;
  TempDir() {
    static int counter = 0;
    const fs::path base = fs::temp_directory_path();
    utf8 = toUtf8(base) + "\\pqdaw-io-\xC3\xA9\xE6\x97\xA5\xE6\x9C\xAC\xF0\x9F\x8E\xB5-" +
           std::to_string(std::random_device{}()) + "-" + std::to_string(counter++);
    std::error_code ec;
    fs::remove_all(fromUtf8(utf8), ec);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(fromUtf8(utf8), ec);
  }
  fs::path file(const char* name) const { return fromUtf8(utf8) / name; }
};

}  // namespace

TEST_CASE("project io: save and load a project in a non-ASCII directory (created on save)") {
  doctest_fixture::RichProject f;
  TempDir dir;
  REQUIRE_FALSE(fs::exists(fromUtf8(dir.utf8)));
  const pq::Status st = saveProject(dir.utf8, f.store);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  CHECK(fs::exists(dir.file("project.pqd")));
  CHECK_FALSE(fs::exists(dir.file("project.pqd.tmp")));
  Result<LoadedProject> r = loadProject(dir.utf8);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(*r->project == f.doc());

  // Open into a store: same document, history cleared, ids continue after the loaded maximum, view installed.
  DocumentStore store;
  REQUIRE(openProject(dir.utf8, store).ok());
  CHECK(*store.current() == f.doc());
  CHECK(store.history().entries().empty());
  CHECK(store.ids().peek() > maxEntityId(f.doc()));
  CHECK(store.view().tracks.at(f.drums).folded);
  // Saving again from the reopened store gives the same bytes.
  const std::string first = readFile(dir.file("project.pqd"));
  REQUIRE(saveProject(dir.utf8, store).ok());
  CHECK(readFile(dir.file("project.pqd")) == first);
}

TEST_CASE("project io: a failed save leaves the previous file intact and removes the temp file") {
  doctest_fixture::RichProject f;
  TempDir dir;
  DocumentStore plain;
  REQUIRE(saveProject(dir.utf8, plain).ok());
  const std::string before = readFile(dir.file("project.pqd"));
  REQUIRE_FALSE(before.empty());
  for (SaveFailPoint at : {SaveFailPoint::WhileWriting, SaveFailPoint::BeforeRename}) {
    CAPTURE(static_cast<int>(at));
    const pq::Status st = saveProject(dir.utf8, f.doc(), f.store.view(), SaveOptions{at});
    REQUIRE_FALSE(st.ok());
    CHECK(st.error().code == pq::ErrorCode::Io);
    CHECK(readFile(dir.file("project.pqd")) == before);
    CHECK_FALSE(fs::exists(dir.file("project.pqd.tmp")));
    Result<LoadedProject> r = loadProject(dir.utf8);
    REQUIRE(r.ok());
    CHECK(*r->project == *plain.current());
  }
  // The next normal save replaces it.
  REQUIRE(saveProject(dir.utf8, f.doc(), f.store.view()).ok());
  Result<LoadedProject> r = loadProject(dir.utf8);
  REQUIRE(r.ok());
  CHECK(*r->project == f.doc());
}

TEST_CASE("project io: errors name the file") {
  TempDir dir;
  Result<LoadedProject> missing = loadProject(dir.utf8);
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().code == pq::ErrorCode::NotFound);

  fs::create_directories(fromUtf8(dir.utf8));
  std::ofstream(dir.file("project.pqd"), std::ios::binary) << "{\n  \"schemaVersion\": 2,\n  \"tracks\": x\n}";
  Result<LoadedProject> bad = loadProject(dir.utf8);
  REQUIRE_FALSE(bad.ok());
  CHECK(bad.error().location.file == dir.utf8 + "\\project.pqd");
  CHECK(bad.error().location.line == 3);
  CHECK(bad.error().location.col == 13);

  DocumentStore store;
  const pq::Status badPath = saveProject("bad\xFFpath", store);
  REQUIRE_FALSE(badPath.ok());
  CHECK(badPath.error().message == "path is not valid UTF-8");
}
