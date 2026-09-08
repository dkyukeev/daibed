#pragma once

#include <string>
#include <vector>

// Raylib-free helper to launch and control a detached child process: the local
// "host" server the GUI Host tab starts in the background. windows.h clashes
// with raylib (Rectangle/DrawText/CloseWindow macros), so every platform header
// lives in ServerProcess.cpp and callers only see this small abstraction.
// See docs/P2P_IMPLEMENTATION.md.

struct ServerProcessHandle
{
    // Opaque native process handle stored as an integer (a Win32 HANDLE on
    // Windows; unused on POSIX, where pid is authoritative).
    unsigned long long native = 0;
    unsigned long pid = 0; // 0 == nothing launched/tracked.

    bool Valid() const { return pid != 0; }
};

// Launches exePath with the given flags (args does NOT include the program
// name — the implementation prepends exePath). The child runs detached with no
// console window. On success fills handle and returns true; on failure fills
// error and returns false.
bool LaunchServerProcess(const std::string& exePath,
                         const std::vector<std::string>& args,
                         ServerProcessHandle& handle,
                         std::string& error);

// True while the tracked process is still alive; false once it exits or when
// the handle is empty.
bool IsServerProcessRunning(const ServerProcessHandle& handle);

// Terminates the tracked process (if still running) and releases the handle.
// Safe to call on an empty or already-stopped handle.
void StopServerProcess(ServerProcessHandle& handle);
