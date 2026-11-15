// KB.help: the text of the piano roll's help panel. DATA ONLY: sections -> entries {gesture, title, text}; the widget
// (roll_help.cpp) lays out whatever is here. To change what the panel says, edit this table and nothing else.
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
     "The note starts on the pitch of the note that ends just before it and slides into its own pitch; how far you "
     "drag sets how long the slide takes. With several notes ending there it comes from the one nearest in pitch."},
    {"[Alt] + drag a note's right edge",
     "Bend out",
     "The end of the note slides to the pitch of the note that starts next, over the length you drag. With several "
     "notes selected each one bends to its own neighbour."},
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

// ---- Free pitch curves and vibrato -------------------------------------------------------------------------
constexpr HelpEntry kCurves[] = {
    {"Draw Mode {edit.toggleDrawMode}\ndrag along a note",
     "Draw a pitch curve",
     "The line you draw becomes the pitch of the note from that point on: a scoop, a fall, a slow drift to another "
     "pitch. Start the stroke where the change should begin."},
    {"Draw Mode {edit.toggleDrawMode}\n[Ctrl] + click on a note",
     "Add a breakpoint",
     "Puts one point on the note's pitch line at the pointer; keep the button down to drag it into place. Use it to "
     "build a bend from a few exact points instead of a freehand line."},
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
    {"Draw Mode {edit.toggleDrawMode}\n[Alt] + drag along a note",
     "Paint vibrato",
     "Vibrato over the stretch you drag. How far the stroke moves up or down sets how wide it swings; a flat stroke "
     "uses the width of the last one. Its speed follows the grid (one swing per cell, kept between about 4 and 8 per "
     "second)."},
    {"right-click a vibrato >\nEdit Vibrato\xE2\x80\xA6",
     "Edit a vibrato",
     "Type its width in cents (the swing each way) and its speed in Hz, for example 30 6. A width of 0 removes it."},
    {"right-click a selected note >\nAdd Vibrato\xE2\x80\xA6",
     "Add vibrato by typing",
     "Gives the selected notes a vibrato of the width and speed you type."},
    {"right-click a selected note >\nClear Bends",
     "Clear Bends",
     "Removes every bend, drawn curve and vibrato from the selected notes: they are plain steady notes again."},
};

// ---- Detune ----------------------------------------------------------------------------------------------
constexpr HelpEntry kDetune[] = {
    {"[Shift] + drag a note up or down",
     "Free detune",
     "Moves the selected notes smoothly in pitch instead of row by row, for a note that should be a few cents sharp "
     "or flat. They become cents notes: fixed pitches that no longer follow the tuning. In time they still snap to "
     "the grid. (Shift + click without dragging only selects.)"},
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
     "above it are its just major third, fifth and harmonic seventh."},
    {"draw \xC2\xB7" " double-click",
     "Notes snap to the harmonics",
     "While a note is declared, the notes you add land on the nearest guide line instead of a row. Each is kept as an "
     "exact ratio to the declared note, so a chord built this way is perfectly in tune with it."},
    {"[Shift] + draw",
     "Draw on the rows instead",
     "With a note declared, hold Shift to put an ordinary note on a row of the tuning. With no note declared it is the "
     "other way round: Shift draws a just note, at the simplest ratio near the pointer."},
    {"the numbers at the\nroll's right edge",
     "How the numbers renumber",
     "The numbers are always the smallest whole numbers that fit every note. When a note falls between two harmonics "
     "the whole series is renumbered so that it gets a whole number too. Declare a note 5 and add a note a perfect "
     "fifth (3/2) above it: 7.5 is not a harmonic, so both double and now read 10 and 15. No note changes pitch; only "
     "the numbers and the lines do."},
    {"Harmonics: JI\n(the box left of the roll)",
     "Show or hide the guide lines",
     "The JI switch appears once the clip has a harmonic series. It shows or hides the guide lines and their "
     "numbers."},
    {"right-click > Remove JI Group",
     "Remove the declaration",
     "Forgets which note was declared. Every note keeps its pitch (the notes that were tied to it become fixed "
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
     "draws a note on its nearest line, as a fixed cents pitch."},
};

// ---- JI groups -----------------------------------------------------------------------------------------------
// SEPARATE BLOCK (KB.help). T7.13 removed the box-object JI (the JI tool, entering a chord, stops, Tune to JI, walking
// a term); what is left is what a group can do today. T7.14 (the group tools: Tune to JI and its chooser, warps,
// walk, typed proportions, Add Note at Ratio) rewrites kJiGroups and its intro in kSections, and touches nothing else.
constexpr HelpEntry kJiGroups[] = {
    {"click a note of a group",
     "Which group the guide lines show",
     "A clip can hold several groups. The guide lines and the snapping follow the group of the note you select; "
     "with nothing selected they stay on the group you used last."},
    {"{pianoroll.stepUp} / {pianoroll.stepDown}\non a note of a group",
     "Next or previous harmonic",
     "A note that sits on a guide line moves to the next harmonic of its group instead of the next row. The declared "
     "note itself moves row by row and takes its whole group with it."},
    {"{pianoroll.copy} / {pianoroll.paste}",
     "Copy a group",
     "Copy the declared note together with its notes and the paste is a group of its own, tuned the same way. A note "
     "copied without its declared note is pasted as a fixed pitch."},
    {"{pianoroll.delete}\non the declared note",
     "Delete the declared note",
     "The lowest remaining note of the group takes its place: every note keeps its pitch and its number."},
};

constexpr HelpSection kSections[] = {
    {"Notes",
     "Every note is a line along its pitch. The rows are the steps of the clip's tuning; the grid divides the time.",
     kNotes},
    {"Bends between notes",
     "A bend makes a note slide out of, or into, its neighbour instead of jumping. What is drawn is exactly what "
     "sounds.",
     kBends},
    {"Free pitch curves and vibrato",
     "In Draw Mode the pencil also draws on notes that are already there: any pitch movement inside a note. A curve "
     "belongs to its note and travels with it when you move, copy or duplicate it.",
     kCurves},
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
    // The separate block: see kJiGroups.
    {"JI groups",
     "A JI group is a declared note (see Harmonics) and the notes tied to it as exact ratios: ordinary notes that are "
     "in tune with each other. A note's number is its harmonic in the group, so a group reads as a proportion such as "
     "4:5:6:7.",
     kJiGroups},
};

}  // namespace

std::span<const HelpSection> rollHelpSections() noexcept { return kSections; }

}  // namespace pq::views::roll
