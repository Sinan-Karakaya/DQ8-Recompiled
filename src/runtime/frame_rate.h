// Runs the game at a frame rate of the player's choosing without changing
// how fast it plays.
//
// DQ8 is a delta-time game: its frame loop measures how many fields (1/60 s)
// the last frame took and scales the simulation by that, with the measurement
// clamped to [interval, 2*interval] fields, where `interval` is 2 in the field
// (30 fps) and 1 in menus (60 fps). Its render routine waits until `interval`
// fields have passed since the last frame. So the game can run at any rate as
// long as (1) the interval it waits for is 1, (2) the wait itself is paced by
// the host instead of the vblank, and (3) the clamp's lower bound can go below
// one field. The first two are hooks on the game's own routines. The third is
// in the translation: config/SLUS_212.07/dq8.toml patches the two instructions
// of the clamp to read the bound from a slot this module writes each frame.
//
// The same slot trick feeds the one animation system that steps a fixed 1.0
// per frame instead of using the delta (the community's 60 fps patch for
// PCSX2 halves that constant); it reads the step from a second slot.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace dq8 {

// Hooks the frame loop. Installed after everything else that wraps the render
// routine (the cadence probe). Returns false when a routine is missing.
bool installFrameRateHooks(PS2Runtime &runtime);

// Target frames per second: 0 keeps the game's own cadence (30 in the field,
// 60 in menus), a negative value runs every frame as soon as the last is done,
// anything else paces the frames to that rate. Any thread.
void setFrameRateTarget(double hz);
double frameRateTarget();

// Reads DQ8_FRAME_RATE: a number, "original", "display" (the display's
// refresh rate, given here) or "uncapped". Returns true when it set a target.
bool applyFrameRateEnvironment(double displayHz);

} // namespace dq8
