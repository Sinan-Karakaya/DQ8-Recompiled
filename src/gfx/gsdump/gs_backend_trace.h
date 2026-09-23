#pragma once

#include "runtime/gs/gs_backend.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

namespace dq8::gfx {

// Local diagnostic format, tied to the GS struct ABI of the producing build.
enum class GsTraceOp : uint32_t { Submit, Transfer, Upload, Flush, TextureFlush,
    Sync, Present, Clear, Write, Read, Consume, Reset };
inline constexpr std::array<uint32_t, 6> kGsTraceHeader{
    0x47535452u, 1u, sizeof(GSPrimitiveBatch), sizeof(GSTransferCommand),
    sizeof(GSPresentationRequest), sizeof(GSContext)};
static_assert(std::is_trivially_copyable_v<GSPrimitiveBatch> &&
              std::is_trivially_copyable_v<GSTransferCommand> &&
              std::is_trivially_copyable_v<GSPresentationRequest> &&
              std::is_trivially_copyable_v<GSContext>);

class GsTraceBackend final : public GSRasterBackend {
public:
    GsTraceBackend(std::unique_ptr<GSRasterBackend> backend, std::string path,
                   uint32_t startFrame, uint32_t frames, std::string trigger = {})
        : m_backend(std::move(backend)), m_path(std::move(path)),
          m_start(startFrame), m_remaining(frames), m_trigger(std::move(trigger)) {}
    ~GsTraceBackend() override { close(); }

    void Initialize(uint8_t *vram, uint32_t size) override { std::lock_guard lock(m_mutex); m_backend->Initialize(vram, size); }
    void Reset() override { std::lock_guard lock(m_mutex); record(GsTraceOp::Reset); m_backend->Reset(); }
    void Submit(const GSPrimitiveBatch &b) override { std::lock_guard lock(m_mutex); record(GsTraceOp::Submit, b); m_backend->Submit(b); }
    void BeginTransfer(const GSTransferCommand &c) override { std::lock_guard lock(m_mutex); record(GsTraceOp::Transfer, c); m_backend->BeginTransfer(c); }
    void UploadImage(const uint8_t *data, uint32_t size) override {
        std::lock_guard lock(m_mutex);
        recordBytes(GsTraceOp::Upload, data, size); m_backend->UploadImage(data, size);
    }
    void Flush() override { std::lock_guard lock(m_mutex); record(GsTraceOp::Flush); m_backend->Flush(); }
    void TextureFlush() override { std::lock_guard lock(m_mutex); record(GsTraceOp::TextureFlush); m_backend->TextureFlush(); }
    void Sync(GSSyncReason r) override { std::lock_guard lock(m_mutex); record(GsTraceOp::Sync, r); m_backend->Sync(r); }
    PresentationFrame Present(const GSPresentationRequest &request) override {
        std::lock_guard lock(m_mutex);
        record(GsTraceOp::Present, request);
        auto frame = m_backend->Present(request);
        if (m_file && --m_remaining == 0u) {
            close();
            std::fprintf(stderr, "[gs-trace] finished %s\n", m_path.c_str());
        }
        ++m_frame;
        if (shouldStart()) {
            m_started = true;
            std::vector<uint8_t> vram;
            m_backend->SnapshotVram(vram);
            m_file = std::fopen(m_path.c_str(), "wb");
            if (m_file) {
                write(kGsTraceHeader.data(), sizeof(kGsTraceHeader));
                const uint32_t size = static_cast<uint32_t>(vram.size());
                write(&size, sizeof(size));
                write(vram.data(), vram.size());
                std::fprintf(stderr, "[gs-trace] starting at frame %u: %s\n", m_frame, m_path.c_str());
            } else std::fprintf(stderr, "[gs-trace] cannot open %s\n", m_path.c_str());
        }
        return frame;
    }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override {
        std::lock_guard lock(m_mutex);
        record(GsTraceOp::Clear, context); write(&rgba, sizeof(rgba));
        return m_backend->ClearFramebuffer(context, rgba);
    }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t size) override {
        std::lock_guard lock(m_mutex);
        record(GsTraceOp::Consume, size); return m_backend->ConsumeLocalToHostBytes(dst, size);
    }
    uint32_t ReadVram(uint32_t p, uint32_t b, uint32_t w, uint32_t x, uint32_t y) const override {
        std::lock_guard lock(m_mutex);
        record(GsTraceOp::Read, std::array<uint32_t, 5>{p, b, w, x, y});
        return m_backend->ReadVram(p, b, w, x, y);
    }
    void WriteVram(uint32_t p, uint32_t b, uint32_t w, uint32_t x, uint32_t y, uint32_t value) override {
        std::lock_guard lock(m_mutex);
        record(GsTraceOp::Write, std::array<uint32_t, 6>{p, b, w, x, y, value});
        m_backend->WriteVram(p, b, w, x, y, value);
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override {
        std::lock_guard lock(m_mutex);
        record(GsTraceOp::Flush); m_backend->SnapshotVram(out);
    }
    GSTransferSnapshot GetTransferSnapshot() const override { std::lock_guard lock(m_mutex); return m_backend->GetTransferSnapshot(); }

private:
    bool shouldStart() {
        if (m_started || m_remaining == 0u) return false;
        if (m_trigger.empty()) return m_frame == m_start;
        const auto now = std::chrono::steady_clock::now();
        if (now < m_nextTriggerCheck) return false;
        m_nextTriggerCheck = now + std::chrono::milliseconds(250);
        std::error_code error;
        return std::filesystem::exists(m_trigger, error);
    }
    void close() const { if (m_file) std::fclose(m_file); m_file = nullptr; }
    void write(const void *data, size_t size) const {
        if (m_file && std::fwrite(data, 1u, size, m_file) != size) {
            std::fprintf(stderr, "[gs-trace] write failed: %s\n", m_path.c_str());
            close();
        }
    }
    void recordBytes(GsTraceOp op, const void *data, uint32_t size) const {
        if (!m_file) return;
        write(&op, sizeof(op)); write(&size, sizeof(size)); write(data, size);
    }
    template <class T> void record(GsTraceOp op, const T &value) const {
        recordBytes(op, &value, sizeof(value));
    }
    void record(GsTraceOp op) const { recordBytes(op, nullptr, 0u); }
    std::unique_ptr<GSRasterBackend> m_backend;
    std::string m_path;
    uint32_t m_start, m_remaining, m_frame = 0;
    std::string m_trigger;
    bool m_started = false;
    std::chrono::steady_clock::time_point m_nextTriggerCheck{};
    mutable FILE *m_file = nullptr;
    mutable std::mutex m_mutex;
};
} // namespace dq8::gfx
