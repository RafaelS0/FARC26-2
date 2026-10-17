#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

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

void send_file(int client_socket, const char *file_path)
{
    char obj_location[512] = "objects/"; // Caminho base para os arquivos
    char *full_path = strcat(obj_location, file_path);
    
   
    // Proteção contra Path Traversal (evita acesso a arquivos como ../../etc/passwd)
    if (strstr(file_path, ".."))
    {
        send_http_error(client_socket, 403, "Forbidden");
        return;
    }

    FILE *file = fopen(full_path, "rb");

    if (file == NULL)
    {
        perror("Erro ao abrir arquivo");
        send_http_error(client_socket, 404, "Not Found");
        return;
    }

    // Calcula o tamanho do arquivo
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    const char *content_type = get_content_type(file_path);

    // Monta e envia os cabeçalhos HTTP
    char http_header[BUFFER_SIZE];
    snprintf(http_header, sizeof(http_header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %ld\r\n"
             "Connection: keep-alive\r\n"
             "\r\n",
             content_type, file_size);

    send(client_socket, http_header, strlen(http_header), 0);
    printf("Resposta enviada:\n%s\n", http_header);

    // Transfere o conteúdo do arquivo em blocos
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0)
    {
        send(client_socket, buffer, bytes_read, 0);
    }

    fclose(file);
}