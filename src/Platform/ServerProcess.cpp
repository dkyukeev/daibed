#include "ServerProcess.h"

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
// Quote a single argument following the Windows CommandLineToArgvW rules so a
// path or value containing spaces/quotes survives the round trip. Mirrors the
// algorithm previously used by the GUI launcher.
std::string QuoteArg(const std::string& arg)
{
    std::string out = "\"";
    int backslashes = 0;
    for (char c : arg)
    {
        if (c == '\\')
        {
            ++backslashes;
        }
        else if (c == '"')
        {
            out.append(static_cast<std::size_t>(backslashes * 2 + 1), '\\');
            out.push_back('"');
            backslashes = 0;
        }
        else
        {
            out.append(static_cast<std::size_t>(backslashes), '\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(static_cast<std::size_t>(backslashes * 2), '\\');
    out.push_back('"');
    return out;
}

#ifdef _WIN32
std::wstring Widen(const std::string& utf8)
{
    if (utf8.empty())
    {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
    {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0], needed);
    return wide;
}
#endif
}

bool LaunchServerProcess(const std::string& exePath,
                         const std::vector<std::string>& args,
                         ServerProcessHandle& handle,
                         std::string& error)
{
    handle = ServerProcessHandle {};

#ifdef _WIN32
    std::string commandLine = QuoteArg(exePath);
    for (const std::string& arg : args)
    {
        commandLine.push_back(' ');
        commandLine += QuoteArg(arg);
    }

    std::wstring application = Widen(exePath);
    std::wstring command = Widen(commandLine);
    command.push_back(L'\0'); // CreateProcessW may write to the buffer.

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info {};

    const BOOL ok = CreateProcessW(
        application.empty() ? nullptr : application.c_str(),
        command.empty() ? nullptr : &command[0],
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &startup,
        &info);
    if (!ok)
    {
        error = "Не удалось запустить серверный процесс (ошибка CreateProcess, код "
            + std::to_string(static_cast<unsigned long>(GetLastError())) + ").";
        return false;
    }

    CloseHandle(info.hThread); // We only need the process handle for status/stop.
    handle.native = reinterpret_cast<unsigned long long>(info.hProcess);
    handle.pid = static_cast<unsigned long>(info.dwProcessId);
    return true;
#else
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(exePath);
    for (const std::string& arg : args)
    {
        storage.push_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& s : storage)
    {
        argv.push_back(&s[0]);
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0)
    {
        error = "Не удалось запустить серверный процесс (ошибка fork).";
        return false;
    }
    if (pid == 0)
    {
        setsid(); // Detach from the controlling terminal.
        execv(exePath.c_str(), argv.data());
        _exit(127); // execv only returns on failure.
    }

    handle.native = static_cast<unsigned long long>(pid);
    handle.pid = static_cast<unsigned long>(pid);
    return true;
#endif
}

bool IsServerProcessRunning(const ServerProcessHandle& handle)
{
    if (!handle.Valid())
    {
        return false;
    }

#ifdef _WIN32
    HANDLE process = reinterpret_cast<HANDLE>(handle.native);
    if (process == nullptr)
    {
        return false;
    }
    return WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
#else
    const pid_t pid = static_cast<pid_t>(handle.pid);
    int status = 0;
    const pid_t reaped = waitpid(pid, &status, WNOHANG); // Reap if it has exited.
    if (reaped == pid)
    {
        return false;
    }
    return kill(pid, 0) == 0;
#endif
}

void StopServerProcess(ServerProcessHandle& handle)
{
    if (!handle.Valid())
    {
        handle = ServerProcessHandle {};
        return;
    }

#ifdef _WIN32
    HANDLE process = reinterpret_cast<HANDLE>(handle.native);
    if (process != nullptr)
    {
        TerminateProcess(process, 0);
        CloseHandle(process);
    }
#else
    const pid_t pid = static_cast<pid_t>(handle.pid);
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, WNOHANG);
#endif

    handle = ServerProcessHandle {};
}
