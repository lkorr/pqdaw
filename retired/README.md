# retired/

Code that left the app and is kept to read (docs/plans/GU.md "One hierarchy"): it mirrors the paths it had, is **not
built** and is never loaded. Git history keeps everything too. One line per file: what it was, why it left.

GU.10 (2026-11-07): the stock devices became group presets (`pq/document/modules.h`); Auto Filter and EQ Eight wait for
a Filter module.

| File | What it was | Why it left |
|---|---|---|
| `stdlib/devices/auto_filter.pqp` | Auto Filter (T4.11): filter types, circuit models, LFO, envelope follower, sidechain | no Filter module yet; GU.10 |
| `stdlib/devices/auto_filter.panel.json` | Auto Filter's panel description (response display, LFO shape) | no Filter module yet; GU.10 |
| `stdlib/devices/eq_eight.pqp` | EQ Eight (T4.10): eight SVF bands, 1× / 2×, L/R and M/S modes | no Filter module yet; GU.10 |
| `stdlib/devices/eq_eight.panel.json` | EQ Eight's panel description (response with band dots over the spectrum) | no Filter module yet; GU.10 |
| `stdlib/devices/utility.pqp` | Utility (T4.9) as a stock patch | rebuilt as a group preset (`utilityBlocks`); GU.10 |
| `stdlib/devices/utility.panel.json` | Utility's panel description | the preset's panel replaces it; GU.10 |
| `stdlib/devices/saturator.pqp` | Saturator (T4.9): nine curves, colour EQ, HQ oversampling | rebuilt as a group preset (`saturatorBlocks`, seven curves); GU.10 |
| `stdlib/devices/saturator.panel.json` | Saturator's panel description (transfer curve display) | the preset's panel replaces it; GU.10 |
| `stdlib/devices/lfo_device.pqp` | the LFO device (T4.7): audio pass-through plus an `lfo` export | rebuilt as a group preset (`lfoBlocks`: its Out is the LFO); GU.10 |
| `stdlib/devices/lfo_device.panel.json` | the LFO's panel description | the preset's panel replaces it; GU.10 |
| `stdlib/devices/operator.pqp` | Operator (T4.12): four operators, 11 algorithms, waveforms, filter, LFO, pitch envelope | rebuilt smaller as a group preset (`operatorBlocks`); GU.10 |
| `stdlib/devices/operator.panel.json` | Operator's panel description (oscillator rows, envelope, algorithm picker, partials) | the preset's panel replaces it; GU.10 |
| `stdlib/instruments/basic_synth.pqp` | Basic Synth, the M3 default instrument | the default instrument is a group preset (`defaultInstrumentBlocks`); GU.10 |
| `src/views/src/devices/device_displays_eq_af.cpp` | EQ Eight's and Auto Filter's panel displays (cut from `device_displays.cpp`) | their devices left; GU.10 |
| `src/views/src/devices/device_math_eq_af.h` | EQ Eight's and Auto Filter's display math (cut from `device_math.h`) | their devices left; GU.10 |
| `bench/jit/bench_devices.cpp` | `pq_bench_devices`: Utility, Saturator, EQ Eight, Auto Filter vs their C++ references | the stock patches left; GU.10 |
| `tests/stdlib/test_devices.cpp` | Utility, Saturator, EQ Eight vs `device_reference.h` | the stock patches left; GU.10 |
| `tests/stdlib/device_reference.h` | hand-written C++ references of Utility, Saturator, EQ Eight, Auto Filter | the stock patches left; GU.10 |
| `tests/stdlib/test_operator.cpp` | Operator (and msenv) vs `operator_reference.h` | the stock Operator left; GU.10 |
| `tests/stdlib/operator_reference.h` | Operator's hand-written C++ reference | the stock Operator left; GU.10 |
| `tests/stdlib/test_auto_filter.cpp` | Auto Filter vs its reference, analytic responses, modulation | no Filter module yet; GU.10 |
| `tests/document/test_analysis_taps.cpp` | the lowering's analysis taps of EQ Eight and Auto Filter | no analysed device left; GU.10 |
| `tests/app/test_analysis_link.cpp` | EQ Eight's measured response through the link | no analysed device left; GU.10 |
| `tests/views/test_device_panels.cpp` | the stock panel descriptions, their displays vs the DSP references, the device-panel goldens | the stock faces left; GU.10 |
| `tests/views/golden/device-*.png` | goldens of the six stock device faces (Utility, Saturator, EQ Eight, Operator, its filter, Auto Filter) | the stock faces left; GU.10 |

Still live because something else uses them (QUESTIONS Q?-GU.10-a): the stdlib functions the retired devices imported
(`filters/svf*.pqp`, `filters/circuit.pqp`, `util/oversample.pqp`, `envelopes/msenv.pqp`, `dynamics/envfollow.pqp`,
`control/*.pqp`), the oversample atoms, `shaper`, the analysis-tap path of the lowering (`analysedDevice`, now true for
no device) and the panel-description system (`panel_desc.h`; GU.11 retires it). The tests use copies of the retired
patches as stand-in devices (`tests/stand_in_devices`, not shipped).
