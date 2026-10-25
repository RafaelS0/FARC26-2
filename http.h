#ifndef HTTP_H
#define HTTP_H

#include "metrics.h"

#define BUFFER_SIZE 4096

typedef struct {
    char method[16];
    char path[256];
    char protocol[16];
    char connection[32];
    char accept[256];
} http_request;

void parse_http_request(const char *raw_request, http_request *req);
void send_file(int client_socket,const char *file_path,const char *client_ip,transfer_record *record);
void send_http_error(int client_socket, int status_code, const char *status_text);
void send_http_service_unavailable(int client_socket, int retry_after_sec);

#endif