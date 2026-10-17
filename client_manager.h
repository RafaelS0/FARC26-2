#ifndef CLIENT_MANAGER_H
#define CLIENT_MANAGER_H

#include <arpa/inet.h>
#include <time.h>

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
} client_slot;

int register_client(const client_info *info);
void set_busy(int slot, int busy);
void unregister_client(int slot);
void list_clients(void);

#endif