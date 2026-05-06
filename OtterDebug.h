#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

#ifndef OTTER_USE_RAYLIB
#include "OtterPlatform.h"
#endif

namespace Otter
{
    enum class LogLevel
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Fatal
    };

    class Logger
    {
    public:
        static Logger& instance()
        {
            static Logger logger;
            return logger;
        }

        void set_min_level(LogLevel level)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            min_level_ = level;
        }

        void set_file(const std::filesystem::path& path)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            file_.close();
            file_.open(path, std::ios::out | std::ios::app);
        }

        void write(LogLevel level, std::string_view subsystem, std::string_view message,
                   const char* file = nullptr, int line = 0)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (level < min_level_)
                return;

            std::ostringstream out;
            out << "[" << timestamp() << "]"
                << "[" << level_name(level) << "]"
                << "[" << subsystem << "] " << message;
            if (file && *file)
                out << " (" << file << ":" << line << ")";
            out << "\n";

            const std::string text = out.str();
            std::fwrite(text.data(), 1, text.size(), stderr);
            if (file_.is_open())
                file_ << text << std::flush;
#ifndef OTTER_USE_RAYLIB
            Platform::debug_output(text);
#endif
        }

    private:
        static const char* level_name(LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Trace: return "trace";
            case LogLevel::Debug: return "debug";
            case LogLevel::Info: return "info";
            case LogLevel::Warning: return "warn";
            case LogLevel::Error: return "error";
            case LogLevel::Fatal: return "fatal";
            default: return "unknown";
            }
        }

        static std::string timestamp()
        {
            using namespace std::chrono;
            const auto now = system_clock::now();
            const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
            std::time_t t = system_clock::to_time_t(now);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                          tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<int>(ms.count()));
            return buffer;
        }

        std::mutex mutex_;
        LogLevel min_level_ = LogLevel::Info;
        std::ofstream file_;
    };

    inline void log(LogLevel level, std::string_view subsystem, std::string_view message,
                    const char* file = nullptr, int line = 0)
    {
        Logger::instance().write(level, subsystem, message, file, line);
    }

    inline std::string format_log(const char* format, ...)
    {
        char stack_buffer[1024]{};
        va_list args;
        va_start(args, format);
        int needed = std::vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
        va_end(args);
        if (needed < 0)
            return {};
        if (needed < static_cast<int>(sizeof(stack_buffer)))
            return stack_buffer;

        std::string result(static_cast<size_t>(needed) + 1u, '\0');
        va_start(args, format);
        std::vsnprintf(result.data(), result.size(), format, args);
        va_end(args);
        result.resize(static_cast<size_t>(needed));
        return result;
    }

    class ScopeTimer
    {
    public:
        ScopeTimer(std::string subsystem, std::string label, LogLevel level = LogLevel::Debug)
            : subsystem_(std::move(subsystem))
            , label_(std::move(label))
            , level_(level)
            , start_(std::chrono::steady_clock::now())
        {
        }

        ~ScopeTimer()
        {
            using namespace std::chrono;
            const auto elapsed = duration_cast<microseconds>(steady_clock::now() - start_).count();
            log(level_, subsystem_, format_log("%s took %.3f ms", label_.c_str(), elapsed / 1000.0));
        }

    private:
        std::string subsystem_;
        std::string label_;
        LogLevel level_;
        std::chrono::steady_clock::time_point start_;
    };
}

#define OTTER_LOG_TRACE(subsystem, message) ::Otter::log(::Otter::LogLevel::Trace, subsystem, message, __FILE__, __LINE__)
#define OTTER_LOG_DEBUG(subsystem, message) ::Otter::log(::Otter::LogLevel::Debug, subsystem, message, __FILE__, __LINE__)
#define OTTER_LOG_INFO(subsystem, message) ::Otter::log(::Otter::LogLevel::Info, subsystem, message, __FILE__, __LINE__)
#define OTTER_LOG_WARN(subsystem, message) ::Otter::log(::Otter::LogLevel::Warning, subsystem, message, __FILE__, __LINE__)
#define OTTER_LOG_ERROR(subsystem, message) ::Otter::log(::Otter::LogLevel::Error, subsystem, message, __FILE__, __LINE__)
