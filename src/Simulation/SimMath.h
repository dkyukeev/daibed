#pragma once

#include <cmath>

// Raylib-free 3D vector for the simulation / replication layer. The simulation
// and network snapshots use this instead of raylib's Vector3 so they build and
// link without the renderer. This is the prerequisite for migrating spatial
// state (generators, then world/players) into the raylib-free MatchSimulation —
// see docs/MULTIPLAYER_TARGET_ARCHITECTURE.md.
//
// Conversion to/from raylib Vector3 happens at the Game boundary, where raylib
// is already included (e.g. Vec3{ v.x, v.y, v.z }); this header never pulls in
// raylib so it stays usable from headless/server code.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    float LengthSquared() const { return x * x + y * y + z * z; }
    float Length() const { return std::sqrt(LengthSquared()); }
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return Vec3 { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return Vec3 { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(const Vec3& v, float s) { return Vec3 { v.x * s, v.y * s, v.z * s }; }
