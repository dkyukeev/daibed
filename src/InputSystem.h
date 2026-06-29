#pragma once

#include "raylib.h"

constexpr int kMouseBindingOffset = 10000;

constexpr int MouseBinding(int button) { return kMouseBindingOffset + button; }
constexpr bool IsMouseBinding(int binding) { return binding >= kMouseBindingOffset && binding < kMouseBindingOffset + 32; }
constexpr int MouseButtonFromBinding(int binding) { return binding - kMouseBindingOffset; }

struct KeyBindings
{
    int moveForward = KEY_W;
    int moveBackward = KEY_S;
    int moveLeft = KEY_A;
    int moveRight = KEY_D;
    int jump = KEY_SPACE;
    int sneak = KEY_LEFT_SHIFT;
    int bridgeMode = KEY_C;
    int sprint = KEY_LEFT_CONTROL;
    int attack = MouseBinding(MOUSE_BUTTON_LEFT);
    int place = MouseBinding(MOUSE_BUTTON_RIGHT);
    int interact = KEY_R;
    int inventory = KEY_E;
    int drop = KEY_Q;
    int debugRespawn = KEY_P;
    int cameraToggle = KEY_F5;
    int heroActive1 = KEY_F;
    int heroActive2 = KEY_G;
    int heroUltimate = KEY_H;
    int shoot = KEY_B;
    int fireball = KEY_G;
    int heal = KEY_H;
    int teleport = KEY_T;
    int dash = KEY_F;
    int molotov = KEY_M;
    int alarm = KEY_N;
};

struct GamepadBindings
{
    int jump = GAMEPAD_BUTTON_RIGHT_FACE_DOWN;
    int sneak = GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
    int sprint = GAMEPAD_BUTTON_LEFT_TRIGGER_1;
    int attack = GAMEPAD_BUTTON_RIGHT_TRIGGER_2;
    int place = GAMEPAD_BUTTON_LEFT_TRIGGER_2;
    int interact = GAMEPAD_BUTTON_RIGHT_FACE_RIGHT;
    int inventory = GAMEPAD_BUTTON_MIDDLE_RIGHT;
    int drop = GAMEPAD_BUTTON_RIGHT_FACE_UP;
    int cameraToggle = GAMEPAD_BUTTON_LEFT_THUMB;
    int heroActive1 = GAMEPAD_BUTTON_RIGHT_TRIGGER_1;
    int heroActive2 = GAMEPAD_BUTTON_LEFT_FACE_LEFT;
    int heroUltimate = GAMEPAD_BUTTON_LEFT_FACE_UP;
};

struct PlayerInput
{
    Vector3 move {};
    bool jump = false;
    bool jumpHeld = false;
    bool attackPressed = false;
    bool attackHeld = false;
    bool attackReleased = false;
    bool middlePressed = false;
    bool placePressed = false;
    bool placeHeld = false;
    bool sneak = false;
    bool bridgeMode = false;
    bool scopeHeld = false;
    bool cameraTogglePressed = false;
    bool sprint = false;
    bool sprintTapped = false;
    bool interactPressed = false;
    bool inventoryPressed = false;
    bool debugRespawnPressed = false;
    bool restartPressed = false;
    bool exitPressed = false;
    bool shootPressed = false;
    bool fireballPressed = false;
    bool healPressed = false;
    bool teleportPressed = false;
    bool dashPressed = false;
    bool molotovPressed = false;
    bool alarmPressed = false;
    bool heroActive1Pressed = false;
    bool heroActive2Pressed = false;
    bool heroUltimatePressed = false;
    bool dropPressed = false;
    bool botDebugPressed = false;
    int shopChoice = 0;
    int hotbarSlot = 0;
    float mouseWheel = 0.0f;
    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;
};

class InputSystem
{
public:
    PlayerInput Poll() const;

    const KeyBindings& GetBindings() const;
    KeyBindings& MutableBindings();
    const GamepadBindings& GetGamepadBindings() const;
    GamepadBindings& MutableGamepadBindings();
    void SetMouseSensitivity(float sensitivity);
    float GetMouseSensitivity() const;
    // Developer kit: when enabled, Poll() adds keyboard fallbacks for the
    // otherwise mouse-only actions (attack/place/break/scope and look), so the
    // whole game is drivable without a mouse. Additive (OR'd with mouse), so it
    // never disturbs normal play. See --dev-keyboard / docs/NETWORK_PREP_PLAN.md.
    void SetDevKeyboard(bool enabled);
    bool IsDevKeyboard() const;
    void SetGamepadDeadZone(float deadZone);
    float GetGamepadDeadZone() const;
    void SetGamepadSensitivity(float sensitivity);
    float GetGamepadSensitivity() const;

private:
    KeyBindings bindings_ {};
    GamepadBindings gamepadBindings_ {};
    float mouseSensitivity_ = 1.0f;
    bool devKeyboard_ = false;
    float gamepadDeadZone_ = 0.18f;
    float gamepadSensitivity_ = 1.0f;
    mutable double lastForwardTapTime_ = -10.0;
    mutable bool forwardWasDown_ = false;
    mutable bool doubleTapSprintActive_ = false;
    mutable bool sprintToggled_ = false;
};
