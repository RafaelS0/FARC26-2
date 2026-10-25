#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#include <arpa/inet.h>
#include <stddef.h>
#include <time.h>
#include "metrics.h"

#define MAX_CLIENTS 30000
#define TIMEOUT_SEC 15

typedef struct {
    int socket;
    char ip[INET_ADDRSTRLEN];
    int port;
} client_info;

typedef struct {
    int active;
    int busy;
    char ip[INET_ADDRSTRLEN];
    int port;
    int socket;
    struct timespec last_activity;
    client_metrics metrics;
} client_slot;

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int port;
    int busy;
    double idle_remaining_sec;
    client_metrics metrics;
} client_view;

int register_client(const client_info *info);
void set_busy(int slot, int busy);
void client_publish_metrics(int slot, const client_metrics *metrics);
void unregister_client(int slot);
size_t client_snapshot(client_view *out, size_t max, size_t *total_active);

#endif
