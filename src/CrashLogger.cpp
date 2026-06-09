#include "CrashLogger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_running{false};
std::atomic<bool> g_crashLogWritten{false};
std::atomic<bool> g_hangLogWritten{false};
std::atomic<std::int64_t> g_lastHeartbeatMs{0};
std::atomic<std::uint64_t> g_frame{0};
std::thread g_watchdog;
std::mutex g_mutex;
std::string g_phase = "startup";
std::string g_commandLine;
std::vector<std::string> g_events;

constexpr std::size_t kMaxEvents = 96;
constexpr std::int64_t kHangTimeoutMs = 10000;

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string WallTimeStamp()
{
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif

    char buffer[64]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
    return buffer;
}

std::string CrashFileName(const char* kind)
{
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif

    char buffer[128]{};
    std::strftime(buffer, sizeof(buffer), "crash_%Y%m%d_%H%M%S", &tm);
    return std::string(buffer) + "_" + kind + ".log";
}

std::string CurrentPhase()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_phase;
}

std::vector<std::string> RecentEvents()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_events;
}

void WriteCrashLog(const std::string& kind, const std::string& reason, bool force = false)
{
    if (!force)
    {
        if (kind == "hang")
        {
            if (g_hangLogWritten.exchange(true))
            {
                return;
            }
        }
        else if (g_crashLogWritten.exchange(true))
        {
            return;
        }
    }

    const std::string phase = CurrentPhase();
    const std::vector<std::string> events = RecentEvents();
    const std::uint64_t frame = g_frame.load();
    const std::int64_t now = NowMs();
    const std::int64_t lastHeartbeat = g_lastHeartbeatMs.load();

    std::ofstream out(CrashFileName(kind.c_str()), std::ios::out | std::ios::trunc);
    if (!out)
    {
        return;
    }

    out << "DaiBed crash log\n";
    out << "time: " << WallTimeStamp() << "\n";
    out << "kind: " << kind << "\n";
    out << "reason: " << reason << "\n";
    out << "commandLine: " << g_commandLine << "\n";
    out << "frame: " << frame << "\n";
    out << "phase: " << phase << "\n";
    out << "millisecondsSinceHeartbeat: " << (now - lastHeartbeat) << "\n";
    out << "\nrecentEvents:\n";
    for (const std::string& event : events)
    {
        out << "- " << event << "\n";
    }
}

void WatchdogLoop()
{
    while (g_running.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!g_running.load())
        {
            break;
        }

        const std::int64_t elapsed = NowMs() - g_lastHeartbeatMs.load();
        if (elapsed > kHangTimeoutMs)
        {
            std::ostringstream reason;
            reason << "main loop did not report heartbeat for " << elapsed << " ms";
            WriteCrashLog("hang", reason.str());
        }
    }
}

void SignalHandler(int signal)
{
    std::ostringstream reason;
    reason << "signal " << signal;
    WriteCrashLog("signal", reason.str());
    std::_Exit(128 + signal);
}

void TerminateHandler()
{
    WriteCrashLog("terminate", "std::terminate was called");
    std::_Exit(1);
}

#ifdef _WIN32
std::string ExceptionCodeName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    default:
        break;
    }

    std::ostringstream out;
    out << "0x" << std::hex << code;
    return out.str();
}

LONG WINAPI DaiBedUnhandledExceptionFilter(EXCEPTION_POINTERS* info)
{
    std::ostringstream reason;
    if (info != nullptr && info->ExceptionRecord != nullptr)
    {
        reason << ExceptionCodeName(info->ExceptionRecord->ExceptionCode);
        reason << " at " << info->ExceptionRecord->ExceptionAddress;
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            info->ExceptionRecord->NumberParameters >= 2)
        {
            reason << " access=" << info->ExceptionRecord->ExceptionInformation[0];
            reason << " address=0x" << std::hex << info->ExceptionRecord->ExceptionInformation[1];
        }
    }
    else
    {
        reason << "unknown structured exception";
    }

    WriteCrashLog("exception", reason.str());
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
}

namespace CrashLogger
{
void Initialize(int argc, char** argv)
{
    {
        std::ostringstream commandLine;
        for (int i = 0; i < argc; ++i)
        {
            if (i > 0)
            {
                commandLine << ' ';
            }
            commandLine << (argv[i] != nullptr ? argv[i] : "");
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_commandLine = commandLine.str();
        g_phase = "startup";
        g_events.clear();
    }

    g_crashLogWritten.store(false);
    g_hangLogWritten.store(false);
    g_lastHeartbeatMs.store(NowMs());
    g_frame.store(0);
    std::set_terminate(TerminateHandler);
    std::signal(SIGABRT, SignalHandler);
    std::signal(SIGFPE, SignalHandler);
    std::signal(SIGILL, SignalHandler);
    std::signal(SIGSEGV, SignalHandler);
#ifdef SIGTERM
    std::signal(SIGTERM, SignalHandler);
#endif
#ifdef _WIN32
    SetUnhandledExceptionFilter(DaiBedUnhandledExceptionFilter);
#endif

    g_running.store(true);
    g_watchdog = std::thread(WatchdogLoop);
}

void Shutdown()
{
    g_running.store(false);
    if (g_watchdog.joinable())
    {
        g_watchdog.join();
    }
}

void Heartbeat(const char* phase)
{
    g_lastHeartbeatMs.store(NowMs());
    g_frame.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_phase = phase != nullptr ? phase : "unknown";
}

void LogEvent(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_events.size() >= kMaxEvents)
    {
        g_events.erase(g_events.begin());
    }
    g_events.push_back(WallTimeStamp() + " " + message);
}

void WriteManualLog(const std::string& reason)
{
    WriteCrashLog("manual", reason, true);
}
}
