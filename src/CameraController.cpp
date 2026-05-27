#include "CameraController.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kHalfPi = 1.57079632679f;
constexpr float kMinPitch = -kHalfPi + 0.01f;
constexpr float kMaxPitch = kHalfPi - 0.01f;
constexpr float kCameraDistance = 6.8f;
constexpr float kFocusHeight = 1.05f;
constexpr float kEyeHeight = 0.78f;
constexpr float kCameraLift = 1.15f;

float Length2D(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.z * value.z);
}
}

void CameraController::Reset(float yaw, float pitch, Vector3 focus)
{
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, kMinPitch, kMaxPitch);
    mode_ = ViewMode::FirstPerson;
    shakeTimer_ = 0.0f;
    shakeDuration_ = 0.0f;
    shakeIntensity_ = 0.0f;

    camera_.fovy = fov_;
    camera_.projection = CAMERA_PERSPECTIVE;
    camera_.up = Vector3 { 0.0f, 1.0f, 0.0f };

    const Vector3 look = LookDirection();
    const Vector3 target { focus.x, focus.y + kEyeHeight, focus.z };
    camera_.target = Vector3 {
        target.x + look.x * 4.5f,
        target.y + look.y * 4.5f,
        target.z + look.z * 4.5f
    };
    camera_.position = target;
}

void CameraController::AddLook(float yawDelta, float pitchDelta)
{
    yaw_ += yawDelta;
    pitch_ = std::clamp(pitch_ + pitchDelta, kMinPitch, kMaxPitch);
}

void CameraController::AddShake(float intensity, float duration)
{
    shakeIntensity_ = std::max(shakeIntensity_, intensity);
    shakeDuration_ = std::max(shakeDuration_, duration);
    shakeTimer_ = std::max(shakeTimer_, duration);
}

void CameraController::Update(Vector3 focus, float dt)
{
    const Vector3 look = LookDirection();
    const Vector3 targetBase {
        focus.x,
        focus.y + (mode_ == ViewMode::FirstPerson ? kEyeHeight : kFocusHeight),
        focus.z
    };
    Vector3 desiredTarget {
        targetBase.x + look.x * 4.5f,
        targetBase.y + look.y * 4.5f,
        targetBase.z + look.z * 4.5f
    };
    Vector3 desiredPosition {};
    if (mode_ == ViewMode::FirstPerson)
    {
        desiredPosition = targetBase;
    }
    else
    {
        desiredPosition = Vector3 {
            targetBase.x - look.x * kCameraDistance,
            std::max(targetBase.y + 0.45f, targetBase.y + kCameraLift - look.y * 2.3f),
            targetBase.z - look.z * kCameraDistance
        };
    }

    if (shakeTimer_ > 0.0f && shakeDuration_ > 0.0f)
    {
        const float t = shakeTimer_ / shakeDuration_;
        const float waveA = std::sin((shakeDuration_ - shakeTimer_) * 82.0f);
        const float waveB = std::sin((shakeDuration_ - shakeTimer_) * 61.0f + 1.7f);
        const Vector3 right = GetFlatRight();
        const Vector3 up { 0.0f, 1.0f, 0.0f };
        const Vector3 offset {
            right.x * waveA * shakeIntensity_ * t,
            up.y * waveB * shakeIntensity_ * 0.55f * t,
            right.z * waveA * shakeIntensity_ * t
        };
        desiredPosition.x += offset.x;
        desiredPosition.y += offset.y;
        desiredPosition.z += offset.z;
        desiredTarget.x += offset.x * 0.35f;
        desiredTarget.y += offset.y * 0.35f;
        desiredTarget.z += offset.z * 0.35f;
        shakeTimer_ = std::max(0.0f, shakeTimer_ - dt);
    }

    if (mode_ == ViewMode::FirstPerson)
    {
        camera_.position = desiredPosition;
        camera_.target = desiredTarget;
    }
    else
    {
        camera_.position = Smooth(camera_.position, desiredPosition, 14.0f, dt);
        camera_.target = Smooth(camera_.target, desiredTarget, 18.0f, dt);
    }
}

void CameraController::ToggleMode()
{
    mode_ = mode_ == ViewMode::FirstPerson ? ViewMode::ThirdPerson : ViewMode::FirstPerson;
}

void CameraController::SetMode(ViewMode mode)
{
    mode_ = mode;
}

void CameraController::SetFov(float fov)
{
    fov_ = std::clamp(fov, 45.0f, 95.0f);
    camera_.fovy = fov_;
}

const Camera3D& CameraController::GetCamera() const
{
    return camera_;
}

Vector3 CameraController::GetAimOrigin() const
{
    return camera_.position;
}

Vector3 CameraController::GetAimDirection() const
{
    return LookDirection();
}

Vector3 CameraController::GetFlatForward() const
{
    Vector3 forward { std::sin(yaw_), 0.0f, -std::cos(yaw_) };
    const float length = Length2D(forward);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, -1.0f };
    }

    return Vector3 { forward.x / length, 0.0f, forward.z / length };
}

Vector3 CameraController::GetFlatRight() const
{
    return Vector3 { std::cos(yaw_), 0.0f, std::sin(yaw_) };
}

float CameraController::GetYaw() const
{
    return yaw_;
}

float CameraController::GetPitch() const
{
    return pitch_;
}

ViewMode CameraController::GetMode() const
{
    return mode_;
}

const char* CameraController::GetModeName() const
{
    return mode_ == ViewMode::FirstPerson ? "First-person" : "Third-person";
}

Vector3 CameraController::LookDirection() const
{
    const float cp = std::cos(pitch_);
    return Vector3 {
        std::sin(yaw_) * cp,
        std::sin(pitch_),
        -std::cos(yaw_) * cp
    };
}

Vector3 CameraController::Smooth(Vector3 current, Vector3 target, float sharpness, float dt) const
{
    const float t = 1.0f - std::exp(-sharpness * dt);
    return Vector3 {
        current.x + (target.x - current.x) * t,
        current.y + (target.y - current.y) * t,
        current.z + (target.z - current.z) * t
    };
}
