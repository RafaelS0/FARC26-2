#include "server.h"
#include "client_manager.h"
#include "http.h"
#include "qos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

#define REQUEST_BUFFER_SIZE 8192

int start_server(int port)
{
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1)
    {
        perror("Erro ao criar socket");
        return -1;
    }

    // Permite reutilizar a porta imediatamente após reiniciar o servidor
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Erro ao vincular o socket (bind)");
        close(server_socket);
        return -1;
    }

    if (listen(server_socket, 128) < 0)
    {
        perror("Falha ao colocar o socket em escuta (listen)");
        close(server_socket);
        return -1;
    }

    return server_socket;
}

void *input_thread(void *arg)
{
    (void)arg;
    char line[64];
    while (fgets(line, sizeof(line), stdin))
    {
        list_clients();
    }
    return NULL;
}

void *handle_client(void *arg)
{
    client_info info = *(client_info *)arg;
    free(arg);

    int client_socket = info.socket;
    int admitted = 0;
    int slot = -1;
    const char *motivo = "erro desconhecido";

    if (!qos_admit(info.ip))
    {
        send_http_error(client_socket, 429, "Too Many Requests");
        motivo = "qos negou a conexão";
        goto fim;
    }
    admitted = 1;

    // Configura timeout de inatividade para a leitura do socket
    struct timeval tv = {TIMEOUT_SEC, 0};
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    slot = register_client(&info);

    printf("[+] Cliente conectado: %s:%d (socket %d)\n", info.ip, info.port, client_socket);

    char buffer[REQUEST_BUFFER_SIZE]; // bytes recebidos ainda não tratados
    size_t buffered = 0;              // quantos bytes há no buffer
    buffer[0] = '\0';

    while (1)
    {
        // 1. acumula bytes até o cabeçalho estar completo (termina em \r\n\r\n)
        char *end;
        while ((end = strstr(buffer, "\r\n\r\n")) == NULL)
        {
            if (buffered >= sizeof(buffer) - 1)
            {
                const char *too_large =
                    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                send(client_socket, too_large, strlen(too_large), MSG_NOSIGNAL);
                motivo = "requisição maior que o buffer";
                goto fim;
            }

            ssize_t bytes_received = recv(client_socket, buffer + buffered,
                                          sizeof(buffer) - 1 - buffered, 0);

            if (bytes_received == 0)
            {
                motivo = "cliente fechou a conexão";
                goto fim;
            }

            if (bytes_received < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    motivo = "tempo limite de 15s sem requisição";
                else
                    motivo = strerror(errno);
                goto fim;
            }

            buffered += bytes_received;
            buffer[buffered] = '\0';
        }

        // 2. isola a primeira requisição (as seguintes ficam no buffer)
        size_t req_len = (end - buffer) + 4; // inclui o \r\n\r\n
        char saved = buffer[req_len];
        buffer[req_len] = '\0';

        set_busy(slot, 1);

        printf("Requisição recebida de %s:%d:\n%s\n", info.ip, info.port, buffer);

        http_request req;
        parse_http_request(buffer, &req);

        printf("Dados extraídos da requisição:\n");
        printf("Método: %s\n", req.method);
        printf("Caminho: %s\n", req.path);
        printf("Protocolo: %s\n", req.protocol);
        printf("Connection: %s\n", req.connection);
        printf("Accept: %s\n\n", req.accept);

        if (strcmp(req.method, "GET") == 0)
        {
            if (strcmp(req.path, "/") == 0)
                send_file(client_socket, "index.html", info.ip);
            else
                send_file(client_socket, req.path + 1, info.ip);
        }
        else
        {
            send_http_error(client_socket, 405, "Method Not Allowed");
        }

        // 3. descarta só a requisição tratada e mantém o que sobrou
        buffer[req_len] = saved;
        memmove(buffer, buffer + req_len, buffered - req_len + 1); // +1 leva o '\0'
        buffered -= req_len;

        set_busy(slot, 0);

        // o cliente pediu para encerrar depois da resposta
        if (strcasecmp(req.connection, "close") == 0)
        {
            motivo = "cliente pediu Connection: close";
            goto fim;
        }
    }

fim:
    if (admitted)
    {
        qos_release(info.ip);
        unregister_client(slot);
    }
    close(client_socket);
    printf("[-] Cliente desconectado: %s:%d (motivo: %s)\n", info.ip, info.port, motivo);
    return NULL;
}