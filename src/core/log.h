#pragma once
#include <string>

namespace ggdpvr::log {
// Opens the log. Path: env GGDPVR_LOG if set, else %LOCALAPPDATA%\GGDPVR\logs\ggdpvr.log
// (the previous run is kept as ggdpvr.prev.log). Every line is flushed immediately so the
// log survives a crash.
void Init();
void Write(const char* fmt, ...);
// log=1 (the default since 1.0.1) writes the one, full log; log=0 writes nothing. Lines before Configure() are held
// in memory. GGDPVR_LOG (testing) always logs.
void Configure(bool enabled);
bool Enabled();
std::wstring Directory();  // %LOCALAPPDATA%\GGDPVR
}  // namespace ggdpvr::log

#define LOG(...) ::ggdpvr::log::Write(__VA_ARGS__)
#define LOGP(...) ::ggdpvr::log::Write(__VA_ARGS__)  // (0.9.102: one log level - same as LOG)
