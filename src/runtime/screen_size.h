#pragma once

class PS2Runtime;

namespace dq8 {

// Follows the game's Screen Size option on the executor thread, for the
// display's Auto aspect. Install after loadELF().
bool installScreenSizeProbe(PS2Runtime &runtime);
// Any thread: 1 for Wide Screen (16:9), 0 for Normal (4:3), -1 before the
// game has set or applied it.
int gameScreenSize();

} // namespace dq8
