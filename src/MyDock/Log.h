#pragma once

#include <string>

namespace MyDock::Log {

// Appends a timestamped line to %TEMP%\MyDock.log. For rare events (startup, failures, one-off
// diagnostics), not for per-frame or per-refresh logging. The file is reset at the first write of a
// run once it exceeds a small size cap, so it never grows without bound.
void Write(const std::wstring& message);

// Verbose diagnostics (e.g. icon resolution) to %TEMP%\MyDock-debug.log. Compiled out of Release
// builds: the call and its argument construction disappear entirely.
#ifdef MYDOCK_DEBUG
void Debug(const std::wstring& message);
#define MYDOCK_LOG_DEBUG(message) ::MyDock::Log::Debug(message)
#else
#define MYDOCK_LOG_DEBUG(message) ((void)0)
#endif

} // namespace MyDock::Log
