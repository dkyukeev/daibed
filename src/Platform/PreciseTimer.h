#pragma once

// High-resolution blocking wait for the headless server loop.
//
// RunNetworkServer paces its authoritative loop to the fixed tick rate by
// sleeping the leftover of each tick. std::this_thread::sleep_for is bounded by
// the OS timer granularity: on Windows that defaults to ~15.6 ms, so a ~15 ms
// leftover (a 60 Hz / 16.67 ms tick minus a little work) overshoots to ~30 ms
// and the server ends up running at ~33 Hz. A client predicts + sends input at
// a steady 60 Hz, so against a half-speed server it perpetually out-runs the
// authoritative simulation and gets hard-resynced every snapshot — movement
// jerks toward the server-sampled positions and fast moves (Orbita's dash)
// stutter badly. See docs/MULTIPLAYER_QUALITY_TARGET.md.
//
// PreciseSleepSeconds waits the requested duration accurately (~sub-millisecond
// on Windows via a high-resolution waitable timer) with low CPU, so the server
// actually holds its target tick rate. The header stays free of <windows.h> so
// the huge game translation units can call it without the macro clashes that
// header brings (see ServerProcess.h). Non-Windows falls back to a plain sleep.
namespace Platform
{
void PreciseSleepSeconds(double seconds);
}
