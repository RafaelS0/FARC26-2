#include "admission.h"
#include "qos.h"

#include <arpa/inet.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define ADMISSION_MAX_IPS 1024

typedef struct {
    char ip[INET_ADDRSTRLEN];
    unsigned int rate_kbps;
    unsigned long connections;
} ip_reservation;

static pthread_mutex_t admission_mutex = PTHREAD_MUTEX_INITIALIZER;
static ip_reservation reservations[ADMISSION_MAX_IPS];
static size_t reservation_count = 0;
static unsigned int capacity_kbps = 0;
static unsigned long long reserved_kbps = 0;
static unsigned long long peak_reserved_kbps = 0;
static unsigned long active_connections = 0;

static ip_reservation *find_locked(const char *ip)
{
    for (size_t i = 0; i < reservation_count; i++) {
        if (strcmp(reservations[i].ip, ip) == 0) {
            return &reservations[i];
        }
    }

    return NULL;
}

int admission_init(unsigned int capacity)
{
    if (capacity == 0) {
        return -1;
    }

    pthread_mutex_lock(&admission_mutex);

    memset(reservations, 0, sizeof(reservations));
    reservation_count = 0;
    capacity_kbps = capacity;
    reserved_kbps = 0;
    peak_reserved_kbps = 0;
    active_connections = 0;

    pthread_mutex_unlock(&admission_mutex);
    return 0;
}

admission_result admission_try_admit(const char *ip, unsigned int *ip_rate_kbps)
{
    if (ip == NULL || strlen(ip) >= INET_ADDRSTRLEN) {
        return ADMISSION_ERROR;
    }

    unsigned int rate = 0;

    if (qos_get_rate_kbps(ip, &rate) != 0 || rate == 0) {
        return ADMISSION_ERROR;
    }

    admission_result result = ADMISSION_OK;

    pthread_mutex_lock(&admission_mutex);

    ip_reservation *entry = find_locked(ip);

    if (capacity_kbps == 0) {
        result = ADMISSION_ERROR;
    } else if (entry != NULL) {

        if (entry->connections == ULONG_MAX) {
            result = ADMISSION_ERROR;
        } else {
            entry->connections++;
            active_connections++;
            rate = entry->rate_kbps;
        }
    } else if (rate > capacity_kbps) {
        result = ADMISSION_RATE_EXCEEDS_CAPACITY;
    } else if (reserved_kbps + rate > capacity_kbps) {
        result = ADMISSION_NO_CAPACITY;
    } else if (reservation_count >= ADMISSION_MAX_IPS) {
        result = ADMISSION_ERROR;
    } else {
        entry = &reservations[reservation_count++];
        snprintf(entry->ip, sizeof(entry->ip), "%s", ip);
        entry->rate_kbps = rate;
        entry->connections = 1;

        reserved_kbps += rate;
        active_connections++;

        if (reserved_kbps > peak_reserved_kbps) {
            peak_reserved_kbps = reserved_kbps;
        }
    }

    pthread_mutex_unlock(&admission_mutex);

    if (result == ADMISSION_OK && ip_rate_kbps != NULL) {
        *ip_rate_kbps = rate;
    }

    return result;
}

void admission_release(const char *ip)
{
    if (ip == NULL) {
        return;
    }

    pthread_mutex_lock(&admission_mutex);

    ip_reservation *entry = find_locked(ip);

    if (entry != NULL) {
        active_connections--;
        entry->connections--;

        if (entry->connections == 0) {

            reserved_kbps -= entry->rate_kbps;
            *entry = reservations[--reservation_count];
        }
    }

    pthread_mutex_unlock(&admission_mutex);
}

void admission_get_stats(admission_stats *stats)
{
    if (stats == NULL) {
        return;
    }

    pthread_mutex_lock(&admission_mutex);

    stats->capacity_kbps = capacity_kbps;
    stats->reserved_kbps = (unsigned int)reserved_kbps;
    stats->available_kbps = capacity_kbps - (unsigned int)reserved_kbps;
    stats->peak_reserved_kbps = (unsigned int)peak_reserved_kbps;
    stats->active_ips = reservation_count;
    stats->active_connections = active_connections;

    pthread_mutex_unlock(&admission_mutex);
}

const char *admission_result_text(admission_result result)
{
    switch (result) {
    case ADMISSION_OK:
        return "admitido";
    case ADMISSION_NO_CAPACITY:
        return "capacidade de vazão insuficiente";
    case ADMISSION_RATE_EXCEEDS_CAPACITY:
        return "taxa do IP maior que a capacidade total do servidor";
    default:
        return "erro interno no controle de admissão";
    }
}
