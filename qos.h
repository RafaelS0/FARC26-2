#ifndef QOS_H
#define QOS_H

#include <stddef.h>

#define QOS_DEFAULT_RATE_KBPS 1000U

int qos_init(const char *config_path);

int qos_get_rate_kbps(
    const char *client_ip,
    unsigned int *rate_kbps
);

int qos_send_limited(
    int client_socket,
    const void *data,
    size_t length,
    const char *client_ip
);

int send_all_count(
    int socket,
    const void *data,
    size_t length,
    size_t *sent_total
);

// define a vazão máxima do servidor (kbps); deve ser chamada antes de aceitar clientes
void qos_set_capacity(unsigned int max_kbps);

// 1 = cliente admitido, 0 = recusado (vazão máxima excedida)
int qos_admit(const char *client_ip);

// libera a conexão; chamar só se qos_admit retornou 1
void qos_release(const char *client_ip);

#endif