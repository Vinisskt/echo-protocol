#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} LogLevel;

void log_init(void);
void log_close(void);
void log_set_level(LogLevel level);
void log_set_console_level(LogLevel level);
void log_set_file(const char *path);
void log_set_append(int append);

void log_msg(LogLevel level, const char *fmt, ...);
void log_debug(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif