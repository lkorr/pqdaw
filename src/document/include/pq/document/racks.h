#pragma once

// GU.9 v2 (docs/plans/GU.md "One hierarchy" 9; user 2026-11-07: "racks still exist but only as a visual indicator …
// internally in the circuit view racks are PARALLEL GROUPS … 1-1 isomorphic with parallel groups, just another way of
// viewing"). A RACK IS NOT STORED: it is DERIVED FROM THE WIRING (the user's choice), so the circuit and the Device view
// show the same objects and editing either edits them.
//
// ---- What a rack is ------------------------------------------------------------------------------------------------
//   chains     SIBLING GROUPS (the same Device::group: a track's own blocks, or the members of one group) that are FED
//              FROM ONE POINT and SUM INTO ONE POINT: each has exactly one wire into its main input and one main wire out
//              of its main output (into a main input, a group's Out terminal or a mixer), and those ends are the same for
//              all of them. Two or more such groups are a rack; ONE is a rack when it holds a chain mixer (below: what
//              Add Rack / Add Chain make), else it is a plain group. Wiring two groups in parallel by hand makes a rack.
//              Chains are ordered by the track's list (Track::devices: the set's order; Move Chain reorders it). The
//              rack's id is its lowest chain id (a view key, never stored).
//   instrument an INSTRUMENT RACK's chains are fed notes (their feed is the note source, or a group's In terminal that
//              carries notes): each chain is an instrument group (instrument_groups.h) of the same notes — the track's
//              one voice engine, each group running per voice — so voicing never doubles; their sums add.
//   macros     value blocks (kValueDevice) at the chains' level carrying the setting kMacroSetting (the slot, 1 …
//              kMacroCount) whose wires reach a block inside the rack's chains (any depth). A MAPPING is such a wire: a
//              Travel wire from the macro's output into a control, amount = max − min in knob position, the control's
//              own knob = min (so position = min + (max − min) · macro, the macro 0 … 1 = Live's 0 … 127). A macro with
//              no mapping is no block (its knob in the Device view waits for one).
//   selector   the value block of role Selector (made as kChainSelectName; 0 … kSelectMax, whole steps) at the chains' level wired into the
//              chains' Chain Zones (made with the first chain zone).
//   variations snapshots of the macro values: setting kVariationPrefix + k (1 …) of each macro block holds its position
//              in variation k; kRandExcludeSetting excludes a macro from Rand.
//
// ---- What a chain holds (blocks inside its group, found by their role: kRoleSetting, never by name) ----------------
//   series     In → its devices → [zones] → Mixer → Out (main links). An Instrument Rack's chain: In → its instrument →
//              [Key Zone] → [Velocity Zone] → its effects → [Chain Zone] → Mixer → Out; the wire out of the instrument's
//              stage is SINGLE-SIGNAL (Wire::single: the voices are summed there, as T4.8 did) unless the rack's chains
//              run per voice (setVoiceChains). An Audio Effect Rack's chain: In → [Chain Zone] → its devices → Mixer → Out
//              (the select zone gates the chain's input, as T4.8 did).
//   mixer      the group of role Mixer: value blocks Volume (dB, −70 … +6, through dB → gain), Pan (−1 … 1), Gate (0 / 1:
//              the chain plays — its activator and solo, below, and Volume above −70 dB; written by the edits) and a
//              constant 1; atoms g = Volume · Gate, gl = g · min(1, 1 − Pan), gr = g · min(1, 1 + Pan),
//              Out = mux(Gate, pack(0, 0), gain(In, pack(gl, gr))) (the gain smoothed 2 ms: the old chain mixer's law
//              and stage). Undriven, Volume / Pan are runtime params (a drag is a param write); the atoms run at block
//              rate. GU.f7: the undriven Gate chooses, so it lowers as a literal (lower.h "Structural value blocks"): a
//              muted chain folds to silence and nothing of it is computed; muting recompiles the unit (the swap's
//              ≈ 11 ms crossfade; a driven Gate stays a runtime choice and cuts at once).
//   on / solo  settings kChainOnSetting (absent: on) and kChainSoloSetting of the chain group (any soloed chain: only
//              soloed chains play; Meta: the Gate carries the sound).
//   zones      groups of roles KeyZone / VelocityZone / SelectZone, only while a zone is on (not its whole range:
//              an off zone costs nothing): value blocks Lo / Fade Lo / Fade Hi / Hi (the zone), constants, atoms
//              g = clip(min((v − lo + ε)/(fadeLo − lo + ε), (hi − v + ε)/(hi − fadeHi + ε)), 0, 1) and Out = gain(In, g)
//              where v is the voice atom voice.noteHz (in ln: Hz zones, ε = 1e-4) or voice.noteDegree (Degree zones),
//              voice.velocity · 127, or the selector (the zone's value block of role ZoneValue fed by it); ε = 1e-3.
//
// Not RT code (UI / compile threads). O(blocks + wires) per track.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/document/model.h"

namespace pq::doc {

// A chain is a group: its id is the group's DeviceId (the views' name for it).
using ChainId = DeviceId;

inline constexpr uint32_t kMacroCount = 16;
inline constexpr uint32_t kDefaultMacros = 8;   // the macro knobs a rack shows (more once mapped)
inline constexpr double kMacroMax = 127.0;      // a macro shows 0 … 127 (its block is 0 … 1)
inline constexpr double kSelectMax = 127.0;     // the chain selector's range (and a select zone's)
inline constexpr double kVelocityMax = 127.0;

// ---- Roles (GU.f1) -----------------------------------------------------------------------------------------------
// The blocks a rack is made of carry their ROLE as the setting kRoleSetting (a RackRole value), written when they are
// made (chainBlocks, zoneBlocks, the selector). Racks find them by role, never by name: a rename (any of the names
// below) changes the label only, and a user's own group called "Mixer" is a plain group. Not derived from wiring: a
// chain mixer and a zone are gain stages driven by value blocks, the same shape as any user group (telling them apart
// from the wires would mean matching each one's exact atom graph, a rule per type). Copies (Duplicate Chain, paste)
// keep the role with the block.
enum class RackRole : uint8_t {
  None = 0,
  Mixer,         // a chain's mixer group
  Volume,        // its value blocks
  Pan,
  Gate,
  KeyZone,       // a chain's zone groups
  VelocityZone,
  SelectZone,
  ZoneLo,        // a zone group's value blocks
  ZoneFadeLo,
  ZoneFadeHi,
  ZoneHi,
  ZoneValue,     // a select zone's input (the selector adds into it)
  Selector,      // the rack's Chain Select value block (at the chains' level)
};
inline constexpr std::string_view kRoleSetting = "rackRole";
RackRole rackRole(const Device& d) noexcept;  // None: no (valid) role setting

// The names (Device::name) the role blocks are MADE with (labels only: found by role).
inline constexpr std::string_view kChainSelectName = "Chain Select";
inline constexpr std::string_view kChainMixerName = "Mixer";
inline constexpr std::string_view kChainVolumeName = "Volume", kChainPanName = "Pan", kChainGateName = "Gate";
inline constexpr std::string_view kKeyZoneName = "Key Zone", kVelocityZoneName = "Velocity Zone",
                                  kSelectZoneName = "Chain Zone";
inline constexpr std::string_view kZoneLoName = "Lo", kZoneFadeLoName = "Fade Lo", kZoneFadeHiName = "Fade Hi",
                                  kZoneHiName = "Hi", kZoneValueName = "Select";
// Settings (kRoleSetting above).
inline constexpr std::string_view kMacroSetting = "macro";              // a macro block's slot (1 … kMacroCount)
inline constexpr std::string_view kVariationPrefix = "var";             // "var<k>": its position in variation k
inline constexpr std::string_view kRandExcludeSetting = "randExclude";  // 1: Rand leaves it alone
inline constexpr std::string_view kChainOnSetting = "chainOn";          // a chain group: 0 = its activator is off
inline constexpr std::string_view kChainSoloSetting = "chainSolo";      // a chain group: 1 = soloed
inline constexpr std::string_view kKeyUnitSetting = "keyUnit";          // a chain group: 1 = Degree key zones
// Whether `name` is a setting a value block / a group may carry for its rack (checks.cpp).
bool rackValueSetting(std::string_view name) noexcept;
bool rackGroupSetting(std::string_view name) noexcept;

// ---- Zones (T4.8's model, kept: the zone groups hold these four values) ---------------------------------------------
// Gain 0 below `lo` and above `hi`, rising linearly from `lo` to `fadeLo`, 1 from `fadeLo` to `fadeHi`, falling to
// `hi`. A zone covering its whole range is OFF (no zone group).
enum class ZoneKind : uint8_t { Key, Velocity, Select };
// Key zones: in Hz (fades linear in log frequency) or in degrees of each note's own tuning (voice.noteDegree).
enum class KeyZoneUnit : uint8_t { Hz, Degree };
struct Zone {
  double lo = 0.0, fadeLo = 0.0, fadeHi = 0.0, hi = 0.0;
  friend constexpr bool operator==(const Zone&, const Zone&) noexcept = default;
};
struct ZoneRange {
  double min = 0.0, max = 0.0;
};
ZoneRange zoneRange(ZoneKind kind, KeyZoneUnit unit) noexcept;  // Key: Hz 1 … 100000, Degree −1024 … 1024; else 0 … 127
Zone fullZone(ZoneKind kind, KeyZoneUnit unit) noexcept;        // {min, min, max, max}: off
bool zoneOff(const Zone& z, ZoneKind kind, KeyZoneUnit unit) noexcept;
bool zoneValid(const Zone& z, ZoneKind kind, KeyZoneUnit unit) noexcept;  // ordered, finite, inside the range
std::string_view zoneGroupName(ZoneKind kind) noexcept;  // the name a zone group is made with
RackRole zoneRole(ZoneKind kind) noexcept;               // KeyZone / VelocityZone / SelectZone

// ---- The derived rack --------------------------------------------------------------------------------------------
struct RackChain {
  const Device* group = nullptr;
  DeviceId id;                              // the chain group
  std::string name;                         // its name ("" → "Chain")
  std::vector<const Device*> devices;       // its series In → … → Out without the zones and the mixer (a nested rack:
                                            // its first chain stands for it)
  DeviceId mixer, volumeBlock, panBlock, gateBlock;  // invalid: no chain mixer (a hand-wired parallel group)
  float volumeDb = 0.0f, pan = 0.0f;        // the Volume / Pan knobs (dB; −1 … 1)
  bool active = true, solo = false;         // kChainOnSetting, kChainSoloSetting
  Zone key, velocity, select;               // full (off) when there is no zone group
  DeviceId keyZone, velocityZone, selectZone;  // the zone groups (invalid: off)
  const Zone& zone(ZoneKind k) const noexcept {
    return k == ZoneKind::Key ? key : k == ZoneKind::Velocity ? velocity : select;
  }
  DeviceId zoneGroup(ZoneKind k) const noexcept {
    return k == ZoneKind::Key ? keyZone : k == ZoneKind::Velocity ? velocityZone : selectZone;
  }
};
struct RackMapping {
  WireId wire;
  uint32_t macro = 0;  // 0 … kMacroCount − 1
  DeviceId device;
  std::string param;
  double min = 0.0, max = 1.0;  // knob positions
};
struct RackMacro {
  DeviceId block;  // invalid: not made (unmapped)
  std::string name;
  double value = 0.0;  // 0 … kMacroMax
  bool excluded = false;
};
struct RackInfo {
  DeviceId id;           // the lowest chain id (a view key)
  DeviceId parent;       // the group the chains sit in (invalid: the track's own blocks)
  TrackId track;
  bool instrument = false;
  std::vector<RackChain> chains;  // Track::devices order
  std::array<RackMacro, kMacroCount> macros{};
  std::vector<RackMapping> mappings;  // by wire id
  DeviceId selector;
  double selectorValue = 0.0;  // 0 … kSelectMax (its knob)
  KeyZoneUnit keyUnit = KeyZoneUnit::Hz;
  bool voiceChains = false;  // an Instrument Rack whose chains' effects run per voice
  uint32_t variations = 0;
  const RackChain* findChain(DeviceId chain) const noexcept;
  int32_t chainIndex(DeviceId chain) const noexcept;  // −1: none
  // Whether `d` is one of its chains, inside one (any depth), a macro or its selector.
  bool holds(const Project& p, DeviceId d) const;
  std::string macroName(uint32_t k) const;  // the block's name, else "Macro <k + 1>"
};

// The racks of track `t` (outermost first: a rack nested inside a chain comes after it).
std::vector<RackInfo> findRacks(const Project& p, const Track& t);
// The rack that `id` (a chain group, a macro, the selector) belongs to; nullopt: none.
std::optional<RackInfo> rackOf(const Project& p, DeviceId id);
// The rack whose chain `d` is (nullopt: `d` is no rack chain).
std::optional<RackInfo> rackOfChain(const Project& p, const Track& t, DeviceId d);
// A group's series: the blocks from its In terminal along main links to its Out terminal (a fan-out into a nested
// rack's chains: the first of them, then on from where they sum).
std::vector<const Device*> groupSeries(const Project& p, const Track& t, DeviceId group);

// ---- Blocks a rack is made of (the edit builders paste these) -----------------------------------------------------
// An empty chain group `name` (inside `parent`, invalid: a track block) with its chain mixer: In → Mixer → Out (an
// Instrument Rack's empty chain: Mixer → Out only, until it has an instrument). Wires only inside the chain; the feed
// and sum wires are the caller's. The chain group first.
BlockSet chainBlocks(IdSource& ids, DeviceId parent, std::string name, bool instrument);
// The zone group of `kind` inside chain `chain` holding `zone` (In → gain → Out; v from its voice atom, or the zone's
// kZoneValueName block for a select zone). Its wires are internal (the caller splices it and feeds the selector).
BlockSet zoneBlocks(IdSource& ids, DeviceId chain, ZoneKind kind, KeyZoneUnit unit, const Zone& zone);
// The value curves the rack's blocks use.
dsp::Curve chainVolumeCurve();
dsp::Curve chainPanCurve();
dsp::Curve zoneCurve(ZoneKind kind, KeyZoneUnit unit);
dsp::Curve macroCurve();     // 0 … 1, linear
dsp::Curve selectorCurve();  // 0 … kSelectMax, whole steps

}  // namespace pq::doc
