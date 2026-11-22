pq_add_module(pq_dsp
  SOURCES
    src/atoms.cpp
    src/kernels_math.cpp
    src/kernels_logic.cpp
    src/kernels_memory.cpp
    src/kernels_gen.cpp
    src/kernels_tuning.cpp
    src/kernels_gain.cpp
    src/kernels_ramp.cpp
    src/kernels_oversample.cpp
    src/tables.cpp
    src/fft.cpp
    src/spectrum.cpp
    src/transfer.cpp
    src/scope.cpp
    src/spectrogram.cpp
    src/stereo.cpp  # VZ.5: the cross-spectrum (Stereo Image, Phase Scope) and the goniometer state (Stereo Field)
    src/waterfall.cpp     # VZ.7: the waterfall analysis (slices of the spectrum analysis)
    src/stereo_cloud.cpp  # VZ.7: the stereo spectrogram analysis (reassigned stereo STFT, ribbon)
    src/loudness.cpp  # VZ.4: the loudness front end (Level Meter, LUFS Meter, Loudness History)
    src/tone_preview.cpp
    src/band_levels.cpp  # VZ.6: Band Levels (the LR crossover tree, band RMS over time)
    src/waveform.cpp  # VZ.6: the Waveform capture (bins, the beat-locked loop)
    src/resample.cpp
    src/frame_table.cpp
    src/frame_table_read.cpp
    src/frame_table_build.cpp
    src/frame_table_slot.cpp
    src/sample_slot.cpp  # SM.2: live sample slots (sample_read / sample_pos / sample_slice)
    src/kernels_sample.cpp
    src/kernels_sampler_warp.cpp
    src/kernels_granular.cpp  # SM.6: the granular atom (8 grains per AVX2 op)
    src/kernels_frame_table.cpp
    src/kernels_wave_live.cpp
    src/kernels_signal_tap.cpp
    src/kernels_curve.cpp
    src/kernels_svf.cpp
    src/kernels_dynamics.cpp
    src/svf_design.cpp
    src/kernels_noise_step.cpp
    src/noise_step.cpp
    src/curve.cpp
    src/fft_mixed.cpp
    src/warp.cpp
    src/warp_voice.cpp
    src/stretch_pool.cpp  # T6.5 (SM.5: moved from pq_engine): the phase-vocoder stretchers (signalsmith-stretch)
    src/sampler_stretch.cpp  # SM.5: the Sampler's Complex / Complex Pro voices (a stretcher per voice lane)
  DEPS pq_core
  PRIVATE_DEPS pq_signalsmith
)
# T6.5: signalsmith-stretch is third-party code instantiated in this TU alone: its warnings off there (/bigobj: the
# library's templates exceed MSVC's default section count).
set_source_files_properties(src/stretch_pool.cpp PROPERTIES COMPILE_OPTIONS "/W0;/bigobj")
