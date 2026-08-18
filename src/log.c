#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *log_file = NULL;
static LogLevel file_level = LOG_DEBUG;
static LogLevel console_level = LOG_WARN;
static char log_path[256] = "/tmp/echo-protocol.log";
static int append_mode = 0;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

static void write_log(LogLevel level, const char *fmt, va_list args) {
    if (level < file_level && level < console_level) return;

    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm *tm = localtime(&ts.tv_sec);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%03ld",
             tm->tm_hour, tm->tm_min, tm->tm_sec, ts.tv_nsec / 1000000);

    pthread_mutex_lock(&log_mutex);

    if (log_file && level >= file_level) {
        fprintf(log_file, "[%s] [%s] %s\n", timestamp, level_names[level], msg);
        fflush(log_file);
    }

    if (level >= console_level) {
        fprintf(stderr, "[%s] %s\n", level_names[level], msg);
    }

    pthread_mutex_unlock(&log_mutex);
}

void log_init(void) {
    if (log_file) return;
    log_file = fopen(log_path, append_mode ? "a" : "w");
    if (!log_file) {
        log_file = stderr;
    }
}

void log_close(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_file && log_file != stderr) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}

void log_set_level(LogLevel level) {
    file_level = level;
}

void log_set_console_level(LogLevel level) {
    console_level = level;
}

void log_set_file(const char *path) {
    if (!path) return;
    pthread_mutex_lock(&log_mutex);
    strncpy(log_path, path, sizeof(log_path) - 1);
    log_path[sizeof(log_path) - 1] = '\0';
    if (log_file && log_file != stderr) {
        fclose(log_file);
        log_file = fopen(log_path, append_mode ? "a" : "w");
        if (!log_file) log_file = stderr;
    }
    pthread_mutex_unlock(&log_mutex);
}

void log_set_append(int append) {
    append_mode = append;
}

void log_msg(LogLevel level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write_log(level, fmt, args);
    va_end(args);
}

void log_debug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write_log(LOG_DEBUG, fmt, args);
    va_end(args);
}

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write_log(LOG_INFO, fmt, args);
    va_end(args);
}

void log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write_log(LOG_WARN, fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    write_log(LOG_ERROR, fmt, args);
    va_end(args);
}