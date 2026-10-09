#include <atomic>
#include <cstdint>

// Offline GS replay has no EE scheduler or controller clock.
uint64_t ps2PadCurrentGuestFrame() { return 0u; }

// The counters the SDL GPU backend reports, which the game takes from
// ps2xRuntime's MPEG stub and EE scheduler. Here they stay at zero.
std::atomic<uint64_t> g_mpegGetPictureNanos{0};
std::atomic<uint64_t> g_mpegGetPictureCount{0};
std::atomic<uint64_t> g_mpegWriteFrameNanos{0};
std::atomic<uint64_t> g_mpegWriteFrameCount{0};
std::atomic<uint64_t> g_mpegDemuxNanos{0};
std::atomic<uint64_t> g_mpegDemuxCount{0};
std::atomic<uint64_t> g_mpegDemuxRefusedCount{0};
std::atomic<uint64_t> g_mpegPendingEsPeakBytes{0};
std::atomic<uint64_t> g_eeGuestDispatchCount{0};
std::atomic<uint64_t> g_eeTransferThrowCount{0};
std::atomic<uint64_t> g_eeTransferSuspendCount{0};
std::atomic<uint64_t> g_eeRunLoopIterations{0};
std::atomic<uint64_t> g_eeRunLoopResumeCount{0};
std::atomic<uint64_t> g_eeProcessPendingEventsCount{0};
std::atomic<uint64_t> g_eeEnterGuestCount{0};
std::atomic<uint32_t> g_mpegDemuxThreadId{0};
std::atomic<uint32_t> g_mpegGetPictureThreadId{0};
