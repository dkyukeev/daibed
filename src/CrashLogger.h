#pragma once

#include <string>

namespace CrashLogger
{
void Initialize(int argc, char** argv);
void Shutdown();
void Heartbeat(const char* phase);
void LogEvent(const std::string& message);
void WriteManualLog(const std::string& reason);
}
