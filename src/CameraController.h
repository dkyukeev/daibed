#pragma once

#include "raylib.h"

enum class ViewMode
{
    FirstPerson,
    ThirdPerson
};

class CameraController
{
public:
    void Reset(float yaw, float pitch, Vector3 focus);
    void AddLook(float yawDelta, float pitchDelta);
    void AddShake(float intensity, float duration);
    void Update(Vector3 focus, float dt);
    void ToggleMode();
    void SetMode(ViewMode mode);
    void SetFov(float fov);

    const Camera3D& GetCamera() const;
    Vector3 GetAimOrigin() const;
    Vector3 GetAimDirection() const;
    Vector3 GetFlatForward() const;
    Vector3 GetFlatRight() const;
    ViewMode GetMode() const;
    const char* GetModeName() const;
    float GetYaw() const;
    float GetPitch() const;

private:
    Vector3 LookDirection() const;
    Vector3 Smooth(Vector3 current, Vector3 target, float sharpness, float dt) const;

    Camera3D camera_ {};
    float yaw_ = 0.0f;
    float pitch_ = -0.12f;
    float shakeTimer_ = 0.0f;
    float shakeDuration_ = 0.0f;
    float shakeIntensity_ = 0.0f;
    ViewMode mode_ = ViewMode::FirstPerson;
    float fov_ = 62.0f;
};
