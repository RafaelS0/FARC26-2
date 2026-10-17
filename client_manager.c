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

void unregister_client(int slot)
{
    if (slot < 0 || slot >= MAX_CLIENTS) return;
    
    pthread_mutex_lock(&clients_mutex);
    clients[slot].active = 0;
    pthread_mutex_unlock(&clients_mutex);
}

void list_clients(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int total = 0;

    pthread_mutex_lock(&clients_mutex);
    printf("\n===== Clientes conectados =====\n");
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) continue;
        total++;

        if (clients[i].busy) {
            printf("%s:%d (socket %d) - atendendo requisição\n",
                   clients[i].ip, clients[i].port, clients[i].socket);
        } else {
            double idle = (now.tv_sec - clients[i].last_activity.tv_sec)
                        + (now.tv_nsec - clients[i].last_activity.tv_nsec) / 1e9;
            double restante = TIMEOUT_SEC - idle;
            if (restante < 0) restante = 0;
            
            printf("%s:%d (socket %d) - ocioso, encerra em %.0fs\n",
                   clients[i].ip, clients[i].port, clients[i].socket, restante);
        }
    }
    
    pthread_mutex_unlock(&clients_mutex);
    printf("Total: %d\n===============================\n\n", total);
}