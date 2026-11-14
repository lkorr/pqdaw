// VZ.1: pq/meta/user_defaults.h.
#include "pq/meta/user_defaults.h"

#include <map>
#include <string>
#include <utility>

namespace pq::meta {

namespace {

struct Store {
  std::map<std::string, DataPtr, std::less<>> data;
  std::function<void(std::string_view, const DataPtr&)> sink;
};
Store& store() {
  static Store s;
  return s;
}

}  // namespace

DataPtr userDefaultData(std::string_view type) {
  const Store& s = store();
  const auto it = s.data.find(type);
  return it != s.data.end() ? it->second : nullptr;
}

void setUserDefaultData(std::string_view type, DataPtr data) {
  Store& s = store();
  if (data == nullptr) {
    if (const auto it = s.data.find(type); it != s.data.end()) s.data.erase(it);
    return;
  }
  s.data.insert_or_assign(std::string(type), std::move(data));
}

void saveUserDefaultData(std::string_view type, DataPtr data) {
  setUserDefaultData(type, data);
  if (store().sink) store().sink(type, data);
}

void setUserDefaultsSink(std::function<void(std::string_view, const DataPtr&)> sink) { store().sink = std::move(sink); }

}  // namespace pq::meta
