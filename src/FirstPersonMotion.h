#pragma once

#include "raylib.h"

enum class FirstPersonMotionMode
{
    Off = 0,
    Reduced = 1,
    Full = 2,
};

struct FirstPersonMotionInput
{
    Vector3 velocity {};
    bool onGround = false;
    bool sprinting = false;
    bool jumpHeld = false;
    float scopeBlend = 0.0f;
    Vector2 localVelocity {};
    float yawDelta = 0.0f;
    float pitchDelta = 0.0f;
};

// Local-space presentation offsets: X = camera right, Y = world/camera up,
// Z = camera forward. They never feed back into movement, aiming or networking.
struct FirstPersonMotionPose
{
    Vector3 cameraOffset {};
    Vector3 viewmodelOffset {};
    float viewmodelRollDegrees = 0.0f;
    float fovOffset = 0.0f;
};

class FirstPersonMotion
{
public:
    void Reset(bool onGround = false);
    void SetMode(FirstPersonMotionMode mode);
    void Update(const FirstPersonMotionInput& input, float dt);

    const FirstPersonMotionPose& GetPose() const;

private:
    float phase_ = 0.0f;
    float moveBlend_ = 0.0f;
    float sprintBlend_ = 0.0f;
    float verticalResponse_ = 0.0f;
    float verticalResponseVelocity_ = 0.0f;
    float strafeLag_ = 0.0f;
    float strafeLagVelocity_ = 0.0f;
    float forwardLag_ = 0.0f;
    float forwardLagVelocity_ = 0.0f;
    float lookSwayX_ = 0.0f;
    float lookSwayXVelocity_ = 0.0f;
    float lookSwayY_ = 0.0f;
    float lookSwayYVelocity_ = 0.0f;
    float previousVerticalVelocity_ = 0.0f;
    float hopContinuityTimer_ = 0.0f;
    bool previousOnGround_ = false;
    bool initialized_ = false;
    FirstPersonMotionMode mode_ = FirstPersonMotionMode::Full;
    FirstPersonMotionPose pose_ {};
};

// Pure, window-free diagnostic used by --first-person-motion-smoke.
int RunFirstPersonMotionSmoke();
