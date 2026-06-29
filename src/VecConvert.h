#pragma once

#include "Simulation/SimMath.h"
#include "raylib.h"

// Conversion between the raylib-free simulation vector (Vec3) and raylib's
// Vector3. This lives at the Game/render boundary (it includes raylib.h) so the
// simulation-side headers (Generator, Resource, MatchSimulation, snapshots) can
// stay raylib-free while Game/Renderer/Bot code converts at the edges. See
// docs/NETWORK_PREP_PLAN.md.

inline Vector3 ToVector3(const Vec3& v) { return Vector3 { v.x, v.y, v.z }; }
inline Vec3 ToVec3(const Vector3& v) { return Vec3 { v.x, v.y, v.z }; }
