# TL: Track lanes in the project patch view (plan)

User decision 2026-11-08 (SPEC §15 "track lanes in the circuit view"). The track's source stops being a node in its band
(the "Notes" / "Audio" lane window, `NodeKind::Source`) and becomes a **timeline lane across the bottom of the band**,
the band's full width, working like the arrangement's track lane. The circuit view ends up looking like the arrangement
with each track's circuit drawn above its lane. The Device panel at the bottom stays (for now).

## What exists (read these, not the whole docs)

- Band view: `src/views/src/project_patch/` (`model.cpp` bands/layout/source node ≈ :153, :732–752, :960–1217;
  `canvas.cpp` underlay :220, drag-grow :1729–2020; `lane_window.cpp`, `lane_edit.cpp`, `view.cpp` lane time
  :863–897), `include/pq/views/project_patch/project_patch.h` (`Band` :223, lane-window sizes :275–281, `laneBody()`),
  `project_patch_view.h` (design comment :3–123, `laneStart_/laneSpan_/laneUnit_` :868).
- Source cables: `doc::Wire` with `noteSource = trackId`, `from` invalid, `port` = `kMainPort` or a `kNoteOutputs`
  entry (`model.h` :735–781, :809); lowering `lower.cpp` :462, :491, :1750, :1811, :1899, `lower.h` :283–290.
- Arrangement lane editing is already shared: `arrangement/edit_surface.h` (`EditSurface`, `Surfaces::Bind`),
  `lane_paint.h` (`LanePaint`, `paintClip`, `paintAudioClip`, `paintGrid`), `LaneEditSurface`
  (`project_patch/internal.h` :66–105). One editor, one selection, one clipboard, one undo history: keep it that way.
- Arrangement time window: `ArrangementView::originTick_ / pxPerTick_` (`arrangement.h` :286–295, :533–534).
- Ruler click → `ArrangementView::locateAt` (`arrangement_view.cpp:932`); empty-lane click → insert marker / time
  selection (`editing.cpp:659–682`).
- Control → automation: `ViewContext::showAutomation(track, ParamRef)` (`view_context.h:371–377`); arrangement handles
  it at `arrangement_view.cpp:373` → `automation_->showParam`. No caller in `project_patch/` or `circuit/` yet.
- ARCHITECTURE bullets: "Project patch view" :633, "Lane-window editing" :639 (update both).

## The design

1. **The lane.** Every band ends in a lane strip: x from the band's left edge to its right edge, y at the band's bottom,
   inside the band (same colour and header; clearly the same track). Its height is resizable by dragging its bottom
   edge, like an arrangement track, separately from the circuit area above it. A MIDI track's lane shows its MIDI clips,
   an audio track's its audio clips (waveforms). Group / return / master tracks (today a Σ node) get a lane with no clip
   row, only automation (TL.2). The Source node and its lane window are deleted (no compatibility: old layouts that
   placed it just lose it).
2. **One time window, shared with the arrangement.** The visible time range (start tick + visible span) moves out of
   `ArrangementView` into shared view state (`ViewContext` or a small object both views hold). The arrangement and every
   band lane show the same range; each maps it onto its own pixel width ("takes up the same space, shows more or less
   time"). The lane-window time state (`laneStart_` …) is deleted. Zooming / scrolling in either view moves both. All
   bands' lanes span the same x range (the common band width) so one ruler lines up with all of them.
3. **Ruler.** One time ruler pinned to the top of the project patch view over the lanes' x range: the arrangement ruler
   (`ruler.cpp`), same clicks (locate, loop brace, markers).
4. **Behaves as the arrangement lane, by construction.** Lane editing goes through the existing `EditSurface` path
   (clips: draw, move, resize, select, copy / paste, double-click opens the piano roll / audio clip view; time
   selection; insert marker; the arrangement's hotkeys: zoom, Ctrl+wheel, Z / X, etc.). Painting through `LanePaint`.
   No second implementation.
5. **Clicking moves where play starts.** The user reports clicking the Notes window doesn't set the playhead. Fix it for
   both views together (they share the editor): a click on empty lane space sets the insert marker AND the start marker
   (what Play / Space starts from); when stopped the playhead moves there; while playing it behaves as the arrangement
   does now. Verify through real dispatch that Space after a lane click plays from the clicked tick in both views.
6. **Cables leave from the lane's top edge, pinned anywhere.** Each cable from a track's lane leaves at its own pinned x
   on the lane's top edge (band-local world x, saved with the wire; default: under the block it feeds). Dragging the
   cable's lane end slides it along the edge (one undo step). Several cables can leave from different spots (parallel
   instruments fed from points spread along the graph). Starting a new cable: drag up from anywhere on the lane's top
   edge. A MIDI lane offers the main notes output and the per-note outputs (hz / gate / velocity) the Notes node offers
   today (same menu / port affordance). The signal is unchanged: same `noteSource` wires, same lowering. Other tracks
   cross-feed by wiring from their own lanes, as now (one lane per track).
7. **Collapsing the circuit** leaves the lane (and its automation sub-lanes): a fully collapsed project patch view reads
   as the arrangement.

## Automation (TL.2, TL.3)

8. **Sub-lanes.** A lane's automation sub-lanes open below it (expand toggle in the band header, as the arrangement's
   track header), one per shown parameter of any block in that track's circuit, plus mixer params. The same automation
   data and the same `ArrangementAutomation` editing as the arrangement (shared `EditSurface::laneRows()/laneRect()`,
   which are empty outside the arrangement today). Master and returns get the same (lane with no clip row).
9. **Click a knob → show its automation.** Clicking a control in a band (face, panel control, atom param) calls
   `ViewContext::showAutomation(track, param)`: the band's lane opens that parameter's sub-lane (and the arrangement
   does what it does now).
10. **The automation wire.** While a control's automation sub-lane is shown and that control is selected / last
    clicked, a thin red wire is drawn from the control to the nearest point on its sub-lane (the point of the sub-lane
    rect nearest the control, clamped to the lane's visible x range), as a hanging cable in the cable-look's style but
    thin, red and with a soft glow; it follows the control while dragging blocks and while the sub-lane scrolls / resizes.
    Draw-only (no document state); one per selected control.

## Tasks

- **TL.1** the lane strip (1–7). Large; split TL.1a (lane strip + shared time window + ruler + click/start marker +
  resize + collapse, Source node deleted with its cables temporarily leaving from a default spot) and TL.1b (pinned
  cable taps, dragging along the edge, note outputs) if a worker finds it too big.
- **TL.2** automation sub-lanes in band lanes, master / returns included, click a control → show its sub-lane (8, 9).
- **TL.3** the red automation wire (10).

Verification: UI tasks (`pq_tests_views` default tier; gestures through real window / overlay dispatch; goldens of the
band view re-recorded and reviewed); TL.1 also touches the document (`Wire` tap x) and the shared time state:
`-Affected`. No audio / playback suites except the one Space-plays-from-click check. 👁 each: `docs/REVIEW.md` entry.

CPU: the lane paints only its visible tick range (`LanePaint` already culls); the shared time window is one struct read,
no per-frame allocation; the red wire is one curve per selected control.

## TL.4: the look pass, scrub ruler, Play-from-click, wire curve, mixer size (user 2026-11-09)

1. **Lane look.** The lane must read as a proper track, as good as the arrangement's: a border / inset frame round the
   lane (and its sub-lanes), the arrangement's alternating bar shading (every bar / the zoom-dependent grid the
   arrangement background uses, `paintGrid` / `LanePaint` with the arrangement's colours), clip styling identical to the
   arrangement. Take a screenshot of both views at the same zoom and make the lane indistinguishable from an arrangement
   track except for its width.
2. **Thin scrub ruler per lane.** Each lane gets a thin version of the top ruler along its top (just under the cable
   tap edge, which stays the topmost edge): bar numbers, the playhead, click / drag scrubs exactly as the top ruler does
   (same `Ruler` code, a compact mode). The top ruler stays.
3. **Play from the click (bug).** The user: clicking bar 4 in the MIDI lane then pressing Play does not play from bar 4;
   it "stays behind". TL.1 claimed this and its test passed, so the test doesn't model what the user does: reproduce in
   the real app (a process you launch), likely cases: clicking on / near a clip, clicking while playing, the
   arrangement's start marker vs insert marker, Space vs the Play button, the transport not locating when stopped at
   a different position. Rule: a click anywhere on a lane's time area (empty space or a clip, not a clip drag) moves
   where Play starts to the click (snapped as the arrangement snaps); stopped: the playhead moves there now; playing:
   the next Play / Stop-then-Play starts there. Same in the arrangement. Test the exact gesture through real dispatch.
4. **Wire curve.** The automation wire curves visibly: end on the sub-lane at a point offset along x from the control
   (e.g. towards the sub-lane's label / left, or a fixed lateral offset), with a gentle sag, never a straight drop.
5. **Mixer face.** Resizable (drag its corner like other resizable blocks, saved, one undo step) and 2× its current size
   by default.

