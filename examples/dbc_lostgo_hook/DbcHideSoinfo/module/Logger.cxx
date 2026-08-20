module;

#include <iostream>
#include <print>
#include <string_view>

#ifdef __ANDROID__

#include <android/log.h>
#include <sys/syslog.h>

#endif

export module Logger;

static constexpr auto TAG = "DbcHK";


export enum class LogType {
    Android,
    Print,
    System
};

export enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warning,
    Error
};

export class Logger {
public:
#if !LOG_DEBUG_MODE

    template<LogType Type = LogType::Android, typename... Args>
    inline static void Debug(std::format_string<Args...> fmt, Args &&... args) {
        ShowImpl<Type, LogLevel::Debug, Args...>(fmt, std::forward<Args>(args)...);
    }

    template<LogType Type = LogType::Android, typename... Args>
    inline static void Info(std::format_string<Args...> fmt, Args &&... args) {
        ShowImpl<Type, LogLevel::Info, Args...>(fmt, std::forward<Args>(args)...);
    }

    template<LogType Type = LogType::Android, typename... Args>
    inline static void Warning(std::format_string<Args...> fmt, Args &&... args) {
        ShowImpl<Type, LogLevel::Warning, Args...>(fmt, std::forward<Args>(args)...);
    }

    template<LogType Type = LogType::Android, typename... Args>
    inline static void Error(std::format_string<Args...> fmt, Args &&... args) {
        ShowImpl<Type, LogLevel::Error, Args...>(fmt, std::forward<Args>(args)...);
    }

#else

    template<typename... Args>
    inline static void Debug(Args &&...) {
    }

    template<typename... Args>
    inline static void Info(Args &&...) {
    }

    template<typename... Args>
    inline static void Warning(Args &&...) {
    }

    template<typename... Args>
    inline static void Error(Args &&...) {
    }

#endif


private:
    template<LogType Type, LogLevel Level, typename ...Args>
    static void ShowImpl(std::format_string<Args...> fmt, Args &&... args);

    template<LogLevel Level, typename ...Args>
    static void ShowAndroidLog(std::format_string<Args...> fmt, Args &&... args);

    template<LogLevel Level, typename ...Args>
    static void ShowPrintLog(std::format_string<Args...> fmt, Args &&... args);

    template<LogLevel Level, typename ...Args>
    static void ShowSystemLog(std::format_string<Args...> fmt, Args &&... args);

};


template<LogType Type, LogLevel Level, typename... Args>
void Logger::ShowImpl(std::format_string<Args...> fmt, Args &&... args) {
    if constexpr (Type == LogType::Android) {
        ShowAndroidLog<Level, Args...>(fmt, std::forward<Args>(args)...);
        return;
    }
    if constexpr (Type == LogType::Print) {
        ShowPrintLog<Level, Args...>(fmt, std::forward<Args>(args)...);
        return;
    }
    if constexpr (Type == LogType::System) {
        ShowSystemLog<Level, Args...>(fmt, std::forward<Args>(args)...);
        return;
    }
}


template<LogLevel Level, typename ...Args>
void Logger::ShowAndroidLog(std::format_string<Args...> fmt,
                            Args &&... args) {

#ifdef __ANDROID__
    constexpr auto priority = [&] {
        if constexpr (Level == LogLevel::Debug) return ANDROID_LOG_DEBUG;
        if constexpr (Level == LogLevel::Info) return ANDROID_LOG_INFO;
        if constexpr (Level == LogLevel::Warning) return ANDROID_LOG_WARN;
        if constexpr (Level == LogLevel::Error) return ANDROID_LOG_ERROR;
        return ANDROID_LOG_DEFAULT;
    }();
    __android_log_print(priority, TAG, "%s", std::format(fmt, std::forward<Args>(args)...).data());
#endif
}

template<LogLevel Level, typename... Args>
void Logger::ShowPrintLog(std::format_string<Args...> fmt, Args &&... args) {
    std::cout << std::format(fmt, std::forward<Args>(args)...) << std::endl;
}

template<LogLevel Level, typename... Args>
void Logger::ShowSystemLog(std::format_string<Args...> fmt, Args &&... args) {
#ifdef __ANDROID__
    constexpr auto priority = [&] {
        if constexpr (Level == LogLevel::Debug) return LOG_DEBUG;
        if constexpr (Level == LogLevel::Info) return LOG_INFO;
        if constexpr (Level == LogLevel::Warning) return LOG_WARNING;
        if constexpr (Level == LogLevel::Error) return LOG_ERR;
        return LOG_EMERG;
    }();
    syslog(priority, "%s", std::format(fmt, std::forward<Args>(args)...).data());
#endif
}
