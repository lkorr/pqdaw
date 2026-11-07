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
`control/*.pqp`), the oversample atoms, `shaper`.

GU.11 (2026-11-07): one hierarchy — a block is a group, an atom block, a value block or a native engine; no stock patch,
no patch text of a device's own, one panel system (a group's panel; a leaf's Device-view frame is generated).

| File | What it was | Why it left |
|---|---|---|
| `src/views/include/pq/views/devices/panel_desc.h`, `src/views/src/devices/panel_desc.cpp` | T4.6 panel descriptions: the `.panel.json` reader (selectors, conditions, repeats, visuals) and the generated panels | the stock devices' panels left; a leaf's frame is `panel_layout.h` now; GU.11 |
| `src/views/include/pq/views/devices/panel_face.h`, `src/views/src/devices/panel_face.cpp` | TX.panel-faces: a stock device drawn in the circuit as its Device-view panel | no stock device; a group is drawn as its own panel; GU.11 |
| `src/views/src/devices/device_displays.cpp`, `device_math.h` | the panel descriptions' displays (Saturator's transfer curve, the spectrum display) and their math | their panels left; GU.11 |
| `src/views/src/devices/operator_displays.cpp` | the stock Operator's envelope, algorithm picker and partial editor | the stock Operator left (GU.10); GU.11 |
| `src/views/include/pq/views/devices/device_view.h`, `src/views/src/devices/device_view.cpp` | T4.6 stand-alone device preview (View > Developer > Preview Device File) | it previewed stock device files; GU.11 |
| `src/views/include/pq/views/circuit/device_circuit.h`, `src/views/src/circuit/device_circuit.cpp` | T4.4-fix1 the circuit editor on a device of the project (fork on edit, Revert to Stock) | a block has no patch text of its own: opening a block with an inside opens its group; GU.11 |
| `bench/views/bench_device_panels.cpp` | `pq_bench_device_panels`: the stock panels' paints and drags | the stock panels left; GU.11 |

Deleted outright (git history keeps them): the user devices (`Device::source`, `.pqdevice` files, `DeviceLibrary::resolve`),
`Device::chainInput`, the stock device list and its `PQ_STAND_IN_DEVICES` test hook with `tests/stand_in_devices`, the
analysis tap of analysed stock devices, the app's `DevicePlayer`, and the tests of those (test_user_devices, the
schema-migration and all-free fixture renders, the GU.1 curve renders of stock devices).
