#include <cstdint>

// Offline GS replay has no EE scheduler or controller clock.
uint64_t ps2PadCurrentGuestFrame() { return 0u; }
