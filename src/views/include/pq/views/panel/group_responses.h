#pragma once

// EQ.4 (docs/plans/EQ.md "Displays (EQ.4)"): the analytic responses (doc::GroupResponse, pq/document/group_response.h)
// the `response` and `phase` displays draw, shared by every panel of a ViewContext.
//
//   lowering   EQ.4b: the APP'S OWN COMPILE (ViewContext::currentLowering: the playback link hands each compile's
//              doc::LoweredProject over, shared, no copy): nothing is lowered on the UI thread. After a structural
//              edit (ViewContext::structuralRevision: a band added, a Type / Stages / Mode literal, an undo of one) the
//              groups prepared before keep their responses (the displays keep drawing, their controls re-read from
//              the document) until the compile of the new structure arrives (view_change::kResponses); a group new in
//              the edit has none until then. Without an app (tests, scenes: !ViewContext::loweringProvided) one
//              doc::lower() of the document per structural revision, on the UI thread, the first time a display asks.
//              A knob turn, a drag, a typed value (param changes) never lowers or prepares again.
//   prepare    want(groups): the groups a display draws that are not prepared yet at this revision, prepared together
//              (GroupResponse::prepareMany: one inference per track patch, not one per group).
//   update     get(group): the response with its controls read from the current snapshot (GroupResponse::update, once
//              per snapshot per group); its stamp() tells a display whether its curve changed.
//
// UI thread only. Not RT code.

#include <complex>
#include <deque>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pq/document/group_response.h"
#include "pq/document/lower.h"
#include "pq/document/store.h"

namespace pq::views {

class ViewContext;

class GroupResponses {
public:
  explicit GroupResponses(ViewContext& ctx);
  ~GroupResponses();
  GroupResponses(const GroupResponses&) = delete;
  GroupResponses& operator=(const GroupResponses&) = delete;

  // The groups a display is about to get(): those not prepared at this structural revision are prepared together.
  void want(std::span<const doc::DeviceId> groups);
  // `group`'s response at the current document's control values; null: none (no document or library, the group is not
  // lowered — switched off, folded away —, nothing reaches its In; why() says).
  doc::GroupResponse* get(doc::DeviceId group);
  std::string why(doc::DeviceId group) const;
  // H of `group` at frequencies `hz` (In channel i → Out channel j: graph::LinearResponse::evaluate's layout), cached per
  // group until its response changes (a structural revision, a control it reads) or `key` changes (the caller's name
  // for `hz`: equal keys, equal frequencies) — the response and phase displays of one panel share one evaluation.
  // `version` is unique per evaluation (a display keeps what it derived from it until it changes). Null: no response.
  struct Curve {
    graph::ResponseKind kind = graph::ResponseKind::Unavailable;
    uint16_t in = 1, out = 1;
    std::vector<std::complex<double>> h;
    uint64_t version = 0;
    // H at point k from In channel i to Out channel j.
    std::complex<double> at(size_t k, uint16_t i = 0, uint16_t j = 0) const noexcept { return h[(k * in + i) * out + j]; }
  };
  const Curve* curve(doc::DeviceId group, std::span<const double> hz, uint64_t key);
  // The response of a SERIES of groups — each one's Out wired into the next one's In and nothing else (a chain of bands:
  // the caller checks the wires) — as the matrix product of their own curves at each point; a member marked in
  // `passes` (switched off: it passes its input through) without a response is the identity. Exact for any linear
  // members, and a drag re-evaluates only the member it moves (a 32-band chain evaluated whole costs ≈ 30× more).
  // Cached under `name` until a member's evaluation changes. Null: a member has no response (and does not pass), or
  // the members' channel counts do not chain.
  const Curve* series(doc::DeviceId name, std::span<const doc::DeviceId> members, std::span<const uint8_t> passes,
                      std::span<const double> hz, uint64_t key);
  // The current document (null: none) and the rate the displays evaluate at.
  const doc::Snapshot& doc() const;
  double sampleRate() const noexcept;
  // Tests: lowerings made here so far (one per structural revision a display asked at; none with the app's).
  uint32_t lowerings() const noexcept { return lowerings_; }
  // A structural edit's compile has not arrived yet (the entries are the previous structure's).
  bool waiting() const noexcept { return waiting_; }

private:
  struct Entry {
    doc::DeviceId group;
    std::unique_ptr<doc::GroupResponse> response;
    std::string error;            // empty: prepared
    doc::Snapshot updated;        // the snapshot its controls were read from
    Curve curve;
    uint64_t curveStamp = 0, curveKey = 0;
    size_t curvePoints = 0;
    double curveRate = 0.0;  // (the rate it was evaluated at)
  };
  struct Series {
    doc::DeviceId name;
    std::vector<uint64_t> versions;  // the members' evaluations it was made from (0: passed through)
    uint64_t key = 0;
    Curve curve;
  };
  void sync();  // a new structural revision: drop what was prepared
  Entry* find(doc::DeviceId g) noexcept;

  ViewContext& ctx_;
  uint64_t revision_ = 0;
  std::shared_ptr<const doc::LoweredProject> lowered_;  // the app's (shared), or made here
  std::string lowerError_;
  bool lowerTried_ = false;
  bool waiting_ = false;  // EQ.4b: a structural revision the app's compile has not lowered yet
  std::deque<Entry> entries_;  // (references stay valid while entries are added)
  std::deque<Series> series_;
  std::vector<uint64_t> scratchVersions_;
  std::vector<doc::DeviceId> scratchMissing_;
  std::vector<const Curve*> scratchCurves_;
  std::vector<std::complex<double>> scratchH_, scratchNext_;
  uint32_t lowerings_ = 0;
  uint64_t versions_ = 0;
};

}  // namespace pq::views
