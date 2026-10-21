#pragma once

// Helpers for the patch-language tests: reading .pqp files, an ImportLoader over a directory, and listing the
// round-trip corpus (tests/graph/cases/ and stdlib/).

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pq/graph/parse.h"

namespace text_util {

namespace fs = std::filesystem;

inline std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline fs::path casesDir() { return fs::path(PQ_TEST_DATA_DIR) / "graph" / "cases"; }
inline fs::path stdlibDir() { return fs::path(PQ_TEST_DATA_DIR) / ".." / "stdlib"; }

// Every .pqp file under `dir` (recursively), sorted; empty if the directory does not exist.
inline std::vector<fs::path> pqpFiles(const fs::path& dir) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return files;
  for (const auto& e : fs::recursive_directory_iterator(dir))
    if (e.is_regular_file() && e.path().extension() == ".pqp") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  return files;
}

// Loads imports relative to the importing file's directory (or `root` for file names without a directory),
// recursively, with a cycle guard and a cache.
class DirLoader : public pq::graph::ImportLoader {
public:
  explicit DirLoader(fs::path root) : root_(std::move(root)) {}

  pq::Result<std::vector<pq::graph::SubcircuitRef>> load(std::string_view path, std::string_view importer) override {
    fs::path base = fs::path(importer).parent_path();
    if (base.empty()) base = root_;
    const fs::path full = fs::weakly_canonical(base / fs::path(path));
    const std::string key = full.string();
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    if (active_.count(key)) return pq::Error{pq::ErrorCode::InvalidArgument, "import cycle", {}};
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) return pq::Error{pq::ErrorCode::NotFound, "file not found", {}};
    active_.insert(key);
    const std::string text = readFile(full);
    pq::graph::ParseOptions opts;
    opts.imports = this;
    auto r = pq::graph::parse(text, key, opts);
    active_.erase(key);
    if (!r) return pq::Error{pq::ErrorCode::Parse, "the imported file has errors:\n" + r.error().message, {}};
    auto sigs = pq::graph::signatures(r.value());
    cache_[key] = sigs;
    return sigs;
  }

private:
  fs::path root_;
  std::map<std::string, std::vector<pq::graph::SubcircuitRef>> cache_;
  std::set<std::string> active_;
};

}  // namespace text_util
