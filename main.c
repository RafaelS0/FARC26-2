#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "server.h"
#include "client_manager.h"

int main(void)
{
    int server_socket = start_server(PORT);
    if (server_socket < 0) {
        fprintf(stderr, "Erro ao iniciar o servidor.\n");
        exit(EXIT_FAILURE);
    }

    printf("Servidor ouvindo na porta %d ...\n", PORT);

    pthread_t input_tid;
    pthread_create(&input_tid, NULL, input_thread, NULL);
    pthread_detach(input_tid);
    printf("Pressione ENTER para listar os clientes conectados.\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("Falha ao aceitar conexão");
            continue;
        }

        client_info *info = malloc(sizeof(client_info));
        if (!info) {
            perror("Erro ao alocar memória");
            close(client_socket);
            continue;
        }

        info->socket = client_socket;
        info->port = ntohs(client_addr.sin_port);
        inet_ntop(AF_INET, &client_addr.sin_addr, info->ip, sizeof(info->ip));

        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, info) != 0) {
            perror("Erro ao criar thread");
            close(client_socket);
            free(info);
            continue;
        }
        pthread_detach(thread);
    }

    close(server_socket);
    return 0;
}