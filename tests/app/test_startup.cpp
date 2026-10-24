// T0.7-fix6: the startup phase log and the on-disk pipeline cache file (round trip; a truncated, corrupt, foreign or
// missing file reads as empty, so it never reaches the driver).

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pq/app/pipeline_cache.h"
#include "pq/app/startup.h"

using namespace pq::app;

namespace {

std::string tempFile(const char* name) {
  return (std::filesystem::temp_directory_path() / name).string();
}

std::vector<char> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void writeAll(const std::string& p, const std::vector<char>& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(b.data(), static_cast<std::streamsize>(b.size()));
}

}  // namespace

TEST_CASE("startup: pipeline cache file round trip; corrupt, truncated or missing files read as empty") {
  const std::string path = tempFile("pq_test_pipeline_cache.bin");
  std::filesystem::remove(path);
  CHECK(loadPipelineCache(path).empty());  // missing
  std::vector<uint8_t> blob(5000);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i * 31 + 7);
  REQUIRE(savePipelineCache(path, blob));
  CHECK(loadPipelineCache(path) == blob);
  CHECK_FALSE(std::filesystem::exists(path + ".tmp"));
  const std::vector<char> good = readAll(path);
  // One flipped payload byte (checksum), a truncated file, a trailing byte, a wrong magic, an empty file.
  std::vector<char> b = good;
  b[100] ^= 1;
  writeAll(path, b);
  CHECK(loadPipelineCache(path).empty());
  writeAll(path, std::vector<char>(good.begin(), good.end() - 10));
  CHECK(loadPipelineCache(path).empty());
  b = good;
  b.push_back(0);
  writeAll(path, b);
  CHECK(loadPipelineCache(path).empty());
  b = good;
  b[0] = 'X';
  writeAll(path, b);
  CHECK(loadPipelineCache(path).empty());
  writeAll(path, {});
  CHECK(loadPipelineCache(path).empty());
  // Saving again replaces whatever is there.
  REQUIRE(savePipelineCache(path, blob));
  CHECK(loadPipelineCache(path) == blob);
  CHECK_FALSE(savePipelineCache(path, {}));
  std::filesystem::remove(path);
  CHECK_FALSE(pipelineCachePath().empty());
}

TEST_CASE("startup: phases are reported in time order with their deltas") {
  markStartupPhase("test phase A");
  markStartupPhase("test phase B");
  addStartupNote("a note");
  const std::string r = startupPhaseReport();
  const size_t a = r.find("test phase A"), b = r.find("test phase B");
  REQUIRE(a != std::string::npos);
  REQUIRE(b != std::string::npos);
  CHECK(a < b);
  CHECK(r.find("a note") != std::string::npos);
}
