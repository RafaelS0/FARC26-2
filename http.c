#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "qos.h"
#include <errno.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>


#define PROGRESS_INTERVAL_SECONDS 0.5
/// FEITO COM AUXILIO DE INTELIGÊNCIA ARTIFICIAL 
static pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec)
         + (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static void print_transfer_progress(const char *file_path,
                                    const char *client_ip,
                                    size_t bytes_sent,
                                    long file_size,
                                    double elapsed,
                                    int finished)
{
    double percentage = file_size > 0
        ? (double)bytes_sent * 100.0 / (double)file_size
        : 100.0;
    double rate_kbps = elapsed > 0.0
        ? (double)bytes_sent * 8.0 / 1000.0 / elapsed
        : 0.0;

    pthread_mutex_lock(&progress_mutex);
    printf("[progresso] %s -> %s: %zu/%ld bytes (%.1f%%), "
           "%.2f kbps, %.2f s%s\n",
           file_path, client_ip, bytes_sent, file_size, percentage,
           rate_kbps, elapsed,
           finished ? " - concluido" : "");
    fflush(stdout);
    pthread_mutex_unlock(&progress_mutex);
}
/// FEITO COM AUXILIO DE INTELIGÊNCIA ARTIFICIAL


static int is_html_file(const char *path){
    const char *ext = strrchr(path, '.');

    if (ext == NULL) {
        return 0;
    }

    return strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0;
}

// Função privada para mapear extensão para MIME type
static const char *get_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0)
        return "text/html";
    if (strcmp(ext, ".css") == 0)
        return "text/css";
    if (strcmp(ext, ".js") == 0)
        return "text/javascript";
    if (strcmp(ext, ".png") == 0)
        return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(ext, ".ico") == 0)
        return "image/x-icon";
    if (strcmp(ext, ".txt") == 0)
        return "text/plain";

    return "application/octet-stream";
}

void parse_http_request(const char *raw_request, http_request *req)
{
    memset(req, 0, sizeof(http_request));

    // Extrai Método, Caminho e Protocolo da primeira linha
    sscanf(raw_request, "%15s %255s %15s", req->method, req->path, req->protocol);

    // Extrai o cabeçalho Connection
    const char *connection = strstr(raw_request, "\r\nConnection:");
    if (connection)
    {
        connection += strlen("\r\nConnection:");
        while (*connection == ' ' || *connection == '\t')
            connection++;
        sscanf(connection, "%31s", req->connection);
    }

    // Extrai o cabeçalho Accept
    const char *accept = strstr(raw_request, "\r\nAccept:");
    if (accept)
    {
        accept += strlen("\r\nAccept:");
        while (*accept == ' ' || *accept == '\t')
            accept++;
        sscanf(accept, "%255s", req->accept);
    }
}

void send_http_error(int client_socket, int status_code, const char *status_text)
{
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
             "HTTP/1.1 %d %s\r\n"
             "Content-Length: 0\r\n"
             "Connection: keep-alive\r\n"
             "\r\n",
             status_code, status_text);

    send(client_socket, response, strlen(response), 0);
    printf("Resposta de erro enviada (%d %s)\n", status_code, status_text);
}

void send_file(int client_socket,const char *file_path,const char *client_ip)
{
    if (file_path == NULL || client_ip == NULL || strstr(file_path, "..") != NULL) {
        send_http_error(client_socket, 403, "Forbidden");
        return;
    }

    char full_path[512];

    int path_length = snprintf(
        full_path,
        sizeof(full_path),
        "objects/%s",
        file_path
    );

    if (path_length < 0 || (size_t)path_length >= sizeof(full_path)) {
        send_http_error(client_socket, 414, "URI Too Long");
        return;
    }

    FILE *file = fopen(full_path, "rb");

    if (file == NULL) {
        send_http_error(client_socket, 404, "Not Found");
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        send_http_error(client_socket, 500, "Internal Server Error");
        return;
    }

    long file_size = ftell(file);

    if (file_size < 0) {
        fclose(file);
        send_http_error(client_socket, 500, "Internal Server Error");
        return;
    }

    rewind(file);

    const char *content_type = get_content_type(file_path);
    int html = is_html_file(file_path);

    char header[BUFFER_SIZE];

    int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        content_type,
        file_size
    );

    if (header_length < 0 || (size_t) header_length >= sizeof(header)) {
        fclose(file);
        return;
    }

    // cabeçalho HTTP não é limitado pela taxa dos objeto
    size_t header_sent = 0;

    if (send_all_count(client_socket, header, (size_t) header_length, &header_sent) != 0) {
        perror("Erro ao enviar cabeçalho HTTP");
        fclose(file);
        return;
    }

    printf("Enviando %s para %s (%s)\n",file_path,client_ip,html ? "HTML sem limitação" : "objeto com QoS");

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    size_t bytes_sent = 0;
    int send_failed = 0;
    struct timespec transfer_started = {0};
    struct timespec last_progress = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &transfer_started) != 0) {
        perror("Erro ao iniciar cronometro da transferencia");
        send_failed = 1;
    }
    last_progress = transfer_started;

    while (!send_failed &&
           (bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        int result;

        if (html) {
            //HTML - envio normal, sem limitação de taxa
            size_t sent = 0;

            result = send_all_count(
                client_socket,
                buffer,
                bytes_read,
                &sent
            );
        } else {
            //imagens, CSS, JS e demais objetos: aplica a taxa compartilhada do IP
            result = qos_send_limited(
                client_socket,
                buffer,
                bytes_read,
                client_ip
            );
        }

        if (result != 0) {
            perror("Erro ao enviar conteúdo do arquivo");
            send_failed = 1;
            break;
        }

        bytes_sent += bytes_read;

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            perror("Erro ao atualizar cronometro da transferencia");
            send_failed = 1;
            break;
        }

        double since_last_progress = elapsed_seconds(&last_progress, &now);
        if (since_last_progress >= PROGRESS_INTERVAL_SECONDS ||
            bytes_sent == (size_t)file_size) {
            print_transfer_progress(file_path, client_ip, bytes_sent,
                                    file_size,
                                    elapsed_seconds(&transfer_started, &now),
                                    bytes_sent == (size_t)file_size);
            last_progress = now;
        }
    }

    if (ferror(file)) {
        perror("Erro ao ler arquivo");
        send_failed = 1;
    }

    if (!send_failed && bytes_sent == 0) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            print_transfer_progress(file_path, client_ip, bytes_sent,
                                    file_size,
                                    elapsed_seconds(&transfer_started, &now),
                                    1);
        } else {
            perror("Erro ao concluir cronometro da transferencia");
            send_failed = 1;
        }
    } else if (send_failed && bytes_sent > 0) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            print_transfer_progress(file_path, client_ip, bytes_sent,
                                    file_size,
                                    elapsed_seconds(&transfer_started, &now),
                                    0);
        } else {
            perror("Erro ao atualizar cronometro da transferencia");
        }
    }

    fclose(file);

    if (send_failed) {
        shutdown(client_socket, SHUT_RDWR);
    }
}