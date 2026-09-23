#pragma once

#include "runtime/gs/gs_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#if defined(__APPLE__)
#include <execinfo.h>
#endif

namespace dq8::diagnostics {

// Opt-in observation only: never alter the submitted draw or its ordering.
class GsFanProbeBackend final : public GSRasterBackend {
public:
    explicit GsFanProbeBackend(std::unique_ptr<GSRasterBackend> backend,
                               const uint8_t *rdram = nullptr,
                               uint32_t ramSize = 0)
        : m_backend(std::move(backend)), m_rdram(rdram), m_ramSize(ramSize),
          m_start(option("DQ8_GS_FAN_PROBE_START", 0)),
          m_interval(option("DQ8_GS_FAN_PROBE_INTERVAL", 60)),
          m_limit(option("DQ8_GS_FAN_PROBE_LIMIT", 16)) {}

    static bool matches(const GSPrimitiveBatch &b) {
        return b.state.prim.type == GS_PRIM_TRIFAN && !b.state.prim.tme &&
               b.state.prim.abe && b.state.context.frame.fbp == 0 &&
               b.state.context.alpha == 0x48 && b.state.context.test == 0x50002;
    }

    void Initialize(uint8_t *v, uint32_t n) override { m_backend->Initialize(v, n); }
    void Reset() override { m_backend->Reset(); }
    void Submit(const GSPrimitiveBatch &b) override {
        {
            std::lock_guard lock(m_mutex);
            if (m_count < m_limit && m_frame >= m_start &&
                (m_count == 0 || m_frame - m_last >= m_interval) && matches(b)) {
                ++m_count;
                m_last = m_frame;
                std::fprintf(stderr, "[gs-fan-probe] match=%u frame=%u xyoffset=%u,%u\n",
                             m_count, m_frame, b.state.context.xyoffset.ofx,
                             b.state.context.xyoffset.ofy);
                for (uint32_t i = 0; i < b.vertexCount; ++i) {
                    const auto &v = b.vertices[i];
                    const auto &offset = b.state.context.xyoffset;
                    std::fprintf(stderr, "[gs-fan-probe] vertex=%u rawxy=%.4f,%.4f screenxy=%.4f,%.4f z=%.0f rgba=%u,%u,%u,%u\n",
                                 i, v.x, v.y, v.x - offset.ofx / 16.0f,
                                 v.y - offset.ofy / 16.0f, v.z, v.r, v.g, v.b, v.a);
                }
                findVertex(b);
#if defined(__APPLE__)
                void *stack[64];
                backtrace_symbols_fd(stack, backtrace(stack, 64), 2);
#endif
            }
        }
        m_backend->Submit(b);
    }
    void BeginTransfer(const GSTransferCommand &c) override { m_backend->BeginTransfer(c); }
    void UploadImage(const uint8_t *d, uint32_t n) override { m_backend->UploadImage(d, n); }
    void Flush() override { m_backend->Flush(); }
    void TextureFlush() override { m_backend->TextureFlush(); }
    void Sync(GSSyncReason r) override { m_backend->Sync(r); }
    PresentationFrame Present(const GSPresentationRequest &r) override {
        auto f = m_backend->Present(r);
        std::lock_guard lock(m_mutex);
        ++m_frame;
        return f;
    }
    bool ClearFramebuffer(const GSContext &c, uint32_t v) override { return m_backend->ClearFramebuffer(c, v); }
    uint32_t ConsumeLocalToHostBytes(uint8_t *d, uint32_t n) override { return m_backend->ConsumeLocalToHostBytes(d, n); }
    uint32_t ReadVram(uint32_t p, uint32_t b, uint32_t w, uint32_t x, uint32_t y) const override { return m_backend->ReadVram(p, b, w, x, y); }
    void WriteVram(uint32_t p, uint32_t b, uint32_t w, uint32_t x, uint32_t y, uint32_t v) override { m_backend->WriteVram(p, b, w, x, y, v); }
    void SnapshotVram(std::vector<uint8_t> &v) const override { m_backend->SnapshotVram(v); }
    GSTransferSnapshot GetTransferSnapshot() const override { return m_backend->GetTransferSnapshot(); }

private:
    static uint32_t option(const char *name, uint32_t fallback) {
        const char *value = std::getenv(name);
        return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : fallback;
    }

    void findVertex(const GSPrimitiveBatch &b) const {
        if (!m_rdram || m_ramSize < 16 || b.vertexCount == 0) return;
        const auto &v = b.vertices[0];
        const double rawX = static_cast<double>(v.x) * 16.0;
        const double rawY = static_cast<double>(v.y) * 16.0;
        if (!std::isfinite(rawX) || !std::isfinite(rawY) || !std::isfinite(v.z) ||
            rawX < 0 || rawX > 65535 || rawY < 0 || rawY > 65535 ||
            v.z < 0 || v.z > std::numeric_limits<uint32_t>::max()) return;
        const uint32_t x = static_cast<uint32_t>(rawX);
        const uint32_t y = static_cast<uint32_t>(rawY);
        const uint32_t z = static_cast<uint32_t>(v.z);
        const uint64_t xyz = x | (static_cast<uint64_t>(y) << 16) |
                             (static_cast<uint64_t>(z) << 32);
        unsigned found = 0;
        for (uint32_t address = 0; address <= m_ramSize - 16; address += 4) {
            uint32_t packed[4];
            std::memcpy(packed, m_rdram + address, sizeof(packed));
            uint64_t direct;
            std::memcpy(&direct, packed, sizeof(direct));
            if (direct == xyz || (packed[0] == x && packed[1] == y && packed[2] == z)) {
                std::fprintf(stderr, "[gs-fan-probe] vertex-memory=%08x format=%s\n",
                             address, direct == xyz ? "xyz64" : "packed128");
                if (++found == 16) break;
            }
        }
        if (found == 0) std::fprintf(stderr, "[gs-fan-probe] vertex absent from RDRAM\n");
    }

    std::unique_ptr<GSRasterBackend> m_backend;
    const uint8_t *m_rdram;
    uint32_t m_ramSize, m_start, m_interval, m_limit;
    uint32_t m_frame = 0, m_last = 0, m_count = 0;
    std::mutex m_mutex;
};

inline std::unique_ptr<GSRasterBackend> wrapGsFanProbe(
    std::unique_ptr<GSRasterBackend> backend, const uint8_t *rdram = nullptr,
    uint32_t ramSize = 0) {
    if (!std::getenv("DQ8_GS_FAN_PROBE")) return backend;
    return std::make_unique<GsFanProbeBackend>(std::move(backend), rdram, ramSize);
}
} // namespace dq8::diagnostics
