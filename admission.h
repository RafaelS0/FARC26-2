#ifndef ADMISSION_H
#define ADMISSION_H

#include <stddef.h>

typedef enum {
    ADMISSION_OK = 0,
    ADMISSION_NO_CAPACITY,
    ADMISSION_RATE_EXCEEDS_CAPACITY,
    ADMISSION_ERROR
} admission_result;

typedef struct {
    unsigned int capacity_kbps;
    unsigned int reserved_kbps;
    unsigned int available_kbps;
    unsigned int peak_reserved_kbps;
    size_t active_ips;
    unsigned long active_connections;
} admission_stats;

int admission_init(unsigned int capacity_kbps);

admission_result admission_try_admit(const char *ip, unsigned int *ip_rate_kbps);

void admission_release(const char *ip);

void admission_get_stats(admission_stats *stats);

const char *admission_result_text(admission_result result);

#endif
