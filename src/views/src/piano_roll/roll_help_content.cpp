// KB.help: the text of the piano roll's help window. DATA ONLY: sections -> entries {gesture, title, text}; the widget
// (roll_help.cpp) lays out whatever is here. To change what the window says, edit this table and nothing else.
//
// Rules for an entry:
//   - gesture: `{action.id}` for a key the keymap owns (the panel shows the key that action has in the LOADED keymap,
//     or "unbound"), `[Alt]` for a literal key cap, `\n` for a new line, plain words for the rest (roll_help.h).
//   - title + text: for a musician, not a programmer: what it does and when you would use it, in one or two plain
//     sentences. No internal names.
//   - Only what the roll really does. An entry whose feature is removed is deleted here in the same commit.
//   - Non-ASCII characters as \x escapes, each escape ending its literal ("Is Harmonic\xE2\x80\xA6" " next words").

#include "roll_help.h"

namespace pq::views::roll {

namespace {

// ---- Quick start (T7.16a): the five most useful JI actions, in the order a first session meets them ------------------
constexpr HelpEntry kQuickStart[] = {
    {"1. select a chord,\n{pianoroll.tuneToJi}",
     "Tune a chord to JI and back",
     "Drag a rectangle around the notes of a chord and press the key: the chord becomes just, inside a dashed frame "
     "named by its proportion, such as 4:5:6. Press it again to hear it in the tuning, and again for the same just "
     "chord: the reading and its warps are kept. Select several chords at once and each is tuned on its own."},
    {"2. drag the frame's bottom-left\ncorner inward",
     "Play it tempered, then glide into JI",
     "Up to where you let go the chord plays as you wrote it; from there it glides into just intonation. The "
     "bottom-right corner glides it back out."},
    {"3. click the chord's header,\n{pianoroll.stepUp} / {pianoroll.stepDown} or drag",
     "Transpose a just chord",
     "A click on the proportion above the frame selects the whole chord; Up, Down or a drag moves it by steps of the "
     "tuning and it stays just. Up or Down on one note of the chord moves that note to the next harmonic instead."},
    {"4. drag the ~ above a note's end",
     "Vibrato",
     "Up for width, sideways for speed; the four small squares under a vibrating note fade it in and out."},
    {"5. right-click a note > Is Harmonic\xE2\x80\xA6" " 5,\n[Alt] + double-click between the lines",
     "Build on a harmonic",
     "Declare a note harmonic 5, then hold Alt over its time: the lines between its harmonics appear. Double-click "
     "the one a fifth above (it is numbered 15): the note is added and the series reads 10:15."},
};

// ---- Notes -----------------------------------------------------------------------------------------------
constexpr HelpEntry kNotes[] = {
    {"double-click",
     "Add or delete a note",
     "Double-click an empty spot to put a note there; double-click a note to delete it."},
    {"{edit.toggleDrawMode}",
     "Draw Mode (the pencil)",
     "Turns the pencil on or off; a coloured bar under the Fold and Scale buttons shows that it is on. With it on, a click adds a "
     "note one grid cell long, a drag paints a note in every cell it crosses, and a click on a note deletes it."},
    {"click \xC2\xB7" " [Shift] + click\ndrag on empty space\n{pianoroll.selectAll}",
     "Select",
     "Click a note to select it; with Shift or Ctrl a click adds it to the selection or takes it out. Drag across "
     "empty space to select everything inside the rectangle. A click on a key at the left selects every note on that "
     "row and lets you hear its pitch."},
    {"{pianoroll.deselect}",
     "Deselect",
     "Clears the selection."},
    {"drag a note\n[Alt] + drag a note",
     "Move or copy",
     "Dragging moves the selected notes: in time they snap to the grid, in pitch they go row by row. Hold Alt to leave "
     "the notes where they are and drag a copy instead."},
    {"{pianoroll.nudgeLeft} / {pianoroll.nudgeRight}\n{pianoroll.nudgeFineLeft} / {pianoroll.nudgeFineRight}",
     "Nudge in time",
     "Moves the selected notes one grid cell earlier or later. With Alt they move by a tiny amount (a 128th note), "
     "off the grid, for a note that should sit just ahead of or behind the beat."},
    {"drag a note's left or right end\n{pianoroll.shorten} / {pianoroll.lengthen}",
     "Resize",
     "Drag an end of a note to change where it starts or stops. The keys make the selected notes one grid cell "
     "shorter or longer."},
    {"{pianoroll.stepUp} / {pianoroll.stepDown}",
     "Up or down by a step",
     "Moves the selected notes to the next step of the tuning: one row. In 12-EDO that is a semitone, in 31-EDO a "
     "31st of an octave, in a scale the next note of the scale."},
    {"{pianoroll.equaveUp} / {pianoroll.equaveDown}",
     "Up or down by an equave",
     "Moves the selected notes by one equave: the interval at which the tuning repeats, an octave in most tunings."},
    {"{pianoroll.duplicate}",
     "Duplicate",
     "Puts a copy of the selected notes right after them, with their bends and pitch curves."},
    {"{pianoroll.copy} / {pianoroll.cut} / {pianoroll.paste}",
     "Copy, cut, paste",
     "The selected notes, with their bends and pitch curves, to and from the clipboard."},
    {"{pianoroll.delete}",
     "Delete",
     "Removes the selected notes."},
    {"{pianoroll.toggleActive}",
     "Mute or unmute notes",
     "Silences the selected notes without deleting them (they are drawn greyed); press again to bring them back."},
    {"{pianoroll.quantize}",
     "Quantize",
     "Pulls the starts of the selected notes onto the grid; with nothing selected, every note of the clip."},
    {"wheel \xC2\xB7" " [Shift] + wheel\n[Ctrl] + wheel\n[Ctrl] + [Alt] + wheel",
     "Scroll and zoom",
     "The wheel scrolls up and down through the pitches, with Shift along the time. Ctrl + wheel zooms the time, "
     "Ctrl + Alt + wheel makes the rows taller or flatter: tall rows help when notes are only a few cents apart."},
};

// ---- Bends between notes -----------------------------------------------------------------------------------
constexpr HelpEntry kBends[] = {
    {"[Alt] + drag a note's left edge",
     "Bend in",
     "The note starts on the pitch of the note before it and slides into its own pitch; how far you drag sets how long "
     "the slide takes. With Alt held the edge is the outer quarter of the note (at least a few pixels), so even a short "
     "note can be bent; Alt on its middle drags a copy. The note it comes from is outlined while you drag: of the notes "
     "that end just before this one, the nearest in pitch."},
    {"[Alt] + drag a note's right edge",
     "Bend out",
     "The end of the note slides to the pitch of the note that starts next, over the length you drag. With several "
     "notes selected each one bends to its own neighbour."},
    {"[Alt] + drag an edge\nup or down",
     "Bend from or to any pitch",
     "A note with no note before it (or after it) to glide with bends to the pitch you drag to: up or down sets the "
     "pitch, sideways how long the slide takes (just up or down: one grid cell). It snaps to the rows, the harmonic "
     "lines and the note's own pitch; Shift goes by single cents. The slide is written into the note's pitch curve, "
     "so its points can be moved afterwards."},
    {"move or retune\nthe other note",
     "A bend follows its notes",
     "A bend remembers which note it leads from or to, not a fixed pitch. Move that note, retune it or change the "
     "tuning, and the slide still lands exactly on it; nothing has to be redone."},
    {"{pianoroll.glide}",
     "Glide into the next note",
     "Select two or more notes: each slides into the next selected one over its last grid cell. The quick way to "
     "turn a melody into one unbroken line. Also in the right-click menu as Glide into Next."},
    {"right-click a bent note >\nBend In / Bend Out",
     "Shape of a bend",
     "S-Curve (the default) leaves the first pitch gently and settles into the second; Straight slides at an even "
     "speed, like a slide on a string."},
    {"[Alt] + drag the edge back",
     "Remove a bend",
     "Alt + drag the bent edge back to where the note starts (or ends) and let go: the note is steady again. Clear "
     "Bends in the right-click menu removes all of a note's bends at once."},
};

// ---- Pitch curves ----------------------------------------------------------------------------------------
constexpr HelpEntry kCurves[] = {
    {"Draw Mode {edit.toggleDrawMode}\ndrag along a note",
     "Draw a pitch curve",
     "The line you draw becomes the pitch of the note from that point on: a scoop, a fall, a slow drift to another "
     "pitch. Start the stroke where the change should begin. (A drag of only a few pixels counts as a click: it "
     "deletes the note, as a click in Draw Mode does.)"},
    {"Draw Mode {edit.toggleDrawMode}\n[Ctrl] + click on a note",
     "Add a breakpoint",
     "Puts one point on the note's pitch line at the pointer; keep the button down to drag it into place. Use it to "
     "build a curve from a few exact points instead of a freehand line."},
    {"drag a breakpoint\n[Shift] + drag",
     "Move a breakpoint",
     "Select a note to see the points of its curve. A point snaps to the rows, to the harmonic guide lines and to the "
     "note's own pitch, and in time to the grid; hold Shift to put it anywhere."},
    {"drag the diamond between\ntwo breakpoints",
     "Curve a segment",
     "Bows the line between two points, from a straight slide to one that leaves slowly and arrives fast (or the "
     "other way round)."},
    {"double-click a breakpoint",
     "Delete a breakpoint",
     "Removes that point; the line joins its neighbours."},
    {"right-click a selected note >\nClear Curves",
     "Clear Curves",
     "Removes every bend, drawn curve and vibrato from the selected notes: they are plain steady notes again."},
};

// ---- Vibrato ---------------------------------------------------------------------------------------------
constexpr HelpEntry kVibrato[] = {
    {"drag the ~ above a note's\nright end up or down",
     "Vibrato: how wide",
     "Point at a note (or select it) and a small ~ appears above its right end. Drag it up and the note starts to "
     "waver; the further up, the wider the swing. You see the wave grow on the note, and the status line reads its "
     "width in cents. Drag it back down to nothing to turn the vibrato off; keep going below the note and the vibrato "
     "turns upside down (it swings down first), the ~ following under the line."},
    {"drag the ~ left or right",
     "Vibrato: how fast",
     "To the right the waves stretch out (slower), to the left they squeeze together (faster), smoothly; the status line "
     "reads the speed in Hz. A new vibrato starts at 6 Hz. A small sideways slip while you set the width changes nothing."},
    {"drag the small squares\nunder a vibrating note",
     "Fade in, fade out, start and end",
     "A note with vibrato shows four squares under it, the corners of the vibrato's outline: where it starts, where it "
     "has grown to full width, where it begins to die away and where it ends. Drag any of them sideways. Two squares "
     "that sit on each other (no fade yet) come apart as you drag: to the right for a fade-in, to the left for a "
     "fade-out. Pull the two inner ones together for a swell that rises and falls. They snap to the grid."},
    {"[Shift] + drag the ~\n[Shift] + drag a square",
     "Fine adjustment",
     "With Shift the ~ moves the width by tenths of a cent and the speed ten times more slowly, and a square goes "
     "anywhere instead of onto the grid."},
    {"several notes selected,\ndrag the ~ of one",
     "Vibrato on several notes",
     "Every selected note gets the same width and speed; each keeps its own start, end and fades. Dragging a square "
     "moves that corner on all of them."},
    {"double-click the ~",
     "Remove a vibrato",
     "Turns the vibrato off (on every selected note, when the note is one of them)."},
    {"Draw Mode {edit.toggleDrawMode}\n[Alt] + drag along a note",
     "Vibrato over a stretch",
     "Vibrato only over the stretch you drag. How far the stroke moves up or down sets how wide it swings; a flat "
     "stroke uses the width of the last one. Afterwards the ~ and the squares change it like any other."},
};

// ---- Detune ----------------------------------------------------------------------------------------------
constexpr HelpEntry kDetune[] = {
    {"[Shift] + drag a note up or down",
     "Free detune",
     "Moves the selected notes smoothly in pitch instead of row by row, for a note that should be a few cents sharp "
     "or flat; the status line reads the amount. Nothing happens until the pointer has moved a little way up or down, "
     "so a Shift + click that slips, or a Shift + drag along the time, never detunes by accident. The notes become "
     "cents notes: fixed pitches that no longer follow the tuning. In time they still snap to the grid."},
    {"right-click a selected note >\nSet Pitch\xE2\x80\xA6",
     "Type an exact pitch",
     "A ratio such as 7/4 (a just pitch), cents such as 386.3c (a fixed pitch) or a step of the tuning such as 4\\22 "
     "(step 4 of 22). Press Enter."},
};

// ---- Harmonics and the virtual fundamental -----------------------------------------------------------------
constexpr HelpEntry kHarmonics[] = {
    {"right-click a note >\nIs Harmonic\xE2\x80\xA6",
     "Declare a note as harmonic N",
     "Type a whole number N and press Enter. The note stays exactly where it is, in the roll and in sound, and a "
     "harmonic series is fitted around it so that it sits on harmonic N. Call a note 4, and the lines 5, 6 and 7 "
     "above it are its just major third, fifth and harmonic seventh. The note is frozen: the series hangs on it."},
    {"draw \xC2\xB7" " double-click",
     "Notes snap to the harmonics",
     "Inside the time of a series (its notes' span; the guide lines are drawn there) the notes you add land on the "
     "nearest line instead of a row and join the series, each an exact ratio to the frozen note. Anywhere else they "
     "go on the rows of the tuning as usual, so the next chord you write is an ordinary chord."},
    {"[Alt] + draw \xC2\xB7" " [Alt] + double-click",
     "Between the harmonics",
     "Hold Alt over a series and the lines between its harmonics appear (the series counted twice as fine). A note "
     "drawn on one renumbers the series so that it gets a whole number: a fifth above harmonic 5 is 15, and the 5 "
     "reads 10."},
    {"[Shift] + draw",
     "Draw on the rows instead",
     "Inside a series, hold Shift to put an ordinary note on a row of the tuning. Elsewhere it is the other way round: "
     "Shift draws a just note, at the simplest ratio near the pointer."},
    {"right-click a note of the series >\nAdd Note at Ratio\xE2\x80\xA6",
     "Add a note at a ratio",
     "Type 3/2 (or 7:4) and press Enter for a note that far above the note you clicked; 2/3 puts it below. A whole "
     "number such as 5 means that harmonic of the series. The new note starts and ends with the one you clicked."},
    {"the numbers on the notes\nand at the roll's right edge",
     "How the numbers renumber",
     "The numbers are always the smallest whole numbers that fit every note. When a note falls between two harmonics "
     "the whole series is renumbered so that it gets a whole number too. Declare a note 5 and add a note a perfect "
     "fifth (3/2) above it: 7.5 is not a harmonic, so both double and now read 10 and 15. No note changes pitch; only "
     "the numbers and the lines do."},
    {"{pianoroll.deselect}",
     "Put the guide lines away",
     "The guide lines show for the series under the pointer, or the one of the note you selected last. Esc puts the "
     "selected one's away. Click any note of a series to bring its lines back."},
    {"Harmonics: JI\n(the box left of the roll)",
     "Draw on the harmonics, or not",
     "The JI switch appears once the clip has a harmonic series. On, notes drawn inside a series snap to its "
     "harmonics and join it; off, every note goes on the rows of the tuning and no guide lines show."},
    {"right-click > Remove JI Group",
     "Remove the declaration",
     "Forgets which note was frozen. Every note keeps its pitch (the notes that were tied to it become fixed "
     "pitches); new notes land on the rows again."},
};

// ---- Tuning ----------------------------------------------------------------------------------------------
constexpr HelpEntry kTuning[] = {
    {"click the tuning's name\n(the box left of the roll)",
     "Change the tuning: the MOS wheel",
     "Opens the wheel for this clip: choose a MOS scale, an EDO, a Scala file or a temperament. Every change applies "
     "at once, so the rows move and sounding notes retune while you drag."},
    {"right-click a selected note >\nDegree / Ratio / Cents",
     "What a note follows",
     "A degree note is a step of the tuning and moves when the tuning changes. A ratio note stays at its just pitch. "
     "A cents note never moves. Converting to cents keeps the exact pitch; to a degree, the note goes to the nearest "
     "step; to a ratio, to the simplest ratio close by."},
    {"Fold",
     "Fold",
     "Shows only the rows that have notes on them (with Scale on: only the rows of the scale), so a wide tuning fits "
     "on the screen."},
    {"Scale",
     "Scale",
     "Highlights the rows of the scale inside the tuning; for an EDO, its diatonic scale."},
    {"right-click > Row Labels",
     "What the keys show",
     "Note names, step numbers, cents or ratios on the keys at the left. Auto picks ratios for a just scale and note "
     "names otherwise."},
    {"[Alt] + draw",
     "Draw on the background EDO",
     "For a scale or MOS tuning the box at the left can show an EDO grid behind the rows. With the grid showing, Alt "
     "draws a note on its nearest line, as a fixed cents pitch (inside a harmonic series Alt draws between its "
     "harmonics instead)."},
};

// ---- JI groups -----------------------------------------------------------------------------------------------
// The whole JI workflow of the roll (T7.14): tune a chord and switch back, the other readings, warps, walking,
// typed proportions, the group as notes.
constexpr HelpEntry kJiGroups[] = {
    {"{pianoroll.tuneToJi}",
     "Tune a chord to JI, and back",
     "Select the notes of a chord and press the key: the lowest note stays where it is and the others move to the "
     "nearest simple just intervals above it. They are the same notes, now inside a dashed frame headed by the chord's "
     "proportion. Press the key again with them selected and they play exactly what you wrote; the frame stays, "
     "headed for example 12-EDO (4:5:6), and the next press brings back the same reading and warps. A chord you "
     "built in JI goes to the nearest rows of the tuning and back. With several chords selected (Ctrl+A over a "
     "progression) each chord is tuned on its own. Also in the right-click menu, as Tune to JI and Back to Tuning."},
    {"double-click the proportion\nabove the frame",
     "Other readings of the chord",
     "A tempered chord can be heard as more than one just chord: C E G B\xE2\x99\xAD as 4:5:6:7, or as 20:25:30:36. The "
     "double-click opens the list (a single click only selects the chord): each reading with how many cents the "
     "notes move in total. Click one, or use Up / Down and Enter; Esc keeps the ticked one. The list also opens by "
     "itself right after tuning, and as Other Readings\xE2\x80\xA6" " in the right-click menu."},
    {"drag the frame's bottom-left\ncorner inward",
     "Warp: play it tempered, then glide into JI",
     "The frame of the selected group has a small arrow on each bottom corner. Drag the left one into the chord: up to "
     "that point the chord plays as you wrote it, then it glides into just intonation. The notes are drawn doing "
     "exactly that, and under the frame the two states are named, for example 12-EDO and 4:5:6."},
    {"drag the frame's bottom-right\ncorner inward",
     "Warp back to the tuning",
     "From that point the chord glides out of just intonation, back to the pitches you wrote."},
    {"drag a warp's diamond\ndrag its round grip",
     "Move a warp, set how long it glides",
     "The diamond on the frame's bottom line is where the glide starts, the round grip where it ends. Put the grip on "
     "the diamond for a sudden switch. Both snap to the grid; hold Shift to place them freely."},
    {"right-click a warp's diamond\nclick it, then {pianoroll.delete}",
     "Shape or remove a warp",
     "The menu offers S-Curve (gentle at both ends, the default), Straight (even speed) and Remove Warp. A diamond "
     "you click is selected: Delete removes it, not the notes."},
    {"right-click inside a frame >\nBack to Tuning Here / Into JI Here",
     "Warp at the pointer",
     "Puts the change at the pointer's time: the next warp moves there, or a new one is made when there is none."},
    {"{pianoroll.stepUp} / {pianoroll.stepDown}\ndrag a note of a group up or down",
     "Next or previous harmonic",
     "A note of a group moves along the group's harmonics instead of the rows: up from the 6 of 4:5:6 is the 7. "
     "Dragged, it snaps to the harmonic nearest the pointer. The frozen note itself moves row by row and takes its "
     "whole group with it; so does the whole chord selected (a click on its header): it moves by steps and stays the "
     "same just chord, and Back to Tuning gives the chord where it now is."},
    {"{pianoroll.walkUp} / {pianoroll.walkDown}",
     "Walk the chord",
     "With a note of a group selected, every number of the group goes up or down by one while the frozen note stays "
     "where it is: 4:5:6 becomes 5:6:7, then 6:7:8. The chord narrows going up and widens going down. Also Walk Up "
     "and Walk Down in the right-click menu."},
    {"{pianoroll.proportionTool}, drag a box,\ntype 8:9:10:12:15, Enter",
     "Type a chord as a proportion",
     "Press the key, drag a box where the chord should sit, type the proportion and press Enter: one note per number, "
     "as long as the box, the lowest on the box's bottom row. With the mouse alone: right-click empty space > Insert "
     "Proportion\xE2\x80\xA6."},
    {"right-click a note of a group >\nIs Harmonic\xE2\x80\xA6",
     "Freeze another note of the group",
     "On a note that already belongs to a group, Is Harmonic N makes THAT note the frozen one, with number N. Every "
     "note keeps its pitch; the numbers are worked out again."},
    {"click a group's header\nclick a note of a group",
     "Select a group; which guide lines show",
     "A click on the header selects all the notes of the group, to move, copy or delete it as one. A clip can hold "
     "several groups: the guide lines and the snapping follow the group of the note you select."},
    {"{pianoroll.copy} / {pianoroll.paste}",
     "Copy a group",
     "Copy the frozen note together with its notes and the paste is a group of its own, tuned the same way. A note "
     "copied without its frozen note is pasted as a fixed pitch."},
    {"{pianoroll.delete}\non the frozen note",
     "Delete the frozen note",
     "The lowest remaining note of the group takes its place: every note keeps its pitch and its number."},
};

// ---- Macro mode (T8.1) -------------------------------------------------------------------------------------
// The keys are the default preset's (assets/macro/trainer.json); Preferences > MIDI > Macro Mode picks another.
constexpr HelpEntry kMacro[] = {
    {"{macro.toggle}\nthe MACRO lamp (top bar)",
     "Turn macro mode on or off",
     "While it is on, the roll has an orange frame and a cursor: a box where the next note will land. The keys below "
     "then belong to macro mode while the note editor has the focus or the mouse pointer (elsewhere they work as "
     "always, and the lamp says MACRO (click the notes)); everything with Ctrl or Alt, and Space, works as always. "
     "The computer MIDI keyboard is switched off while macro mode is on, and back on when it goes off."},
    {"[1] [2] [3] [4] [5] [6] [7] [8] [9] [0]",
     "Go up by a just interval",
     "Multiplies by 2/1, 3/2, 5/4, 7/4, 11/8, 13/8, 17/16, 19/16, 23/16, 29/16: the cursor jumps that far above where "
     "it was and you hear the pitch. Presses add up: 2 then 3 is a fifth plus a third, 15/8. The label beside the "
     "cursor shows what you have built."},
    {"[Q] [W] [E] [R] [T] [Y] [U] [I] [O] [P]",
     "Go down by the same intervals",
     "The key under each number divides by its interval: Q is an octave down, W a fifth down, E a major third down."},
    {"[`]",
     "Place a note",
     "Puts a note at the cursor, as long as the grid step (or as long as the anchor note). The cursor then returns "
     "to the anchor, so the next interval is measured from the same note again: good for stacking a chord."},
    {"[Tab]",
     "Place a note and continue from it",
     "Puts a note at the cursor and makes it the new anchor: the next interval is measured from the note you just "
     "placed. Good for melodies and chains of fifths."},
    {"[Backspace]",
     "Clear, or delete",
     "Throws away the interval you are building and returns the cursor to the anchor. With nothing built, deletes "
     "the note at the cursor."},
    {"[A] / [F]\n[S] / [D]",
     "Move the cursor",
     "A and F move it one grid step earlier or later. S and D move it up or down: by one row in the tempered layer, "
     "to the next harmonic when the note would be just."},
    {"point at a note\nclick a note \xC2\xB7" " click empty space",
     "Choose the anchor",
     "The anchor is the note intervals are measured from; it has a ring on it. With nothing built, the note under "
     "the mouse pointer becomes the anchor (or the note you select), and the cursor goes to its start. A click on "
     "empty space puts the cursor there instead."},
    {"[G]",
     "Switch the layer: tempered or JI",
     "Tempered (the lamp shows the tuning's name): the note goes to the tuning's step for the interval, and the "
     "label shows how far the just interval lies from that step: 3/2 in 12-EDO is 7 steps, +2.0 cents (just is 2 "
     "cents higher). JI: the note is the exact ratio above the anchor, whatever the rows are."},
    {"anchor on a JI note",
     "From a just note, the next note is just",
     "In either layer, when the anchor is a ratio note or a note you declared a harmonic, the new note is an exact "
     "ratio in the same group. So in 12-EDO, 3/2 from an ordinary note lands seven rows up, and 3/2 from a just note "
     "lands exactly a just fifth above it."},
    {"[B] number [Enter]\n[B] [0] [Enter]",
     "Is Harmonic N, or remove the group",
     "Says that the anchor note is harmonic number N of a series (B 5 Enter: the fifth harmonic), the same as Is "
     "Harmonic\xE2\x80\xA6" " in a note's menu. S and D then step the cursor through that series, and notes placed from it "
     "are just. On a note of a JI group it renumbers that group from this note (it becomes the group's anchor); every "
     "note keeps its pitch. B without a number uses the last number. B 0 Enter removes the anchor note's group: its "
     "notes stay where they sound. Esc, or a key that is not a macro key, cancels the number."},
    {"[-] / [=]",
     "Macro undo and redo",
     "Step back and forward through what you did in macro mode: cursor moves, intervals and placed notes. Undo "
     "and redo of the whole program still work as usual."},
    {"labels on the keys",
     "Fewest keys to each row",
     "While macro mode is on in an EDO, each key at the left shows the shortest run of interval keys that reaches "
     "that row from the anchor: 23 means press 2, then 3."},
};

constexpr HelpSection kSections[] = {
    {"Quick start",
     "Just intonation in five moves. Each is described in full in its section below. This window stays open while you "
     "try them in the roll: drag its title to move it, an edge to resize it; \xC3\x97 or ? closes it.",
     kQuickStart},
    {"Notes",
     "Every note is a line along its pitch. The rows are the steps of the clip's tuning; the grid divides the time.",
     kNotes},
    {"Bends between notes",
     "A bend makes a note slide out of, or into, its neighbour instead of jumping. What is drawn is exactly what "
     "sounds.",
     kBends},
    {"Pitch curves",
     "In Draw Mode the pencil also draws on notes that are already there: any pitch movement inside a note. A curve "
     "belongs to its note and travels with it when you move, copy or duplicate it.",
     kCurves},
    {"Vibrato",
     "Vibrato is set on the note itself, by dragging: nothing is typed. It belongs to the note and travels with it; "
     "what is drawn is what sounds.",
     kVibrato},
    {"Detune", "", kDetune},
    {"Harmonics and the virtual fundamental",
     "Just intonation in the roll starts from one note. Say that a note is harmonic number N and the roll works out "
     "the series it belongs to: its fundamental (harmonic 1, usually far below the music, which is why it is called "
     "virtual) and every harmonic above it, drawn as numbered guide lines.",
     kHarmonics},
    {"Tuning",
     "Each clip plays in a tuning. The rows of the roll are that tuning's steps, so the same roll can be 12-EDO, "
     "31-EDO, a MOS scale or a just scale.",
     kTuning},
    {"JI groups",
     "A JI group is a chord of ordinary notes that share one fundamental: a frozen note and the others as exact "
     "ratios to it. The roll draws a dashed frame around the group; its header is the chord as a proportion such as "
     "4:5:6:7, and every note carries its number (the frozen note's has a small diamond). You can write a chord in "
     "the clip's tuning, tune it to JI, switch back, or let it glide from one to the other.",
     kJiGroups},
    {"Macro mode",
     "Enter notes from the computer keyboard by interval, without the mouse: the number row goes up by just "
     "intervals, the row under it goes down, and one key places the note.",
     kMacro},
};

}  // namespace

std::span<const HelpSection> rollHelpSections() noexcept { return kSections; }

}  // namespace pq::views::roll
