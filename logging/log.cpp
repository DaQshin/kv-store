#include "../../include/log.h"

static const char* level_str[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

void log_msg(LogLevel level, const char* file, int line, const char* fmt, ...){
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_r(&t, &tm);

    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);

    std::fprintf(stderr, "%s [%s] %s:%d ", timebuf, level_str[level], file, line);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);

    std::fprintf(stderr, "\n");
}