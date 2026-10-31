# pqdaw hotkeys

The default keymap is data: [`assets/keymaps/ableton.json`](../assets/keymaps/ableton.json) (format in `src/ui/include/pq/ui/keymap_file.h`). It copies Ableton Live 12's shortcuts (SPEC §6.1) onto pqdaw action ids. A binding is either `implemented` (its action must be registered when the keymap is applied, or loading fails) or `deferred:M<n>` (the chord is reserved for the milestone that implements it and does nothing until then). `tests/ui/test_controls.cpp` checks that every chord parses, that no chord is bound twice in one scope, that every implemented action resolves, and that every binding is listed below.

Scopes: `global` (always active, consulted last), `arrangement` (focus inside the arrangement view), `pianoroll` (focus inside the clip view's note editor), `devices` (focus inside the Device view, T4.1). A focused text field takes printable keys first (Q27), so `B` while renaming types a "b". Chords match the layout key (Q27). Windows key names; on macOS Ctrl would read Cmd.

The "Live 12 manual section" column names the subsection of the Live Reference Manual's *Live Keyboard Shortcuts* chapter that lists the shortcut. The subsection names follow that chapter's layout as of Live 11/12 and were written from memory: check them against the installed Live 12 manual (QUESTIONS.md, Q48).

| Action | Keys | Scope | Live 12 manual section | Status |
|---|---|---|---|---|
| `transport.playStop` | `Space` | global | Transport | implemented (T3.7) |
| `transport.continue` | `Shift+Space` | global | Transport | implemented (T3.7) |
| `transport.record` | `F9` | global | Transport | M6 (recording) |
| `edit.undo` | `Ctrl+Z` | global | Editing | implemented (T3.7) |
| `edit.redo` | `Ctrl+Shift+Z` | global | Editing | implemented (T3.7) |
| `edit.redo` | `Ctrl+Y` | global | Editing | implemented (T3.7) |
| `file.new` | `Ctrl+N` | global | Accessing Menus (File) | implemented (T3.7) |
| `file.open` | `Ctrl+O` | global | Accessing Menus (File) | implemented (T3.7) |
| `file.save` | `Ctrl+S` | global | Accessing Menus (File) | implemented (T3.7) |
| `file.saveAs` | `Ctrl+Shift+S` | global | Accessing Menus (File) | implemented (T3.7) |
| `file.collectAllAndSave` | — | global | Accessing Menus (File) | T5.8: File menu only (Live has no chord): copies every used sample from outside the project into `samples/imported/`, then saves |
| `file.relinkSamples` | — | global | — | T5.8: File menu (enabled while samples are missing): pick a folder, every missing sample found in it (by name; size and hash checked) is relinked |
| `file.exportAudio` | `Ctrl+Shift+R` | global | Accessing Menus (File) | M10 (export) |
| `view.toggleClipDevice` | `Shift+Tab` | global | Showing and Hiding Views | implemented (T3.7) |
| `view.toggleDetail` | `Ctrl+Alt+L` | global | Showing and Hiding Views | implemented (T3.7) |
| `view.toggleBrowser` | `Ctrl+Alt+B` | global | Showing and Hiding Views | implemented (T5.10; also View > Browser) |
| `browser.search` | `Ctrl+F` | global | Browser | implemented (T5.10; shows the browser and puts the cursor in its search field) |
| `view.toggleMixer` | `Ctrl+Alt+M` | global | Showing and Hiding Views | implemented (T3.10; the mixer view takes the track area while shown, Q89) |
| `view.togglePatch` | `Ctrl+Alt+P` | global | — (pqdaw; SPEC §15 2026-10-24) | implemented (T4.13; the project patch view takes the track area while shown, morphing from the arrangement and back; View > Arrangement / Project Patch) |
| `view.toggleFollow` | `Ctrl+Shift+F` | global | Arrangement View Commands | implemented (T3.8; the arrangement owns the switch until the piano roll follows too) |
| `grid.narrow` | `Ctrl+1` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `grid.widen` | `Ctrl+2` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `grid.toggleTriplet` | `Ctrl+3` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `grid.toggleSnap` | `Ctrl+4` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `grid.toggleFixed` | `Ctrl+5` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `edit.toggleDrawMode` | `B` | global | Editing | implemented (T3.11) |
| `track.newMidi` | `Ctrl+Shift+T` | global | Commands for Tracks | implemented (T3.10; after the last selected track; Live 12's chord since T3.x-fix-look, user confirmed 2026-10-24: was Ctrl+T) |
| `track.newAudio` | `Ctrl+T` | global | Commands for Tracks | implemented (T5.3; after the last selected track, empty chain; also in the track context menu and Create menu) |
| `track.newReturn` | `Ctrl+Alt+T` | global | Commands for Tracks | implemented (T3.10) |
| `tuning.openMosWheel` | `Ctrl+Shift+W` | global | — (pqdaw; Q15 provisional) | implemented (T3.14; the MOS wheel for the clip in the clip view, else the first main tuning; pressed again: closes it, keeping the change) |
| `app.preferences` | `Ctrl+,` | global | Accessing Menus (Options → Preferences) | implemented (T3.18; the preferences window, Audio tab; pressed again: closes it) |
| `keyboard.toggleComputerMidi` | `M` | global | Key/MIDI Map Commands | M8 (Q15) |
| `mapping.toggleMidiMap` | `Ctrl+M` | global | Key/MIDI Map Commands | M8 |
| `mapping.toggleKeyMap` | `Ctrl+K` | global | Key/MIDI Map Commands | M8 |
| `arrangement.zoomToSelection` | `Z` | arrangement | Arrangement View Commands | implemented (T3.8; the time selection, T3.9, else the whole arrangement) |
| `arrangement.zoomBack` | `X` | arrangement | Arrangement View Commands | implemented (T3.8) |
| `arrangement.zoomIn` | `=` | arrangement | Zooming, Display and Selection | implemented (T3.8; Live's `+`, unshifted) |
| `arrangement.zoomOut` | `-` | arrangement | Zooming, Display and Selection | implemented (T3.8) |
| `arrangement.markerLeft` | `Left` | arrangement | Arrangement View Commands | implemented (T3.x-fix-look; the insert marker one grid cell earlier, the selected tracks kept; T4.16b: with clips selected, not a range, the clips one grid cell earlier instead, one undo step) |
| `arrangement.markerRight` | `Right` | arrangement | Arrangement View Commands | implemented (T3.x-fix-look; one grid cell later; T4.16b: the selected clips likewise) |
| `arrangement.nudgeFineLeft` | `Alt+Left` | arrangement | Arrangement View Commands | implemented (T4.16b; the selected clips a 1/128 note earlier, off the grid; one undo step) |
| `arrangement.nudgeFineRight` | `Alt+Right` | arrangement | Arrangement View Commands | implemented (T4.16b; a 1/128 note later) |
| `arrangement.extendLeft` | `Shift+Left` | arrangement | Zooming, Display and Selection | implemented (T4.16b; the time selection's end one grid cell earlier, never before its start) |
| `arrangement.extendRight` | `Shift+Right` | arrangement | Zooming, Display and Selection | implemented (T4.16b; the end one grid cell later; from the insert marker it becomes a range over the selected tracks, selecting the clips it touches) |
| `arrangement.deselect` | `Escape` | arrangement | — (pqdaw; Live has no binding) | implemented (T4.16b; drops the clip, time and breakpoint selection, the tracks stay; a running drag is cancelled first by the view) |
| `arrangement.trackUp` | `Up` | arrangement | Commands for Tracks | implemented (T3.x-fix-look; selects the track above, scrolled into view) |
| `arrangement.trackDown` | `Down` | arrangement | Commands for Tracks | implemented (T3.x-fix-look; selects the track below) |
| `track.group` | `Ctrl+G` | arrangement | Commands for Tracks | implemented (T3.10; any parents, nesting allowed) |
| `track.ungroup` | `Ctrl+Shift+G` | arrangement | Commands for Tracks | implemented (T3.10) |
| `arrangement.rename` | `Ctrl+R` | arrangement | Editing | implemented (T3.10; the selected track's title, Enter commits, Esc cancels) |
| `arrangement.toggleActive` | `0` | arrangement | Commands for Tracks | implemented (T3.9; Q15: deactivates / reactivates the selected clips, else toggles the selected tracks' activators = mute since T3.10, a Param) |
| `arrangement.toggleAutomationMode` | `A` | arrangement | Commands for Breakpoint Envelopes | implemented (T3.15; the chosen parameter's envelope over every track's clips, Live's Automation Mode) |
| `arrangement.selectAll` | `Ctrl+A` | arrangement | Editing | implemented (T3.9) |
| `arrangement.copy` | `Ctrl+C` | arrangement | Editing | implemented (T3.9) |
| `arrangement.cut` | `Ctrl+X` | arrangement | Editing | implemented (T3.9) |
| `arrangement.paste` | `Ctrl+V` | arrangement | Editing | implemented (T3.9) |
| `arrangement.duplicate` | `Ctrl+D` | arrangement | Editing | implemented (T3.9) |
| `arrangement.delete` | `Delete` | arrangement | Editing | implemented (T3.9) |
| `arrangement.split` | `Ctrl+E` | arrangement | Arrangement View Commands | implemented (T3.9) |
| `arrangement.consolidate` | `Ctrl+J` | arrangement | Arrangement View Commands | implemented (T3.9) |
| `arrangement.loopSelection` | `Ctrl+L` | arrangement | Loop Brace and Start/End Markers | implemented (T3.9) |
| `arrangement.insertMidiClip` | `Ctrl+Shift+M` | arrangement | Arrangement View Commands | implemented (T3.9) |
| `arrangement.insertTime` | `Ctrl+I` | arrangement | Arrangement View Commands | implemented (T3.9) |
| `arrangement.duplicateTime` | `Ctrl+Shift+D` | arrangement | Arrangement View Commands | implemented (T3.9) |
| `arrangement.deleteTime` | `Ctrl+Shift+Delete` | arrangement | Arrangement View Commands | implemented (T3.9) |
| `arrangement.cutTime` | `Ctrl+Shift+X` | arrangement | Arrangement View Commands | M10 (not in T3.9) |
| `pianoroll.selectAll` | `Ctrl+A` | pianoroll | MIDI Note Editor | implemented (T3.11) |
| `pianoroll.copy` | `Ctrl+C` | pianoroll | Editing | implemented (T3.11) |
| `pianoroll.cut` | `Ctrl+X` | pianoroll | Editing | implemented (T3.11) |
| `pianoroll.paste` | `Ctrl+V` | pianoroll | Editing | implemented (T3.11) |
| `pianoroll.duplicate` | `Ctrl+D` | pianoroll | Editing | implemented (T3.11) |
| `pianoroll.delete` | `Delete` | pianoroll | Editing | implemented (T3.11) |
| `pianoroll.quantize` | `Ctrl+U` | pianoroll | MIDI Note Editor | implemented (T3.11) |
| `pianoroll.quantizeSettings` | `Ctrl+Shift+U` | pianoroll | MIDI Note Editor | M10 (quantize settings) |
| `pianoroll.stepUp` | `Up` | pianoroll | MIDI Note Editor (microtonal: SPEC §6.1) | implemented (T3.11) |
| `pianoroll.nudgeLeft` | `Left` | pianoroll | MIDI Note Editor | implemented (T3.x-fix-look; the selected notes one grid cell earlier, one undo step) |
| `pianoroll.nudgeRight` | `Right` | pianoroll | MIDI Note Editor | implemented (T3.x-fix-look; one grid cell later) |
| `pianoroll.nudgeFineLeft` | `Alt+Left` | pianoroll | MIDI Note Editor | implemented (T4.16b; the selected notes a 1/128 note earlier, off the grid; one undo step) |
| `pianoroll.nudgeFineRight` | `Alt+Right` | pianoroll | MIDI Note Editor | implemented (T4.16b; a 1/128 note later) |
| `pianoroll.shorten` | `Shift+Left` | pianoroll | MIDI Note Editor | implemented (T4.16b; the selected notes' ends one grid cell earlier, measured at the earliest note's end; the shortest keeps at least one cell; one undo step) |
| `pianoroll.lengthen` | `Shift+Right` | pianoroll | MIDI Note Editor | implemented (T4.16b; one grid cell longer) |
| `pianoroll.deselect` | `Escape` | pianoroll | — (pqdaw; Live has no binding) | implemented (T4.16b; with nothing selected Esc falls through) |
| `pianoroll.stepDown` | `Down` | pianoroll | MIDI Note Editor (microtonal: SPEC §6.1) | implemented (T3.11) |
| `pianoroll.equaveUp` | `Shift+Up` | pianoroll | MIDI Note Editor (octave → equave) | implemented (T3.11) |
| `pianoroll.equaveDown` | `Shift+Down` | pianoroll | MIDI Note Editor (octave → equave) | implemented (T3.11) |
| `pianoroll.toggleActive` | `0` | pianoroll | MIDI Note Editor | implemented (T3.11) |
| `device.delete` | `Delete` | devices | Editing | implemented (T4.1: deletes the selected devices) |
| `device.copy` | `Ctrl+C` | devices | Editing | implemented (T4.1) |
| `device.cut` | `Ctrl+X` | devices | Editing | implemented (T4.1) |
| `device.paste` | `Ctrl+V` | devices | Editing | implemented (T4.1: after the selected device, else at the end) |
| `device.duplicate` | `Ctrl+D` | devices | Editing | implemented (T4.1) |
| `device.toggleActive` | `0` | devices | Editing (Activate/Deactivate) | implemented (T4.1: the device activator) |
| (gesture) double-click a device's title | — | devices | — | T4.4-fix1: Open in Circuit View (also in the device's context menu, with Revert to Stock); the triangle folds |
| `device.group` | `Ctrl+G` | devices | Editing (Group) | implemented (T4.8: the selected devices of one chain into a new rack; with the instrument: an Instrument Rack. T4.16c: a selection of two or more devices of one list says so in the status line) |
| `device.selectPrev` | `Left` | devices | Device View (Live: arrow keys select devices) | implemented (T4.16c: the previous device in display order — out of a rack's chain to the rack, then on to the devices before it; nothing selected: the last; scrolled into view) |
| `device.selectNext` | `Right` | devices | Device View | implemented (T4.16c: the next device in display order — a rack, then the devices of the chain it shows (not when folded), then what follows the rack; nothing selected: the first) |
| `device.extendPrev` | `Shift+Left` | devices | Device View (Shift: extend the selection) | implemented (T4.16c: adds the previous device of the lead device's own list, or steps back out of the selection; then Ctrl+G groups it) |
| `device.extendNext` | `Shift+Right` | devices | Device View | implemented (T4.16c: adds the next device of the lead device's own list, or steps back) |
| `device.selectFirst` | `Home` | devices | Device View | implemented (T4.16c: the first device of the selected device's list — a rack chain is its own list — else of the track's chain) |
| `device.selectLast` | `End` | devices | Device View | implemented (T4.16c: the last device of that list) |
| `device.moveLeft` | `Ctrl+Left` | devices | — (pqdaw; Live moves devices by dragging only, Q142) | implemented (T4.16c: the one selected device a step left along its list, one undo step; the instrument stays first) |
| `device.moveRight` | `Ctrl+Right` | devices | — (pqdaw, Q142) | implemented (T4.16c: a step right) |
| `device.chainUp` | `Up` | devices | Device View (Live: the chain list's Up / Down) | implemented (T4.16c: the selected rack, or the rack around the selected device, shows its previous chain; a device inside hands the selection to the first device of the chain shown; a focused knob takes Up / Down first) |
| `device.chainDown` | `Down` | devices | Device View | implemented (T4.16c: the next chain) |
| (gesture) double-click a chain's name | — | devices | Rack chain list | T4.16c: rename the chain in place (also its right-click menu's Rename); `Enter` commits, `Esc` cancels, one undo step |

## Handled by controls, not the keymap (T3.2, implemented)

These keys belong to the focused control and never reach a keymap while it has focus:

| Control | Keys | Effect |
|---|---|---|
| Number box (drag box), knob, fader | `Up` / `Down` (`Shift`: fine) | step the value (Live: Adjusting Values) |
| Number box, knob, fader | `Delete` / `Backspace` | return to the default value |
| Number box | `Enter`, double-click | type a value; `Enter` commits, `Esc` cancels, focus loss commits |
| Number box | a digit, `.` or `-` (no modifiers) while focused | T4.16a: the type-in opens with that character (Live: click the tempo, type `128`, `Enter`); a focused box takes these keys before the keymap (`0` and `-` included) |
| Number box, knob, fader | mouse wheel over it (`Shift`: fine; `Ctrl` / `Alt` pass it on) | T4.16a: one key step per notch; a run of notches is one undo step (ends 0.4 s after the last notch, or when the pointer leaves); `Esc` during the run cancels it. The arrangement headers' boxes leave the wheel to the track scroll |
| Knob, fader | double-click | T4.16a: return to the default value (one undo step) |
| Text field | `Left` / `Right` (`Ctrl`: by word, `Shift`: select), `Home` / `End`, `Backspace` / `Delete` (`Ctrl`: word), `Ctrl+A` / `C` / `X` / `V` | editing; `Enter` commits, `Esc` reverts |
| Button, toggle | `Space` / `Enter` | press / flip |
| Dropdown | `Up` / `Down` change the item; `Space` / `Enter` open the list | |
| Menu | `Up` / `Down` / `Home` / `End` move, `Right` / `Enter` open a submenu, `Left` / `Esc` close one level, `Enter` / `Space` choose | modal while open |
| Popover | `Esc` | dismiss |
| any | `Tab` / `Shift+Tab` | focus traversal (T3.1) when no binding takes the chord |
| Code editor (the circuit editor's text pane, T4.4) | arrows (`Ctrl`: by word), `Home` / `End` (`Ctrl`: of the text), `Page Up` / `Page Down`, `Shift` selects; `Backspace` / `Delete` (`Ctrl`: word); `Enter` keeps the indent; `Tab` / `Shift+Tab` indent / outdent; `Ctrl+A` / `C` / `X` / `V` | editing; `Ctrl+Z` / `Ctrl+Y` go to the editor's one history |

## Pointer affordances: arrangement, piano roll, automation (T4.16b)

Every part shows the cursor of what a press there does (the same hit test the press uses), and the part a press would grab is highlighted, as in Live. Modifier changes with the pointer still re-evaluate it (Alt over an envelope).

| Part | Cursor | Hover highlight | Press / drag |
|---|---|---|---|
| Note body | Move | note brightened | select, drag moves (Alt copies) |
| Note start / end (4 px) | ↔ | note brightened + the edge marked | drag resizes that edge |
| Note area, empty | Arrow | — | selection rectangle; double-click adds a note |
| Note area in Draw Mode (B) | Crosshair | note body | click adds a cell's note / deletes the note under it |
| Velocity / chance stem | ↕ | — | drag changes the selected notes' values |
| Keys | Pointer | — | click auditions, selects the row (horizontal drag zooms: ↔ while dragging) |
| Clip ruler: start / end flag, loop brace ends | ↔ | — | drag moves it |
| Clip ruler: loop brace | Move | — | drag moves the loop |
| Clip ruler: lower strip | ↕ | — | drag zooms (sideways scrolls) |
| Clip title strip | Move | strip brightened | drag moves (Alt copies) |
| Clip start / end | ↔ | the edge marked | drag trims |
| Clip body, empty lane | Arrow | — | a time selection (Live) |
| Beat ruler: loop brace ends | ↔ | — | drag moves the loop start / end (one undo step; Esc cancels) |
| Beat ruler: loop brace | Move | — | drag moves the loop |
| Beat ruler: locator flag | ↔ | — | drag moves the locator; click jumps there |
| Beat ruler: numbers / elsewhere | ↕ | — | click locates; drag zooms (sideways scrolls) |
| Overview | Move | — | click / drag moves the view |
| Track header / lane header bottom edge | ↕ | — | drag resizes the row / lane |
| Fold triangle, lanes toggle, parameter choosers, lane + / −, Re-enable Automation | Pointer | (their own) | click |
| Breakpoint | Move | handle enlarged | drag moves the selected points |
| Envelope line | Pointer | — | click adds a breakpoint (dragged on from there) |
| Envelope segment with Alt | ↕ | — | drag bends it |
| Lane / automation band elsewhere (or Shift / Ctrl on the line) | Arrow | — | a time selection |
| Audio clip: fade-in / fade-out handle (top of the body, at the fade's end; T5.6) | ↔ (Alt: ↕) | handle lit | drag = fade length; Alt-drag = its curve; past the clip's edge onto an adjacent audio clip = that boundary's crossfade (centred, length 2 × the distance); one undo step; Esc cancels |
| Audio clip: gain handle (top centre of the body; T5.6) | ↕ | handle lit | drag = clip gain (0.2 dB per px, Shift 0.02); double-click = 0 dB; one undo step |
| Audio clip edges (T5.6) | ↔ | the edge marked | drag trims; stops at the file's ends (warp Off; Re-Pitch without a loop), a looped Re-Pitch clip extends through its loop |
| Audio Clip View: start marker (ruler strip, T5.6) | ↔ | — | drag = where the clip starts in the file (warp Off: frames; Re-Pitch: 1/16 notes, Shift free); one undo step |
| Audio Clip View: Re-Pitch loop brace / its ends (T5.6) | Move / ↔ | — | drag the loop (1/16 notes, Shift free); one undo step |

## Circuit editor (T4.4, handled by the editor window, not the keymap)

T4.4-fix1 (SPEC §15 2026-10-26): the editor works on the project's devices — right-click a device > Open in Circuit View, or double-click its title (the fold triangle folds the panel). Options > Developer > Open Patch File in Circuit View… opens a loose `.pqp` file (canvas left, text right). These keys work while the editor has focus (the canvas ones while the canvas has it); they are not keymap entries because the editor is a modal full-window popover.

| Keys | Where | Effect |
|---|---|---|
| `Ctrl+Z` / `Ctrl+Shift+Z`, `Ctrl+Y` | editor | undo / redo (one history over canvas and text); on a project device: the project's undo / redo (one history with the Ableton view) |
| `Ctrl+S` | editor | save (the drawn layout spliced into the file); a project device has no file (File > Save Set saves it) |
| `Ctrl+Enter` | editor | play / stop the shown patch (edits hot-swap while it plays); a project device plays in the project |
| `Esc` | text pane / editor | back to the canvas; closes the editor (twice when there are unsaved changes) |
| drag a pin to a pin | canvas | connect (a loop with no delay gets a z⁻¹) |
| drag a node | canvas | move the selection |
| double-click empty space, `N` | canvas | add a node from a spec (`tanh`, `0.5`, `440Hz`, `*`, `svf`, `param x = 1`, `in x : audio`, `out y`, `export e`) |
| double-click a node, `Ctrl+R` | canvas | rename (a subcircuit opens instead on double-click) |
| `Delete` / `Backspace` | canvas | delete the selected nodes and wires |
| `Ctrl+G` | canvas | make a subcircuit of the selection |
| `P` | canvas | turn the selected constant into a param pin |
| `Enter` / double-click | canvas, a subcircuit selected | open it; `Backspace` with nothing selected goes back |
| `F`, `=` / `-`, wheel, `Ctrl+wheel`, middle-drag | canvas | fit, zoom, scroll, zoom at the pointer, pan (T4.2) |

## Cross-device wiring (T4.7, handled by the cable layer, not the keymap)

| Keys / gesture | Where | Effect |
|---|---|---|
| drag a jack onto a knob | Device view | wire the export to the param (one undo step); hover a track row in the arrangement first to reach another track's devices (the list scrolls at its edges) |
| `Esc` | while dragging a cable | cancel |
| drag the badge up / down (`Shift`: fine), double-click | a cable's badge | the wire's amount (−100 … +100 %, one undo step); back to 50 % |
| `Delete` / `Backspace` | a selected badge | delete the wire (also `wire.delete`, and the badge's right-click menu) |
| View > Cables | menu | `view.cycleCables`: Show → Fade → Hide (no default chord) |

## Project patch view (T4.13, handled by its canvas, not the keymap)

`Ctrl+Alt+P` (View > Arrangement / Project Patch) shows the whole project as one circuit in the track area. These keys and gestures work while its canvas has focus; navigation (wheel, `Ctrl`+wheel, middle / `Alt` drag, `=` / `-`) is the circuit canvas's.

| Keys / gesture | Where | Effect |
|---|---|---|
| `F`, `Esc` (nothing selected), double-click outside the bands | canvas | zoom to the whole project (animated) |
| double-click a band (its empty space or name) | canvas | zoom onto that band |
| double-click a device block / its frame | canvas | open it to its atoms in place (read-only) / close it |
| double-click a mixer strip | canvas | expand it (each send's level beside its pin) / collapse |
| drag an export pin (right side of a device, under its output) onto a device or a param pin | canvas | modulation wire (a menu of the device's params when dropped on its body); one undo step |
| drag a mixer's `out` pin onto a group's, return's or the master's Σ | canvas | route the track there (a return: its send to 0 dB); one undo step |
| drag a send pin (A, B …) onto its return | canvas | turn the send on (0 dB) |
| drag a device block along its band or onto another band | canvas | move it (reorder / to that track); one undo step |
| `Delete` / `Backspace` | a selection | delete the selected devices; wires: a modulation wire is removed, a send goes to −∞, a route becomes Sends Only (one undo step for all) |
| `0` | selected devices | toggle their activators |
| click M / S on a mixer strip (`Ctrl`: additive solo) | canvas | the track's activator (mute) / solo |
| drag a mixer strip's volume bar / pan / send bar sideways (`Shift`: fine) | canvas | volume / pan / send level (one undo step per drag; `Esc` cancels) |
| drag a band's title strip, `Ctrl`+drag a device block | canvas | nudge the layout (view state, saved with the project, not undoable); band menu: Reset Layout |
| right-click a band / a block / a wire | canvas | Add Device ▸, Zoom to Band / Project, Reset Layout; Delete, Activate, Open to Atoms, Open in Circuit View, Output ▸; Delete Wire / Send to −inf / Sends Only |

## Drops and the browser (T5.7, T5.10; handled by the widgets, not the keymap)

| Where | Input | Does |
|---|---|---|
| Arrangement lanes | drop files (Explorer) or browser items | onto an audio track: the first file there at the snapped tick, one new audio track per further file; onto a MIDI / group track: new audio tracks after it; below the tracks: new tracks at the end; one undo step |
| Arrangement lanes | hold `Ctrl` while dropping | every file on one track, one after another |
| Arrangement / browser | drop a folder | adds it to the browser's Places |
| Browser (focus inside) | `Up` / `Down` | select the previous / next item (a sample previews while Preview is on) |
| Browser | `Right` / `Left` | unfold a folder or enter it / fold it or go to its folder |
| Browser | `Enter`, double-click | load: a sample becomes a clip at the start marker on the selected audio track (else a new one); a folder folds. T5.11: a device (Instruments / Audio Effects), preset or user device goes at the end of the selected track's chain (a new device with its default preset); a tuning becomes a new main tuning; a clip lands at the start marker on the selected track (or a new track of its kind); in hot-swap mode Enter loads in place and leaves the mode |
| Browser (hot-swap) | `Up` / `Down`, click | loads the selected preset / tuning in place (one undo step each) |
| Browser chips | click / right-click | a filter chip (Dur, BPM, Rate, Ch) steps to its next range / back to Any; a tag chip toggles that tag |
| Browser right edge | drag, double-click | resizes the browser / back to the default width |
| Browser | `Esc` | stops the preview; T5.11: leaves hot-swap mode |
| Browser | a printable key | goes to the search field |
| Browser list | drag | carries the item (drop it on the arrangement; T5.11: a device, preset or user device on a device chain; T5.12: out of the window, a file goes to Explorer) |
| Device title bar | click on the preset slot | hot-swap: the browser lists the device's presets (T5.11) |
| Device / tuning chip / tunings card | right-click | T5.11: Save Preset, Save as Default Preset, Save as New Device, Hot-Swap Presets…; Save to Library, Hot-Swap Tuning… |
| Arrangement clips | drag onto the browser | T5.12: saves them as .pqclip into the folder under the pointer (`Alt`: renders audio clips to WAV) |
| Arrangement clips | drag out of the window | T5.12: Explorer receives WAV files rendered as the clips play (MIDI clips: M11) |
| Browser preview | click in the waveform | plays from there (scrub) |

## Not bound yet

- Wiring (T4.7): `view.cycleCables` and `wire.delete` have no default chord (Live has none); the View menu and the badge run them.
- Piano roll (T3.11): `pianoroll.fold` (fold to used notes / scale) and `pianoroll.toggleScale` (scale highlighting) are registered actions without a default chord (Live has none); the clip view's Fold and Scale buttons trigger them.
- Macro mode toggle `Ctrl+CapsLock` (SPEC §6.2): CapsLock is a lock key, which a keymap chord cannot use as its key (T3.1); M8 handles it in the macro-mode scope.
- Live shortcuts outside M3–M10 scope (Session View, racks, plug-in windows) are not listed.
