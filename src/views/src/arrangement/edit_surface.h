#pragma once

// TX.lane-edit: where the arrangement's editing code (Editor: clips and time selection; ArrangementAutomation:
// breakpoints) draws and hit-tests. The arrangement's lanes are one EditSurface; the project patch view's band lanes
// (TL.1) are another (pq/views/project_patch: LaneEditSurface). Both edit the same document through the same Editor and
// ArrangementAutomation instances (one selection, one clipboard, one undo history): a gesture binds its surface for its
// duration (drag state remembers it), damage of a selection or a hover goes to every active surface.
//
// A row is a clip track (a MIDI or audio track; the arrangement's rows also include groups). Each row has its own linear
// time axis (TimeAxis): the arrangement's rows share one; the band lanes share theirs (TL.1: one x range over the same
// time window), kept per row so a surface may differ. UI thread only.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "pq/core/time.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/arrangement/automation.h"

namespace pq::views::arrangement {

// x = x0 + (tick − t0) · k (device px).
struct TimeAxis {
  double x0 = 0.0;
  double t0 = 0.0;
  double k = 1.0;  // device px per tick
  float x(double tick) const noexcept { return static_cast<float>(x0 + (tick - t0) * k); }
  double tick(float x) const noexcept { return t0 + (static_cast<double>(x) - x0) / k; }
};

class Surfaces;

class EditSurface {
public:
  EditSurface() = default;
  EditSurface(const EditSurface&) = delete;
  EditSurface& operator=(const EditSurface&) = delete;
  virtual ~EditSurface();

  // The widget gestures happen in (focus, context menus, the tree; a clip drag leaving its rect becomes a drag session).
  virtual ui::Widget& widget() const = 0;
  // Shown: damage goes only to active surfaces.
  virtual bool active() const = 0;
  // The clip rows (in top-to-bottom order) and their geometry, device px in window coordinates.
  virtual std::span<const ArrangementRow> rows() const = 0;
  virtual int32_t rowAt(float deviceY) const = 0;     // −1: none
  virtual int32_t clampRow(float deviceY) const = 0;  // the nearest row (−1: no rows)
  virtual ui::IRect rowLaneRect(int32_t row) const = 0;
  virtual ui::IRect viewport() const = 0;  // damage is clipped to it
  virtual TimeAxis axis(int32_t row) const = 0;
  // The grid's density (logical px per tick: the arrangement's zoom, a window's own).
  virtual double gridPxPerTick() const = 0;
  virtual int32_t clipTitleHeight(int32_t row) const = 0;  // device px of a clip's title strip on a row
  // Preview / envelope tiles (the arrangement's TileAtlas tiles are keyed by its zoom; a window draws directly).
  virtual bool tiles() const { return false; }
  virtual bool zooming() const { return false; }
  // The arrangement's extras (empty elsewhere).
  virtual void revealTick(Ticks) {}
  virtual void revealRow(int32_t) {}
  virtual std::span<const AutomationLaneRow> laneRows() const { return {}; }
  virtual ui::IRect laneRect(size_t) const { return {}; }
  virtual std::span<const ArrangementRow> bottomRows() const { return {}; }
  virtual ui::IRect pinnedRowRect(size_t) const { return {}; }
  virtual ui::IRect bottomRect() const { return {}; }
  // Repaints everything the surface shows of the editing state (automation mode toggled, a lane chosen).
  virtual void damageAll() = 0;
  // Entries appended to the editor's right-click menu on this surface.
  virtual std::vector<ui::MenuItem> menuItems() { return {}; }

  int32_t rowOf(doc::TrackId id) const noexcept {
    const auto rs = rows();
    for (size_t i = 0; i < rs.size(); ++i)
      if (rs[i].id == id) return static_cast<int32_t>(i);
    return -1;
  }
  Surfaces* set() const noexcept { return set_; }

private:
  friend class Surfaces;
  Surfaces* set_ = nullptr;
};

// The surfaces of one arrangement (owned by ArrangementView): its own first, then the registered ones; and the one a
// call binds (Bind) — the arrangement's when none is bound.
class Surfaces {
public:
  explicit Surfaces(EditSurface& own) : own_(own) {
    own.set_ = this;
    all_.push_back(&own);
  }
  ~Surfaces() {
    for (EditSurface* s : all_) s->set_ = nullptr;
  }
  Surfaces(const Surfaces&) = delete;
  Surfaces& operator=(const Surfaces&) = delete;

  EditSurface& own() const noexcept { return own_; }
  EditSurface& current() const noexcept { return bound_ ? *bound_ : own_; }
  std::span<EditSurface* const> all() const noexcept { return all_; }
  // Registered surfaces unregister themselves (EditSurface::~EditSurface); the view cancels their drags first.
  void add(EditSurface& s) {
    if (s.set_ == this) return;
    s.set_ = this;
    all_.push_back(&s);
  }
  void remove(EditSurface& s) noexcept {
    std::erase(all_, &s);
    if (bound_ == &s) bound_ = nullptr;
    s.set_ = nullptr;
  }
  // Every surface but the arrangement's own (automation mode, a chosen parameter: the lane windows repaint).
  void damageOthers() {
    for (EditSurface* s : all_)
      if (s != &own_ && s->active()) s->damageAll();
  }

  // Binds `s` (null: the arrangement's) for the scope's lifetime.
  class Bind {
  public:
    Bind(Surfaces& set, EditSurface* s) noexcept : set_(set), prev_(set.bound_) { set.bound_ = s; }
    ~Bind() { set_.bound_ = prev_; }
    Bind(const Bind&) = delete;
    Bind& operator=(const Bind&) = delete;

  private:
    Surfaces& set_;
    EditSurface* prev_;
  };

private:
  EditSurface& own_;
  std::vector<EditSurface*> all_;
  EditSurface* bound_ = nullptr;
};

inline EditSurface::~EditSurface() {
  if (set_ != nullptr) set_->remove(*this);
}

}  // namespace pq::views::arrangement
