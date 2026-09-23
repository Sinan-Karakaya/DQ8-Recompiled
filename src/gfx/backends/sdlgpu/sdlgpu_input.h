#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <string>

namespace dq8::gfx {

// Main-thread SDL input, published in the PS2 pad report's button/axis encoding.
class SdlPadInput {
public:
    SdlPadInput() = default;
    SdlPadInput(const SdlPadInput &) = delete;
    SdlPadInput &operator=(const SdlPadInput &) = delete;
    ~SdlPadInput();
    bool initialize(std::string &error);
    void handleEvent(const SDL_Event &event);
    void poll(const bool *keys, bool focused);
    void read(uint32_t &held, uint32_t &pressed, uint32_t &sticks);

private:
    void selectGamepad();
    SDL_Gamepad *m_gamepad = nullptr;
    bool m_initialized = false;
    uint32_t m_held = 0u;
    uint32_t m_pressed = 0u;
    uint32_t m_sticks = 0x80808080u;
};

} // namespace dq8::gfx
