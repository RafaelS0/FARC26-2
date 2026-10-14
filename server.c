#include <arpa/inet.h> //Internet operations
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>      // errno para identificar o motivo da desconexão
#include <unistd.h>     //Unix standard functions
#include <sys/time.h>   // timer para encerrar conexão
#include <sys/socket.h> // socket
#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 500
#define TIMEOUT_SEC 15

// Struct para guardar dados do cliente conectado
typedef struct {
    int socket;
    char ip[INET_ADDRSTRLEN];
    int port;
} client_info;

typedef struct {
    int active;                   // slot em uso?
    int busy;                     // 1 = processando/enviando uma resposta
    char ip[INET_ADDRSTRLEN];
    int port;
    int socket;
    struct timespec last_activity; // fim da última atividade
} client_slot;

static client_slot clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// registra um cliente na tabela e devolve o índice (ou -1 se estiver cheia)
static int register_client(const client_info *info)
{
    int slot = -1;
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            clients[i].active = 1;
            clients[i].busy = 0;
            strcpy(clients[i].ip, info->ip);
            clients[i].port = info->port;
            clients[i].socket = info->socket;
            clock_gettime(CLOCK_MONOTONIC, &clients[i].last_activity);
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return slot;
}

// marca se está ocupado e renova o instante da última atividade
static void set_busy(int slot, int busy)
{
    if (slot < 0) return;
    pthread_mutex_lock(&clients_mutex);
    clients[slot].busy = busy;
    clock_gettime(CLOCK_MONOTONIC, &clients[slot].last_activity);
    pthread_mutex_unlock(&clients_mutex);
}

static void unregister_client(int slot)
{
    if (slot < 0) return;
    pthread_mutex_lock(&clients_mutex);
    clients[slot].active = 0;
    pthread_mutex_unlock(&clients_mutex);
}

// imprime os clientes conectados e o tempo restante até o timeout
static void list_clients(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int total = 0;

    pthread_mutex_lock(&clients_mutex);
    printf("\n===== Clientes conectados =====\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) continue;
        total++;

        if (clients[i].busy) {
            printf("%s:%d (socket %d) - atendendo requisição\n",
                   clients[i].ip, clients[i].port, clients[i].socket);
        } else {
            double idle = (now.tv_sec - clients[i].last_activity.tv_sec)
                        + (now.tv_nsec - clients[i].last_activity.tv_nsec) / 1e9;
            double restante = TIMEOUT_SEC - idle;
            if (restante < 0) restante = 0;
            printf("%s:%d (socket %d) - ocioso, encerra em %.0fs\n",
                   clients[i].ip, clients[i].port, clients[i].socket, restante);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    printf("Total: %d\n===============================\n\n", total);
}

// thread que espera ENTER no terminal e lista os clientes
void *input_thread(void *arg)
{
    (void)arg;
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        list_clients();
    }
    return NULL;
}



// Função auxiliar para indicar que tipo de arquivo vamos enviar
static const char *get_content_type(const char *path)
{
    const char *ext = strrchr(path, '.'); // última ocorrência do ponto = extensão
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".txt")  == 0) return "text/plain";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";

    return "application/octet-stream";
}

// Função para enviar um arquivo para o cliente
void send_file(int client_socket, const char *file_path)
{
    FILE *file = fopen(file_path, "rb");

    if (file == NULL)
    {
        perror("Erro ao abrir arquivo");
        const char *not_found_response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        send(client_socket, not_found_response, strlen(not_found_response), 0);
        return;
    }

    fseek(file, 0, SEEK_END);     // move o ponteiro para o final
    long file_size = ftell(file); // obtém o tamanho do arquivo
    rewind(file);                 // volta para o início

    const char *content_type = get_content_type(file_path); // tipo do arquivo

    char http_header[BUFFER_SIZE];
    snprintf(
        http_header,
        sizeof(http_header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        content_type,
        file_size);

    printf("Resposta enviada:\n%s\n", http_header);

    char buffer[BUFFER_SIZE];
    size_t bytes_read;

    //// TRECHO PROPOSTO POR INTELIGENCIA ARTIFICIAL /////
    send(client_socket, http_header, strlen(http_header), 0);
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0)
    {
        send(client_socket, buffer, bytes_read, 0);
    }
    fclose(file);
    //// TRECHO PROPOSTO POR INTELIGENCIA ARTIFICIAL /////
}

// trata uma comunicação de forma persistente em uma thread própria
void *handle_client(void *arg)
{
    client_info info = *(client_info *)arg; // copia os dados para a stack da thread
    free(arg);                              // agora é seguro liberar o original
    int client_socket = info.socket;

    // se o cliente ficar 15s sem enviar nada, encerra a conexão
    struct timeval tv = {TIMEOUT_SEC, 0};
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int slot = register_client(&info);

    printf("[+] Cliente conectado: %s:%d (socket %d)\n", info.ip, info.port, client_socket);

    const char *motivo = "erro desconhecido";

    while (1){
        char request[BUFFER_SIZE];

        ssize_t bytes_received = recv(client_socket, request, sizeof(request) - 1, 0);

        if (bytes_received == 0) {
            motivo = "cliente fechou a conexão";
            break;
        }

        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                motivo = "tempo limite de 15s sem requisição";
            else
                motivo = strerror(errno);
            break;
        }

        request[bytes_received] = '\0';
         set_busy(slot, 1); 
        printf("Requisição recebida de %s:%d:\n%s\n", info.ip, info.port, request);

        char method[16] = "", path[256] = "", protocol[16] = "";

        // limites evitam estouro dos arrays
        sscanf(request, "%15s %255s %15s", method, path, protocol);

        printf("Método: %s\n", method);
        printf("Caminho: %s\n", path);
        printf("Protocolo: %s\n\n", protocol);

        if (strcmp(method, "GET") == 0){
            if (strcmp(path, "/") == 0){
                send_file(client_socket, "index.html");
            }
            else{
                send_file(client_socket, path + 1);
            }
        }
        else{
            const char *response =
                "HTTP/1.1 405 Method Not Allowed\r\n"
                "Content-Length: 0\r\n"
                "Connection: keep-alive\r\n"
                "\r\n";

            send(client_socket, response, strlen(response), 0);
            set_busy(slot, 0);   
        }
    }

    close(client_socket);
    printf("[-] Cliente desconectado: %s:%d (motivo: %s)\n", info.ip, info.port, motivo);

    unregister_client(slot);
    return NULL;
}

int main()
{
    int server_socket, client_socket;

    struct sockaddr_in server_addr, client_addr; // estrutura para armazenar o endereço do servidor e do cliente
    socklen_t client_len = sizeof(client_addr);  // tamanho do endereço do cliente

    server_socket = socket(AF_INET, SOCK_STREAM, 0); // define o tipo de socket como TCP/IP
    if (server_socket == -1)
    {
        perror("Erro ao criar socket");
        exit(EXIT_FAILURE);
    }

    //// TRECHO PROPOSTO POR INTELIGENCIA ARTIFICIAL /////

    server_addr.sin_family = AF_INET;         // define a família de endereços como IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // define o endereço IP do servidor como qualquer endereço disponível
    server_addr.sin_port = htons(PORT);       // define a porta do servidor como 8080

    //// TRECHO PROPOSTO POR INTELIGENCIA ARTIFICIAL /////

    // Vincula o SOCKET ao endereço e porta especificados
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Erro ao vincular o socket");
        close(server_socket);
        exit(EXIT_FAILURE); 
    }

    // Coloca o socket em modo de escuta para aguardar conexões de clientes
    if (listen(server_socket, 5) < 0)
    {
        perror("falha de comunicação");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    printf("Servidor ouvindo na porta %d ... \n", PORT);
    pthread_t input;
    pthread_create(&input, NULL, input_thread, NULL);
    pthread_detach(input);
    printf("Pressione ENTER para listar os clientes conectados.\n");

    while (1) // loop para ficar ouvindo requisições de clientes
    {
        client_len = sizeof(client_addr);

        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

        if (client_socket < 0)
        {
            perror("falha de comunicação");
            continue;
        }

        // guarda socket, IP e porta do cliente para passar à thread
        client_info *info = malloc(sizeof(client_info));

        if (info == NULL){
            perror("Erro ao alocar memória");
            close(client_socket);
            continue;
        }

        info->socket = client_socket;
        info->port = ntohs(client_addr.sin_port);
        inet_ntop(AF_INET, &client_addr.sin_addr, info->ip, sizeof(info->ip));

        pthread_t thread;

        if (pthread_create(&thread, NULL, handle_client, info) != 0){
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