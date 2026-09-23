#include "gfx/backends/sdlgpu/sdlgpu_input.h"
#include <array>
#include <cstdio>
#include <stdexcept>

using dq8::gfx::SdlPadInput;
namespace {
void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(std::string(message) + ": " + SDL_GetError());
}
void events(SdlPadInput &pad) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) pad.handleEvent(event);
}
struct Report { uint32_t held, pressed, sticks; };
Report read(SdlPadInput &pad, const bool *keys = nullptr, bool focused = true) {
    events(pad);
    pad.poll(keys, focused);
    Report r{};
    pad.read(r.held, r.pressed, r.sticks);
    return r;
}
SDL_JoystickID attach() {
    SDL_VirtualJoystickDesc desc{};
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;
    desc.name = "DQ8 test controller";
    const auto id = SDL_AttachVirtualJoystick(&desc);
    require(id != 0u, "attach virtual gamepad");
    return id;
}
} // namespace

int main() try {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SdlPadInput pad;
    std::string error;
    require(pad.initialize(error), error.c_str());
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    keys[SDL_SCANCODE_W] = keys[SDL_SCANCODE_D] = keys[SDL_SCANCODE_J] = true;
    keys[SDL_SCANCODE_UP] = keys[SDL_SCANCODE_R] = keys[SDL_SCANCODE_F] = true;
    auto r = read(pad, keys.data());
    require(r.sticks == 0x0080ff00u && r.held == 0x16u, "keyboard analog and thumb buttons");
    keys[SDL_SCANCODE_S] = keys[SDL_SCANCODE_A] = keys[SDL_SCANCODE_L] = true;
    r = read(pad, keys.data());
    require(r.sticks == 0x80808080u, "opposing keys center sticks");
    r = read(pad, keys.data(), false);
    require(!r.held && !r.pressed && r.sticks == 0x80808080u, "focus loss clears input");
    keys.fill(false);
    SDL_Event tap{};
    tap.type = SDL_EVENT_KEY_DOWN;
    tap.key.scancode = SDL_SCANCODE_X;
    pad.handleEvent(tap);
    r = read(pad, keys.data());
    require(!r.held && r.pressed == 0x4000u, "short keyboard tap is latched");
    require(read(pad).pressed == 0u, "keyboard edge is consumed once");

    auto id = attach();
    SDL_Joystick *joystick = SDL_OpenJoystick(id);
    require(joystick != nullptr, "open virtual joystick");
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    read(pad);
    require(SDL_GetGamepadFromID(id) != nullptr, "gamepad hotplug opens device");
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, -32768);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, 32767);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHTY, -32768);
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
    r = read(pad);
    require(r.sticks == 0xff0000ffu, "gamepad axis order and full range");
    require(r.held == 0x4104u, "face, trigger and thumb mapping");
    keys[SDL_SCANCODE_E] = true;
    require((read(pad, keys.data()).held & 0x4904u) == 0x4904u, "keyboard and gamepad combine");
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
    read(pad);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST, true);
    events(pad);
    SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_EAST, false);
    r = read(pad);
    require(!(r.held & 0x2000u) && (r.pressed & 0x2000u), "short gamepad tap is latched");
    require(!(read(pad).pressed & 0x2000u), "gamepad edge is consumed once");
    SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, 1000);
    require(((read(pad).sticks >> 8u) & 255u) == 128u, "stick drift dead zone");
    require(SDL_DetachVirtualJoystick(id), "detach controller");
    SDL_CloseJoystick(joystick);
    r = read(pad);
    require(!r.held && !r.pressed && r.sticks == 0x80808080u, "disconnect clears input");
    id = attach();
    read(pad);
    require(SDL_GetGamepadFromID(id) != nullptr, "controller reconnects");
    require(SDL_DetachVirtualJoystick(id), "detach reconnected controller");
    read(pad);
    std::puts("PASS: SDL pad keyboard, analog, buttons, latching, focus and hotplug");
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
