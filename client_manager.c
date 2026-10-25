#include "client_manager.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

// Tabela de clientes e mutex de sincronização encapsulados dentro do módulo
static client_slot clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

int register_client(const client_info *info)
{
    int slot = -1;
    pthread_mutex_lock(&clients_mutex);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].active = 1;
            clients[i].busy = 0;
            strcpy(clients[i].ip, info->ip);
            clients[i].port = info->port;
            clients[i].socket = info->socket;
            memset(&clients[i].metrics, 0, sizeof(clients[i].metrics));
            clock_gettime(CLOCK_MONOTONIC, &clients[i].last_activity);
            slot = i;
            break;
        }
    }
    
    pthread_mutex_unlock(&clients_mutex);
    return slot;
}

void set_busy(int slot, int busy)
{
    if (slot < 0 || slot >= MAX_CLIENTS) return;
    
    pthread_mutex_lock(&clients_mutex);
    clients[slot].busy = busy;
    clock_gettime(CLOCK_MONOTONIC, &clients[slot].last_activity);
    pthread_mutex_unlock(&clients_mutex);
}

void client_publish_metrics(int slot, const client_metrics *metrics)
{
    if (slot < 0 || slot >= MAX_CLIENTS || metrics == NULL) return;

    pthread_mutex_lock(&clients_mutex);
    clients[slot].metrics = *metrics;
    pthread_mutex_unlock(&clients_mutex);
}

void unregister_client(int slot)
{
    if (slot < 0 || slot >= MAX_CLIENTS) return;
    
    pthread_mutex_lock(&clients_mutex);
    clients[slot].active = 0;
    pthread_mutex_unlock(&clients_mutex);
}

size_t client_snapshot(client_view *out, size_t max, size_t *total_active)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    size_t copied = 0;
    size_t total = 0;

    pthread_mutex_lock(&clients_mutex);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) continue;
        total++;

        if (out == NULL || copied >= max) continue;

        client_view *view = &out[copied++];
        snprintf(view->ip, sizeof(view->ip), "%s", clients[i].ip);
        view->port = clients[i].port;
        view->busy = clients[i].busy;
        view->metrics = clients[i].metrics;

        double idle = (now.tv_sec - clients[i].last_activity.tv_sec)
                    + (now.tv_nsec - clients[i].last_activity.tv_nsec) / 1e9;
        double restante = TIMEOUT_SEC - idle;
        view->idle_remaining_sec = restante < 0 ? 0 : restante;
    }

    pthread_mutex_unlock(&clients_mutex);

    if (total_active != NULL) {
        *total_active = total;
    }

    return copied;
}
