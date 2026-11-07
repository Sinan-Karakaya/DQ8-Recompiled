// A sampling profiler inside the game process, for machines where a system
// profiler is not at hand: it needs no elevation and no extra executable.
//
// DQ8_SAMPLER=1 turns it on. Every millisecond (DQ8_SAMPLER_HZ) each thread is
// suspended for a few microseconds while its stack is unwound, and the thread's
// cycle counter tells whether it was running or waiting since the last sample.
// Every DQ8_SAMPLER_EVERY seconds (default 10) the top functions of each
// thread are printed: where it burns CPU and where it waits. Windows only; a
// Linux or macOS build reports it unavailable and runs the game unchanged.
#pragma once

namespace dq8::diagnostics {

// Starts the sampler when DQ8_SAMPLER asks for it. Returns false when it is
// off or unavailable on this platform.
bool startHostSampler();
// Prints a final report and stops the sampling thread. Safe when not started.
void stopHostSampler();

} // namespace dq8::diagnostics
