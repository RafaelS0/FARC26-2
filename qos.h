#ifndef QOS_H
#define QOS_H

#include <stddef.h>

#define QOS_DEFAULT_RATE_KBPS 1000U

int qos_init(const char *config_path);

int qos_get_rate_kbps(
    const char *client_ip,
    unsigned int *rate_kbps
);

int send_all_count(int socket, const void *data, size_t length, size_t *sent_total);

int qos_send_limited(
    int client_socket,
    const void *data,
    size_t length,
    const char *client_ip
);

#endif