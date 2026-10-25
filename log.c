#include "log.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static FILE *log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

int log_open(const char *path)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        return -1;
    }

    pthread_mutex_lock(&log_mutex);

    if (log_file != NULL) {
        fclose(log_file);
    }

    log_file = file;

    pthread_mutex_unlock(&log_mutex);
    return 0;
}

void log_printf(const char *fmt, ...)
{
    struct timespec now;
    struct tm local;

    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &local);

    pthread_mutex_lock(&log_mutex);

    FILE *out = log_file != NULL ? log_file : stderr;

    fprintf(out, "[%02d:%02d:%02d.%03ld] ",
            local.tm_hour, local.tm_min, local.tm_sec, now.tv_nsec / 1000000L);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fputc('\n', out);
    fflush(out);

    pthread_mutex_unlock(&log_mutex);
}
