#pragma once

#include "raylib.h"

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
    int interact = KEY_R;
    int debugRespawn = KEY_P;
    int cameraToggle = KEY_F5;
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
    void SetMouseSensitivity(float sensitivity);
    float GetMouseSensitivity() const;

private:
    KeyBindings bindings_ {};
    float mouseSensitivity_ = 1.0f;
    mutable double lastForwardTapTime_ = -10.0;
    mutable bool forwardWasDown_ = false;
    mutable bool doubleTapSprintActive_ = false;
};
