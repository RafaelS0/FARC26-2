#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "qos.h"
#include "log.h"
#include <time.h>
#include <errno.h>
#include <strings.h>

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
    log_printf("Resposta de erro enviada (%d %s)", status_code, status_text);
}

void send_file(int client_socket,const char *file_path,const char *client_ip,transfer_record *record)
{
    memset(&record->first_byte_time, 0, sizeof(record->first_byte_time));
    memset(&record->end_time, 0, sizeof(record->end_time));
    record->object_size = 0;
    record->bytes_sent = 0;
    record->status = TRANSFER_NONE;

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

    record->object_size = (size_t) file_size;

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
        log_printf("Erro ao enviar cabeçalho HTTP: %s", strerror(errno));
        clock_gettime(CLOCK_MONOTONIC, &record->first_byte_time);
        record->end_time = record->first_byte_time;
        record->status = TRANSFER_INTERRUPTED;
        fclose(file);
        return;
    }

    log_printf("Enviando %s para %s (%s)",file_path,client_ip,html ? "HTML sem limitação" : "objeto com QoS");

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    int send_failed = 0;

    clock_gettime(CLOCK_MONOTONIC, &record->first_byte_time);

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
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
            log_printf("Erro ao enviar conteúdo do arquivo: %s", strerror(errno));
            send_failed = 1;
            break;
        }

        record->bytes_sent += bytes_read;
    }

    clock_gettime(CLOCK_MONOTONIC, &record->end_time);

    if (ferror(file)) {
        log_printf("Erro ao ler arquivo: %s", strerror(errno));
        send_failed = 1;
    }

    fclose(file);

    record->status = send_failed ? TRANSFER_INTERRUPTED : TRANSFER_COMPLETE;

    if (send_failed) {
        shutdown(client_socket, SHUT_RDWR);
    }
}

void send_http_service_unavailable(int client_socket, int retry_after_sec)
{
    const char *body = "Servidor sem capacidade de vazao disponivel. Tente novamente mais tarde.\n";
    char response[BUFFER_SIZE];

    int length = snprintf(response, sizeof(response),
                          "HTTP/1.1 503 Service Unavailable\r\n"
                          "Content-Type: text/plain\r\n"
                          "Content-Length: %zu\r\n"
                          "Retry-After: %d\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "%s",
                          strlen(body), retry_after_sec, body);

    if (length > 0 && (size_t)length < sizeof(response)) {
        send(client_socket, response, (size_t)length, MSG_NOSIGNAL);
    }
}
