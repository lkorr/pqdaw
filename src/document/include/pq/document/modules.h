#pragma once

// GU.4 (SPEC §15 2026-11-06 "built GUIs"; docs/plans/GU.md "Phases" 4): the BUILT-IN MODULES made with the kit — group
// presets of atom blocks and value blocks with a panel of generic elements, built here (not files: the node menu and the
// browser paste them; a user saving one gets an ordinary .pqpreset group preset). They replace the meta modules of the
// same names (GU.f7: a file that still holds one is refused on load, as any non-block device):
//
//   Phase Accumulator   a group: In → [hz + Frequency] → × 2^(Pitch/1200) (cents_to_hz(Pitch, 1)) → phasor → + Phase
//                       → frac → × Amplitude → Out. The value blocks carry the old params' curves: Frequency 0 … 20 kHz
//                       exp0 (k 14), SIGNED (a wire may take it below 0: the phase runs backwards); Phase 0 … 1, WRAP;
//                       Amplitude 0 … 4; Pitch ±4800 cents (GU.5b: ±1200 over the middle half of the travel, as the
//                       old face's arc); and a constant 1 (the ratio's reference). Its panel (GU.5b): the old face — a
//                       body circle, four `arc` elements round it and the `orbit` in the middle. Phase modulation is
//                       a wire into Phase (its curve wraps).
//   VCA                 a group: In → mul ← Value → Out; the value block's curve is the VCA's law (dsp::CurvePreset::Vca,
//                       ±20000, the fine ±1 zone over 40 % of the travel), its knob at 0.7 (× 1). Its panel (GU.5b): the
//                       old face, one `dial` (the curve's ±1 / ±10 / ±100 points as its markers, range and curve in its
//                       popup).
//
// Both play exactly the atoms the meta modules' expansions scheduled (the Phase Accumulator bit for bit; the VCA's law
// is a curve now: the same function, evaluated by dsp::evalCurve / the curve atom instead of its own atom chain, and
// `mul` instead of the 2 ms smoothed gain: tests/app/test_module_presets.cpp).
//
// Not RT code.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/ids.h"
#include "pq/document/model.h"
#include "pq/document/panel.h"
#include "pq/dsp/curve.h"

namespace pq::doc {

enum class ModuleKind : uint8_t {
  PhaseAccumulator, Vca, Wavetable, Envelope, Oscilloscope, Spectrum, Spectrogram, WavetableSynth,
  Utility, Saturator, Lfo, Operator,  // GU.10: the stock devices
  EqBand, Eq,                         // EQ.3
  DelayTap,                           // DT.1
  Noise,                              // NZ.1
  Compressor,                         // DY.4
  Limiter                             // DY.6
};
struct ModuleInfo {
  ModuleKind kind;
  std::string_view key;   // "phase_acc", "vca" (a menu / browser item: kModuleItemPrefix + key)
  std::string_view name;  // "Phase Accumulator", "VCA"
  std::string_view description;
  bool instrument = false;  // GU.8: an instrument group (spawned at a MIDI track's notes when its path is empty)
};
inline constexpr std::string_view kModuleItemPrefix = "module:";
std::span<const ModuleInfo> moduleInfos() noexcept;
const ModuleInfo* findModule(std::string_view key) noexcept;  // by key or name

// The value blocks' names (also their panel labels) and the atoms' (Device::name).
inline constexpr std::string_view kPaFrequency = "Frequency", kPaPhase = "Phase", kPaAmplitude = "Amplitude",
                                  kPaPitch = "Pitch", kPaReference = "1";
inline constexpr std::string_view kVcaValueName = "Value";

// The roles of a built Phase Accumulator (its blocks' ids).
struct PhaseAccumulatorRoles {
  DeviceId group, freq, phase, amp, pitch, ref;  // the group, the value blocks
  DeviceId sum, ratio, pitched, phasor, addPhase, frac, out;  // the atoms
};
struct PhaseAccumulatorOptions {
  DeviceId id;                  // the group's id (invalid: a fresh one)
  DeviceId inGroup;             // the group it sits in (nested), or none
  std::string name;             // the group's name ("" = "Phase Accumulator")
  double freq = 0.0, phase = 0.0, amp = 0.5, pitch = 0.5;  // knob positions (the defaults: 0 Hz, 0, × 1, 0 cents)
  std::optional<dsp::Curve> freqCurve, phaseCurve, ampCurve, pitchCurve;  // (none: the module's own)
  enum class Feed : uint8_t { Hz, None } feed = Feed::Hz;  // what the In terminal feeds (the old chain input)
  bool active = true;
};
BlockSet phaseAccumulatorBlocks(IdSource& ids, const PhaseAccumulatorOptions& o = {}, PhaseAccumulatorRoles* roles = nullptr);
// The module's own control curves.
dsp::Curve phaseAccumulatorCurve(std::string_view control);  // kPaFrequency …

struct VcaRoles {
  DeviceId group, value, mul;
};
struct VcaOptions {
  DeviceId id;
  DeviceId inGroup;
  std::string name;  // "" = "VCA"
  double value = 0.7;  // the knob position
  std::optional<dsp::Curve> curve;  // none: the VCA's law (vcaCurve())
  bool plugged = true;              // the In terminal feeds the multiplier
  bool active = true;
};
BlockSet vcaBlocks(IdSource& ids, const VcaOptions& o = {}, VcaRoles* roles = nullptr);
// The VCA's curve: its law over [min, max] with fine zone `zone` (0: the default 0.4).
dsp::Curve vcaCurve(double min = -20000.0, double max = 20000.0, double zone = 0.0);

// ---- GU.7: modules around a NATIVE ENGINE (docs/plans/GU.md "Phases" 7, "One hierarchy") ----
// The Wavetable, the Envelope and the analysers (Oscilloscope, Spectrum, Spectrogram) are groups holding one native
// engine — a meta device (`meta:<type>`, a leaf: its expansion is its DSP; the analysers' is their tap) — wired
// In → the engine's chain input, its `out` → Out, with a panel that reproduces the engine's old face from elements:
//   Wavetable    the rich elements "drawtools" (mode switch, draw tools, basic shapes), "harmonics" (the harmonic
//                editor), "wavetable" (the table display with the position slider and its keyframe markers) and
//                "warpmode" (the warp-mode glyphs and the oversampling cell), bound to the engine's DATA; the phase
//                slider and the fold / warp / sync knobs generic elements bound to its controls
//   Envelope     "envelope" (the shape with its breakpoints and bends) bound to the engine's data; the amount slider a
//                generic slider
//   analysers    "scope" / "spectrum" / "spectrogram" filling the panel, bound to the engine's data (the display
//                state: saved, undoable); the engine's own tap feeds them
//   Noise        NZ.1 (docs/plans/NZ.md): "noise" (the trace of the output, the voices' dots, the colour's slope line)
//                bound to the engine's data; generic knobs Rate (shown in Rate clock) or a Sync menu (Sync clock: the
//                note values, kNoiseSyncItems), Smooth, Colour, Density, Steps, Min, Max, Seed; tabs (chips) bound to
//                the settings Clock (Rate | Sync | Note), Distribution (Uniform | Gauss), Restart (Play | Free | Note),
//                Voices (Voice | Shared). Its In is the engine's clock input (wired: a clock that overrides the setting)
// The engine is the one block inside; the elements' rects are the old face's (the views' rich kinds show the same
// sections of it). A Wavetable module at an instrument group's In hears the note's frequency, not its phase (a nested
// group's main input reads the note's Hz: GU.4): put a Phase Accumulator before it (QUESTIONS Q?-GU.7-a).
struct EngineRoles {
  DeviceId group, engine;
};
struct EngineModuleOptions {
  DeviceId id;       // the group's (invalid: a fresh one)
  DeviceId inGroup;  // the group it sits in, or none
  std::string name;  // "" = the module's name
  bool active = true;
};
// The engine type of an engine module ("wavetable" …), "" for the Phase Accumulator and the VCA.
std::string_view moduleEngineType(ModuleKind k) noexcept;
// The module built around engine type `typeId` (null: none).
const ModuleInfo* moduleOfEngine(std::string_view typeId) noexcept;
BlockSet engineModuleBlocks(IdSource& ids, ModuleKind k, const EngineModuleOptions& o = {}, EngineRoles* roles = nullptr);
// The module's panel bound to engine block `engine` (the Phase Accumulator and the VCA: an empty panel).
Panel enginePanel(ModuleKind k, DeviceId engine);

// ---- GU.8: the example instrument (docs/plans/GU.md "Phases" 8) ----------------------------------------------------
// The Wavetable Synth: an instrument group (makeInstrumentGroup: 16 voices) built only from the modules above —
//   In (the notes) → Phase Accumulator (Osc: its Pitch on the panel) → Wavetable → Envelope → Out
//   LFO n (n = 1, 2): a free Phase Accumulator (its In unplugged: a ramp at its Frequency, restarting per note) →
//                     Wavetable (oversampling ×2: an LFO) → mul ← Depth (a value block) → a Units wire into
//                     LFO 1: the oscillator Wavetable's `position` (Depth 0 … 1)
//                     LFO 2: the oscillator's Pitch value block (Depth 0 … 1200 cents: vibrato)
// Its panel: a `tabs` element "Osc|Env|LFO 1|LFO 2" (unbound: view state), the Pitch knob always shown, and per tab the
// child modules' panels embedded (GU.6): Osc the Wavetable, Env the Envelope, LFO n its Phase Accumulator (the rate on
// its Frequency arc), its Wavetable (the shape) and its Depth knob. Everything is per voice (inside the instrument).
struct WavetableSynthRoles {
  DeviceId group;
  PhaseAccumulatorRoles osc;
  EngineRoles wavetable, envelope;
  PhaseAccumulatorRoles lfo[2];
  EngineRoles lfoShape[2];
  DeviceId depth[2], mul[2];
  uint32_t tabs = 0;      // the tabs element's id
  uint32_t pitchKnob = 0;
};
inline constexpr const char* kSynthTabs = "Osc|Env|LFO 1|LFO 2";
BlockSet wavetableSynthBlocks(IdSource& ids, DeviceId inGroup = {}, WavetableSynthRoles* roles = nullptr);

// ---- GU.10: the stock devices as group presets (docs/plans/GU.md "One hierarchy") ----
// Utility, Saturator, the LFO and Operator are groups of atom blocks, value blocks and modules, each with a panel laid
// out like its old face (retired/stdlib/devices/<name>.panel.json); the default instrument of a new MIDI track too.
// What each keeps of the old patch (QUESTIONS Q?-GU.10-a lists what it leaves out):
//   Utility      the old patch's param-rate matrix term for term (channel mode, phase L / R, width, Mid/Side mode and
//                balance, mono, balance, gain with −70 dB = silence, mute) and its per-sample smoothed gains (gain /
//                gain_add, 2 ms, from their first value): the same arithmetic as the old device without bass mono and
//                the DC filter (both need a filter: no Filter module yet).
//   Saturator    drive (a smoothed gain) → seven of the old nine curves (each a nested group of the old sat_shape's
//                atoms) chosen by Type (a menu: a chain of muxes) → output (a smoothed gain) → soft clip → dry/wet (a
//                smoothed gain_add). No Waveshaper, Shaper (a drawn table), colour EQ (a filter) or HQ oversampling.
//   LFO          stdlib/control/lfo.pqp's atoms (free phasor or song-synced phase, + phase, the five waves chosen by
//                Shape), then offset + depth · wave on the group's Out (a modulation source: no audio passes through);
//                its panel has the jack (a modsource element).
//   Operator     an instrument group of four operator rows A … D: In (the note's Hz) → × Coarse → Phase Accumulator
//                (Pitch = fine tune) → Wavetable (a sine) → Envelope (Amount = level); FM by wires, D → C → B → A, each
//                Envelope into the next row's Phase Accumulator Phase (a per-voice cable into the control: algorithm 1),
//                only A's to Out; B … D at level 0.
//                Its panel: Coarse / Fine / Level per row and the four Envelope panels embedded under A … D tabs.
//   default      an instrument group "Basic Synth": In → Phase Accumulator → Wavetable → Envelope (Amount 0.25) → Out.
// Every one is built here (the node menu, the browser and the Create menu paste moduleBlocks; createTrack the default).
struct StockModuleOptions {
  DeviceId id;       // the group's (invalid: a fresh one)
  DeviceId inGroup;  // the group it sits in, or none
  std::string name;  // "" = the module's name
  bool active = true;
};
inline constexpr std::string_view kDefaultInstrumentName = "Basic Synth";
BlockSet utilityBlocks(IdSource& ids, const StockModuleOptions& o = {});
BlockSet saturatorBlocks(IdSource& ids, const StockModuleOptions& o = {});
BlockSet lfoBlocks(IdSource& ids, const StockModuleOptions& o = {});
BlockSet operatorBlocks(IdSource& ids, const StockModuleOptions& o = {});
// The default instrument (an instrument group, its polyphony written out, and its blocks; the group first).
BlockSet defaultInstrumentBlocks(IdSource& ids, const StockModuleOptions& o = {});

// ---- EQ.3: the band and the EQ (docs/plans/EQ.md "Structure") ------------------------------------------------------
// BAND (module:eq_band): a group In → svf atom → Out whose value blocks carry a `bandRole` setting (found by role, never
// by name, as racks.h's rackRole):
//   Frequency  0 … 20 kHz on one Exp segment of bend ln 2001: the knob travel t is the response display's axis
//              x = log(1 + f / 10 Hz) / log(2001) exactly (f = 10 Hz · (2001^t − 1)) → the svf's cutoff
//   Q          0.1 … 40, octave law → q
//   Gain       ±24 dB (a dB curve with the DbToGain output: the block gives the linear gain the svf's `gain` takes)
//   Stages     1 … 64, stepped → the svf's const input `stages` (a structural value: lower.h "Structural value blocks")
//   Type       0 … 7, stepped, the svf modes in their order (kBandTypeItems) → the const input `mode`
// Bell and Notch run one stage whatever Stages says (the atom's rule). Bypass = the band group switched off.
// EQ (module:eq): value blocks Mode (Stereo | L/R | M/S, stepped: structural), Output (±24 dB) and Edit Chain (the strip's
// A / B tabs; feeds nothing), each with an `eqRole`; two chain groups with a `bandChain` setting (1 = A, 2 = B) holding
// bands in series (In → band → … → Out; an empty chain wires In to Out); the matrix as literal muxes on Mode:
//   Stereo  In → A → Out (B and the matrix folded away: no op of theirs is scheduled)
//   L/R     channel 0 → A, channel 1 → B, pack(A, B)
//   M/S     M = (L + R)/2 → A, S = (L − R)/2 → B, pack(M + S, M − S)
// then a smoothed gain by Output. Its panel: the `response` display (EQ.4; its analyser on the EQ's Out, its input dimmed)
// with the `phase` strip under it, the Mode menu, the Output knob, the chain tabs (L|R or M|S, shown when Mode ≠ Stereo)
// and the BAND STRIP: per band of each chain a column — a plate bound to the band (lit while it is selected on the
// display), its Type as a `bandtype` glyph menu, Hz, Q, a gain slider for the gain types, Stages for the others —, chain
// A's shown in Stereo or when the tabs pick A, chain B's when they pick B. The ops (edit_ops.h "EQ") lay the strip out
// again.
enum class BandRole : uint8_t { None = 0, Frequency, Q, Gain, Stages, Type };
enum class EqRole : uint8_t { None = 0, Mode, Output, EditChain };
inline constexpr std::string_view kBandRoleSetting = "bandRole", kEqRoleSetting = "eqRole";
inline constexpr std::string_view kBandChainSetting = "bandChain";  // a chain group: 1 = A, 2 = B
// Band solo (ops::setBandSolo): the soloed band carries kBandSoloSetting = 1, each other band of its chain the active
// state it had (kSoloRestoreSetting 0 / 1) while it is held off.
inline constexpr std::string_view kBandSoloSetting = "bandSolo", kSoloRestoreSetting = "soloRestore";
inline constexpr const char* kBandTypeItems = "High Cut|Low Cut|Band Pass|Notch|Bell|Low Shelf|High Shelf|All Pass";
inline constexpr const char* kEqModeItems = "Stereo|L/R|M/S";
inline constexpr const char* kOversampleItems = "1×|2×|4×|8×";  // TX.group-oversample: a group's `oversample` (model.h)
inline constexpr uint32_t kMaxBandsPerChain = 32;
inline constexpr double kBandMaxHz = 20000.0, kBandAxisHz = 10.0;  // the Frequency curve: f = 10 · (2001^t − 1)
enum class EqMode : uint8_t { Stereo = 0, LeftRight, MidSide };
BandRole bandRole(const Device& d) noexcept;
EqRole eqRole(const Device& d) noexcept;
uint32_t bandChainOf(const Device& d) noexcept;  // 1 / 2, 0: not a chain group
// Whether a svf mode has a gain (Bell, Low Shelf, High Shelf) / honours Stages (not Bell, Notch).
bool bandTypeHasGain(uint32_t type) noexcept;
bool bandTypeHasStages(uint32_t type) noexcept;
dsp::Curve bandFrequencyCurve();

struct BandInfo {
  DeviceId group;
  DeviceId frequency, q, gain, stages, type;  // the role blocks (invalid: missing)
};
struct EqInfo {
  DeviceId group;
  DeviceId mode, output, editChain;
  DeviceId chain[2];               // A, B
  std::vector<BandInfo> bands[2];  // per chain in series order (In → … → Out; bands off the series after them)
};
// The band group `band` (a group holding a Frequency role block), nullopt: not one.
std::optional<BandInfo> bandInfo(const Project& p, DeviceId band);
// The EQ group `eq` (a group holding chain groups A and B), nullopt: not one.
std::optional<EqInfo> eqInfo(const Project& p, DeviceId eq);
// The EQ (and chain index 0 / 1) a band sits in, nullopt: none.
std::optional<std::pair<EqInfo, uint32_t>> eqOfBand(const Project& p, DeviceId band);
// A value block's value in its curve's units (dB for a gain).
double valueBlockUnits(const Device& valueBlock) noexcept;

struct EqBandOptions {
  DeviceId id;       // the group's (invalid: a fresh one)
  DeviceId inGroup;  // the chain group it sits in, or none
  std::string name;  // "" = "Band"
  uint32_t type = 4; // dsp::SvfMode (Bell)
  double freqHz = 1000.0, q = 0.70710678118654752, gainDb = 0.0;
  uint32_t stages = 1;
  bool active = true;
};
BlockSet eqBandBlocks(IdSource& ids, const EqBandOptions& o = {});
BlockSet eqBlocks(IdSource& ids, const StockModuleOptions& o = {});
// `panel` with its band strip laid out again for `bands` (A, B): the elements tagged as strip columns replaced, the
// panel's width (and its plate) following the longest chain. Element ids: fresh ones past the panel's.
Panel layEqStrip(const Panel& panel, const std::vector<BandInfo> (&bands)[2]);

// ---- DT.1: the Delay Tap (docs/plans/DT.md, SPEC §15 2026-11-12 "a delay tap meta module") ----------------------
// A group In → delay_fade → Out: the input delayed, nothing else (feedback is a wire from its Out back into its In
// through something that scales: a VCA). Stereo when the input is. Its value blocks (found by role, kDelayRoleSetting):
//   Time   the knob position x through the curve 62.5 ms · (e^(k·x) − 1), k = 2·ln 9 (Exp0: one Exp segment of bend
//          ln 81): 0 at 0, 500 ms at 0.5, 5 s at 1, in SECONDS (dsp::CurveUnit::Seconds: it reads "375 ms" / "1.25 s";
//          a wire adds seconds, the delay atom's own unit)
//   Sync   a stepped index into noteValues() (ascending duration); seconds = beats · 60 / transport.bpm (the block's
//          tempo), at most 5 s
//   Mode   0 = Time (ms), 1 = Sync (♪); stepped, automatable
//   Glide  DT.4: 0 = Repitch (default: every time change glides the read head, a tape bend), 1 = Crossfade (DT.2: a
//          change beyond the octave bound crossfades two heads); stepped, automatable; delay_fade's `crossfade` input
//          (param rate, read per block: no recompile)
// The delay = lerp(Time, Sync seconds, Mode) → delay_fade's time (max 5 s: its ring sized and allocated off-thread by
// the scheduler; per-voice lanes inside instrument groups). The read is ≥ 1 sample (DT.2: linear in [1, 2)). The
// note value's beats are arithmetic on the index, and Mode a lerp, not muxes: a stepped block reaching a mux index
// would lower as a literal choice (lower.h "Structural value blocks"), so a sync step or a mode switch would recompile
// the unit (and a compile per step of a sync drag). As written all three are runtime params. DT.2: delay_fade moves
// its read head itself (its time is not param-ramped): a change it can glide within an octave of pitch over
// kDelayTapFadeSeconds (automation, drags, tempo drift) glides; a larger one (a mode switch, a sync step, a click that
// jumps the slider) crossfades the old and the new time, equal power over the same 30 ms (pq/dsp/atoms.h
// "delay_fade"). An audio-rate wire into Time is followed sample by sample (a step beyond the octave bound fades).
// DT.4: that is Glide's Crossfade; its default, Repitch, glides every change instead (pq/dsp/atoms.h "delay_fade").
// Its panel: one `temposlider` element (the reusable tempo-marked slider, pq/views/panel) bound to Time, Sync, Mode
// and Glide.
enum class DelayRole : uint8_t { None = 0, Time, Sync, Mode, Glide };
inline constexpr std::string_view kDelayRoleSetting = "delayRole";
inline constexpr double kDelayTapMaxSeconds = 5.0;
inline constexpr double kDelayTapFadeSeconds = 0.030;  // DT.2: delay_fade's glide / crossfade
// A sync note value: its duration in beats (a quarter = 1 beat; in ticks beats · pq::kPpq), its label (DT.3: the reduced
// fraction of a whole note: "1/4", "3/16" for a dotted eighth, "1/6" for a quarter triplet) and its kind.
enum class NoteValueKind : uint8_t { Straight, Dotted, Triplet };
struct NoteValue {
  double beats = 1.0;
  std::string_view label;
  NoteValueKind kind = NoteValueKind::Straight;
};
// 1/64 … 1/1, straight, dotted and triplet: 21 values in ascending duration.
std::span<const NoteValue> noteValues() noexcept;
inline constexpr uint32_t kNoteValueQuarter = 13;  // noteValues()[13] is 1/4
// noteValues()[index] (clamped) in seconds at `bpm`, at most kDelayTapMaxSeconds.
double noteValueSeconds(uint32_t index, double bpm) noexcept;
dsp::Curve delayTimeCurve();
DelayRole delayRole(const Device& d) noexcept;
struct DelayTapRoles {
  DeviceId group, time, sync, mode, delay, glide;
};
struct DelayTapOptions {
  DeviceId id;       // the group's (invalid: a fresh one)
  DeviceId inGroup;  // the group it sits in, or none
  std::string name;  // "" = "Delay Tap"
  double time = 0.5;                       // the Time knob position (0.5: 500 ms)
  uint32_t sync = kNoteValueQuarter;       // the Sync index
  uint32_t mode = 0;                       // 0 Time, 1 Sync
  uint32_t glide = 0;                      // DT.4: 0 Repitch, 1 Crossfade
  bool active = true;
};
BlockSet delayTapBlocks(IdSource& ids, const DelayTapOptions& o = {}, DelayTapRoles* roles = nullptr);

// ---- DY.4: the Compressor (docs/plans/DY.md "DY.4"; SPEC §8 Compressor, sidechain) -------------------------------------
// A group of atom and value blocks (no native engine), stereo, everything block-wise (the only recursion is inside the
// `follow` kernel):
//   detector  det = mux(Sidechain, In, lerp(In, SC, SC Mix)) — SC is the child group "Sidechain" (its In is where a cable
//             from another track lands: a compensated sidechain, lower.h "Sidechains"; inside: an EQ Band child group
//             (Low Cut 100 Hz) chosen by "EQ On" (off), then a smoothed Gain); Sidechain is set to 1 by the cablesource
//             picker when a source is picked (0 at None)
//   level     Detector 0 Peak: max(|L|, |R|) (linked); 1 RMS: sqrt(window(max(L², R²), 5 ms, mean)) (Q261: the
//             detector and the law are two switches, so RMS expansion works)
//   envelope  Env Log (default): dB = fast_gain_to_db(max(level, 1e-6)) → Gain Computer → follow(GR, Attack, Release);
//             Env Lin: follow(level, Attack, Release) → dB → Gain Computer. Auto release (Auto 1): the follower is
//             max(follow(·, a, rel / 4), follow(·, a, rel · 4) − 3 dB) — on GR (Log: minus 3 dB of reduction) or on the
//             level (Lin: × 10^(−3/20)): a short transient recovers at rel / 4, a long stretch of compression lets go of
//             its last 3 dB at rel · 4
//   computer  the child group "Gain Computer" (memoryless: In = level dB, Out = reduction dB ≥ 0; its own value blocks
//             Threshold, Ratio, Knee): s = over = x − T (Law 0 Compress) or under = T − x (Law 1 Expand), k = clip(s + W/2, 0, W),
//             body = k² / (2·max(W, 1e-3)) + max(0, s − W/2); Compress GR = (1 − 1/R)·body; Expand (downward, slope R
//             below T) GR = min((R − 1)·body, 70 dB). The `transfer` element draws it (units db, output reduction)
//   output    GR = max(env, 0) (the block "GR": the meter's and the jack's signal, a cable source like any output);
//             g = fast_db_to_gain(Makeup − GR); out = latency(In, 0, Lookahead) · gain_add(1, g − 1, Dry/Wet) (the dry /
//             wet a smoothed mono gain on the delayed input: dry + w·(wet − dry) with wet = dry·g, one stereo multiply);
//             Listen 1: Out = det (what the detector hears)
// Stepped switches (Detector, Law, Envelope, Auto, Sidechain, EQ On, Listen) lower as literals (lower.h "Structural value
// blocks"): a change recompiles and only the chosen path is computed. Lookahead (0 / 1 / 10 ms: a stepped curve in
// seconds, its positions 0, ½, 1) feeds latency's const `time` (constFeeds): the delay and the reported latency are
// round(la · sr) samples (PDC follows; a change recompiles). Value blocks are found by name (Device::name).
// Panel: the transfer display, the GR (reduction) and Out (level, on the module's output) meters with their labels
// and dB scales (AU.dsp, RB-DY L13), the GR jack; knobs Threshold, Ratio, Attack, Release, Knee, Makeup, Dry/Wet; Auto
// under Release; chips Envelope (under Knee), Lookahead, Detector, Law; the Sidechain section (cablesource, Gain, Mix,
// EQ On + Frequency, Listen).
// Sleep (AU.dsp, RB-DY H4; pq/graph/schedule.h "Sleep plan"): on a silent input the Gain Computer's input is the
// −120 dB floor, its reduction a constant (0 below the threshold: Z by its range when the controls are undriven), so
// the followers converge or decay and the region sleeps once they rest; the reduction releases to 2^-30 dB first (≈ 24
// release time constants, Auto: of rel · 4: QUESTIONS Q?-AU.dsp-a).
inline constexpr double kCompressorRmsSeconds = 0.005;
inline constexpr double kCompressorExpandCapDb = 70.0;
inline constexpr double kCompressorAutoOffsetDb = 3.0;
BlockSet compressorBlocks(IdSource& ids, const StockModuleOptions& o = {});

// ---- DY.6: the Limiter (docs/plans/DY.md "DY.6"; SPEC §8 Limiter) -------------------------------------------------------
// A group of atom and value blocks (no native engine), stereo, block-wise like the Compressor. With la the Lookahead
// (1.5 / 3 / 6 ms: a stepped curve in seconds, positions 0 / ½ / 1, feeding the const `time` of the windows and the
// delay: constFeeds) and L = round(la · sr):
//   src     In · Gain (a smoothed gain; Gain −12 … +24 dB). Link: Stereo (one gain for both) or L-R (a gain per
//           channel); M-S was removed (Q266, user 2026-11-13: it hard-clipped up to 6 dB)
//   det     True Peak off: latency(|src|, 7) — Stereo linked as max(|L|, |R|), else per channel; True Peak on:
//           latency(truepeak(src), 1) (linked the same way). Both are 7 frames behind src (truepeak's fixed group delay
//           6, plus 1), so toggling True Peak never changes the audio's delay or the reported latency
//   r       max(0, fast_gain_to_db(det) − Ceiling) (dB over the ceiling; fast_ errors ≤ ~1e-6 relative)
//   h       window(r, la, max); g = follow(h, 0, Release) (instant attack; Auto: the Compressor's law, max(follow(h, 0,
//           rel / 4), follow(h, 0, rel · 4) − 3 dB)); b = window(g, la, mean) — the reduction (dB ≥ 0: the meter's)
//   out     latency(src, 6, la) · fast_db_to_gain(−b), then clip(·, −c, c), c = 10^(Ceiling / 20)
// Why the gain alone keeps a sample under the ceiling (Link Stereo / L-R): the output at frame n is src[n − 6 − L], whose
// det is r[m], m = n − L + 1 (det is 7 frames behind src). h[k] (k = n − L + 1 … n) is a max over r[k − L + 1 … k],
// which holds r[m] for every such k, and the follower's instant attack keeps g[k] ≥ h[k] ≥ r[m]: b[n], the mean of
// those L values, is ≥ r[m], so |out| ≤ |src|·10^(−r[m]/20) ≤ c. The final clip only trims float rounding (≈ 1e-6).
// An inf sample is ignored by the detector (window / follow read it as 0: AU.dsp, RB-DY M4) and clipped at c in the
// audio; a huge finite one (1e30: ~600 dB) is a signal like any other and releases by the release law.
// True Peak: r[m] also covers the interpolants between src[n − 6 − L] and the next sample, so the reconstructed output
// stays within the estimator's error (≤ 0.2 dB) of the ceiling.
// Reported latency = L + 6 in both True Peak modes. Every switch (Auto, Link, True Peak) is a stepped value block reaching
// muxes: literals, only the chosen path is scheduled (the default: one window pair, two followers (Auto), no truepeak,
// one linking max). Panel: knobs Gain, Ceiling, Release; Auto; chips Lookahead, Link (Stereo | L-R), True Peak; the
// reduction meter (on b) and the output level meter (on the clip), labelled, with dB scales (AU.dsp, RB-DY L13).
// Sleep (AU.dsp, RB-DY H4): fast_gain_to_db of a silent detector is −inf, so r is exactly 0 for any finite Ceiling
// and its windows and followers decay; the region sleeps once the reduction has released to 2^-30 dB.
inline constexpr double kLimiterDetectorDelay = 7.0;  // frames det runs behind src (truepeak's 6 + 1)
BlockSet limiterBlocks(IdSource& ids, const StockModuleOptions& o = {});

// A module's default group preset with ids from 1 (the group first): what spawning it pastes (ops::pasteBlocks gives
// fresh ids).
BlockSet moduleBlocks(ModuleKind k);

}  // namespace pq::doc
