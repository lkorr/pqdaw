# pqdaw hotkeys

The default keymap is data: [`assets/keymaps/ableton.json`](../assets/keymaps/ableton.json) (format in `src/ui/include/pq/ui/keymap_file.h`). It copies Ableton Live 12's shortcuts (SPEC §6.1) onto pqdaw action ids. A binding is either `implemented` (its action must be registered when the keymap is applied, or loading fails) or `deferred:M<n>` (the chord is reserved for the milestone that implements it and does nothing until then). `tests/ui/test_controls.cpp` checks that every chord parses, that no chord is bound twice in one scope, that every implemented action resolves, and that every binding is listed below.

Scopes: `global` (always active, consulted last), `arrangement` (focus inside the arrangement view), `pianoroll` (focus inside the clip view's note editor). A focused text field takes printable keys first (Q27), so `B` while renaming types a "b". Chords match the layout key (Q27). Windows key names; on macOS Ctrl would read Cmd.

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
| `file.exportAudio` | `Ctrl+Shift+R` | global | Accessing Menus (File) | M10 (export) |
| `view.toggleClipDevice` | `Shift+Tab` | global | Showing and Hiding Views | implemented (T3.7) |
| `view.toggleDetail` | `Ctrl+Alt+L` | global | Showing and Hiding Views | implemented (T3.7) |
| `view.toggleBrowser` | `Ctrl+Alt+B` | global | Showing and Hiding Views | M5 (browser) |
| `view.toggleMixer` | `Ctrl+Alt+M` | global | Showing and Hiding Views | implemented (T3.10; the mixer view takes the track area while shown, Q89) |
| `view.toggleFollow` | `Ctrl+Shift+F` | global | Arrangement View Commands | implemented (T3.8; the arrangement owns the switch until the piano roll follows too) |
| `grid.narrow` | `Ctrl+1` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `grid.widen` | `Ctrl+2` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `grid.toggleTriplet` | `Ctrl+3` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `grid.toggleSnap` | `Ctrl+4` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `grid.toggleFixed` | `Ctrl+5` | global | Arrangement View Commands; MIDI Note Editor | implemented (T3.11) |
| `edit.toggleDrawMode` | `B` | global | Editing | implemented (T3.11) |
| `track.newMidi` | `Ctrl+Shift+T` | global | Commands for Tracks | implemented (T3.10; after the last selected track; Live 12's chord since T3.x-fix-look, user confirmed 2026-10-24: was Ctrl+T) |
| `track.newAudio` | `Ctrl+T` | global | Commands for Tracks | M6 (audio tracks; Live 12: Ctrl+T inserts an audio track) |
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
| `arrangement.markerLeft` | `Left` | arrangement | Arrangement View Commands | implemented (T3.x-fix-look; the insert marker one grid cell earlier, the selected tracks kept) |
| `arrangement.markerRight` | `Right` | arrangement | Arrangement View Commands | implemented (T3.x-fix-look; one grid cell later) |
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
| `pianoroll.stepDown` | `Down` | pianoroll | MIDI Note Editor (microtonal: SPEC §6.1) | implemented (T3.11) |
| `pianoroll.equaveUp` | `Shift+Up` | pianoroll | MIDI Note Editor (octave → equave) | implemented (T3.11) |
| `pianoroll.equaveDown` | `Shift+Down` | pianoroll | MIDI Note Editor (octave → equave) | implemented (T3.11) |
| `pianoroll.toggleActive` | `0` | pianoroll | MIDI Note Editor | implemented (T3.11) |

## Handled by controls, not the keymap (T3.2, implemented)

These keys belong to the focused control and never reach a keymap while it has focus:

| Control | Keys | Effect |
|---|---|---|
| Number box (drag box), knob, fader | `Up` / `Down` (`Shift`: fine) | step the value (Live: Adjusting Values) |
| Number box, knob, fader | `Delete` / `Backspace` | return to the default value |
| Number box | `Enter`, double-click | type a value; `Enter` commits, `Esc` cancels, focus loss commits |
| Text field | `Left` / `Right` (`Ctrl`: by word, `Shift`: select), `Home` / `End`, `Backspace` / `Delete` (`Ctrl`: word), `Ctrl+A` / `C` / `X` / `V` | editing; `Enter` commits, `Esc` reverts |
| Button, toggle | `Space` / `Enter` | press / flip |
| Dropdown | `Up` / `Down` change the item; `Space` / `Enter` open the list | |
| Menu | `Up` / `Down` / `Home` / `End` move, `Right` / `Enter` open a submenu, `Left` / `Esc` close one level, `Enter` / `Space` choose | modal while open |
| Popover | `Esc` | dismiss |
| any | `Tab` / `Shift+Tab` | focus traversal (T3.1) when no binding takes the chord |
| Code editor (the circuit editor's text pane, T4.4) | arrows (`Ctrl`: by word), `Home` / `End` (`Ctrl`: of the text), `Page Up` / `Page Down`, `Shift` selects; `Backspace` / `Delete` (`Ctrl`: word); `Enter` keeps the indent; `Tab` / `Shift+Tab` indent / outdent; `Ctrl+A` / `C` / `X` / `V` | editing; `Ctrl+Z` / `Ctrl+Y` go to the editor's one history |

## Circuit editor (T4.4, handled by the editor window, not the keymap)

View > Open Patch in Circuit View… opens a `.pqp` file in the editor (canvas left, text right). These keys work while the editor has focus (the canvas ones while the canvas has it); they are not keymap entries because the editor is a modal full-window popover.

| Keys | Where | Effect |
|---|---|---|
| `Ctrl+Z` / `Ctrl+Shift+Z`, `Ctrl+Y` | editor | undo / redo (one history over canvas and text) |
| `Ctrl+S` | editor | save (the drawn layout spliced into the file) |
| `Ctrl+Enter` | editor | play / stop the shown patch (edits hot-swap while it plays) |
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

## Not bound yet

- Piano roll (T3.11): `pianoroll.fold` (fold to used notes / scale) and `pianoroll.toggleScale` (scale highlighting) are registered actions without a default chord (Live has none); the clip view's Fold and Scale buttons trigger them.
- Macro mode toggle `Ctrl+CapsLock` (SPEC §6.2): CapsLock is a lock key, which a keymap chord cannot use as its key (T3.1); M8 handles it in the macro-mode scope.
- Live shortcuts outside M3–M10 scope (Session View, racks, plug-in windows) are not listed.
