#include "server.h"
#include "client_manager.h"
#include "http.h"
#include "admission.h"
#include "metrics.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>

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

#define REFUSE_RETRY_AFTER_SEC 5
#define REFUSE_DRAIN_TIMEOUT_SEC 1

static void refuse_client(const client_info *info, admission_result reason)
{
    struct timeval tv = {REFUSE_DRAIN_TIMEOUT_SEC, 0};
    setsockopt(info->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char discard[BUFFER_SIZE];
    recv(info->socket, discard, sizeof(discard), 0);

    send_http_service_unavailable(info->socket, REFUSE_RETRY_AFTER_SEC);
    shutdown(info->socket, SHUT_WR);
    close(info->socket);

    log_printf("Cliente recusado: %s:%d (%s)", info->ip, info->port, admission_result_text(reason));
}

static void log_transfer(const client_info *info, const char *path, const transfer_record *record,
                         const transfer_metrics *tm, const client_metrics *metrics)
{
    char bandwidth[48], delay[48], rtt[48];

    metrics_format(bandwidth, sizeof(bandwidth), tm->has_bandwidth, tm->bandwidth_kbps, 3, "kbps");
    metrics_format(delay, sizeof(delay), tm->has_delay, tm->delay_ms, 3, "ms");
    metrics_format(rtt, sizeof(rtt), metrics->has_rtt, metrics->rtt_ms, 3, "ms");

    log_printf("[metricas] %s:%d %s %s bytes=%zu/%zu envio=%.3f ms banda=%s rtt=%s atraso=%s",
               info->ip, info->port, path, metrics_status_text(record->status),
               record->bytes_sent, record->object_size, tm->send_ms, bandwidth, rtt, delay);
}

void *handle_client(void *arg)
{
    client_info info = *(client_info *)arg;
    free(arg); // Libera a memória alocada na main

    int client_socket = info.socket;

    unsigned int reserved_kbps = 0;
    admission_result admitted = admission_try_admit(info.ip, &reserved_kbps);

    if (admitted != ADMISSION_OK)
    {
        refuse_client(&info, admitted);
        return NULL;
    }

    // Configura timeout de inatividade para a leitura do socket
    struct timeval tv = {TIMEOUT_SEC, 0};
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int slot = register_client(&info);
    log_printf("Cliente conectado: %s:%d (socket %d, %u kbps reservados para o IP)",
               info.ip, info.port, client_socket, reserved_kbps);

    client_metrics metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics_update_rtt(&metrics, client_socket);
    client_publish_metrics(slot, &metrics);

    const char *motivo = "erro desconhecido";

    while (1)
    {
        char buffer[BUFFER_SIZE];
        ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (bytes_received == 0)
        {
            motivo = "cliente fechou a conexão";
            break;
        }

        if (bytes_received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                motivo = "tempo limite de 15s sem requisição";
            }
            else
            {
                motivo = strerror(errno);
            }
            break;
        }

        transfer_record record;
        memset(&record, 0, sizeof(record));
        clock_gettime(CLOCK_MONOTONIC, &record.request_time);

        buffer[bytes_received] = '\0';
        set_busy(slot, 1);

        log_printf("Requisição recebida de %s:%d:\n%s", info.ip, info.port, buffer);

        http_request req;
        parse_http_request(buffer, &req);

        log_printf("Dados extraídos da requisição de %s:%d:\n"
                   "  Método: %s\n  Caminho: %s\n  Protocolo: %s\n  Connection: %s\n  Accept: %s",
                   info.ip, info.port, req.method, req.path, req.protocol, req.connection, req.accept);

        if (strcmp(req.method, "GET") == 0)
        {
            if (strcmp(req.path, "/") == 0)
            {
                send_file(client_socket, "index.html", info.ip, &record);
            }
            else
            {
                // Remove a barra inicial '/' do caminho recebido
                send_file(client_socket, req.path + 1, info.ip, &record);
            }
        }
        else
        {
            send_http_error(client_socket, 405, "Method Not Allowed");
        }

        if (record.status != TRANSFER_NONE)
        {
            transfer_metrics tm;

            metrics_update_rtt(&metrics, client_socket);
            metrics_apply_transfer(&metrics, &record, &tm);
            client_publish_metrics(slot, &metrics);
            log_transfer(&info, req.path, &record, &tm, &metrics);
        }

        set_busy(slot, 0);
    }

    close(client_socket);
    log_printf("Cliente desconectado: %s:%d (motivo: %s)", info.ip, info.port, motivo);

    admission_release(info.ip);
    unregister_client(slot);
    return NULL;
}
