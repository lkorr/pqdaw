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
- Known differences: none vs PQ Meter on colour handling (both blend in gamma space on a UNORM target). Rounded rects are geometric rather than SDF (see QUESTIONS Q1). No text yet (T0.8). The glow sprite is a 64×64 atlas texture (`exp(-3r²)·(1-r²)` falloff), not PQ Meter's per-module glow passes; T0.9 ports those.
