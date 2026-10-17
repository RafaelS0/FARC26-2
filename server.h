#ifndef SERVER_H
#define SERVER_H

#define PORT 8080

int start_server(int port);
void *handle_client(void *arg);
void *input_thread(void *arg);

#endif