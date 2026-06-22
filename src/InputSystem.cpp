#include "InputSystem.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kDoubleTapSprintWindow = 0.28;

float ApplyDeadZone(float value, float deadZone)
{
    const float magnitude = std::fabs(value);
    if (magnitude <= deadZone)
    {
        return 0.0f;
    }
    return std::copysign((magnitude - deadZone) / std::max(0.001f, 1.0f - deadZone), value);
}

bool BindingDown(int binding)
{
    if (binding == KEY_NULL)
    {
        return false;
    }
    if (IsMouseBinding(binding))
    {
        return IsMouseButtonDown(MouseButtonFromBinding(binding));
    }
    return IsKeyDown(binding);
}

bool BindingPressed(int binding)
{
    if (binding == KEY_NULL)
    {
        return false;
    }
    if (IsMouseBinding(binding))
    {
        return IsMouseButtonPressed(MouseButtonFromBinding(binding));
    }
    return IsKeyPressed(binding);
}

bool BindingReleased(int binding)
{
    if (binding == KEY_NULL)
    {
        return false;
    }
    if (IsMouseBinding(binding))
    {
        return IsMouseButtonReleased(MouseButtonFromBinding(binding));
    }
    return IsKeyReleased(binding);
}
}

PlayerInput InputSystem::Poll() const
{
    PlayerInput input {};
    const bool gamepadAvailable = IsGamepadAvailable(0);
    const auto padDown = [gamepadAvailable](int button)
    {
        return gamepadAvailable && button != GAMEPAD_BUTTON_UNKNOWN && IsGamepadButtonDown(0, button);
    };
    const auto padPressed = [gamepadAvailable](int button)
    {
        return gamepadAvailable && button != GAMEPAD_BUTTON_UNKNOWN && IsGamepadButtonPressed(0, button);
    };
    const auto padReleased = [gamepadAvailable](int button)
    {
        return gamepadAvailable && button != GAMEPAD_BUTTON_UNKNOWN && IsGamepadButtonReleased(0, button);
    };
    const auto reservedForHeroAbility = [this](int key)
    {
        return key == bindings_.heroActive1 || key == bindings_.heroActive2 || key == bindings_.heroUltimate;
    };

    const bool forwardDown = BindingDown(bindings_.moveForward);
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
        sprintToggled_ = false;
    }
    forwardWasDown_ = forwardDown;

    if (forwardDown)
    {
        input.move.z += 1.0f;
    }
    if (BindingDown(bindings_.moveBackward))
    {
        input.move.z -= 1.0f;
    }
    if (BindingDown(bindings_.moveRight))
    {
        input.move.x += 1.0f;
    }
    if (BindingDown(bindings_.moveLeft))
    {
        input.move.x -= 1.0f;
    }
    if (gamepadAvailable)
    {
        input.move.x += ApplyDeadZone(GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X), gamepadDeadZone_);
        input.move.z -= ApplyDeadZone(GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y), gamepadDeadZone_);
        const float moveLength = std::sqrt(input.move.x * input.move.x + input.move.z * input.move.z);
        if (moveLength > 1.0f)
        {
            input.move.x /= moveLength;
            input.move.z /= moveLength;
        }
    }

    input.jump = BindingPressed(bindings_.jump) || padPressed(gamepadBindings_.jump);
    input.jumpHeld = BindingDown(bindings_.jump) || padDown(gamepadBindings_.jump);
    input.attackPressed = BindingPressed(bindings_.attack) || padPressed(gamepadBindings_.attack);
    input.attackHeld = BindingDown(bindings_.attack) || padDown(gamepadBindings_.attack);
    input.attackReleased = BindingReleased(bindings_.attack) || padReleased(gamepadBindings_.attack);
    input.middlePressed = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
    input.placePressed = BindingPressed(bindings_.place) || padPressed(gamepadBindings_.place);
    input.placeHeld = BindingDown(bindings_.place) || padDown(gamepadBindings_.place);
    input.sneak = BindingDown(bindings_.sneak) || padDown(gamepadBindings_.sneak);
    input.scopeHeld = IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || padDown(gamepadBindings_.place);
#if DAIBED_DEVELOPER_BUILD
    input.bridgeMode = BindingDown(bindings_.bridgeMode);
#else
    input.bridgeMode = false;
#endif
    input.cameraTogglePressed = BindingPressed(bindings_.cameraToggle) || padPressed(gamepadBindings_.cameraToggle);
    if (BindingPressed(bindings_.sprint))
    {
        sprintToggled_ = forwardDown ? !sprintToggled_ : false;
        if (sprintToggled_)
        {
            input.sprintTapped = true;
        }
    }
    if (input.sneak || input.bridgeMode || input.move.z <= 0.05f)
    {
        sprintToggled_ = false;
    }
    input.sprint = sprintToggled_ || doubleTapSprintActive_ || padDown(gamepadBindings_.sprint);
    input.interactPressed = BindingPressed(bindings_.interact) || padPressed(gamepadBindings_.interact);
    input.inventoryPressed = BindingPressed(bindings_.inventory) || padPressed(gamepadBindings_.inventory);
    input.debugRespawnPressed = BindingPressed(bindings_.debugRespawn);
    input.restartPressed = IsKeyPressed(KEY_ENTER);
    input.exitPressed = IsKeyPressed(KEY_ESCAPE);
    input.heroActive1Pressed = BindingPressed(bindings_.heroActive1) || padPressed(gamepadBindings_.heroActive1);
    input.heroActive2Pressed = BindingPressed(bindings_.heroActive2) || padPressed(gamepadBindings_.heroActive2);
    input.heroUltimatePressed = BindingPressed(bindings_.heroUltimate) || padPressed(gamepadBindings_.heroUltimate);
#if DAIBED_DEVELOPER_BUILD
    input.shootPressed = BindingPressed(bindings_.shoot) && !reservedForHeroAbility(bindings_.shoot);
    input.fireballPressed = BindingPressed(bindings_.fireball) && !reservedForHeroAbility(bindings_.fireball);
    input.healPressed = BindingPressed(bindings_.heal) && !reservedForHeroAbility(bindings_.heal);
    input.teleportPressed = BindingPressed(bindings_.teleport) && !reservedForHeroAbility(bindings_.teleport);
    input.dashPressed = BindingPressed(bindings_.dash) && !reservedForHeroAbility(bindings_.dash);
    input.molotovPressed = BindingPressed(bindings_.molotov) && !reservedForHeroAbility(bindings_.molotov);
    input.alarmPressed = BindingPressed(bindings_.alarm) && !reservedForHeroAbility(bindings_.alarm);
#endif
    input.dropPressed = BindingPressed(bindings_.drop) || padPressed(gamepadBindings_.drop);
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
    if (gamepadAvailable)
    {
        const float lookX = ApplyDeadZone(GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_X), gamepadDeadZone_);
        const float lookY = ApplyDeadZone(GetGamepadAxisMovement(0, GAMEPAD_AXIS_RIGHT_Y), gamepadDeadZone_);
        input.yawDelta += lookX * 0.055f * gamepadSensitivity_;
        input.pitchDelta -= lookY * 0.045f * gamepadSensitivity_;
    }
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

const GamepadBindings& InputSystem::GetGamepadBindings() const
{
    return gamepadBindings_;
}

GamepadBindings& InputSystem::MutableGamepadBindings()
{
    return gamepadBindings_;
}

void InputSystem::SetMouseSensitivity(float sensitivity)
{
    mouseSensitivity_ = sensitivity;
}

float InputSystem::GetMouseSensitivity() const
{
    return mouseSensitivity_;
}

void InputSystem::SetGamepadDeadZone(float deadZone)
{
    gamepadDeadZone_ = std::clamp(deadZone, 0.05f, 0.45f);
}

float InputSystem::GetGamepadDeadZone() const
{
    return gamepadDeadZone_;
}

void InputSystem::SetGamepadSensitivity(float sensitivity)
{
    gamepadSensitivity_ = std::clamp(sensitivity, 0.4f, 2.5f);
}

float InputSystem::GetGamepadSensitivity() const
{
    return gamepadSensitivity_;
}
