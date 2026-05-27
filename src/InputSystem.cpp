#include "InputSystem.h"

namespace
{
constexpr double kDoubleTapSprintWindow = 0.28;
}

PlayerInput InputSystem::Poll() const
{
    PlayerInput input {};
    const bool forwardDown = IsKeyDown(bindings_.moveForward);
    const bool forwardPressed = forwardDown && !forwardWasDown_;
    const double now = GetTime();
    if (forwardPressed)
    {
        if (now - lastForwardTapTime_ <= kDoubleTapSprintWindow)
        {
            doubleTapSprintActive_ = true;
            input.sprintTapped = true;
        }
        lastForwardTapTime_ = now;
    }
    if (!forwardDown)
    {
        doubleTapSprintActive_ = false;
    }
    forwardWasDown_ = forwardDown;

    if (forwardDown)
    {
        input.move.z += 1.0f;
    }
    if (IsKeyDown(bindings_.moveBackward))
    {
        input.move.z -= 1.0f;
    }
    if (IsKeyDown(bindings_.moveRight))
    {
        input.move.x += 1.0f;
    }
    if (IsKeyDown(bindings_.moveLeft))
    {
        input.move.x -= 1.0f;
    }

    input.jump = IsKeyPressed(bindings_.jump);
    input.jumpHeld = IsKeyDown(bindings_.jump);
    input.attackPressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    input.attackHeld = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    input.attackReleased = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    input.middlePressed = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
    input.placePressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    input.placeHeld = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    input.sneak = IsKeyDown(bindings_.sneak);
    input.bridgeMode = IsKeyDown(bindings_.bridgeMode);
    input.cameraTogglePressed = IsKeyPressed(bindings_.cameraToggle);
    input.sprint = IsKeyDown(bindings_.sprint) || doubleTapSprintActive_;
    input.interactPressed = IsKeyPressed(bindings_.interact);
    input.inventoryPressed = IsKeyPressed(KEY_E);
    input.debugRespawnPressed = IsKeyPressed(bindings_.debugRespawn);
    input.restartPressed = IsKeyPressed(KEY_ENTER);
    input.exitPressed = IsKeyPressed(KEY_ESCAPE);
    input.shootPressed = IsKeyPressed(KEY_B);
    input.fireballPressed = IsKeyPressed(KEY_G);
    input.healPressed = IsKeyPressed(KEY_H);
    input.teleportPressed = IsKeyPressed(KEY_T);
    input.dashPressed = IsKeyPressed(KEY_F);
    input.molotovPressed = IsKeyPressed(KEY_M);
    input.alarmPressed = IsKeyPressed(KEY_N);
    input.dropPressed = IsKeyPressed(KEY_Q);
    input.botDebugPressed = IsKeyPressed(KEY_F3);

    if (IsKeyPressed(KEY_ONE))
    {
        input.shopChoice = 1;
        input.hotbarSlot = 1;
    }
    else if (IsKeyPressed(KEY_TWO))
    {
        input.shopChoice = 2;
        input.hotbarSlot = 2;
    }
    else if (IsKeyPressed(KEY_THREE))
    {
        input.shopChoice = 3;
        input.hotbarSlot = 3;
    }
    else if (IsKeyPressed(KEY_FOUR))
    {
        input.shopChoice = 4;
        input.hotbarSlot = 4;
    }
    else if (IsKeyPressed(KEY_FIVE))
    {
        input.shopChoice = 5;
        input.hotbarSlot = 5;
    }
    else if (IsKeyPressed(KEY_SIX))
    {
        input.shopChoice = 6;
        input.hotbarSlot = 6;
    }
    else if (IsKeyPressed(KEY_SEVEN))
    {
        input.shopChoice = 7;
        input.hotbarSlot = 7;
    }
    else if (IsKeyPressed(KEY_EIGHT))
    {
        input.shopChoice = 8;
        input.hotbarSlot = 8;
    }
    else if (IsKeyPressed(KEY_NINE))
    {
        input.shopChoice = 9;
        input.hotbarSlot = 9;
    }

    const Vector2 mouseDelta = GetMouseDelta();
    input.mouseWheel = GetMouseWheelMove();
    input.yawDelta = mouseDelta.x * 0.0035f * mouseSensitivity_;
    input.pitchDelta = -mouseDelta.y * 0.0028f * mouseSensitivity_;
    return input;
}

const KeyBindings& InputSystem::GetBindings() const
{
    return bindings_;
}

KeyBindings& InputSystem::MutableBindings()
{
    return bindings_;
}

void InputSystem::SetMouseSensitivity(float sensitivity)
{
    mouseSensitivity_ = sensitivity;
}

float InputSystem::GetMouseSensitivity() const
{
    return mouseSensitivity_;
}
