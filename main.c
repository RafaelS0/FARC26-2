#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "qos.h"
#include "server.h"
#include "client_manager.h"
#include "config.h"
#include "admission.h"
#include "monitor.h"
#include "log.h"
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[])
{
    int cfg = config_load_args(argc, argv);
    if (cfg != 0) {
        return cfg > 0 ? EXIT_SUCCESS : 2;
    }

    if (qos_init("qos.conf") != 0) {
        fprintf(stderr, "Erro ao carregar configuração de QoS.\n");
        exit(EXIT_FAILURE);
    }

    if (admission_init(config_get_max_capacity_kbps()) != 0) {
        fprintf(stderr, "Erro ao iniciar o controle de admissão.\n");
        exit(EXIT_FAILURE);
    }
    if (log_open(LOG_FILE_PATH) != 0) {
        perror("Erro ao abrir arquivo de log");
        exit(EXIT_FAILURE);
    }
    printf("Vazão máxima do servidor: %u kbps\n", config_get_max_capacity_kbps());

    int server_socket = start_server(PORT);
    if (server_socket < 0) {
        fprintf(stderr, "Erro ao iniciar o servidor.\n");
        exit(EXIT_FAILURE);
    }

    printf("Servidor ouvindo na porta %d ...\n", PORT);

    if (monitor_start() != 0) {
        fprintf(stderr, "Erro ao iniciar o monitoramento.\n");
        exit(EXIT_FAILURE);
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) {
            log_printf("Falha ao aceitar conexão: %s", strerror(errno));
            continue;
        }

        client_info *info = malloc(sizeof(client_info));
        if (!info) {
            log_printf("Erro ao alocar memória: %s", strerror(errno));
            close(client_socket);
            continue;
        }

        info->socket = client_socket;
        info->port = ntohs(client_addr.sin_port);
        inet_ntop(AF_INET, &client_addr.sin_addr, info->ip, sizeof(info->ip));

        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, info) != 0) {
            log_printf("Erro ao criar thread: %s", strerror(errno));
            close(client_socket);
            free(info);
            continue;
        }
        pthread_detach(thread);
    }

    close(server_socket);
    return 0;
}