#ifndef MONITOR_H
#define MONITOR_H

#include <stddef.h>

#define MONITOR_INTERVAL_MS 1000
#define MONITOR_MAX_ROWS 40
#define MONITOR_FRAME_SIZE 16384

size_t monitor_render(char *buf, size_t size, int use_ansi);
int monitor_start(void);

#endif
