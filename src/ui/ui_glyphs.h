// Vector drawings for the menu: icons, the PS2 pad's button marks, what a
// binding asks the player to press, and a DualShock 2 lit by live input.
// Drawn rather than loaded, so they stay sharp at any scale and ship nothing.
#pragma once

#include "gfx/backends/sdlgpu/sdlgpu_input.h"

#include <imgui.h>
#include <string>

namespace dq8::ui {

enum class Icon : uint8_t {
    Display, Sound, SoundOff, Controls, Interface, Crown, Camera, Pause, Play,
    Keyboard, Info, Chart, Close, Folder, Link, Speed, Check, Warning, Reset, Disc, Wrench, Gear, Copy
};

void drawIcon(ImDrawList *list, Icon icon, ImVec2 center, float size, ImU32 color);
// Lays out an icon-sized item and draws the icon in it.
void icon(Icon icon, float size, ImU32 color);

// A PS2 input as the pad itself marks it.
void drawPadGlyph(ImDrawList *list, gfx::PadInput input, ImVec2 center, float size);

// What a binding asks the player to press, as their keyboard or controller
// type labels it. Chips are `height` tall; the width depends on the label.
float bindingChipWidth(const gfx::PadBinding &binding, SDL_GamepadType type, float height);
void drawBindingChip(ImDrawList *list, const gfx::PadBinding &binding, SDL_GamepadType type,
                     ImVec2 min, float height, float alpha = 1.0f);
std::string bindingLabel(const gfx::PadBinding &binding, SDL_GamepadType type);
const char *controllerFamilyName(SDL_GamepadType type);

// A DualShock 2 fitted into the box, lit by `pad`. `highlight` rings one
// input, or none when it is PadInput::Count.
void drawPadDiagram(ImDrawList *list, ImVec2 min, ImVec2 size, const gfx::PadSnapshot &pad,
                    gfx::PadInput highlight);

} // namespace dq8::ui
