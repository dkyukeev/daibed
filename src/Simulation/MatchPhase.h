#pragma once

// Lifecycle phase of a match. Raylib-free leaf header shared by MatchSimulation
// (which owns the phase) and the network snapshot. See docs/MULTIPLAYER_TARGET_ARCHITECTURE.md.
enum class MatchPhase
{
    Lobby,
    Playing,
    Finished
};

inline const char* ToString(MatchPhase phase)
{
    switch (phase)
    {
    case MatchPhase::Lobby:
        return "Lobby";
    case MatchPhase::Playing:
        return "Playing";
    case MatchPhase::Finished:
        return "Finished";
    }
    return "Unknown";
}
