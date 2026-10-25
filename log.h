#ifndef LOG_H
#define LOG_H

#define LOG_FILE_PATH "server.log"

int log_open(const char *path);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
