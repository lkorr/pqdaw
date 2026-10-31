#pragma once

// The browser index (T5.9; SPEC §9.1; docs/plans/M5.md T5.9): every indexable file under the places (the User
// Library, the current project, added folders), as flat columns (struct of arrays) plus one string pool.
//
// An Index is immutable once published: the indexer (indexer.h) builds a new one for every batch of changes and
// hands it out as shared_ptr<const Index>, so the browser and the search worker read it without locks. Entries are
// grouped by directory (directories in depth-first order, each directory's entries sorted by case-folded name), which
// lets a rebuild merge a fresh directory listing against the old one in one pass and copy unchanged directories as
// column slices.
//
// Only files of a kind the browser shows are indexed (indexedKind): audio files the media probe reads (Samples), the
// user-library formats (.pqpreset, .pqdevice, .pqtuning, .pqclip), Scala tunings (.scl), MIDI clips (.mid), patches
// (.pqp) and projects (.pqd). Directories are kept as their own table. Hidden and system files, names starting with
// '.', and reparse points (junctions, symlinks: no cycles) are skipped.
//
// Not RT code (no audio-thread use at all). Paths are UTF-8; directory separators in stored relative paths are '\\'.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pq::library {

enum class PlaceKind : uint8_t { UserLibrary, Project, Folder };

// A place root as configured (the indexer's input).
struct PlaceSpec {
  std::string root;  // absolute UTF-8 path of the directory
  PlaceKind kind = PlaceKind::Folder;
  std::string name;  // shown in the sidebar ("User Library", the project's name, the folder's name)
  friend bool operator==(const PlaceSpec&, const PlaceSpec&) = default;
};

// What a file is (by extension).
enum class ItemKind : uint8_t {
  Sample,   // audio: .wav .wave .w64 .rf64 .bw64 .aif .aiff .aifc .flac .mp3 .ogg
  Preset,   // .pqpreset: a device or rack preset (T5.11)
  Device,   // .pqdevice: a user device (T5.11)
  Tuning,   // .pqtuning, .scl
  Clip,     // .pqclip, .mid, .midi
  Patch,    // .pqp: a patch / subcircuit file
  Project,  // .pqd
  Count
};
std::string_view itemKindName(ItemKind k) noexcept;
// The kind of a file name by its extension (case-insensitive), or nullopt for files the index leaves out.
std::optional<ItemKind> indexedKind(std::string_view fileName) noexcept;

// The browser's categories (virtual views over the index; SPEC §9.1). None = in places only (projects, presets whose
// device kind is unknown). Grooves stays empty until M10.
enum class Category : uint8_t {
  None,
  Instruments,
  AudioEffects,
  Racks,
  Samples,
  Clips,
  Tunings,
  Patches,  // "Patches/Subcircuits"
  Grooves,
  Count
};
std::string_view categoryName(Category c) noexcept;

// Entry flags.
inline constexpr uint8_t kFlagProbeFailed = 1u << 0;   // a Sample the probe could not read (fields zero)
inline constexpr uint8_t kFlagFramesScanned = 1u << 1; // media::SampleInfo::framesScanned
inline constexpr uint8_t kFlagHasLoop = 1u << 2;       // a sustain loop in the file
inline constexpr uint8_t kFlagOffline = 1u << 3;       // a cloud placeholder (recall on access): never opened
inline constexpr uint8_t kFlagBpmFromName = 1u << 4;   // bpm came from the file name, not an acid chunk

using FileIdBytes = std::array<uint8_t, 16>;

// One entry as a row (the builder, the journal and tests use it; the Index stores columns).
struct EntryRecord {
  std::string name;  // file name with extension (UTF-8)
  ItemKind kind = ItemKind::Sample;
  Category category = Category::None;
  uint8_t flags = 0;
  uint64_t size = 0;
  int64_t mtime = 0;            // FILETIME ticks (100 ns since 1601), as media::FileIdentity::mtime
  uint64_t volume = 0;          // volume serial (media::FileIdentity::volumeSerial)
  FileIdBytes fileId{};         // NTFS 128-bit file ID; zero where the file system has none
  // Probe fields (Samples; zero otherwise).
  uint64_t frames = 0;
  uint32_t rate = 0;
  uint16_t channels = 0;
  uint8_t bits = 0;             // bits per sample as stored (0 for MP3 / Vorbis)
  uint8_t container = 0;        // media::Container
  int8_t rootKey = -1;          // MIDI note, −1 = unknown
  float bpm = 0;                // acid chunk tempo, else the file-name BPM, else 0
  friend bool operator==(const EntryRecord&, const EntryRecord&) = default;
};

// The 64-bit character set of a (case-folded) string: bit c for each byte present (a–z → 0–25, 0–9 → 26–35, every
// other byte hashed into 36–63). A term can be a subsequence of a name only if term mask & ~name mask == 0.
uint64_t charMask(std::string_view s) noexcept;
// ASCII case folding (bytes ≥ 0x80 are left alone: UTF-8 multibyte sequences compare bytewise).
inline char foldChar(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
// Byte order of the case-folded strings, ties broken by the raw bytes (a total order; the directory sort).
int compareFolded(std::string_view a, std::string_view b) noexcept;
// The name without its extension (the part search matches).
std::string_view stemOf(std::string_view fileName) noexcept;

struct Index {
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  std::vector<PlaceSpec> places;
  std::vector<uint8_t> placeOnline;  // per place: its root could be listed in the last scan (1) or not (0)

  // Directories, depth-first per place (a place's root directory first: name empty, parent kNone).
  struct Dirs {
    std::vector<uint32_t> parent;
    std::vector<uint32_t> nameOff;
    std::vector<uint16_t> nameLen;
    std::vector<uint16_t> place;
    std::vector<uint16_t> depth;       // 0 = a place root
    std::vector<uint32_t> firstEntry;  // this directory's entries: [firstEntry, firstEntry + entryCount)
    std::vector<uint32_t> entryCount;
    template <class F> void forEachColumn(F&& f) {
      f(parent); f(nameOff); f(nameLen); f(place); f(depth); f(firstEntry); f(entryCount);
    }
    template <class F> void forEachColumn(F&& f) const {
      f(parent); f(nameOff); f(nameLen); f(place); f(depth); f(firstEntry); f(entryCount);
    }
    std::size_t size() const noexcept { return parent.size(); }
  } dirs;

  // Files.
  struct Entries {
    std::vector<uint32_t> dir;
    std::vector<uint32_t> nameOff;
    std::vector<uint16_t> nameLen;
    std::vector<uint16_t> stemLen;
    std::vector<uint8_t> kind;      // ItemKind
    std::vector<uint8_t> category;  // Category
    std::vector<uint8_t> flags;
    std::vector<uint8_t> depth;     // the directory depth below the place root (search's depth penalty), ≤ 255
    std::vector<uint64_t> size;
    std::vector<int64_t> mtime;
    std::vector<uint64_t> volume;
    std::vector<FileIdBytes> fileId;
    std::vector<uint64_t> frames;
    std::vector<uint32_t> rate;
    std::vector<uint16_t> channels;
    std::vector<uint8_t> bits;
    std::vector<uint8_t> container;
    std::vector<int8_t> rootKey;
    std::vector<float> bpm;
    std::vector<uint64_t> mask;     // charMask of the folded stem
    template <class F> void forEachColumn(F&& f) {
      f(dir); f(nameOff); f(nameLen); f(stemLen); f(kind); f(category); f(flags); f(depth); f(size); f(mtime);
      f(volume); f(fileId); f(frames); f(rate); f(channels); f(bits); f(container); f(rootKey); f(bpm); f(mask);
    }
    template <class F> void forEachColumn(F&& f) const {
      f(dir); f(nameOff); f(nameLen); f(stemLen); f(kind); f(category); f(flags); f(depth); f(size); f(mtime);
      f(volume); f(fileId); f(frames); f(rate); f(channels); f(bits); f(container); f(rootKey); f(bpm); f(mask);
    }
  } entries;

  std::vector<char> strings;  // every name, UTF-8, unterminated

  uint32_t entryCount() const noexcept { return static_cast<uint32_t>(entries.dir.size()); }
  uint32_t dirCount() const noexcept { return static_cast<uint32_t>(dirs.parent.size()); }

  std::string_view name(uint32_t e) const noexcept {
    return {strings.data() + entries.nameOff[e], entries.nameLen[e]};
  }
  std::string_view stem(uint32_t e) const noexcept {
    return {strings.data() + entries.nameOff[e], entries.stemLen[e]};
  }
  std::string_view dirName(uint32_t d) const noexcept { return {strings.data() + dirs.nameOff[d], dirs.nameLen[d]}; }
  uint32_t placeOf(uint32_t e) const noexcept { return dirs.place[entries.dir[e]]; }
  ItemKind kind(uint32_t e) const noexcept { return static_cast<ItemKind>(entries.kind[e]); }
  Category category(uint32_t e) const noexcept { return static_cast<Category>(entries.category[e]); }
  // Seconds of audio (Samples with a known rate), else 0.
  double seconds(uint32_t e) const noexcept {
    return entries.rate[e] != 0 ? static_cast<double>(entries.frames[e]) / entries.rate[e] : 0.0;
  }

  // Relative path of a directory below its place root ("" for the root, "Drums\\Kicks").
  std::string dirRelPath(uint32_t d) const;
  // Absolute paths (place root + relative path), UTF-8 with '\\' separators.
  std::string dirPath(uint32_t d) const;
  std::string path(uint32_t e) const;

  EntryRecord record(uint32_t e) const;
  // Appends one entry / directory (builder use).
  uint32_t addDir(uint32_t parent, std::string_view name, uint16_t place, uint16_t depth);
  void addEntry(uint32_t dir, const EntryRecord& r);

  // The directory with this relative path in place `place` (case-insensitive), or kNone. Linear in the path's
  // depth × siblings (tests and the browser's "reveal").
  uint32_t findDir(uint32_t place, std::string_view relPath) const;
  // The entry with this absolute path (case-insensitive), or kNone.
  uint32_t findPath(std::string_view absPath) const;
};

// Folded key of an absolute path for comparisons: '/' → '\\', ASCII lower case, no trailing separator.
std::string foldedPathKey(std::string_view path);

// Category of an entry from its kind, its place and relative directory, and (presets / user devices) the kind its
// file declares (`sniffed`: Instruments, AudioEffects, Racks or None). User Library layout (T5.11): Racks/… → Racks;
// Presets/<Device>/… and Defaults/<Device>.pqpreset → the stock device's kind (doc::stockDevices(), by display or
// patch name).
Category categorize(ItemKind kind, PlaceKind place, std::string_view relDir, std::string_view fileName,
                    Category sniffed) noexcept;

// The kind a preset or user device file declares, from its first bytes (`head`, up to 4 KiB): a "rack" key → Racks;
// else the first "kind" value: "instrument" → Instruments, "audioEffect" → AudioEffects; else None. Provisional until
// T5.11 fixes the preset format (QUESTIONS Q?-T5.9-a).
Category sniffDeviceCategory(std::string_view head) noexcept;

// A stock device as a category item (Instruments / Audio Effects list the stock devices themselves, as in Live).
struct StockItem {
  std::string_view name;
  std::string_view patch;
  Category category = Category::AudioEffects;
};
std::vector<StockItem> stockItems();

}  // namespace pq::library
