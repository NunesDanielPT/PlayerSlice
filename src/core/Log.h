#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdarg>

namespace ps {

enum class LogLevel { Info, Warn, Error };

struct LogEntry {
    LogLevel level;
    double time;
    std::string text;
};

// Log thread-safe: escreve em ficheiro (%LOCALAPPDATA%\PLAYERSLICE\playerslice.log)
// e guarda as últimas entradas para mostrar na consola da interface.
class Log {
public:
    static void init(const std::wstring& dir);
    static void write(LogLevel lvl, const char* fmt, ...);
    static std::vector<LogEntry> recent(size_t max = 300);
    static void shutdown();
};

std::wstring appDataDir();               // %LOCALAPPDATA%\PLAYERSLICE
std::string  toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);
double       nowSeconds();               // relógio monotónico de alta resolução

} // namespace ps

#define PS_INFO(...)  ::ps::Log::write(::ps::LogLevel::Info,  __VA_ARGS__)
#define PS_WARN(...)  ::ps::Log::write(::ps::LogLevel::Warn,  __VA_ARGS__)
#define PS_ERROR(...) ::ps::Log::write(::ps::LogLevel::Error, __VA_ARGS__)
