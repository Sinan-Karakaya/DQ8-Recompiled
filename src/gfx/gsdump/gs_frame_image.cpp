#include "gs_frame_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(DQ8_GSDUMP_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace dq8::gfx {
namespace {

constexpr char kMagic[8] = {'G', 'S', 'F', 'R', 'A', 'M', 'E', '\0'};

// Self-contained CRC-32 (PNG chunk checksum) so the harness has no build
// dependency at all when zlib development headers are absent -- which is the
// common case on a machine that only has the runtime .so installed.
uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        built = true;
    }
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t adler32Of(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// Minimal zlib stream using stored (uncompressed) deflate blocks. Output is
// larger than a real deflate, but a frame PNG is a human-viewing artifact --
// the canonical CI artifact is the .raw32 next to it.
std::vector<uint8_t> zlibStore(const std::vector<uint8_t> &raw) {
    std::vector<uint8_t> out;
    out.reserve(raw.size() + raw.size() / 65535u * 5u + 16u);
    out.push_back(0x78); // CMF: deflate, 32K window
    out.push_back(0x01); // FLG: no dict, fastest
    size_t pos = 0;
    do {
        const size_t chunk = std::min<size_t>(65535u, raw.size() - pos);
        const bool last = (pos + chunk) >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<uint8_t>(chunk & 0xFFu));
        out.push_back(static_cast<uint8_t>(chunk >> 8));
        const uint16_t nlen = static_cast<uint16_t>(~chunk);
        out.push_back(static_cast<uint8_t>(nlen & 0xFFu));
        out.push_back(static_cast<uint8_t>(nlen >> 8));
        out.insert(out.end(), raw.begin() + static_cast<long>(pos),
                   raw.begin() + static_cast<long>(pos + chunk));
        pos += chunk;
    } while (pos < raw.size());
    const uint32_t adler = adler32Of(raw.data(), raw.size());
    out.push_back(static_cast<uint8_t>(adler >> 24));
    out.push_back(static_cast<uint8_t>(adler >> 16));
    out.push_back(static_cast<uint8_t>(adler >> 8));
    out.push_back(static_cast<uint8_t>(adler));
    return out;
}

void putBE32(std::vector<uint8_t> &v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void appendChunk(std::vector<uint8_t> &out, const char tag[4], const std::vector<uint8_t> &data) {
    putBE32(out, static_cast<uint32_t>(data.size()));
    const size_t crcStart = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = crc32Update(0u, out.data() + crcStart, out.size() - crcStart);
    putBE32(out, crc);
}

} // namespace

bool writeRaw32(const std::string &path, const FrameImage &img, std::string &error) {
    std::FILE *fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        error = "cannot write " + path;
        return false;
    }
    uint32_t hdr[2] = {img.width, img.height};
    bool ok = std::fwrite(kMagic, 1, 8, fp) == 8 && std::fwrite(hdr, 1, 8, fp) == 8;
    if (ok && !img.rgba.empty())
        ok = std::fwrite(img.rgba.data(), 1, img.rgba.size(), fp) == img.rgba.size();
    std::fclose(fp);
    if (!ok)
        error = "short write on " + path;
    return ok;
}

bool readRaw32(const std::string &path, FrameImage &img, std::string &error) {
    std::FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        error = "cannot open " + path;
        return false;
    }
    char magic[8] = {};
    uint32_t hdr[2] = {};
    if (std::fread(magic, 1, 8, fp) != 8 || std::memcmp(magic, kMagic, 8) != 0 ||
        std::fread(hdr, 1, 8, fp) != 8) {
        std::fclose(fp);
        error = path + " is not a GSFRAME raw32 image";
        return false;
    }
    img.width = hdr[0];
    img.height = hdr[1];
    const size_t bytes = static_cast<size_t>(img.width) * img.height * 4u;
    img.rgba.resize(bytes);
    const bool ok = bytes == 0 || std::fread(img.rgba.data(), 1, bytes, fp) == bytes;
    std::fclose(fp);
    if (!ok)
        error = "truncated raw32 image " + path;
    return ok;
}

bool writePng(const std::string &path, const FrameImage &img, std::string &error) {
    if (img.empty()) {
        error = "refusing to write an empty PNG";
        return false;
    }

    // Raw scanlines with a leading filter byte (0 = None) per row.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(img.height) * (1u + static_cast<size_t>(img.width) * 4u));
    for (uint32_t y = 0; y < img.height; ++y) {
        raw.push_back(0);
        const uint8_t *row = img.rgba.data() + static_cast<size_t>(y) * img.width * 4u;
        raw.insert(raw.end(), row, row + static_cast<size_t>(img.width) * 4u);
    }

    std::vector<uint8_t> compressed;
#if defined(DQ8_GSDUMP_HAVE_ZLIB)
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    compressed.resize(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(),
                  static_cast<uLong>(raw.size()), 6) != Z_OK) {
        error = "zlib compress2 failed";
        return false;
    }
    compressed.resize(compressedSize);
#else
    compressed = zlibStore(raw);
#endif

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    putBE32(ihdr, img.width);
    putBE32(ihdr, img.height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // colour type: RGBA
    ihdr.push_back(0); // deflate
    ihdr.push_back(0); // adaptive filtering
    ihdr.push_back(0); // no interlace
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", compressed);
    appendChunk(png, "IEND", {});

    std::FILE *fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        error = "cannot write " + path;
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), fp) == png.size();
    std::fclose(fp);
    if (!ok)
        error = "short write on " + path;
    return ok;
}

uint64_t nonBlackPixels(const FrameImage &img) {
    uint64_t n = 0;
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        if (img.rgba[i] | img.rgba[i + 1] | img.rgba[i + 2])
            ++n;
    }
    return n;
}

bool resampleTo(const FrameImage &src, uint32_t width, uint32_t height, FrameImage &out) {
    if (src.empty() || width == 0 || height == 0)
        return false;
    if (src.width == width && src.height == height) {
        out = src;
        return true;
    }

    FrameImage dst;
    dst.resize(width, height);
    const double sx = static_cast<double>(src.width) / width;
    const double sy = static_cast<double>(src.height) / height;

    for (uint32_t y = 0; y < height; ++y) {
        const double y0 = y * sy;
        const double y1 = y0 + sy;
        const uint32_t iy0 = static_cast<uint32_t>(y0);
        const uint32_t iy1 = std::min<uint32_t>(src.height - 1, static_cast<uint32_t>(
                                                                    std::ceil(y1) - 1.0));
        for (uint32_t x = 0; x < width; ++x) {
            const double x0 = x * sx;
            const double x1 = x0 + sx;
            const uint32_t ix0 = static_cast<uint32_t>(x0);
            const uint32_t ix1 = std::min<uint32_t>(src.width - 1, static_cast<uint32_t>(
                                                                       std::ceil(x1) - 1.0));
            double acc[4] = {0, 0, 0, 0};
            double weight = 0.0;
            for (uint32_t syi = iy0; syi <= iy1; ++syi) {
                const double wy = std::min<double>(y1, syi + 1.0) - std::max<double>(y0, syi);
                if (wy <= 0.0)
                    continue;
                for (uint32_t sxi = ix0; sxi <= ix1; ++sxi) {
                    const double wx = std::min<double>(x1, sxi + 1.0) - std::max<double>(x0, sxi);
                    if (wx <= 0.0)
                        continue;
                    const double w = wx * wy;
                    const uint8_t *p = src.rgba.data() +
                                       (static_cast<size_t>(syi) * src.width + sxi) * 4u;
                    for (int c = 0; c < 4; ++c)
                        acc[c] += p[c] * w;
                    weight += w;
                }
            }
            uint8_t *d = dst.rgba.data() + (static_cast<size_t>(y) * width + x) * 4u;
            for (int c = 0; c < 4; ++c) {
                const double v = weight > 0.0 ? acc[c] / weight : 0.0;
                d[c] = static_cast<uint8_t>(std::lround(std::min(255.0, std::max(0.0, v))));
            }
        }
    }
    out = std::move(dst);
    return true;
}

namespace {

DiffResult diffCommon(const FrameImage &a, const FrameImage &b, uint32_t tolerance,
                      FrameImage *heatmap, bool includeAlpha) {
    DiffResult r{};
    if (a.width != b.width || a.height != b.height || a.empty() || b.empty())
        return r;

    r.comparable = true;
    r.totalPixels = static_cast<uint64_t>(a.width) * a.height;
    if (heatmap)
        heatmap->resize(a.width, a.height);

    const int channels = includeAlpha ? 4 : 3;
    uint64_t sumAbs = 0;
    uint64_t sumSq = 0;
    int32_t x0 = static_cast<int32_t>(a.width), y0 = static_cast<int32_t>(a.height);
    int32_t x1 = -1, y1 = -1;

    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * a.width + x) * 4u;
            uint32_t worst = 0;
            for (int c = 0; c < channels; ++c) {
                const int d = static_cast<int>(a.rgba[i + c]) - static_cast<int>(b.rgba[i + c]);
                const uint32_t ad = static_cast<uint32_t>(d < 0 ? -d : d);
                worst = std::max(worst, ad);
                sumAbs += ad;
                sumSq += static_cast<uint64_t>(ad) * ad;
            }
            if (worst > 0) {
                ++r.differingPixels;
                x0 = std::min<int32_t>(x0, static_cast<int32_t>(x));
                y0 = std::min<int32_t>(y0, static_cast<int32_t>(y));
                x1 = std::max<int32_t>(x1, static_cast<int32_t>(x));
                y1 = std::max<int32_t>(y1, static_cast<int32_t>(y));
            }
            if (worst > tolerance)
                ++r.pixelsOverTolerance;
            r.maxChannelDelta = std::max(r.maxChannelDelta, worst);

            if (heatmap) {
                const uint8_t v = static_cast<uint8_t>(std::min<uint32_t>(255u, worst * 8u));
                uint8_t *px = heatmap->rgba.data() + i;
                px[0] = v;
                px[1] = static_cast<uint8_t>(worst ? 32 : 0);
                px[2] = static_cast<uint8_t>(worst ? 0 : 0);
                px[3] = 255;
            }
        }
    }

    const double n = static_cast<double>(r.totalPixels) * channels;
    r.meanAbsError = n > 0.0 ? static_cast<double>(sumAbs) / n : 0.0;
    const double mse = n > 0.0 ? static_cast<double>(sumSq) / n : 0.0;
    r.psnr = (mse <= 0.0) ? 99.0 : 10.0 * std::log10((255.0 * 255.0) / mse);
    if (x1 >= 0) {
        r.x0 = x0;
        r.y0 = y0;
        r.x1 = x1;
        r.y1 = y1;
    } else {
        r.x0 = 0;
        r.y0 = 0;
        r.x1 = -1;
        r.y1 = -1;
    }
    return r;
}

} // namespace

DiffResult diffImages(const FrameImage &a, const FrameImage &b, uint32_t tolerance,
                      FrameImage *heatmap) {
    return diffCommon(a, b, tolerance, heatmap, true);
}

DiffResult diffImagesRgb(const FrameImage &a, const FrameImage &b, uint32_t tolerance,
                         FrameImage *heatmap) {
    return diffCommon(a, b, tolerance, heatmap, false);
}

} // namespace dq8::gfx
