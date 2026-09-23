// Frame images and image diffing for the GS-dump replay harness.
//
// Two on-disk forms are used deliberately:
//   *.raw32  canonical, lossless, trivially parsed  -> the CI comparison artifact
//   *.png    for humans to look at                  -> never used as an input
//
// Keeping the canonical form raw means the harness needs no PNG *decoder*, and
// a diff can never be perturbed by an encoder difference between machines.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dq8::gfx {

struct FrameImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // width*height*4, top-down, R,G,B,A

    bool empty() const { return width == 0 || height == 0 || rgba.empty(); }
    void resize(uint32_t w, uint32_t h) {
        width = w;
        height = h;
        rgba.assign(static_cast<size_t>(w) * h * 4u, 0u);
    }
};

// "GSFRAME\0" + u32 width + u32 height, then width*height*4 RGBA bytes.
bool writeRaw32(const std::string &path, const FrameImage &img, std::string &error);
bool readRaw32(const std::string &path, FrameImage &img, std::string &error);

// Minimal RGBA8 PNG writer (zlib deflate, filter type 0). View-only output.
bool writePng(const std::string &path, const FrameImage &img, std::string &error);

// Pixels whose RGB is not all-zero. "Did anything draw?" in one number.
uint64_t nonBlackPixels(const FrameImage &img);

// Box-resample references onto the native grid; this approximates the
// reference renderer's scaling filter.
bool resampleTo(const FrameImage &src, uint32_t width, uint32_t height, FrameImage &out);

struct DiffResult {
    bool comparable = false;   // same dimensions
    uint64_t totalPixels = 0;
    uint64_t differingPixels = 0;   // any channel differs at all
    uint64_t pixelsOverTolerance = 0; // any channel differs by > tolerance
    uint32_t maxChannelDelta = 0;
    double meanAbsError = 0.0;  // over all channels
    double psnr = 0.0;          // dB; +inf reported as 99.0 for identical images
    // Bounding box of differing pixels (inclusive); x0 > x1 when none differ.
    int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;

    double differingFraction() const {
        return totalPixels ? static_cast<double>(differingPixels) / static_cast<double>(totalPixels) : 0.0;
    }
};

// Compares two images. `tolerance` is a per-channel absolute delta that is
// still considered a match (0 = exact). When `heatmap` is non-null it receives
// an amplified visualisation of |a-b|.
DiffResult diffImages(const FrameImage &a, const FrameImage &b, uint32_t tolerance,
                      FrameImage *heatmap);

// Ignores alpha when comparing; the GS alpha channel carries FBA/blend state
// that a HW backend may legitimately represent differently.
DiffResult diffImagesRgb(const FrameImage &a, const FrameImage &b, uint32_t tolerance,
                         FrameImage *heatmap);

} // namespace dq8::gfx
