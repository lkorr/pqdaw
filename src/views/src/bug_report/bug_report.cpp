// The bug report's files and pictures (T5.x-bugreport): pq/views/bug_report/bug_report.h describes them.
#include "pq/views/bug_report/bug_report.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace pq::views::bugreport {

namespace fs = std::filesystem;

namespace {

constexpr const char* kSuggestDir = "suggestions";
constexpr const char* kFixedDir = "fixed";
constexpr const char* kTagFile = "tags.txt";

std::string trimRight(std::string_view s) {
  size_t e = s.size();
  while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
  return std::string(s.substr(0, e));
}

// Start of the first line at or after `from` that begins with `prefix`; npos when none.
size_t findLine(const std::string& s, std::string_view prefix, size_t from) {
  for (size_t p = from; p < s.size();) {
    if (s.compare(p, prefix.size(), prefix) == 0 && (p == 0 || s[p - 1] == '\n')) return p;
    const size_t nl = s.find('\n', p);
    if (nl == std::string::npos) break;
    p = nl + 1;
  }
  return std::string::npos;
}

bool hasTag(std::span<const std::string> tags, std::string_view t) {
  return std::find(tags.begin(), tags.end(), t) != tags.end();
}

std::string joinTags(std::span<const std::string> tags) {
  std::string out;
  for (const std::string& t : tags) {
    if (!out.empty()) out += ", ";
    out += t;
  }
  return out;
}

// shot_N.png → N (shot_N_cursor.png and anything else → 0).
int shotIndex(const std::string& name) {
  if (name.rfind("shot_", 0) != 0 || name.size() < 10 || name.compare(name.size() - 4, 4, ".png") != 0) return 0;
  const std::string mid = name.substr(5, name.size() - 9);
  if (mid.empty() || mid.size() > 6) return 0;
  for (char c : mid)
    if (c < '0' || c > '9') return 0;
  return std::stoi(mid);
}

Result<std::string> readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return Error{ErrorCode::NotFound, "cannot open " + p.string(), {}};
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Written through `<name>.tmp` + rename, so a reader never sees half a file.
Status writeFileAtomic(const fs::path& p, const void* data, size_t size) {
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return Error{ErrorCode::Io, "cannot write " + tmp.string(), {}};
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!f.good()) {
      f.close();
      std::error_code ec;
      fs::remove(tmp, ec);
      return Error{ErrorCode::Io, "write failed: " + tmp.string(), {}};
    }
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return Error{ErrorCode::Io, "cannot replace " + p.string() + ": " + ec.message(), {}};
  }
  return Ok{};
}

Status writePngFile(const fs::path& p, const ui::Image& img, uint64_t& bytes) {
  const std::vector<uint8_t> png = ui::encodePng(img, shotPngOptions());
  if (png.empty()) return Error{ErrorCode::Unknown, "PNG encoding failed (out of memory)", {}};
  bytes += png.size();
  return writeFileAtomic(p, png.data(), png.size());
}

// The description's first non-empty line (for the ticket list).
std::string firstLineOf(std::string_view text) {
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string_view::npos) e = text.size();
    std::string line = trimRight(text.substr(p, e - p));
    const size_t b = line.find_first_not_of(" \t");
    if (b != std::string::npos) return line.substr(b);
    p = e + 1;
  }
  return {};
}

}  // namespace

// ---- report.md ----------------------------------------------------------------------------------------------------

std::string normalizeTag(std::string_view in) {
  std::string t;
  size_t b = 0, e = in.size();
  while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
  for (size_t i = b; i < e; ++i) {
    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(in[i])));
    if (c == ' ') c = '-';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') t += c;
  }
  while (!t.empty() && t.front() == '-') t.erase(t.begin());
  if (t.size() > 24) t.resize(24);
  return t;
}

std::vector<std::string> cleanTags(std::span<const std::string> tags) {
  std::vector<std::string> out;
  for (const std::string& t : tags)
    if (std::string n = normalizeTag(t); !n.empty() && !hasTag(out, n)) out.push_back(std::move(n));
  return out;
}

const std::vector<std::string>& starterTags() {
  static const std::vector<std::string> k = {"ui",    "arrangement", "piano-roll", "circuit", "mixer", "browser",
                                             "audio", "playback",    "tuning",     "perf",    "crash"};
  return k;
}

ReportMd parseReportMd(std::string md) {
  md.erase(std::remove(md.begin(), md.end(), '\r'), md.end());
  ReportMd m;
  size_t p = 0;
  if (md.rfind("# ", 0) == 0) {
    const size_t e = md.find('\n');
    p = e == std::string::npos ? md.size() : e + 1;
    while (p < md.size() && md[p] == '\n') ++p;
    m.pre = md.substr(0, p);
  }
  if (md.compare(p, 5, "tags:") == 0) {
    size_t e = md.find('\n', p);
    if (e == std::string::npos) e = md.size();
    const std::string line = md.substr(p + 5, e - p - 5);
    size_t a = 0;
    while (a <= line.size()) {
      size_t c = line.find(',', a);
      if (c == std::string::npos) c = line.size();
      if (std::string t = normalizeTag(std::string_view(line).substr(a, c - a)); !t.empty() && !hasTag(m.tags, t))
        m.tags.push_back(std::move(t));
      a = c + 1;
    }
    p = e;
    while (p < md.size() && md[p] == '\n') ++p;
  }
  const size_t h = findLine(md, "## ", p);
  if (h == std::string::npos) {
    m.text = trimRight(std::string_view(md).substr(p));
    return m;
  }
  m.text = trimRight(std::string_view(md).substr(p, h - p));
  size_t t = h;
  if (md.compare(h, 14, "## Screenshots") == 0) {
    const size_t nl = md.find('\n', h);
    t = nl == std::string::npos ? std::string::npos : findLine(md, "## ", nl + 1);
    if (t == std::string::npos) t = md.size();
    m.shots = md.substr(h, t - h);
    for (size_t k = findLine(m.shots, "### Shot ", 0); k != std::string::npos; k = findLine(m.shots, "### Shot ", k + 1))
      ++m.shotCount;
  }
  m.tail = md.substr(t);
  return m;
}

std::string composeReportMd(const ReportMd& m, std::string_view text, std::span<const std::string> tags,
                            std::string_view newSections) {
  std::string out = m.pre.empty() ? std::string("# Bug report\n\n") : m.pre;
  if (!tags.empty()) out += "tags: " + joinTags(tags) + "\n\n";
  const std::string t = trimRight(text);
  out += (t.empty() ? std::string("(no description)") : t) + "\n\n";
  std::string shots = m.shots;
  if (!newSections.empty()) {
    if (shots.empty()) shots = "## Screenshots\n\n";
    while (shots.size() < 2 || shots.compare(shots.size() - 2, 2, "\n\n") != 0) shots += "\n";
  }
  out += shots;
  out += newSections;
  if (!newSections.empty() && !m.tail.empty() && newSections.back() != '\n') out += '\n';
  if (!newSections.empty() && !m.tail.empty() && (out.size() < 2 || out.compare(out.size() - 2, 2, "\n\n") != 0)) out += '\n';
  out += m.tail;
  return out;
}

std::string shotSection(int n, uint32_t w, uint32_t h, bool hasCrop, std::string_view body) {
  char head[192];
  std::snprintf(head, sizeof head, "### Shot %d (%ux%u)\n\n![shot %d](shot_%d.png)\n", n, w, h, n, n);
  std::string s = head;
  if (hasCrop) {
    char crop[96];
    std::snprintf(crop, sizeof crop, "![shot %d at the pointer](shot_%d_cursor.png)\n", n, n);
    s += crop;
  }
  s += '\n';
  s += body;
  if (!body.empty() && body.back() != '\n') s += '\n';
  s += '\n';
  return s;
}

std::string reportTitle(bool suggestion, std::string_view humanTime) {
  return std::string(suggestion ? "# Suggestion " : "# Bug report ") + std::string(humanTime) + "\n\n";
}

std::string localStamp(bool forFolder) {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  localtime_s(&tm, &now);
  char buf[32];
  std::strftime(buf, sizeof buf, forFolder ? "%Y-%m-%d_%H-%M-%S" : "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

// ---- pictures -----------------------------------------------------------------------------------------------------

ui::IRect cursorCropRect(uint32_t w, uint32_t h, float x, float y, uint32_t cropW, uint32_t cropH) noexcept {
  const int32_t cw = static_cast<int32_t>(std::min(cropW, w)), ch = static_cast<int32_t>(std::min(cropH, h));
  const int32_t cx = static_cast<int32_t>(std::floor(x)), cy = static_cast<int32_t>(std::floor(y));
  const int32_t x0 = std::clamp(cx - cw / 2, 0, static_cast<int32_t>(w) - cw);
  const int32_t y0 = std::clamp(cy - ch / 2, 0, static_cast<int32_t>(h) - ch);
  return {x0, y0, cw, ch};
}

ui::Image cursorCrop(const ui::Image& img, float x, float y, uint32_t cropW, uint32_t cropH) {
  if (img.width == 0 || img.height == 0 || !(x >= 0.0f && y >= 0.0f && x < static_cast<float>(img.width) &&
                                             y < static_cast<float>(img.height)))
    return {};
  const ui::IRect r = cursorCropRect(img.width, img.height, x, y, cropW, cropH);
  ui::Image out;
  out.width = static_cast<uint32_t>(r.w);
  out.height = static_cast<uint32_t>(r.h);
  out.rgba.resize(size_t(out.width) * out.height * 4);
  for (int32_t row = 0; row < r.h; ++row)
    std::copy_n(img.pixel(static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y + row)), size_t(r.w) * 4,
                out.pixel(0, static_cast<uint32_t>(row)));
  // The crosshair: two arms of 9 px with a 3 px gap at the pointer, magenta with a dark outline, so it reads on any
  // background and never hides the pixel the pointer was on.
  const int32_t px = static_cast<int32_t>(std::floor(x)) - r.x, py = static_cast<int32_t>(std::floor(y)) - r.y;
  auto put = [&](int32_t qx, int32_t qy, uint8_t cr, uint8_t cg, uint8_t cb) {
    if (qx < 0 || qy < 0 || qx >= r.w || qy >= r.h) return;
    uint8_t* p = out.pixel(static_cast<uint32_t>(qx), static_cast<uint32_t>(qy));
    p[0] = cr;
    p[1] = cg;
    p[2] = cb;
    p[3] = 255;
  };
  constexpr int32_t kGap = 3, kArm = 9;
  for (int pass = 0; pass < 2; ++pass) {
    const bool outline = pass == 0;
    for (int32_t d = kGap; d <= kGap + kArm; ++d)
      for (int32_t s : {-1, 1})
        for (int32_t o = outline ? -1 : 0; o <= (outline ? 1 : 0); ++o) {
          const uint8_t c0 = outline ? 0 : 255, c1 = outline ? 0 : 0, c2 = outline ? 0 : 255;
          put(px + s * d, py + o, c0, c1, c2);
          put(px + o, py + s * d, c0, c1, c2);
        }
  }
  return out;
}

ui::Image downscale(const ui::Image& img, uint32_t maxW, uint32_t maxH) {
  if (img.width == 0 || img.height == 0 || maxW == 0 || maxH == 0) return {};
  const double k = std::min({1.0, static_cast<double>(maxW) / img.width, static_cast<double>(maxH) / img.height});
  ui::Image out;
  out.width = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(img.width * k)));
  out.height = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(img.height * k)));
  if (out.width == img.width && out.height == img.height) return img;
  out.rgba.resize(size_t(out.width) * out.height * 4);
  for (uint32_t ty = 0; ty < out.height; ++ty) {
    const uint32_t y0 = static_cast<uint32_t>(uint64_t(ty) * img.height / out.height);
    const uint32_t y1 = std::max(y0 + 1, static_cast<uint32_t>(uint64_t(ty + 1) * img.height / out.height));
    for (uint32_t tx = 0; tx < out.width; ++tx) {
      const uint32_t x0 = static_cast<uint32_t>(uint64_t(tx) * img.width / out.width);
      const uint32_t x1 = std::max(x0 + 1, static_cast<uint32_t>(uint64_t(tx + 1) * img.width / out.width));
      uint32_t sum[4] = {0, 0, 0, 0};
      for (uint32_t y = y0; y < y1 && y < img.height; ++y) {
        const uint8_t* p = img.pixel(x0, y);
        for (uint32_t x = x0; x < x1 && x < img.width; ++x, p += 4) {
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
          sum[3] += p[3];
        }
      }
      const uint32_t n = std::max<uint32_t>(1, (std::min(y1, img.height) - y0) * (std::min(x1, img.width) - x0));
      uint8_t* d = out.pixel(tx, ty);
      for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
    }
  }
  return out;
}

ui::PngOptions shotPngOptions() noexcept {
  // pq_bench_bug_report on real DAW frames (bench/RESULTS.md, 2026-10-31): RGB (alpha is always 255) with the adaptive
  // filter at level 6 is within 1-2 % of level 9's size at a fraction of its time; Z_RLE / fixed Up filters are larger.
  ui::PngOptions o;
  o.rgb = true;
  o.level = 6;
  o.filter = ui::PngOptions::Filter::Adaptive;
  return o;
}

// ---- folders --------------------------------------------------------------------------------------------------------

fs::path kindRoot(const fs::path& root, bool suggestion) { return suggestion ? root / kSuggestDir : root; }

std::vector<std::string> knownTags(const fs::path& root, std::span<const Ticket> tickets) {
  std::vector<std::string> out = starterTags();
  if (auto f = readFile(root / kTagFile)) {
    size_t p = 0;
    const std::string& s = *f;
    while (p < s.size()) {
      size_t e = s.find('\n', p);
      if (e == std::string::npos) e = s.size();
      if (std::string t = normalizeTag(std::string_view(s).substr(p, e - p)); !t.empty() && !hasTag(out, t))
        out.push_back(std::move(t));
      p = e + 1;
    }
  }
  for (const Ticket& tk : tickets)
    for (const std::string& t : tk.tags)
      if (!hasTag(out, t)) out.push_back(t);
  return out;
}

void rememberTags(const fs::path& root, std::span<const std::string> tags) {
  std::vector<std::string> known = knownTags(root, {});
  std::string add;
  for (const std::string& t : cleanTags(tags))
    if (!hasTag(known, t)) {
      add += t + "\n";
      known.push_back(t);
    }
  if (add.empty()) return;
  std::error_code ec;
  fs::create_directories(root, ec);
  std::ofstream f(root / kTagFile, std::ios::binary | std::ios::app);
  f << add;
}

Result<ReportMd> readTicket(const fs::path& dir) {
  auto md = readFile(dir / "report.md");
  if (!md) return std::move(md).error();
  return parseReportMd(std::move(*md));
}

std::vector<Ticket> listTickets(const fs::path& root, bool suggestions) {
  std::vector<Ticket> out;
  const fs::path base = kindRoot(root, suggestions);
  std::error_code ec;
  for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_directory(e2)) continue;
    const std::string name = it->path().filename().string();
    if (name == kFixedDir || name == kSuggestDir) continue;
    auto md = readFile(it->path() / "report.md");
    if (!md) continue;
    const ReportMd m = parseReportMd(std::move(*md));
    Ticket t;
    t.dir = it->path();
    t.id = name;
    t.firstLine = firstLineOf(m.text);
    t.tags = m.tags;
    t.suggestion = suggestions;
    std::vector<std::pair<int, fs::path>> shots;
    for (fs::directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2))
      if (const int n = shotIndex(f->path().filename().string()); n > 0) shots.emplace_back(n, f->path());
    std::sort(shots.begin(), shots.end());
    for (auto& s : shots) t.shots.push_back(std::move(s.second));
    out.push_back(std::move(t));
  }
  std::sort(out.begin(), out.end(), [](const Ticket& a, const Ticket& b) { return a.id > b.id; });
  return out;
}

Result<ui::Image> loadThumbnail(const fs::path& png, uint32_t maxW, uint32_t maxH) {
  auto bytes = readFile(png);
  if (!bytes) return std::move(bytes).error();
  auto img = ui::decodePng(reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
  if (!img) return std::move(img).error();
  return downscale(*img, maxW, maxH);
}

namespace {

// Writes the shots (and their crops) numbered from `first`, appending each one's section to `sections`. On failure
// the files written so far are removed.
Status writeShots(const fs::path& dir, const Draft& d, int first, std::string_view extraBody, std::string& sections,
                  WriteResult& r, std::vector<fs::path>& written) {
  for (size_t i = 0; i < d.shots.size(); ++i) {
    const Shot& s = d.shots[i];
    const int n = first + static_cast<int>(i);
    const fs::path png = dir / ("shot_" + std::to_string(n) + ".png");
    if (Status st = writePngFile(png, s.image, r.bytes); !st) return st;
    written.push_back(png);
    const bool crop = s.crop.width > 0;
    if (crop) {
      const fs::path cp = dir / ("shot_" + std::to_string(n) + "_cursor.png");
      if (Status st = writePngFile(cp, s.crop, r.bytes); !st) return st;
      written.push_back(cp);
    }
    std::string body = std::string(extraBody) + s.context;
    sections += shotSection(n, s.image.width, s.image.height, crop, body);
    r.shotNumbers.push_back(n);
  }
  return Ok{};
}

void removeAll(const std::vector<fs::path>& files) {
  std::error_code ec;
  for (const fs::path& p : files) fs::remove(p, ec);
}

}  // namespace

Result<WriteResult> writeReport(const fs::path& root, const Draft& d, std::string_view folderStamp,
                                std::string_view humanStamp) {
  WriteResult r;
  std::vector<fs::path> written;
  const std::vector<std::string> tags = cleanTags(d.tags);
  std::error_code ec;
  if (!d.target.empty()) {  // append to an open ticket
    auto m = readTicket(d.target);
    if (!m) return Error{ErrorCode::NotFound, "no report.md in " + d.target.string(), {}};
    // Number after the highest picture on either side: a PNG nobody listed and a section whose PNG went missing both
    // keep their number.
    int top = m->shotCount;
    for (fs::directory_iterator it(d.target, ec), end; !ec && it != end; it.increment(ec))
      top = std::max(top, shotIndex(it->path().filename().string()));
    r.dir = d.target;
    std::string sections;
    const std::string added = "- added: " + std::string(humanStamp) + "\n";
    if (Status st = writeShots(d.target, d, top + 1, added, sections, r, written); !st) {
      removeAll(written);
      return std::move(st).error();
    }
    if (d.attachProject && !d.project.empty()) {
      // Never overwrite: a ticket's first project stays; later ones are named after their first shot.
      const bool have = fs::exists(d.target / "project.pqd", ec);
      const std::string name = have ? "project_shot" + std::to_string(top + 1) + ".pqd" : "project.pqd";
      if (Status st = writeFileAtomic(d.target / name, d.project.data(), d.project.size()); !st) {
        removeAll(written);
        return std::move(st).error();
      }
      written.push_back(d.target / name);
      r.bytes += d.project.size();
      sections += "- project at shot " + std::to_string(top + 1) + ": `" + name + "`\n\n";
    }
    const std::string out = composeReportMd(*m, d.text, tags, sections);
    if (Status st = writeFileAtomic(d.target / "report.md", out.data(), out.size()); !st) {
      removeAll(written);
      return std::move(st).error();
    }
    r.bytes += out.size();
    fs::path tagRoot = d.target.parent_path();
    if (tagRoot.filename() == kSuggestDir) tagRoot = tagRoot.parent_path();
    rememberTags(tagRoot, tags);
    return r;
  }
  if (d.shots.empty() && !d.suggestion) return Error{ErrorCode::InvalidArgument, "a new bug report needs a picture (F12 takes one)", {}};
  if (d.suggestion && d.shots.empty() && trimRight(d.text).empty())
    return Error{ErrorCode::InvalidArgument, "a suggestion needs a picture or some text", {}};
  const fs::path base = kindRoot(root, d.suggestion);
  fs::create_directories(base, ec);
  fs::path dir = base / std::string(folderStamp);
  for (int k = 2; fs::exists(dir, ec) && k < 100; ++k) dir = base / (std::string(folderStamp) + "_" + std::to_string(k));
  if (!fs::create_directory(dir, ec)) return Error{ErrorCode::Io, "cannot create " + dir.string() + ": " + ec.message(), {}};
  r.dir = dir;
  std::string sections;
  auto fail = [&](Status st) -> Result<WriteResult> {
    removeAll(written);
    std::error_code e;
    fs::remove(dir, e);  // only if empty: the folder this call made
    return std::move(st).error();
  };
  if (Status st = writeShots(dir, d, 1, {}, sections, r, written); !st) return fail(std::move(st));
  if (d.attachProject && !d.project.empty()) {
    if (Status st = writeFileAtomic(dir / "project.pqd", d.project.data(), d.project.size()); !st) return fail(std::move(st));
    written.push_back(dir / "project.pqd");
    r.bytes += d.project.size();
  }
  ReportMd m;
  m.pre = reportTitle(d.suggestion, humanStamp);
  m.tail = d.trailer;
  const std::string out = composeReportMd(m, d.text, tags, sections);
  if (Status st = writeFileAtomic(dir / "report.md", out.data(), out.size()); !st) return fail(std::move(st));
  r.bytes += out.size();
  rememberTags(root, tags);
  return r;
}

}  // namespace pq::views::bugreport
