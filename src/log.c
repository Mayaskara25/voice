#define _POSIX_C_SOURCE 200809L
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static enum log_level g_min_level = LOG_INFO;

static const char *level_name(enum log_level level)
{
    switch (level) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    }
    return "?";
}

void log_set_level(enum log_level level)
{
    g_min_level = level;
}

void log_msg(enum log_level level, const char *fmt, ...)
{
    if (level < g_min_level)
        return;

    char timebuf[16];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_now);

    fprintf(stderr, "[%s] %-5s ", timebuf, level_name(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
}
