#include "pq/ui/image.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace pq::ui {

namespace {

constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

void putU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

uint32_t getU32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void putChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
  putU32(out, static_cast<uint32_t>(len));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  if (len) out.insert(out.end(), data, data + len);
  const uLong crc = crc32(0L, out.data() + start, static_cast<uInt>(len + 4));
  putU32(out, static_cast<uint32_t>(crc));
}

uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
  if (pb <= pc) return static_cast<uint8_t>(b);
  return static_cast<uint8_t>(c);
}

// Applies filter `f` to `row` (prev = previous raw row or null), writing into out.
void filterRow(int f, const uint8_t* row, const uint8_t* prev, size_t n, size_t bpp, uint8_t* out) {
  for (size_t i = 0; i < n; ++i) {
    const int a = i >= bpp ? row[i - bpp] : 0;
    const int b = prev ? prev[i] : 0;
    const int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
    int pred = 0;
    switch (f) {
      case 1: pred = a; break;
      case 2: pred = b; break;
      case 3: pred = (a + b) / 2; break;
      case 4: pred = paeth(a, b, c); break;
      default: break;
    }
    out[i] = static_cast<uint8_t>(row[i] - pred);
  }
}

}  // namespace

std::vector<uint8_t> encodePng(const Image& img) {
  const size_t stride = size_t(img.width) * 4;
  std::vector<uint8_t> raw((stride + 1) * img.height);
  std::vector<uint8_t> trial(stride);
  for (uint32_t y = 0; y < img.height; ++y) {
    const uint8_t* row = img.rgba.data() + y * stride;
    const uint8_t* prev = y ? row - stride : nullptr;
    uint8_t* dst = raw.data() + y * (stride + 1);
    // Standard heuristic (libpng's): pick the filter whose output has the smallest sum of |signed byte|.
    uint64_t best = UINT64_MAX;
    for (int f = 0; f < 5; ++f) {
      filterRow(f, row, prev, stride, 4, trial.data());
      uint64_t sum = 0;
      for (uint8_t v : trial) sum += static_cast<uint64_t>(std::abs(static_cast<int8_t>(v)));
      if (sum < best) {
        best = sum;
        dst[0] = static_cast<uint8_t>(f);
        std::memcpy(dst + 1, trial.data(), stride);
      }
    }
  }
  uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> z(zlen);
  if (compress2(z.data(), &zlen, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return {};  // out of memory

  std::vector<uint8_t> out(kSignature, kSignature + 8);
  uint8_t ihdr[13];
  ihdr[0] = static_cast<uint8_t>(img.width >> 24);
  ihdr[1] = static_cast<uint8_t>(img.width >> 16);
  ihdr[2] = static_cast<uint8_t>(img.width >> 8);
  ihdr[3] = static_cast<uint8_t>(img.width);
  ihdr[4] = static_cast<uint8_t>(img.height >> 24);
  ihdr[5] = static_cast<uint8_t>(img.height >> 16);
  ihdr[6] = static_cast<uint8_t>(img.height >> 8);
  ihdr[7] = static_cast<uint8_t>(img.height);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 6;   // colour type RGBA
  ihdr[10] = 0;  // deflate
  ihdr[11] = 0;  // adaptive filtering
  ihdr[12] = 0;  // no interlace
  putChunk(out, "IHDR", ihdr, 13);
  putChunk(out, "IDAT", z.data(), zlen);
  putChunk(out, "IEND", nullptr, 0);
  return out;
}

Result<Image> decodePng(const uint8_t* data, size_t size) {
  auto fail = [](ErrorCode c, const char* msg) { return Error{c, std::string("png: ") + msg, {}}; };
  if (size < 8 || std::memcmp(data, kSignature, 8) != 0) return fail(ErrorCode::Parse, "bad signature");
  size_t pos = 8;
  uint32_t w = 0, h = 0;
  uint8_t colorType = 0;
  bool haveHeader = false, haveEnd = false;
  std::vector<uint8_t> idat;
  std::array<uint8_t, 256 * 4> palette{};
  uint32_t paletteSize = 0;
  for (auto& e : palette) e = 255;
  while (pos + 12 <= size && !haveEnd) {
    const uint32_t len = getU32(data + pos);
    if (len > size - pos - 12) return fail(ErrorCode::Parse, "truncated chunk");
    const uint8_t* type = data + pos + 4;
    const uint8_t* body = data + pos + 8;
    const uint32_t crc = getU32(body + len);
    if (crc32(0L, type, len + 4) != crc) return fail(ErrorCode::Parse, "chunk CRC mismatch");
    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (len != 13) return fail(ErrorCode::Parse, "bad IHDR");
      w = getU32(body);
      h = getU32(body + 4);
      if (body[8] != 8) return fail(ErrorCode::Unsupported, "only 8-bit depth is supported");
      colorType = body[9];
      if (body[10] != 0 || body[11] != 0) return fail(ErrorCode::Parse, "unknown compression/filter method");
      if (body[12] != 0) return fail(ErrorCode::Unsupported, "interlaced PNG");
      if (colorType != 0 && colorType != 2 && colorType != 3 && colorType != 4 && colorType != 6)
        return fail(ErrorCode::Parse, "bad colour type");
      if (w == 0 || h == 0 || w > 32768 || h > 32768) return fail(ErrorCode::Unsupported, "dimensions out of range");
      haveHeader = true;
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      if (len % 3 != 0 || len / 3 > 256) return fail(ErrorCode::Parse, "bad PLTE");
      paletteSize = len / 3;
      for (uint32_t i = 0; i < paletteSize; ++i) {
        palette[i * 4 + 0] = body[i * 3 + 0];
        palette[i * 4 + 1] = body[i * 3 + 1];
        palette[i * 4 + 2] = body[i * 3 + 2];
      }
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      if (colorType == 3)
        for (uint32_t i = 0; i < len && i < 256; ++i) palette[i * 4 + 3] = body[i];
      // tRNS colour keys for grey/RGB are ignored (not produced by our tools).
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), body, body + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      haveEnd = true;
    } else if ((type[0] & 0x20) == 0) {
      return fail(ErrorCode::Unsupported, "unknown critical chunk");
    }
    pos += size_t(len) + 12;
  }
  if (!haveHeader || !haveEnd) return fail(ErrorCode::Parse, "missing IHDR or IEND");
  if (colorType == 3 && paletteSize == 0) return fail(ErrorCode::Parse, "palette image without PLTE");

  const uint32_t channels = colorType == 0 ? 1 : colorType == 2 ? 3 : colorType == 3 ? 1 : colorType == 4 ? 2 : 4;
  const size_t stride = size_t(w) * channels;
  std::vector<uint8_t> raw((stride + 1) * h);
  uLongf rawLen = static_cast<uLongf>(raw.size());
  if (uncompress(raw.data(), &rawLen, idat.data(), static_cast<uLong>(idat.size())) != Z_OK || rawLen != raw.size())
    return fail(ErrorCode::Parse, "corrupt image data");

  // Unfilter in place. Row y's filtered bytes start at raw[y*(stride+1)+1].
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* row = raw.data() + y * (stride + 1) + 1;
    const uint8_t* prev = y ? row - (stride + 1) : nullptr;
    const uint8_t f = row[-1];
    if (f > 4) return fail(ErrorCode::Parse, "bad filter type");
    for (size_t i = 0; i < stride; ++i) {
      const int a = i >= channels ? row[i - channels] : 0;
      const int b = prev ? prev[i] : 0;
      const int c = (prev && i >= channels) ? prev[i - channels] : 0;
      int pred = 0;
      switch (f) {
        case 1: pred = a; break;
        case 2: pred = b; break;
        case 3: pred = (a + b) / 2; break;
        case 4: pred = paeth(a, b, c); break;
        default: break;
      }
      row[i] = static_cast<uint8_t>(row[i] + pred);
    }
  }

  Image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* src = raw.data() + y * (stride + 1) + 1;
    uint8_t* dst = img.pixel(0, y);
    for (uint32_t x = 0; x < w; ++x, dst += 4) {
      const uint8_t* s = src + size_t(x) * channels;
      switch (colorType) {
        case 0: dst[0] = dst[1] = dst[2] = s[0]; dst[3] = 255; break;
        case 2: dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 255; break;
        case 3: {
          if (s[0] >= paletteSize) return fail(ErrorCode::Parse, "palette index out of range");
          std::memcpy(dst, &palette[size_t(s[0]) * 4], 4);
          break;
        }
        case 4: dst[0] = dst[1] = dst[2] = s[0]; dst[3] = s[1]; break;
        default: std::memcpy(dst, s, 4); break;
      }
    }
  }
  return img;
}

Status writePng(const std::string& path, const Image& img) {
  if (img.width == 0 || img.height == 0 || img.rgba.size() != size_t(img.width) * img.height * 4)
    return Error{ErrorCode::InvalidArgument, "writePng: image is empty or malformed", {}};
  const std::vector<uint8_t> bytes = encodePng(img);
  if (bytes.empty()) return Error{ErrorCode::Unknown, "writePng: compression failed", {}};
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path.c_str(), "wb"), &std::fclose);
  if (!f) return Error{ErrorCode::Io, "cannot open for writing: " + path, {}};
  if (std::fwrite(bytes.data(), 1, bytes.size(), f.get()) != bytes.size())
    return Error{ErrorCode::Io, "write failed: " + path, {}};
  return Ok{};
}

Result<Image> readPng(const std::string& path) {
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path.c_str(), "rb"), &std::fclose);
  if (!f) return Error{ErrorCode::NotFound, "cannot open: " + path, {}};
  std::vector<uint8_t> bytes;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f.get())) > 0) bytes.insert(bytes.end(), buf, buf + n);
  auto img = decodePng(bytes.data(), bytes.size());
  if (!img) {
    Error e = std::move(img).error();
    e.message += " (" + path + ")";
    return e;
  }
  return img;
}

namespace {

uint32_t worstChannelDiff(const uint8_t* p, const uint8_t* q) noexcept {
  uint32_t worst = 0;
  for (size_t c = 0; c < 4; ++c) {
    const uint32_t diff = static_cast<uint32_t>(std::abs(int(p[c]) - int(q[c])));
    worst = diff > worst ? diff : worst;
  }
  return worst;
}

}  // namespace

ImageDiff compareImages(const Image& a, const Image& b, uint32_t channelTolerance) {
  ImageDiff d;
  d.channelTolerance = channelTolerance;
  d.sameSize = a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size() &&
               a.rgba.size() == size_t(a.width) * a.height * 4;
  if (!d.sameSize) return d;
  d.pixelCount = uint64_t(a.width) * a.height;
  uint32_t x0 = a.width, y0 = a.height, x1 = 0, y1 = 0;
  const size_t stride = size_t(a.width) * 4;
  for (uint32_t y = 0; y < a.height; ++y) {
    const uint8_t* ra = a.rgba.data() + y * stride;
    const uint8_t* rb = b.rgba.data() + y * stride;
    if (std::memcmp(ra, rb, stride) == 0) continue;  // identical rows: the bulk of a golden comparison
    for (uint32_t x = 0; x < a.width; ++x) {
      const uint32_t worst = worstChannelDiff(ra + size_t(x) * 4, rb + size_t(x) * 4);
      if (worst > d.maxChannelDiff) d.maxChannelDiff = worst;
      if (worst > channelTolerance) {
        ++d.pixelsOverTolerance;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x + 1);
        y1 = std::max(y1, y + 1);
      }
    }
  }
  if (d.pixelsOverTolerance == 0) return d;  // the common case: no second pass
  d.boxX0 = x0;
  d.boxY0 = y0;
  d.boxX1 = x1;
  d.boxY1 = y1;
  // Outlier mask over the bounding box only, then an 8-neighbour test per outlier.
  const uint32_t bw = x1 - x0, bh = y1 - y0;
  std::vector<uint8_t> mask(size_t(bw) * bh);
  for (uint32_t y = y0; y < y1; ++y)
    for (uint32_t x = x0; x < x1; ++x)
      mask[size_t(y - y0) * bw + (x - x0)] = worstChannelDiff(a.pixel(x, y), b.pixel(x, y)) > channelTolerance;
  for (uint32_t y = 0; y < bh; ++y)
    for (uint32_t x = 0; x < bw; ++x) {
      if (!mask[size_t(y) * bw + x]) continue;
      bool neighbour = false;
      for (uint32_t ny = y ? y - 1 : 0; ny <= std::min(bh - 1, y + 1) && !neighbour; ++ny)
        for (uint32_t nx = x ? x - 1 : 0; nx <= std::min(bw - 1, x + 1); ++nx)
          if ((nx != x || ny != y) && mask[size_t(ny) * bw + nx]) {
            neighbour = true;
            break;
          }
      d.clusteredOutliers += neighbour ? 1u : 0u;
    }
  return d;
}

bool passesGolden(const ImageDiff& d, const GoldenTolerance& t) noexcept {
  if (!d.sameSize || d.channelTolerance != t.channel) return false;
  const uint64_t allowed = d.pixelCount * t.outliersPerMillion / 1'000'000;
  return d.maxChannelDiff <= t.outlierCap && d.pixelsOverTolerance <= allowed && d.clusteredOutliers == 0;
}

Image diffImage(const Image& actual, const Image& expected, uint32_t channelTolerance, uint32_t outlierCap) {
  Image out;
  if (actual.width != expected.width || actual.height != expected.height ||
      actual.rgba.size() != size_t(actual.width) * actual.height * 4 || expected.rgba.size() != actual.rgba.size())
    return out;
  out.width = actual.width;
  out.height = actual.height;
  out.rgba.resize(actual.rgba.size());
  uint32_t x0 = out.width, y0 = out.height, x1 = 0, y1 = 0;
  for (uint32_t y = 0; y < out.height; ++y)
    for (uint32_t x = 0; x < out.width; ++x) {
      const uint8_t* e = expected.pixel(x, y);
      uint8_t* o = out.pixel(x, y);
      const uint32_t worst = worstChannelDiff(actual.pixel(x, y), e);
      const uint8_t lum = static_cast<uint8_t>((uint32_t(e[0]) * 54 + uint32_t(e[1]) * 183 + uint32_t(e[2]) * 19) >> 10);
      const uint8_t bright = static_cast<uint8_t>(std::min<uint32_t>(255, 128 + worst / 2));
      if (worst == 0) {
        o[0] = o[1] = o[2] = lum;
      } else if (worst <= channelTolerance) {
        o[0] = o[1] = lum;
        o[2] = 200;
      } else {
        o[0] = bright;
        o[1] = worst <= outlierCap ? bright : 0;
        o[2] = 0;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x + 1);
        y1 = std::max(y1, y + 1);
      }
      o[3] = 255;
    }
  if (x1 > x0) {  // box outline one pixel outside the outliers (clamped to the image)
    const int64_t bx0 = int64_t(x0) - 1, by0 = int64_t(y0) - 1, bx1 = int64_t(x1), by1 = int64_t(y1);
    auto put = [&](int64_t x, int64_t y) {
      if (x < 0 || y < 0 || x >= int64_t(out.width) || y >= int64_t(out.height)) return;
      uint8_t* o = out.pixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
      o[0] = 255;
      o[1] = 0;
      o[2] = 255;
    };
    for (int64_t x = bx0; x <= bx1; ++x) {
      put(x, by0);
      put(x, by1);
    }
    for (int64_t y = by0; y <= by1; ++y) {
      put(bx0, y);
      put(bx1, y);
    }
  }
  return out;
}

}  // namespace pq::ui
