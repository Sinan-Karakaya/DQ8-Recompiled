#include "gfx/backends/sdlgpu/sdlgpu_input.h"

#include <cstdio>

namespace dq8::gfx {
namespace {
enum : uint32_t {
    Select = 0x0001u, L3 = 0x0002u, R3 = 0x0004u, Start = 0x0008u,
    Up = 0x0010u, Right = 0x0020u, Down = 0x0040u, Left = 0x0080u,
    L2 = 0x0100u, R2 = 0x0200u, L1 = 0x0400u, R1 = 0x0800u,
    Triangle = 0x1000u, Circle = 0x2000u, Cross = 0x4000u, Square = 0x8000u
};
constexpr struct { SDL_Scancode key; uint32_t mask; } kKeys[] = {
    {SDL_SCANCODE_UP, Up}, {SDL_SCANCODE_DOWN, Down},
    {SDL_SCANCODE_LEFT, Left}, {SDL_SCANCODE_RIGHT, Right},
    {SDL_SCANCODE_X, Cross}, {SDL_SCANCODE_SPACE, Cross},
    {SDL_SCANCODE_C, Circle}, {SDL_SCANCODE_ESCAPE, Circle},
    {SDL_SCANCODE_Z, Square}, {SDL_SCANCODE_KP_0, Square},
    {SDL_SCANCODE_V, Triangle}, {SDL_SCANCODE_KP_1, Triangle},
    {SDL_SCANCODE_Q, L1}, {SDL_SCANCODE_E, R1},
    {SDL_SCANCODE_LSHIFT, L2}, {SDL_SCANCODE_RSHIFT, R2},
    {SDL_SCANCODE_R, L3}, {SDL_SCANCODE_F, R3},
    {SDL_SCANCODE_RETURN, Start}, {SDL_SCANCODE_TAB, Select},
};
constexpr struct { SDL_GamepadButton button; uint32_t mask; } kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, Cross}, {SDL_GAMEPAD_BUTTON_EAST, Circle},
    {SDL_GAMEPAD_BUTTON_WEST, Square}, {SDL_GAMEPAD_BUTTON_NORTH, Triangle},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, Up}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Down},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Left}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Right},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, R3},
    {SDL_GAMEPAD_BUTTON_START, Start}, {SDL_GAMEPAD_BUTTON_BACK, Select},
};
constexpr int kTriggerThreshold = 8192;

uint8_t axisByte(Sint16 value) {
    // Suppress stick drift; preserve the rest of the controller's travel.
    return value > -4096 && value < 4096 ? 128u : (static_cast<int>(value) + 32768) >> 8;
}
void keyboardAxis(const bool *keys, SDL_Scancode negative, SDL_Scancode positive,
                  uint8_t &value) {
    if (keys && (keys[negative] || keys[positive]))
        value = keys[negative] == keys[positive] ? 128u : keys[negative] ? 0u : 255u;
}
} // namespace

SdlPadInput::~SdlPadInput() {
    if (m_gamepad)
        SDL_CloseGamepad(m_gamepad);
    if (m_initialized)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

bool SdlPadInput::initialize(std::string &error) {
    if (m_initialized)
        return true;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        error = std::string("SDL_InitSubSystem(gamepad): ") + SDL_GetError();
        return false;
    }
    m_initialized = true;
    selectGamepad();
    return true;
}

void SdlPadInput::selectGamepad() {
    if (m_gamepad)
        return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count && !m_gamepad; ++i)
        m_gamepad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    if (m_gamepad)
        std::fprintf(stderr, "[pad] connected: %s\n", SDL_GetGamepadName(m_gamepad));
}

void SdlPadInput::handleEvent(const SDL_Event &event) {
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        for (const auto &binding : kKeys)
            if (event.key.scancode == binding.key)
                m_pressed |= binding.mask;
    }
    if (event.type == SDL_EVENT_GAMEPAD_ADDED)
        selectGamepad();
    if (!m_gamepad)
        return;
    const auto id = SDL_GetGamepadID(m_gamepad);
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED && event.gdevice.which == id) {
        SDL_CloseGamepad(m_gamepad);
        m_gamepad = nullptr;
        m_held = m_pressed = 0u;
        m_sticks = 0x80808080u;
        selectGamepad();
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.which == id) {
        for (const auto &binding : kButtons)
            if (event.gbutton.button == binding.button)
                m_pressed |= binding.mask;
    } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && event.gaxis.which == id &&
               event.gaxis.value > kTriggerThreshold) {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
            m_pressed |= L2;
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            m_pressed |= R2;
    }
}

void SdlPadInput::poll(const bool *keys, bool focused) {
    if (!focused) {
        m_held = m_pressed = 0u;
        m_sticks = 0x80808080u;
        return;
    }
    uint32_t held = 0u;
    uint8_t rx = 128u, ry = 128u, lx = 128u, ly = 128u;
    if (m_gamepad && SDL_GamepadConnected(m_gamepad)) {
        for (const auto &binding : kButtons)
            if (SDL_GetGamepadButton(m_gamepad, binding.button))
                held |= binding.mask;
        if (SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kTriggerThreshold)
            held |= L2;
        if (SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kTriggerThreshold)
            held |= R2;
        rx = axisByte(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
        ry = axisByte(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
        lx = axisByte(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        ly = axisByte(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    }
    if (keys)
        for (const auto &binding : kKeys)
            if (keys[binding.key])
                held |= binding.mask;
    keyboardAxis(keys, SDL_SCANCODE_A, SDL_SCANCODE_D, lx);
    keyboardAxis(keys, SDL_SCANCODE_W, SDL_SCANCODE_S, ly);
    keyboardAxis(keys, SDL_SCANCODE_J, SDL_SCANCODE_L, rx);
    keyboardAxis(keys, SDL_SCANCODE_I, SDL_SCANCODE_K, ry);
    m_pressed |= held & ~m_held;
    m_held = held;
    m_sticks = (uint32_t(rx) << 24u) | (uint32_t(ry) << 16u) | (uint32_t(lx) << 8u) | ly;
}

void SdlPadInput::read(uint32_t &held, uint32_t &pressed, uint32_t &sticks) {
    held = m_held;
    pressed = m_pressed;
    sticks = m_sticks;
    m_pressed = 0u;
}
} // namespace dq8::gfx
