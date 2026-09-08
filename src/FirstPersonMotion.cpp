#include "FirstPersonMotion.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
constexpr float kPi = 3.14159265359f;
constexpr float kTau = kPi * 2.0f;

float HorizontalSpeed(Vector3 velocity)
{
    return std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
}

float ExpApproach(float current, float target, float sharpness, float dt)
{
    return target + (current - target) * std::exp(-sharpness * dt);
}

// Exact critically-damped spring integration. Unlike explicit Euler this
// remains stable and nearly identical at common render frame rates.
void UpdateCriticalSpring(float& value, float& velocity, float target, float sharpness, float dt)
{
    const float displacement = value - target;
    const float junction = velocity + displacement * sharpness;
    const float decay = std::exp(-sharpness * dt);
    value = target + (displacement + junction * dt) * decay;
    velocity = (velocity - junction * sharpness * dt) * decay;
}

float VectorLength(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

bool NearlyZero(const FirstPersonMotionPose& pose)
{
    return VectorLength(pose.cameraOffset) < 0.00001f
        && VectorLength(pose.viewmodelOffset) < 0.00001f
        && std::fabs(pose.viewmodelRollDegrees) < 0.00001f
        && std::fabs(pose.fovOffset) < 0.00001f;
}
}

void FirstPersonMotion::Reset(bool onGround)
{
    phase_ = 0.0f;
    moveBlend_ = 0.0f;
    sprintBlend_ = 0.0f;
    verticalResponse_ = 0.0f;
    verticalResponseVelocity_ = 0.0f;
    strafeLag_ = 0.0f;
    strafeLagVelocity_ = 0.0f;
    forwardLag_ = 0.0f;
    forwardLagVelocity_ = 0.0f;
    lookSwayX_ = 0.0f;
    lookSwayXVelocity_ = 0.0f;
    lookSwayY_ = 0.0f;
    lookSwayYVelocity_ = 0.0f;
    previousVerticalVelocity_ = 0.0f;
    hopContinuityTimer_ = 0.0f;
    previousOnGround_ = onGround;
    initialized_ = true;
    pose_ = FirstPersonMotionPose {};
}

void FirstPersonMotion::SetMode(FirstPersonMotionMode mode)
{
    mode_ = mode;
    if (mode_ == FirstPersonMotionMode::Off)
    {
        pose_ = FirstPersonMotionPose {};
    }
}

void FirstPersonMotion::Update(const FirstPersonMotionInput& input, float dt)
{
    if (dt <= 0.0f)
    {
        return;
    }
    dt = std::min(dt, 0.05f);
    if (!initialized_)
    {
        Reset(input.onGround);
        previousVerticalVelocity_ = input.velocity.y;
    }

    const float horizontalSpeed = HorizontalSpeed(input.velocity);
    const float speedBlend = std::clamp((horizontalSpeed - 0.18f) / 4.15f, 0.0f, 1.0f);
    const bool chainedHop = input.jumpHeld
        && !input.onGround
        && previousVerticalVelocity_ < -1.15f
        && input.velocity.y > 1.15f;
    const bool tookOff = previousOnGround_ && !input.onGround && input.velocity.y > 0.8f;
    const bool landed = !previousOnGround_ && input.onGround;

    if (input.jumpHeld && (tookOff || chainedHop || landed))
    {
        // Keep the gait phase alive through the short ground contact of a held
        // jump. Some fixed-tick paths rebound without exposing an on-ground
        // render frame, hence the falling->rising velocity transition above.
        hopContinuityTimer_ = 0.34f;
    }
    else
    {
        hopContinuityTimer_ = std::max(0.0f, hopContinuityTimer_ - dt);
    }

    // Held-jump is a normal locomotion state in this game, not a sequence of
    // isolated jumps. Preserve cadence for the whole airborne arc.
    const bool locomotionContinuous = input.onGround
        || (input.jumpHeld && horizontalSpeed > 0.18f);
    const float moveTarget = locomotionContinuous ? speedBlend : 0.0f;
    moveBlend_ = ExpApproach(moveBlend_, moveTarget, moveTarget > moveBlend_ ? 12.0f : 7.0f, dt);
    const float sprintTarget = input.sprinting && horizontalSpeed > 0.35f ? 1.0f : 0.0f;
    sprintBlend_ = ExpApproach(sprintBlend_, sprintTarget, sprintTarget > sprintBlend_ ? 7.5f : 5.5f, dt);

    if (horizontalSpeed > 0.18f && locomotionContinuous)
    {
        const float cadence = 1.55f + speedBlend * 0.30f + sprintBlend_ * 0.48f;
        const float airborneCadence = input.onGround ? 1.0f : 0.72f;
        phase_ = std::fmod(phase_ + kTau * cadence * airborneCadence * dt, kTau);
    }

    if (tookOff)
    {
        verticalResponseVelocity_ += input.jumpHeld ? 0.19f : 0.27f;
    }
    if (landed || chainedHop)
    {
        const float impactSpeed = std::max(0.0f, -previousVerticalVelocity_);
        const float impact = std::clamp(0.040f + impactSpeed * 0.020f, 0.040f, 0.18f);
        verticalResponseVelocity_ -= impact * (chainedHop ? 0.38f : 1.0f);
    }
    UpdateCriticalSpring(verticalResponse_, verticalResponseVelocity_, 0.0f, 16.0f, dt);
    verticalResponse_ = std::clamp(verticalResponse_, -0.055f, 0.028f);

    const float strafeTarget = std::clamp(input.localVelocity.x / 5.53f, -1.0f, 1.0f);
    const float forwardTarget = std::clamp(input.localVelocity.y / 5.53f, -1.0f, 1.0f);
    UpdateCriticalSpring(strafeLag_, strafeLagVelocity_, strafeTarget, 10.5f, dt);
    UpdateCriticalSpring(forwardLag_, forwardLagVelocity_, forwardTarget, 10.5f, dt);
    const float strafeInertia = std::clamp(strafeLag_ - strafeTarget, -1.0f, 1.0f);
    const float forwardInertia = std::clamp(forwardLag_ - forwardTarget, -1.0f, 1.0f);

    // Look deltas add angular impulses, then a critical spring returns the
    // viewmodel to centre. Camera orientation and aim are never modified.
    lookSwayXVelocity_ -= std::clamp(input.yawDelta, -0.16f, 0.16f) * 34.0f;
    lookSwayYVelocity_ += std::clamp(input.pitchDelta, -0.14f, 0.14f) * 28.0f;
    UpdateCriticalSpring(lookSwayX_, lookSwayXVelocity_, 0.0f, 15.0f, dt);
    UpdateCriticalSpring(lookSwayY_, lookSwayYVelocity_, 0.0f, 15.0f, dt);
    lookSwayX_ = std::clamp(lookSwayX_, -0.030f, 0.030f);
    lookSwayY_ = std::clamp(lookSwayY_, -0.022f, 0.022f);

    previousOnGround_ = input.onGround;
    previousVerticalVelocity_ = input.velocity.y;

    if (mode_ == FirstPersonMotionMode::Off)
    {
        pose_ = FirstPersonMotionPose {};
        return;
    }

    const float cameraScale = mode_ == FirstPersonMotionMode::Reduced ? 0.28f : 1.0f;
    const float viewmodelScale = mode_ == FirstPersonMotionMode::Reduced ? 0.58f : 1.0f;
    const float cyclicScale = mode_ == FirstPersonMotionMode::Reduced ? 0.0f : 1.0f;
    const float scopeScale = 1.0f - 0.88f * std::clamp(input.scopeBlend, 0.0f, 1.0f);
    const float gait = moveBlend_ * scopeScale;
    const float sideWave = std::sin(phase_);
    const float stepWave = std::cos(phase_ * 2.0f);
    const float sprintAmplitude = 1.0f + sprintBlend_ * 0.58f;
    const float airborneScale = input.onGround ? 1.0f
        : (input.jumpHeld && hopContinuityTimer_ > 0.0f ? 0.42f : 0.18f);
    const float airVelocity = input.onGround ? 0.0f
        : std::clamp(input.velocity.y / 6.72f, -1.0f, 1.0f);

    pose_.cameraOffset = Vector3 {
        sideWave * 0.0055f * gait * sprintAmplitude * airborneScale * cameraScale * cyclicScale,
        (stepWave * 0.0070f * gait * sprintAmplitude * airborneScale * cyclicScale
            + verticalResponse_ + airVelocity * 0.0050f
            + (input.onGround ? 0.0f : (1.0f - std::min(1.0f, std::fabs(airVelocity) * 2.0f)) * 0.0030f))
            * cameraScale * scopeScale,
        -std::fabs(sideWave) * 0.0030f * gait * sprintBlend_ * cameraScale * cyclicScale,
    };
    pose_.viewmodelOffset = Vector3 {
        (sideWave * 0.032f * gait * sprintAmplitude * airborneScale * cyclicScale
            + strafeInertia * 0.026f + lookSwayX_) * viewmodelScale,
        (stepWave * 0.022f * gait * sprintAmplitude * airborneScale * cyclicScale
            + verticalResponse_ * 2.15f - airVelocity * 0.026f) * viewmodelScale * scopeScale,
        (std::fabs(sideWave) * -0.015f * gait * sprintAmplitude * cyclicScale
            + forwardInertia * 0.022f + lookSwayY_
            + verticalResponse_ * 0.65f) * viewmodelScale * scopeScale,
    };
    pose_.viewmodelRollDegrees = (-sideWave * gait * (0.72f + sprintBlend_ * 0.85f) * cyclicScale
        + strafeInertia * 0.65f - lookSwayX_ * 15.0f) * viewmodelScale * scopeScale;
    pose_.fovOffset = (sprintBlend_ * 4.8f
        + std::max(0.0f, -stepWave) * sprintBlend_ * gait * 0.28f * cyclicScale)
        * (mode_ == FirstPersonMotionMode::Reduced ? 0.45f : 1.0f)
        * scopeScale;
}

const FirstPersonMotionPose& FirstPersonMotion::GetPose() const
{
    return pose_;
}

int RunFirstPersonMotionSmoke()
{
#if DAIBED_DIAGNOSTICS
    constexpr float dt = 1.0f / 120.0f;
    FirstPersonMotion motion;
    motion.SetMode(FirstPersonMotionMode::Off);
    motion.Reset(true);
    motion.Update(FirstPersonMotionInput { Vector3 { 4.32f, 0.0f, 0.0f }, true, false, false, 0.0f }, dt);
    if (!NearlyZero(motion.GetPose()))
    {
        std::cerr << "first-person-motion smoke failed: Off mode produced an offset\n";
        return 2;
    }

    motion.SetMode(FirstPersonMotionMode::Full);
    motion.Reset(true);
    float walkPeak = 0.0f;
    for (int i = 0; i < 240; ++i)
    {
        motion.Update(FirstPersonMotionInput { Vector3 { 4.32f, 0.0f, 0.0f }, true, false, false, 0.0f }, dt);
        walkPeak = std::max(walkPeak, VectorLength(motion.GetPose().viewmodelOffset));
    }

    motion.Reset(true);
    float sprintPeak = 0.0f;
    for (int i = 0; i < 240; ++i)
    {
        motion.Update(FirstPersonMotionInput { Vector3 { 5.53f, 0.0f, 0.0f }, true, true, false, 0.0f }, dt);
        sprintPeak = std::max(sprintPeak, VectorLength(motion.GetPose().viewmodelOffset));
    }
    if (walkPeak < 0.015f || sprintPeak <= walkPeak * 1.15f)
    {
        std::cerr << "first-person-motion smoke failed: gait amplitudes walk="
                  << walkPeak << " sprint=" << sprintPeak << "\n";
        return 3;
    }

    const auto sampleWalkAtRate = [](float sampleDt, int frames)
    {
        FirstPersonMotion sampled;
        sampled.SetMode(FirstPersonMotionMode::Full);
        sampled.Reset(true);
        for (int i = 0; i < frames; ++i)
        {
            sampled.Update(FirstPersonMotionInput { Vector3 { 4.32f, 0.0f, 0.0f }, true, false, false, 0.0f }, sampleDt);
        }
        return sampled.GetPose();
    };
    const FirstPersonMotionPose pose60 = sampleWalkAtRate(1.0f / 60.0f, 120);
    const FirstPersonMotionPose pose144 = sampleWalkAtRate(1.0f / 144.0f, 288);
    const float frameRateError = VectorLength(Vector3 {
        pose60.viewmodelOffset.x - pose144.viewmodelOffset.x,
        pose60.viewmodelOffset.y - pose144.viewmodelOffset.y,
        pose60.viewmodelOffset.z - pose144.viewmodelOffset.z });
    if (frameRateError > 0.001f)
    {
        std::cerr << "first-person-motion smoke failed: frame-rate error="
                  << frameRateError << "\n";
        return 4;
    }

    motion.SetMode(FirstPersonMotionMode::Reduced);
    motion.Reset(true);
    for (int i = 0; i < 240; ++i)
    {
        motion.Update(FirstPersonMotionInput { Vector3 { 4.32f, 0.0f, 0.0f }, true, false, false, 0.0f }, dt);
    }
    const float reducedSteadyGait = VectorLength(motion.GetPose().cameraOffset)
        + VectorLength(motion.GetPose().viewmodelOffset);
    motion.Update(FirstPersonMotionInput { Vector3 { 4.32f, 6.72f, 0.0f }, false, false, true, 0.0f }, dt);
    const float reducedJumpFeedback = VectorLength(motion.GetPose().cameraOffset)
        + VectorLength(motion.GetPose().viewmodelOffset);
    if (reducedSteadyGait > 0.001f || reducedJumpFeedback < 0.003f)
    {
        std::cerr << "first-person-motion smoke failed: Reduced gait="
                  << reducedSteadyGait << " jump=" << reducedJumpFeedback << "\n";
        return 5;
    }

    // A held-jump chain may go directly from falling to rising between render
    // samples. Verify it stays bounded and does not reset into a camera kick.
    motion.SetMode(FirstPersonMotionMode::Full);
    motion.Reset(true);
    motion.Update(FirstPersonMotionInput { Vector3 { 5.53f, 6.72f, 0.0f }, false, true, true, 0.0f }, dt);
    for (int i = 0; i < 75; ++i)
    {
        const float vertical = 6.2f - static_cast<float>(i) * 0.16f;
        motion.Update(FirstPersonMotionInput { Vector3 { 5.53f, vertical, 0.0f }, false, true, true, 0.0f }, dt);
    }
    const FirstPersonMotionPose beforeBounce = motion.GetPose();
    motion.Update(FirstPersonMotionInput { Vector3 { 5.53f, 6.72f, 0.0f }, false, true, true, 0.0f }, dt);
    const FirstPersonMotionPose afterBounce = motion.GetPose();
    const float bounceDelta = VectorLength(Vector3 {
        afterBounce.cameraOffset.x - beforeBounce.cameraOffset.x,
        afterBounce.cameraOffset.y - beforeBounce.cameraOffset.y,
        afterBounce.cameraOffset.z - beforeBounce.cameraOffset.z });
    if (bounceDelta > 0.025f || VectorLength(afterBounce.cameraOffset) > 0.075f)
    {
        std::cerr << "first-person-motion smoke failed: held-hop camera discontinuity="
                  << bounceDelta << "\n";
        return 6;
    }

    std::cout << "first-person-motion smoke passed: walk=" << walkPeak
              << " sprint=" << sprintPeak
              << " fps-error=" << frameRateError
              << " reduced-jump=" << reducedJumpFeedback
              << " held-hop-delta=" << bounceDelta << "\n";
    return 0;
#else
    std::cerr << "diagnostic smokes are disabled in this build\n";
    return 2;
#endif
}
