#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dq8::gfx {

// The PS2 pad's inputs, in the order the settings list them.
enum class PadInput : uint8_t {
    Cross, Circle, Square, Triangle,
    Up, Down, Left, Right,
    L1, R1, L2, R2, L3, R3,
    Start, Select,
    LeftStickUp, LeftStickDown, LeftStickLeft, LeftStickRight,
    RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
    Count
};
constexpr size_t kPadInputCount = static_cast<size_t>(PadInput::Count);

// One host input: a key, a gamepad button, or one direction of a gamepad axis.
struct PadBinding {
    enum class Kind : uint8_t { None, Key, Button, Axis };
    Kind kind = Kind::None;
    int16_t code = 0; // SDL_Scancode, SDL_GamepadButton or SDL_GamepadAxis
    int8_t direction = 1;

    static PadBinding key(SDL_Scancode scancode);
    static PadBinding button(SDL_GamepadButton button);
    static PadBinding axis(SDL_GamepadAxis axis, int direction);
    bool bound() const { return kind != Kind::None; }
    bool operator==(const PadBinding &) const = default;
};

constexpr size_t kBindingSlots = 2u;
using PadBindingSlots = std::array<PadBinding, kBindingSlots>;

enum class ControllerSelection : uint8_t { Any, One, None };

struct PadConfig {
    std::array<PadBindingSlots, kPadInputCount> controller{};
    std::array<PadBindingSlots, kPadInputCount> keyboard{};
    bool keyboardEnabled = true;
    ControllerSelection controllers = ControllerSelection::Any;
    // ControllerSelection::One; the name is shown while it is disconnected.
    std::string controllerGuid;
    std::string controllerName;
    float stickDeadZone = 0.125f;
    float triggerThreshold = 0.25f;
    bool invertCameraX = false;
    bool invertCameraY = false;

    static PadConfig defaults();
    PadBindingSlots &slots(PadInput input, bool keyboardSlots) {
        return (keyboardSlots ? keyboard : controller)[static_cast<size_t>(input)];
    }
    const PadBindingSlots &slots(PadInput input, bool keyboardSlots) const {
        return (keyboardSlots ? keyboard : controller)[static_cast<size_t>(input)];
    }
};

struct ControllerInfo {
    SDL_JoystickID id = 0;
    std::string name;
    std::string guid;
    SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
    bool active = false; // drives the game under the current selection
    SDL_PowerState power = SDL_POWERSTATE_UNKNOWN;
    int batteryPercent = -1;
};

// The pad as the game last saw it.
struct PadSnapshot {
    uint32_t held = 0u;
    uint32_t sticks = 0x80808080u;
};

// The pad report's bit for a button; 0 for a stick direction.
uint32_t padButtonMask(PadInput input);
const char *padInputName(PadInput input);
// Stable lower-case names for settings files.
const char *padInputKey(PadInput input);
std::string padBindingToString(const PadBinding &binding);
PadBinding padBindingFromString(const std::string &text);

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

    void setConfig(const PadConfig &config);
    const PadConfig &config() const { return m_config; }
    std::vector<ControllerInfo> controllers() const;
    PadSnapshot snapshot() const { return {m_held, m_sticks}; }
    // The pad as the bindings read the devices now, even while the menu
    // keeps it from the game; for a live preview of the bindings.
    PadSnapshot preview() const;
    // Buttons held now stay released for the game until they are let go, so a
    // button that closed the menu does not reach the game as a fresh press.
    void suppressHeld();

private:
    struct Gamepad {
        SDL_Gamepad *handle = nullptr;
        std::string guid;
        std::array<bool, SDL_GAMEPAD_AXIS_COUNT * 2> axisPressed{};
    };
    PadSnapshot sample(const bool *keys) const;
    void openGamepad(SDL_JoystickID id);
    void openAll();
    void clearState();
    bool gamepadActive(const Gamepad &gamepad) const;
    Gamepad *findGamepad(SDL_JoystickID id);

    PadConfig m_config = PadConfig::defaults();
    std::vector<Gamepad> m_gamepads;
    bool m_initialized = false;
    uint32_t m_held = 0u;
    uint32_t m_pressed = 0u;
    uint32_t m_suppressed = 0u;
    bool m_suppressPending = false;
    uint32_t m_sticks = 0x80808080u;
};

} // namespace dq8::gfx
