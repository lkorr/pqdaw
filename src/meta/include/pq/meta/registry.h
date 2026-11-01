#pragma once

// The meta-module type registry and the PRELUDE (TMM.3; docs/plans/MM.md "Architecture").
//
// Calls: a meta module in the patch language is an ordinary call `name = meta_<type>(…)` (PATCH_LANGUAGE §3.2). The
// prefix `meta_` is reserved for it; the compiler's loaders supply every registered type's default expansion as a
// prelude patch named `meta_<type>` (its signature as a parse external, the patch in the callee library), so a
// standalone .pqp (pqc) checks and renders with the default state, and the document lowering replaces each call by
// the instance's own expansion `meta_<type>__d<device>_<path>_<key>` (pq/document/lower.h "Meta modules").
//
// Registration happens at startup (the built-in types) and in tests (a test-only type) before any loader runs;
// lookups are thread-safe. Not RT code.

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/graph/patch.h"
#include "pq/meta/type.h"

namespace pq::meta {

inline constexpr std::string_view kCallPrefix = "meta_";

// Every registered type (the built-in ones first, in registration order). The pointers live for the process.
std::vector<const Type*> types();
const Type* findType(std::string_view id) noexcept;
// Adds a type (its id must be new and valid; else false). The prelude is rebuilt on its next use.
bool registerType(const Type& type);

// "meta_<id>".
std::string callName(const Type& t);
// The type a callee name calls: "meta_<id>" (the prelude) or "meta_<id>__…" (an instance's expansion); null for any
// other name.
const Type* typeOfCall(std::string_view callee) noexcept;
// The name of an instance's expansion: "meta_<id>__d<device>_<path, '.' → '_'>_<16 hex digits of key>"; `key` covers
// the path too (the name is unique per instance and state).
std::string instanceName(const Type& t, uint32_t device, std::string_view path, uint64_t key);
// A name path of a nested instance: call binding names joined by '.' ("acc1", "opA.acc"); "" is the chain device
// itself. Valid: "" or identifiers separated by single dots.
bool validPath(std::string_view path) noexcept;

// The default expansion of every registered type, per form: what a loader supplies for `meta_<id>` calls (Nested,
// voice context, default settings, every param static, slot ids 0) and the chain-device forms a param lookup reads
// (their params are the type's: the same names, decls and order).
struct Prelude {
  struct Entry {
    const Type* type = nullptr;
    graph::Patch nested;                    // named meta_<id>
    // TMM.10-fix1: every type has every chain form (its type's chain input; the same params in each).
    std::optional<graph::Patch> notes;  // Form::ChainNotes
    std::optional<graph::Patch> audio;  // Form::ChainAudio
    std::optional<graph::Patch> voice;  // TMM.10: Form::ChainVoice (a type with a chain input)
    // The form a chain device of the type is described by (params, signature for panels) when what reaches it is not
    // known: its audio form; null when the type's expansion failed.
    const graph::Patch* chain() const noexcept { return audio ? &*audio : notes ? &*notes : nullptr; }
  };
  std::vector<Entry> entries;                  // per type, in types() order; never resized after the build
  std::vector<graph::SubcircuitRef> signatures; // of each `nested` (parse externals)
  const Entry* find(const Type* t) const noexcept;
  const Entry* find(std::string_view id) const noexcept;
};
// The current prelude (built on first use, rebuilt after a registration). Thread-safe; the snapshot stays valid while
// held.
std::shared_ptr<const Prelude> prelude();

}  // namespace pq::meta
