// The GS transfer engine: BITBLTBUF/TRXPOS/TRXREG/TRXDIR against local memory.
//
// Host->local, local->local and local->host all move logical pixels, not bytes,
// so every one of them is a swizzle-aware copy. None of that changes between a
// software and a hardware backend, which is why this lives outside both and is
// tested directly against GSCpuBackend.
//
// The two hooks let a caching backend take part: pages are resolved before they
// are read and invalidated after they are written, so a GPU-side copy of a
// region cannot go stale behind a transfer's back.
#pragma once

#include "gfx/gs/gs_vram.h"

#include "runtime/gs/gs_types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace dq8::gfx {

class GsTransferEngine {
public:
    using PageHook = std::function<void(const GsPageSet &)>;

    explicit GsTransferEngine(GsVram &vram);

    // Called with the pages a transfer is about to read; the backend must make
    // local memory current for them.
    void setResolveHook(PageHook hook) { m_resolve = std::move(hook); }
    // Called with the pages a transfer has just written.
    void setInvalidateHook(PageHook hook) { m_invalidate = std::move(hook); }

    void reset();

    // Local->local and local->host execute here, exactly as they do on the GS:
    // the direction field in TRXDIR is what starts them.
    void begin(const GSTransferCommand &command);
    void upload(const uint8_t *data, uint32_t sizeBytes);
    uint32_t consumeLocalToHost(uint8_t *dst, uint32_t maxBytes);

    GSTransferSnapshot snapshot() const { return m_state; }
    size_t pendingLocalToHostBytes() const {
        return m_localToHost.size() - m_localToHostReadPos;
    }

private:
    void performLocalToLocal();
    void performLocalToHost();
    GsPageSet destinationPages() const;
    GsPageSet sourcePages() const;

    GsVram &m_vram;
    PageHook m_resolve;
    PageHook m_invalidate;

    GSTransferCommand m_command{};
    GSTransferSnapshot m_state{};

    // A GIF IMAGE payload is a byte stream, so one packed pixel can straddle
    // two UploadImage calls. Those bytes wait here rather than being dropped.
    std::array<uint8_t, 4> m_partialPixel{};
    uint32_t m_partialPixelBytes = 0u;

    std::vector<uint8_t> m_localToHost;
    size_t m_localToHostReadPos = 0u;
};

} // namespace dq8::gfx
