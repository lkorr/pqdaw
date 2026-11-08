# Toolchain smoke tests: prove the compiler, CRT, and LLVM ORC JIT link and run.
pq_add_test(pq_tests_smoke
  SOURCES smoke/test_toolchain.cpp smoke/test_llvm_jit.cpp smoke/test_cpu_baseline.cpp
  DEPS pq_core pq_llvm)

# Module test suites are added below as modules appear, e.g.
#   pq_add_test(pq_tests_core SOURCES core/test_spsc_ring.cpp DEPS pq_core)

pq_add_test(pq_tests_core
  SOURCES
    core/test_result.cpp
    core/test_alloc_trap.cpp
    core/test_aligned_buffer.cpp
    core/test_spsc_ring.cpp
    core/test_mpsc_queue.cpp
    core/test_fixed_pool.cpp
    core/test_arena.cpp
    core/test_epoch_reclaimer.cpp
    core/test_denormal_guard.cpp
    core/test_fast_math.cpp
    core/test_time.cpp
    core/test_clip_time.cpp
    core/test_grid.cpp
    core/test_math_time_rt.cpp
    core/test_stable_id.cpp
    core/test_rt_log.cpp
    core/test_tap_ring.cpp
    core/test_voice_tap.cpp  # TMM.2: voice taps (records, stride sampling, lossy reads, the reader)
  DEPS pq_core)

pq_add_test(pq_tests_tuning
  SOURCES
    tuning/test_rational.cpp
    tuning/test_monzo.cpp
    tuning/test_edo.cpp
    tuning/test_resolve.cpp
    tuning/test_mos.cpp
    tuning/test_mos_fixtures.cpp
    tuning/test_tuning_rt.cpp
    tuning/test_scala.cpp
    tuning/test_scale_tuning.cpp
    tuning/test_ji.cpp
    tuning/test_ji_fixtures.cpp
  DEPS pq_tuning nlohmann_json::nlohmann_json)

# media (T5.1, T5.2): probing, decoding (bit-exact lossless, lossy SNR, random seeks), malformed files (+ a `slow`
# mutation fuzz), content hashes, the sample pool and RT-safe disk streaming (RtScope, two-thread stress, the
# 64-stream realtime run in `perf`). Fixtures: tests/media/fixtures (tools/fixtures/make_audio_fixtures.py).
pq_add_test(pq_tests_media PERF
  SOURCES
    media/test_probe.cpp
    media/test_decode.cpp
    media/test_malformed.cpp
    media/test_stream.cpp
    media/test_waveform.cpp
  DEPS pq_media pq_engine pq_doc)  # pq_doc: the content hash is checked against doc::fileIdentity

# Child process for the "default handler aborts with a stack trace" test: links the trap, no handler.
add_executable(pq_alloc_trap_abort_probe core/alloc_trap_abort_probe.cpp)
target_link_libraries(pq_alloc_trap_abort_probe PRIVATE pq_alloc_trap)
pq_target_defaults(pq_alloc_trap_abort_probe)
add_dependencies(pq_tests_core pq_alloc_trap_abort_probe)
target_compile_definitions(pq_tests_core PRIVATE PQ_ALLOC_TRAP_PROBE=pq_build_alloc_trap_probe)
include(${CMAKE_CURRENT_SOURCE_DIR}/core/queue_mutation.cmake)
# ui: draw list, PNG, damage, text, line primitives, colormaps (CPU) and the offscreen Vulkan path incl. the
# test-pattern / text / lines goldens (GPU). PQ_ASSETS_DIR: the bundled fonts.
pq_add_test(pq_tests_ui PERF
  SOURCES
    ui/test_draw_list.cpp
    ui/test_primitives.cpp
    ui/test_image.cpp
    ui/test_render.cpp
    ui/test_text.cpp
    ui/test_lines.cpp
    ui/test_colormap.cpp
    ui/test_layout.cpp
    ui/test_widgets.cpp
    ui/test_hidpi.cpp
    ui/test_controls.cpp
    ui/test_control_affordances.cpp  # T4.16a: cursors, wheel, double-click default, typed numbers
    ui/test_partial_redraw.cpp
    ui/test_tile_atlas.cpp
    ui/test_pipeline_cache.cpp
    ui/test_waveform.cpp  # T5.5: drawWaveform, the waveform-lod golden
    ui/test_drag.cpp  # T5.7: drag sessions (internal, external, ghost, auto-scroll)
    ui/thread_cpu.cpp
  DEPS pq_ui pq_ui_scenes)
target_compile_definitions(pq_tests_ui PRIVATE PQ_ASSETS_DIR=pq_build_source_assets_dir)

# app: CLI parsing (window, --screenshot, --list-devices, --sine, --play), the --screenshot command, the --play compile path.
pq_add_test(pq_tests_app
  SOURCES
    app/test_cli.cpp
    app/test_play.cpp
    app/test_shell_app.cpp
    app/test_startup.cpp
    app/test_patch_preview.cpp
    app/test_probe_preview.cpp  # T4.5: probes and cost meters through the editor and the preview
    app/test_os_drop.cpp  # T5.7: the OLE drop target (CF_HDROP data object, effects, registration)
    app/test_os_drag.cpp  # T5.12: the OLE drag source's data object (CF_HDROP, lazy files), the drag-out temp folder
  DEPS pq_app pq_ui)

# T3.17: project lowering and the playback link (golden render of tests/app/fixtures/playback, compile-count hooks,
# solo/mute vs effectiveAudible, topology swaps, the NullDevice end-to-end playhead); T2.6-fix1: the tuning signal in
# playback (tests/app/fixtures/tuning_automation, the bit-identical static render, live tuning params); T1b.4: the AOT
# seed and the disk cache under ProjectLink (a stock track compiles nothing; a second load of a project neither).
pq_add_test(pq_tests_playback PERF SHARDS 4  # T5.12: the 60-s clip render (perf)
  SOURCES
    app/test_playback.cpp
    app/test_playback_link.cpp
    app/test_playback_ir.cpp
    app/test_playback_semantics.cpp
    app/test_tuning_playback.cpp
    app/test_live_jit.cpp
    app/test_tuning_coverage.cpp
    app/test_jit_aot.cpp
    app/test_pan_laws.cpp  # TX.pan-laws: every pan law through the lowered mixer; a law change compiles its track only
    app/test_device_chains.cpp  # T4.1: device chains through the link (migration bit-identity, unit compiles, hand-built graph)
    app/test_wires.cpp  # T4.7: modulation wires (audible LFO wire, hand-built graph diff 0, interpreter == JIT, loops, taps)
    app/test_patching.cpp  # TX.patching: wires add in units, any output into any param / input at audio rate, free devices
    app/test_groups.cpp  # TX.groups: Ctrl+G / Ctrl+Shift+G round trips (series = group bit for bit, interpreter == JIT, same document)
    app/test_pass_through.cpp  # TX.scope-sound: an analyser (any pass-through) never changes the sound: chain, group, free
    app/test_free_instruments.cpp  # TX.all-free-fix1: instruments / racks anywhere play by their cables; cross-track mixer cables
    app/test_panel_link.cpp  # GU.3: a panel's edits through the link (a setting drag compiles once, a control / curve point never, a shape once)
    app/test_nested_groups.cpp  # GU.2: nested groups / atom + value blocks / pasted block sets render as their references
    app/test_instrument_groups.cpp  # IG.1-2: per note inside instrument groups (per-voice references), reducers, layering, cross-track, single-signal cables
    app/test_cross_track.cpp  # TX.cross-track: loops between tracks per sample (hand-written references), notes cables across tracks
    app/test_mod_all.cpp  # TX.mod-all: a wire into any param makes it live at audio rate (repro, the sweep over every device)
    app/test_module_presets.cpp  # GU.4: the Phase Accumulator / VCA presets render as the meta modules they replace; load conversion
    app/test_wavetable_synth.cpp  # GU.8: the Wavetable Synth spawned at the notes; a chord plays, interpreter == JIT
    app/test_engine_modules.cpp  # GU.7: the Wavetable / Envelope modules render as their engines; the orbit's lowering tap
    app/test_stock_presets.cpp  # GU.10: the stock devices and the default instrument as group presets (renders vs the retired patches, interp == JIT)
    app/test_eq_render.cpp  # EQ.3: a band == a plain svf bit for bit per type; the EQ's L/R and M/S == Stereo with equal chains; chain B costs nothing in Stereo
    app/test_project_patch_cost.cpp  # T4.13: per-device CPU cost through the link (meters per unit schedule, attribution)
    app/test_inspect_lifetime.cpp  # T4.x-fix-safety: cost meters and probe taps live as long as the images that write them
    app/test_racks.cpp  # GU.9: racks are parallel groups (renders vs the T4.8 racks' references, interpreter == JIT)
    app/test_audio_tracks.cpp  # T5.3: audio tracks (golden lowering text, clip slot, clip edits compile nothing)
    app/test_audio_playback.cpp  # T5.4: audio clips through the link (golden audio-clips, streaming, idle units, edits)
    app/test_drop_playback.cpp  # T5.7: a dropped file plays at once (the hover preloads it into the pool)
    app/test_clip_export.cpp  # T5.12: clips rendered for drag-out (== the solo render, WAV files; 60-s clip in perf)
    app/test_device_automation.cpp  # T4.18: device / macro / chain / wire lanes reach the DSP, click-free, idle when stopped
    app/test_meta_link.cpp  # TMM.3: meta modules through the link (a nested freq drag compiles nothing; chain instrument)
    app/test_scope_link.cpp  # TX.scope: the Oscilloscope through the link (its tap: beat, note, signal; pass-through)
    app/test_spectrogram_link.cpp  # TX.spectrogram: the Spectrogram through the link (its tap, RtScope, a feed's columns, pass-through)
    app/test_meta_wavetable.cpp  # TMM.5: the Wavetable through the link (off-thread tables, click-free, alias suite, golden; taps → face dots)
    golden/golden_audio.cpp
  DEPS pq_app)

# T3.18: the audio device manager (settings file, startup settings, device switching / hot-plug / ASIO driver resets
# over scripted devices, test tone, CPU meter). Real devices only with PQ_TEST_AUDIO_DEVICE=1.
pq_add_test(pq_tests_audio PERF  # T5.10: the cold preview start (perf)
  SOURCES
    app/test_audio_manager.cpp
    app/test_preview_service.cpp  # T5.10: the browser preview through AudioOutput's cue source, Cue Out, library settings
  DEPS pq_app)

# views (T3.7): ViewContext (project I/O, undo, transport routing), the shell (layout, hotkeys, control bar), goldens.
pq_add_test(pq_tests_views PERF  # T4.13: the project patch view's wall-clock checks (64 tracks: paint, morph)
  SOURCES
    views/test_view_context.cpp
    views/test_spectrum_display.cpp  # TX.spectrum: the analysis feed, the spectrum plot, the Spectrum face, scenes, 8-display frame
    views/test_scope_display.cpp  # TX.scope: the feed's scope history, the Oscilloscope face, scenes, 4-scope frame
    views/test_spectrogram_display.cpp  # TX.spectrogram: the feed's columns, the Spectrogram face, its ring texture, scenes, 4-spectrogram frame
    views/test_bug_report.cpp  # T5.x-bugreport: report.md round trip, the writer, the input ring, the pointer crop
    views/test_samples_ui.cpp  # T5.8: missing samples, relink, Collect All and Save through the ViewContext
    views/test_waveforms.cpp  # T5.5: the views' waveform overviews (sync / worker + poster, caches, windows)
    views/test_shell.cpp
    views/test_shell_affordances.cpp  # T4.16a: cursors of the shell, headers, mixer, tunings, prefs, MOS wheel; wheel; typed tempo
    views/test_arrangement.cpp
    views/test_arrangement_editing.cpp
    views/test_arrangement_tiles.cpp
    views/test_arrangement_audio.cpp  # T5.6: audio clips in the arrangement, the audio clip view, goldens, perf
    views/test_piano_roll.cpp
    views/test_piano_roll_scale.cpp
    views/test_mos_chart.cpp
    views/test_mixer.cpp
    views/test_mos_wheel.cpp
    views/test_automation.cpp
    views/test_audio_prefs.cpp
    views/test_look.cpp
    views/test_tunings_ui.cpp
    views/test_circuit.cpp
    views/test_patch_editor.cpp
    views/test_circuit_editor.cpp
    views/test_circuit_probes.cpp  # T4.5: probes and cost meters in the circuit editor
    views/test_device_chain.cpp  # T4.1: the Device view (chain edits, generic panels, the panel hook, golden)
    views/test_wiring.cpp  # T4.7: cables (jack → knob drags, across tracks, scrolling, badge, rings), golden
    views/test_project_patch.cpp  # T4.13: the project patch view (graph match, layout, edits, morph, zoom), goldens
    views/test_lane_edit.cpp  # TX.lane-edit: the lane windows as the arrangement's editing surface (gestures, keys, automation), golden, perf
    views/test_nested_groups_view.cpp  # GU.2: capsules in capsules, the instrument group's look, nest / unnest, copy / paste, group presets from the node menu (real dispatch), golden
    views/test_group_panels.cpp  # GU.3: element goldens, a group drawn as its panel (circuit, Device view + inspector), every element edit through real dispatch
    views/test_module_faces.cpp  # GU.5b: the modules' old faces from rich elements (dial markers as curve points), an opened group as a circuit, goldens
    views/test_stock_panels.cpp  # GU.10: the stock presets closed as their panels (goldens)
    views/test_engine_elements.cpp  # GU.7: the engines' faces as rich elements, the modules' panels, harmonic / envelope drags
    views/test_panel_window.cpp  # GU.5: the panel window (open / move / resize / minimise), send to panel (button, P, drag), the editor and inspector, mod-source cables, link lines, goldens
    views/test_panel_embed.cpp  # GU.6: a child group embedded on its parent's panel (send, play, rename, move out), break apart, any depth, golden
    views/test_wavetable_synth_panel.cpp  # GU.8: the Wavetable Synth panel on each tab (goldens), a tab click in the patch view
    views/test_groups_view.cpp  # TX.groups: Ctrl+G / Ctrl+Shift+G in the circuit, capsules, the Device view group panel, goldens
    views/test_node_menu.cpp  # TX.node-menu: catalog, search, the menu, spawn / insert / Z / splice / dissolve / Ctrl-release / push-apart, golden, perf
    views/test_cables.cpp  # TX.cables: hanging cables (rest shape, rope sim, drag swing, hit tests on the curve, glow, goldens)
    views/test_orbit_ports.cpp  # TX.orbit-ports: orbiting main ports, the round VCA's floating panel, Delete on a hovered cable
    views/test_free_rack.cpp  # TX.free-rack: the Device view's free-device rack (gestures, prefs), the signal-path cables, goldens, perf
    views/test_all_free_views.cpp  # TX.all-free: the Device view lists the main path; its gestures rewire; the patch view places anywhere
    views/test_racks_view.cpp  # GU.9: racks (parallel groups) in the Device view: chains, mixer, macros, Map, zones (real dispatch), goldens
    views/test_circuit_legibility.cpp  # T4.15: node tooltips, inactive branches, recognised constants, goldens
    views/test_device_affordances.cpp  # T4.16c: Device view keys (arrows, Shift, Ctrl, Home / End, chains), cursors, rename
    views/test_eq_displays.cpp  # EQ.4: the response / phase displays and the EQ panel (curve = GroupResponse, real-dispatch gestures, goldens, perf)
    views/test_panel_gestures.cpp  # GU.13: one panel system: a leaf's and a group's knob take the same gestures (wheel, type-in, menu, map mode) through real dispatch
    views/test_affordances.cpp  # T4.16b: cursors, hover highlights and editing keys (arrangement, piano roll, automation)
    views/test_file_drop.cpp  # T5.7: files / browser items dropped on the arrangement (rules, ghosts, one undo step, 100 files)
    views/test_browser.cpp  # T5.10: the browser panel (tree, search, keys, preview strip, drag out, goldens, 200k rows)
    views/test_user_library.cpp  # T5.11 / T5.12: user library, hot-swap, chips, width, clips dragged to the browser
    views/test_device_automation.cpp  # T4.18: device lanes (chooser sections, Show Automation, automated knobs, golden)
    views/test_meta_faces.cpp  # TMM.4: meta-module faces (toolkit, hosting at every zoom, drags, expand, editor, goldens)
    views/test_meta_wavetable_face.cpp  # TMM.7: the Wavetable face (display model, goldens, voices, gestures, paint cost)
    views/test_meta_envelope_face.cpp  # TMM.11: the Envelope face (goldens with voices, gestures, scenes, paint cost)
    views/test_meta_wavetable_edit.cpp  # TMM.9: its harmonic editor and draw tools (math, gestures, goldens, paint cost)
    views/test_meta_devices.cpp  # TMM.10: chain meta devices in the Device view (face panels, menus, presets, goldens)
  DEPS pq_views nlohmann_json::nlohmann_json)
# T3.7: the shell window idling over the empty fixture: 0 frames in 10 s at < 0.1 % CPU (exit 77 = no display).
add_test(NAME pqdaw_idle_shell
  COMMAND pqdaw --project ${PQ_SOURCE_DIR}/tests/views/fixtures/empty --idle-check 10)
# T0.x-flake: up to 30 s of settling and 3 measurement windows when outside events disturb one.
# T3.18: its own (empty) settings directory, so the user's saved audio device (an ASIO driver, say) is not opened.
set_tests_properties(pqdaw_idle_shell PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
  ENVIRONMENT PQ_SETTINGS_DIR=${CMAKE_CURRENT_BINARY_DIR}/settings_idle_shell)
add_test(NAME pqdaw_screenshot_shell
  COMMAND pqdaw --screenshot shell ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_shell.png --project ${PQ_SOURCE_DIR}/tests/views/fixtures/empty)
# T3.8: the arrangement scene through the real executable.
add_test(NAME pqdaw_screenshot_arrangement
  COMMAND pqdaw --screenshot arrangement-basic ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_arrangement.png --project ${PQ_SOURCE_DIR}/tests/views/fixtures/arrangement)
# T3.11: a piano-roll view scene through the executable's --screenshot path.
add_test(NAME pqdaw_screenshot_pianoroll
  COMMAND pqdaw --screenshot pianoroll-31edo ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_pianoroll.png --project ${PQ_SOURCE_DIR}/tests/views/fixtures/pianoroll-31edo)

# The real executable: headless screenshot, and the scripted window resize/minimise/restore run
# (exit 77 = no display available → reported as skipped, not passed).
add_test(NAME pqdaw_screenshot
  COMMAND pqdaw --screenshot test-pattern ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot.png --size 400x240)
add_test(NAME pqdaw_screenshot_text   # the executable finds the bundled fonts (pq::ui::assetsDir)
  COMMAND pqdaw --screenshot text ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_text.png)
# T0.1-fix1: the startup CPU baseline check on an emulated CPU without AVX2 (and without FMA): message, exit 1.
add_test(NAME pqdaw_cpu_check_no_avx2 COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:pqdaw> -DDISABLE=avx2
  -DEXPECT=AVX2 -P ${CMAKE_CURRENT_SOURCE_DIR}/app/cpu_baseline_check.cmake)
add_test(NAME pqdaw_cpu_check_no_avx2_fma COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:pqdaw> -DDISABLE=fma,avx2
  "-DEXPECT=AVX2, FMA" -P ${CMAKE_CURRENT_SOURCE_DIR}/app/cpu_baseline_check.cmake)
add_test(NAME pqdaw_window_selftest COMMAND pqdaw --window-selftest)
set_tests_properties(pqdaw_window_selftest PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
# Same run with the live widget tree (T3.1): SDL window loop → UiTree layout/paint at every size.
add_test(NAME pqdaw_window_selftest_widgets COMMAND pqdaw --window-selftest --scene widgets-core)
set_tests_properties(pqdaw_window_selftest_widgets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
# T3.2: the control gallery live (themed controls, overlays, SDL text input and clipboard) through the same run.
add_test(NAME pqdaw_window_selftest_gallery COMMAND pqdaw --window-selftest --scene widgets)
set_tests_properties(pqdaw_window_selftest_gallery PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_test(NAME pqdaw_screenshot_widgets COMMAND pqdaw --screenshot widgets ${CMAKE_CURRENT_BINARY_DIR}/pqdaw_screenshot_widgets.png)
# T0.7-fix1: partial redraw through the swapchain (2 px playhead over `lines`: pixels shaded, CPU vs full).
add_test(NAME pqdaw_window_selftest_partial COMMAND pqdaw --window-selftest --scene lines-playhead)
set_tests_properties(pqdaw_window_selftest_partial PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)

pq_add_test(pq_tests_graph PERF
  SOURCES
    graph/test_types.cpp
    graph/test_atoms.cpp
    graph/test_patch_build.cpp
    graph/test_validate.cpp
    graph/test_patch_equal.cpp
    graph/test_id_match.cpp
    graph/test_patch_file.cpp
    graph/test_parse_roundtrip.cpp
    graph/test_parse_errors.cpp
    graph/test_parse_fuzz.cpp
    graph/test_kernel_map.cpp
    graph/test_infer.cpp
    graph/test_optimise.cpp
    graph/test_schedule.cpp
    graph/test_sleep_plan.cpp
    graph/test_voice_tuning.cpp
    graph/test_layout.cpp
    graph/test_edit.cpp
    graph/test_probe.cpp
    graph/test_structure.cpp  # T4.15: structural constants, switched-off branches
    graph/test_oversample.cpp  # TMM.6: the oversample(n) region: text, checks, converters, schedule
  DEPS pq_graph)

# engine: block adapter, NullDevice (cadence, buffer sizes, RtScope on the callback path), RtAudio backend checks,
# WASAPI backend (conversion, underrun tracker, opt-in device runs with PQ_TEST_AUDIO_DEVICE=1), the ASIO host
# (fake driver; opt-in real drivers), the interpreter
# backend vs the reference evaluator (T1.7).
pq_add_test(pq_tests_engine PERF
  SOURCES
    engine/test_block_adapter.cpp
    engine/test_null_device.cpp
    engine/test_rtaudio_device.cpp
    engine/test_wasapi.cpp
    engine/test_asio.cpp
    engine/test_rt_log_device.cpp
    engine/test_interpreter.cpp
    engine/test_newton.cpp
    engine/test_voices.cpp
    engine/test_hot_swap.cpp
    engine/test_text_edits.cpp
    engine/test_offline_render.cpp
    engine/test_tuning_signal.cpp
    engine/test_tuning_voices.cpp
    engine/test_silence.cpp
    engine/test_probes.cpp  # T4.5: live probes and cost meters on images
    engine/test_linear_response.cpp  # EQ.2: analytic responses vs interpreted impulse responses
  DEPS pq_engine nlohmann_json::nlohmann_json)

# T1.x-arch1: per-track compile units (partition, composition, the project runner, per-unit hot swap).
pq_add_test(pq_tests_project
  SOURCES
    engine/test_project.cpp
    engine/test_project_silence.cpp  # T1.x-silence: dormant units, silent buses, the engine's gate waits for tails
  DEPS pq_engine)

# T3.16: the engine timeline (transport, note sequencer, automation renderer), with the VoiceEngine and renderOffline.
pq_add_test(pq_tests_timeline
  SOURCES
    engine/test_timeline.cpp
    engine/test_clip_player.cpp  # T5.4: the clip player, the audio sequence image, streamed reads
    engine/test_preview_player.cpp  # T5.10: the browser preview player (cue bus)
  DEPS pq_engine pq_doc)

# T1.10: golden offline renders (tests/golden: patches, committed WAVs, the absolute + spectral comparison helper) on
# both backends, and the pqc CLI (in-process, plus the real pqc.exe once).
pq_add_test(pq_tests_render
  SOURCES
    golden/golden_audio.cpp
    golden/test_golden_audio.cpp
    golden/test_golden_render.cpp
    tools/test_pqc.cpp
  DEPS pq_pqc)
add_dependencies(pq_tests_render pqc)
target_compile_definitions(pq_tests_render PRIVATE PQC_EXE=pq_build_pqc_exe)

# jit (T1b.1): the LLVM backend against the interpreter and the reference evaluator over the T1.7 corpus, plus the
# ProgramImage contract cases and compile times.
pq_add_test(pq_tests_jit PERF SHARDS 4 SLOW_SHARDS 8
  SOURCES
    jit/test_jit.cpp
    engine/test_voices.cpp  # the T1.8 voice suite again, on JIT images (rigImageBuilder, set in test_jit.cpp)
    engine/test_tuning_voices.cpp  # T2.6: the tuning node through the VoiceEngine, on JIT images
    jit/test_live_swap.cpp  # T1b.5: interpreter -> JIT live swap through the background compiler
    jit/test_jit_cache.cpp  # T1b.4: the compile cache (memory, disk, read-only AOT directories)
    engine/test_silence.cpp  # T1.x-silence: the sleep suite again, on JIT images
    jit/test_jit_silence.cpp  # T1.x-silence: interpreter and JIT sleep identically
    jit/test_oversample.cpp  # TMM.6: the oversample(n) region on both backends (reference, aliasing, pitch, voices, swap)
    engine/test_probes.cpp  # T4.5: probes and cost meters again, on JIT images (and interpreter == JIT probe streams)
    jit/test_jit_frame_table.cpp  # TMM.2: wavetable_pos / voice_tap on both backends (swaps, taps, hot swap, reference)
    jit/test_jit_curve.cpp  # GU.1: the curve atom on both backends (audio / param rate, voices, a feedback loop, swaps)
    jit/test_jit_svf.cpp  # EQ.1: the svf atom on both backends (bit-identical, = svfResponse, audio-rate cutoff)
    jit/test_jit_wave_live.cpp  # TMM.8: wavetable_live / wavetable_shape in oversample regions on both backends
    jit/test_param_ramps.cpp  # TX.smooth: param ramps, interpreter == JIT under param motion, the ramp's shape
    jit/test_uniform_branch.cpp  # TX.uniform-branch: selects of block-uniform conditions, both backends, with and without
  DEPS pq_jit pq_compile)

# T1b.3: the stock subcircuits of stdlib/ (filters, envelopes, LFO, followers) against their hand-written C++
# references (tests/stdlib/stdlib_reference.h, also the baseline of bench/jit/bench_stdlib.cpp), interpreter + JIT.
pq_add_test(pq_tests_stdlib SHARDS 4 SLOW_SHARDS 4
  SOURCES
    stdlib/test_stdlib.cpp
    stdlib/test_smooth.cpp  # TX.smooth: every continuous param of every meta module jumps click-free (GU.10: the stock devices retired)
  DEPS pq_jit pq_pqc pq_tuning)

pq_add_test(pq_tests_dsp SLOW_SHARDS 10  # TMM.8: the live path's slow alias proof and slow-modulation cases (≈ 20 – 30 s each)
  SOURCES
    dsp/test_atoms_math_logic.cpp
    dsp/test_atoms_memory_gen.cpp
    dsp/test_phasor_drift.cpp
    dsp/test_tables.cpp
    dsp/test_dsp_rt.cpp
    dsp/test_exact_lanes.cpp
    dsp/test_lane_state.cpp
    dsp/test_gain.cpp
    dsp/test_param_ramp.cpp  # TX.smooth: the param_ramp kernel
    dsp/test_spectrum.cpp
    dsp/test_transfer.cpp  # TX.spectrum: the transfer-function estimate
    dsp/test_scope.cpp  # TX.scope: the oscilloscope's trigger, pitch lock and beat slots
    dsp/test_spectrogram.cpp  # TX.spectrogram: the reassigned spectrogram's columns (sine, sweep, chord, sub, impulse, sequence)
    dsp/test_silence.cpp
    dsp/test_resample.cpp
    dsp/test_frame_table.cpp
    dsp/test_frame_table_alias.cpp
    dsp/test_frame_table_sine.cpp  # TMM.12-fix1: sine tables (detection, the readers' sine path vs the rows)
    dsp/test_wavetable_pos.cpp  # TMM.2: wavetable_pos (slots, crossfade, taps, swap stress), voice_tap
    dsp/test_oversample.cpp  # TMM.6: the oversample region's rate converters
    dsp/test_wave_live.cpp  # TMM.8: wavetable_live / wavetable_shape (baked agreement, the alias proof, ADAA)
    dsp/test_curve.cpp  # GU.1: control curves (presets vs the old knob laws, the inverse, the curve atom vs evalCurve)
    dsp/test_svf.cpp  # EQ.1: the svf atom (impulse response == svfResponse, lanes, matched vs analog, stability, splits)
    support/alias_meter.h  # TMM.1: the reusable alias meter (listed so tests/support maps to its users)
  DEPS pq_dsp)

# meta (TMM.3): the meta-module framework (registry, expansion, prelude, data) and the Phase accumulator through pqc
# (standalone check + render, bit-identity with frac(phasor(voice.hz)), pitch over 10 s, both backends); TMM.5: the
# Wavetable type (data codec, expansion, table jobs, the off-thread TableBuilder, pqc render).
pq_add_test(pq_tests_meta PERF  # TMM.13-fix1 M2: the preview rate of a drag (wall-clock)
  SOURCES
    meta/test_meta.cpp
    meta/test_wavetable.cpp
    meta/test_envelope.cpp  # TMM.11: the Envelope (expansion, bit-identity with msenv, curves, VCA)
    meta/test_spectrum.cpp  # TX.spectrum: the Spectrum module (expansion, the tap, pass-through bit for bit on both backends)
    meta/test_oscilloscope.cpp  # TX.scope: the Oscilloscope module (expansion, the tap, data, pass-through bit for bit on both backends)
    meta/test_spectrogram_module.cpp  # TX.spectrogram: the Spectrogram module (expansion, the tap, data, pass-through bit for bit on both backends)
  DEPS pq_meta pq_pqc)

# document: note sequence, every command kind's ChangeSet, history/gestures, structural sharing.
pq_add_test(pq_tests_document
  SOURCES
    document/test_note_seq.cpp
    document/test_commands.cpp
    document/test_store.cpp
    document/test_serialize.cpp
    document/test_serialize_errors.cpp
    document/test_project_io.cpp
    document/test_edit_ops.cpp
    document/test_edit_pitch.cpp
    document/test_edit_time.cpp
    document/test_audibility.cpp
    document/test_main_tunings.cpp
    document/test_virtual_fundamental.cpp
    document/test_devices.cpp  # T4.1: device chains (commands, builders, catalog, lowering)
    document/test_audio.cpp    # T5.3: audio tracks + clips (commands, builders, warp laws, file)
    document/test_samples.cpp  # T5.8: project samples (resolve, relink, collect all and save)
    document/test_library_files.cpp  # T5.11: user-library files, hot-swap, library clips
    document/test_device_automation.cpp  # T4.18: device / macro / chain / wire lanes (rules, following, file, lowering)
    document/test_meta_modules.cpp  # TMM.3: meta-module state (file, builders, lowering goldens, renames)
    document/test_meta_wavetable.cpp  # TMM.5: the Wavetable's data in the file, its baked-only controls
    document/test_meta_devices.cpp  # TMM.10: meta devices in chains (voice runs, kinds, copy / presets keep state)
    document/test_wire_laws.cpp  # TX.patching: wire laws, schema 13 + migration, any output / input, free devices
    document/test_main_path.cpp  # TX.all-free: the main path, its rewiring builders, the 16 -> 17 migration, the lowering of the wiring
    document/test_knob_curves.cpp  # TX.knob-curves: knob curves, the wire laws they decide, schema 16 + migration
    document/test_blocks.cpp  # GU.2: nested groups (rules, remove / move, nest / unnest), atom + value blocks, block sets, group presets
    document/test_one_hierarchy.cpp  # GU.11: every fixture and module preset holds only blocks, groups and native engines
    document/test_group_response.cpp  # EQ.2: a group's analytic In -> Out response (lowered once, knobs re-read)
    document/test_eq_modules.cpp  # EQ.3: the band and EQ modules (roles, const inputs), their ops (one undo step, save / load), the EQ's response matrix
    document/test_panels.cpp  # GU.3: group panels (rules, SetGroupPanel + builders, final pass on delete / move out, save / load, paste remap, presets)
  DEPS pq_doc)

# library (T5.9): index builds and revalidation on synthetic trees in %TEMP% (2k files; 200k in `slow`), the index
# file + journal, the change watcher (latency in `perf`), search vs a brute-force reference scorer (200k-entry
# keystroke timing in `perf`), tags across renames, progressive indexing (tree first, probes later; odd names).
pq_add_test(pq_tests_library PERF SLOW_SHARDS 4
  SOURCES
    library/test_index.cpp
    library/test_indexer.cpp
    library/test_progressive.cpp
    library/test_search.cpp
    library/test_tags.cpp
  DEPS pq_library)

# T0.9-fix1: pq::ui's line primitives vs PQ Meter's own code, compiled from the PQ Meter source tree when it
# exists on this machine (skipped otherwise). See the file for how and why.
include(ui/pqmeter_diff.cmake)

# T0.x-flake: wall-clock checks that depend on machine load are opt-in under the `perf` label (the test presets
# exclude it; run with ctest -L perf or scripts/build.ps1 -Perf). See tests/README.md.
add_test(NAME pqdaw_window_selftest_perf COMMAND pqdaw --window-selftest --scene lines-playhead)
set_tests_properties(pqdaw_window_selftest_perf PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS perf RUN_SERIAL TRUE
  ENVIRONMENT PQ_PERF_CHECKS=1)
