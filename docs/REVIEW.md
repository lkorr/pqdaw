# Waiting for user review

Visual/UX work (👁 tasks) and anything else the user should look at. Use this format:

```
## <task> · <date>
- What to look at: <command to run, e.g. `build/release/src/app/pqdaw.exe --screenshot lines out.png`, or a path to a committed PNG>
- What was matched against: <PQ Meter file / Ableton reference / spec section>
- Known differences: …
```

## T0.7 Window + Vulkan renderer · 2026-10-20
- What to look at:
  - The committed golden: `tests/ui/golden/test-pattern.png` (800×480). Or regenerate it at any size: `build/release/src/app/pqdaw.exe --screenshot test-pattern out.png --size 1280x768`.
  - The live window: `build/release/src/app/pqdaw.exe` (static test pattern; idle = no redraws). `--animate`, or press `A`, drifts the glow blobs so you can see paced continuous redraw. Drag-resize the window (it should redraw live during the drag), minimise/restore, `Esc` quits.
  - Rows, top to bottom: header strip + α14 grid; pixel-aligned band-colour swatches, alpha-blend overlaps, an 8-step white alpha ramp; AA rounded rects at fractional positions (radius 0, 2, 4, 8, 16, pill); gradients (horizontal, vertical fade on a rounded rect, 4-corner); textured quads (Magma ramp, 8×8 checker nearest / linear / tinted); a clipped panel of additive glow blobs, heat dots, and an alpha overlay drawn after the additive pass.
- What was matched against: PQ Meter's theme tokens (`src/ui/theme.h`: backgrounds, accent, violet, waveform colours, band colours, Magma ramp) and its blending (straight alpha; additive = `SRC_ALPHA, ONE`, as in `modules/face.cpp`). AA is ImGui/PQ Meter-style geometric feathering (1 px ramp centred on the edge).
- Known differences: none vs PQ Meter on colour handling (both blend in gamma space on a UNORM target). Rounded rects are geometric rather than SDF (see QUESTIONS Q11). No text yet (T0.8). The glow sprite is a 64×64 atlas texture (`exp(-3r²)·(1-r²)` falloff), not PQ Meter's per-module glow passes; T0.9 ports those.

## T0.8 Text · 2026-10-20
- What to look at: the committed golden `tests/ui/golden/text.png` (800×480), or `build/release/src/app/pqdaw.exe --screenshot text out.png`; live: `build/release/src/app/pqdaw.exe --scene text` (`--animate` or key `A` animates the two meter readouts, to see tabular figures hold still). Rows: header (SemiBold 14 + right-aligned 12 px); the size ladder (Regular 12/13/14, SemiBold 13/14); 24 px SemiBold tabular readouts; kerning and Inter's contextual alternates (`->` becomes an arrow); tuning strings (¢, ·, √, ≈, ±, arrows); the same line with full vs light hinting; band-colour text and the grid-label token (white α70) on the panel; dark-on-light; translucent text over a gradient; a label clipped to its box.
- What was matched against: SPEC §11 / §15 (hinted bitmap glyphs at integer sizes 12/13/14, no MSDF; Inter); PQ Meter `src/ui/settings_theme.h` for the sizes and text colours (226,230,240 / 134,140,158).
- Known differences / decisions to judge: (1) glyphs are hinted with Inter's own TrueType hints (FreeType v40 = vertical-only in practice) and positioned with HarfBuzz's linear advances, each glyph origin rounded to a whole pixel. Hinted per-glyph advances were tried first and looked worse (uneven letter/word spacing: "foxjumps", "t he" at SemiBold 13). (2) Grayscale AA, no LCD subpixel AA, no gamma tweak on coverage. (3) The "light" hinting row is there to compare; full is the default. (4) Inter lacks ♯ ♭ ♮ (QUESTIONS Q16). PQ Meter itself draws its UI with ImGui's ProggyClean bitmap font; that stays for signal readouts per SPEC §15 and is not part of this task.

## T0.9 PQ Meter line primitives · 2026-10-20
- What to look at: the committed golden `tests/ui/golden/lines.png` (800×480), or `build/release/src/app/pqdaw.exe --screenshot lines out.png --size 1600x960` (positions scale; line widths stay in pixels); live: `build/release/src/app/pqdaw.exe --scene lines --animate` (traces, spectrum and the palette-track position move). Panels: (top-left) scope sine at PQ Meter's defaults (1.4 px, glow 0.35) through `splinePath`, plus an orange sine from one control point per 24 px (dots) whose smoothness is all Catmull-Rom, with additive glow; (top-right) a dense noise burst as a min/max envelope band (`scopeEnvelope`) and a Neon-coloured zig-zag whose spikes are bevelled (`smoothPolylineCols`, turns > 120°); (middle) a noise spectrum with PQ Meter's layering (gradient fill, 3-pass level-weighted bloom, body, bright core) through a custom Ice gradient, and a `splineSmooth`ed peak-hold line; (bottom) the ten colormaps and a PaletteTrack strip blending Magma → Ice → Viridis.
- What was matched against: PQ Meter `src/ui/smooth_line.h`, `src/ui/gradient.h`, `src/ui/palette_track.h`, `src/ui/theme.h`, `src/modules/spectrum.cpp` 200–325 and 1340–1405, `src/modules/oscilloscope.cpp` 860–892. Same constants (0.75 px feather, 2.5× miter clamp, bevel past 120°, ≤ 24 steps per segment, ≤ 6000 spline points, bloom 7×/4×/2.2× at α .07/.11/.17, body 1.9×, core 0.85× brightened by 0.30 + 0.45·level).
- Known differences: the signals are synthetic (no analyser yet), so this checks the drawing, not PQ Meter's DSP/ballistics. Glow blend: QUESTIONS Q17. PQ Meter's scope draws its glow passes from i = 3 down, and the i = 3 pass has alpha 0; the port skips that invisible pass.
