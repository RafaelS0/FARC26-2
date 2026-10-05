#include <arpa/inet.h> //Internet operations
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> //Unix standard functions

#define PORT 8080
#define BUFFER_SIZE 1024

// Aqui seria a lógica para tratar a conexão persistente, mas ainda não sei como implementar, então por enquanto o servidor fecha a conexão após enviar o arquivo HTML
void *handle_client(void *arg)
{
    int client_socket = *(int *)arg;
    free(arg);

    close(client_socket);
    return NULL;
}

// Função para enviar o arquivo HTML para o cliente
void send_html(int client_socket, const char *file_path)
{

    FILE *html_file = fopen(file_path, "rb");

    if (html_file == NULL)
    {
        perror("Erro ao abrir arquivo HTML");
          const char *not_found_response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
          send(client_socket, not_found_response, strlen(not_found_response), 0);
        return;
    }

    fseek(html_file, 0, SEEK_END);
    long file_size = ftell(html_file);
    rewind(html_file);

    char http_header[BUFFER_SIZE];

    snprintf(
        http_header,
        sizeof(http_header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        file_size);

    char buffer[BUFFER_SIZE];
    size_t bytes_read;

    
    send(client_socket, http_header, strlen(http_header), 0); // envia o cabeçalho HTTP para o cliente
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, html_file)) > 0)
    {
        send(client_socket, buffer, bytes_read, 0); // lê o arquivo HTML em blocos de tamanho BUFFER_SIZE e envia para o cliente
    }
    fclose(html_file);
}

int main()
{

    int server_socket, client_socket;

    struct sockaddr_in server_addr, client_addr; // estrutura para armazenar o endereço do servidor e do cliente
    socklen_t client_len = sizeof(client_addr);  // tamanho do endereço do cliente

    server_socket = socket(AF_INET, SOCK_STREAM, 0); // define o tipo de socket como TCP/IP
    if (server_socket == -1)
    {
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;         // define a família de endereços como IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY; // define o endereço IP do servidor como qualquer endereço disponível
    server_addr.sin_port = htons(PORT);       // define a porta do servidor como 8080

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

    while (1)
    {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

        if (client_socket < 0)
        {
            perror("falha de comunicação");
            continue;
        }
        printf("CONEXÃO estabelecida com o cliente \n");
        send_html(client_socket, "index.html");

        // Recebe a requisição do cliente
        char request[BUFFER_SIZE];

        ssize_t bytes_received = recv(client_socket, request, sizeof(request) - 1, 0);

        if (bytes_received < 0)
        {
            perror("Erro ao receber a requisição do cliente");
            continue;
        }

        request[bytes_received] = '\0'; // Adiciona o terminador de string
        printf("Requisição recebida do cliente:\n%s\n", request);

        char method[16], path[256], protocol[16];
        sscanf(request, "%s %s %s", method, path, protocol);
        printf("Método: %s\n", method);
        printf("Caminho: %s\n", path);
        printf("Protocolo: %s\n", protocol);
        printf("\n\n");

        if (strcmp(method, "GET") == 0)
        {
            //se a chamada for na raiz do serivdor envia o index.html
            if (strcmp(path, "/") == 0)
            {
                send_html(client_socket, "index.html");
            }
            else
            {
                // Aqui da pra adicionar lógicas para outros caminhos
              
            }
        }
        else
        {
            // Método não suportado
            const char *method_not_allowed_response = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client_socket, method_not_allowed_response, strlen(method_not_allowed_response), 0);
        }

        close(client_socket);
        printf("Cliente desconectado \n");
    }
    close(server_socket);
    return 0;
}
