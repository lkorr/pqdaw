// The meta-module registry and prelude (TMM.3): pq/meta/registry.h.
#include "pq/meta/registry.h"

#include <mutex>

#include "pq/graph/patch_file.h"
#include "pq/meta/envelope.h"
#include "pq/meta/phase_acc.h"
#include "pq/meta/wavetable.h"

namespace pq::meta {

namespace {

bool identifier(std::string_view s) noexcept {
  if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
  for (const char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}

struct Registry {
  std::mutex mutex;
  std::vector<const Type*> types;
  std::shared_ptr<const Prelude> prelude;  // null: rebuild on next use
  Registry() {
    types.push_back(&phaseAccType());
    types.push_back(&wavetableType());  // TMM.5
    types.push_back(&envelopeType());   // TMM.11
  }
};

Registry& registry() {
  static Registry r;
  return r;
}

const Type* findLocked(const Registry& r, std::string_view id) noexcept {
  for (const Type* t : r.types)
    if (t->id == id) return t;
  return nullptr;
}

std::shared_ptr<const Prelude> buildPrelude(const std::vector<const Type*>& types) {
  auto p = std::make_shared<Prelude>();
  p->entries.reserve(types.size());
  for (const Type* t : types) {
    Prelude::Entry e;
    e.type = t;
    ExpandContext c;
    auto nested = expand(*t, c, callName(*t));
    if (!nested) continue;  // a broken type has no prelude entry (its calls are unknown); its tests report why
    e.nested = std::move(nested->patch);
    // TMM.10-fix1: every chain form (the names keep the pre-fix1 suffixes).
    c.form = Form::ChainNotes;
    if (auto x = expand(*t, c, callName(*t) + "__instrument")) e.notes = std::move(x->patch);
    c.form = Form::ChainAudio;
    c.voice = false;
    if (auto x = expand(*t, c, callName(*t) + "__effect")) e.audio = std::move(x->patch);
    if (t->chainInput >= 0) {  // TMM.10
      c.form = Form::ChainVoice;
      c.voice = true;
      if (auto x = expand(*t, c, callName(*t) + "__voice")) e.voice = std::move(x->patch);
    }
    p->signatures.push_back(graph::subcircuitSignature(e.nested));
    p->entries.push_back(std::move(e));
  }
  return p;
}

char hexDigit(uint64_t v) noexcept { return "0123456789abcdef"[v & 15]; }

}  // namespace

std::vector<const Type*> types() {
  Registry& r = registry();
  std::lock_guard lock(r.mutex);
  return r.types;
}

const Type* findType(std::string_view id) noexcept {
  Registry& r = registry();
  std::lock_guard lock(r.mutex);
  return findLocked(r, id);
}

bool registerType(const Type& type) {
  if (!identifier(type.id) || type.id.find("__") != std::string_view::npos || type.body == nullptr ||
      type.outputs.empty())
    return false;
  Registry& r = registry();
  std::lock_guard lock(r.mutex);
  if (findLocked(r, type.id) != nullptr) return false;
  r.types.push_back(&type);
  r.prelude.reset();
  return true;
}

std::string callName(const Type& t) { return std::string(kCallPrefix) + std::string(t.id); }

const Type* typeOfCall(std::string_view callee) noexcept {
  if (!callee.starts_with(kCallPrefix)) return nullptr;
  std::string_view id = callee.substr(kCallPrefix.size());
  if (const size_t cut = id.find("__"); cut != std::string_view::npos) id = id.substr(0, cut);
  return findType(id);
}

std::string instanceName(const Type& t, uint32_t device, std::string_view path, uint64_t key) {
  std::string s = callName(t) + "__d" + std::to_string(device) + "_";
  for (const char c : path) s += c == '.' ? '_' : c;
  // The key covers the path: "a_b" and "a.b" mangle alike but differ in the key.
  uint64_t h = key ^ 0x9E3779B97F4A7C15ull;
  for (const char c : path) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  s += '_';
  for (int i = 15; i >= 0; --i) s += hexDigit(h >> (4 * i));
  return s;
}

bool validPath(std::string_view path) noexcept {
  if (path.empty()) return true;
  size_t a = 0;
  while (true) {
    const size_t b = path.find('.', a);
    if (!identifier(path.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a))) return false;
    if (b == std::string_view::npos) return true;
    a = b + 1;
  }
}

const Prelude::Entry* Prelude::find(const Type* t) const noexcept {
  for (const Entry& e : entries)
    if (e.type == t) return &e;
  return nullptr;
}

const Prelude::Entry* Prelude::find(std::string_view id) const noexcept {
  for (const Entry& e : entries)
    if (e.type->id == id) return &e;
  return nullptr;
}

std::shared_ptr<const Prelude> prelude() {
  Registry& r = registry();
  std::lock_guard lock(r.mutex);
  if (!r.prelude) r.prelude = buildPrelude(r.types);
  return r.prelude;
}

}  // namespace pq::meta
