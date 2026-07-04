#include "Platform/PreciseTimer.h"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace
{
// One process-wide high-resolution waitable timer, created lazily and reused.
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+) gives ~0.5 ms wait
// accuracy WITHOUT raising the global timer resolution the way timeBeginPeriod
// would (which would need winmm and would slow the whole machine down). Returns
// nullptr on older systems / failure, so callers fall back to a plain sleep.
HANDLE HighResTimerHandle()
{
    static HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return timer;
}
} // namespace
#endif

namespace Platform
{
void PreciseSleepSeconds(double seconds)
{
    if (seconds <= 0.0)
    {
        return;
    }

#if defined(_WIN32)
    if (HANDLE timer = HighResTimerHandle())
    {
        // Relative due time in 100-ns units (negative == relative). Round up so
        // we never wake a hair early and busy the loop; the tiny overshoot is
        // far smaller than the granularity we are escaping.
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(seconds * 1.0e7 + 0.5);
        if (due.QuadPart < 0
            && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif

    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}
} // namespace Platform
