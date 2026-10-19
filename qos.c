#include "qos.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define QOS_MAX_IPS 1024

typedef struct {
    char ip[INET_ADDRSTRLEN];
    unsigned int rate_kbps;
    pthread_mutex_t tx_mutex;
    struct timespec next_allowed;
} qos_entry;

static qos_entry entries[QOS_MAX_IPS];
static size_t entry_count = 0;

static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;


//verifica se o endereço é um IPv4 valido
static int valid_ipv4(const char *ip){
    struct in_addr address;
    return ip != NULL && inet_pton(AF_INET, ip, &address) == 1;
}


//procura uma entrada. O chamador deve possuir table_mutex
static qos_entry *find_entry_locked(const char *ip){
    for (size_t i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].ip, ip) == 0) {
            return &entries[i];
        }
    }

    return NULL;
}


//cria uma entrada com a taxa padrão
static qos_entry *create_entry_locked(const char *ip){
    if (entry_count >= QOS_MAX_IPS) {
        errno = ENOSPC;
        return NULL;
    }

    qos_entry *entry = &entries[entry_count];
    memset(entry, 0, sizeof(*entry));

    snprintf(entry->ip, sizeof(entry->ip), "%s", ip);
    entry->rate_kbps = QOS_DEFAULT_RATE_KBPS;

    int rc = pthread_mutex_init(&entry->tx_mutex, NULL);

    if (rc != 0) {
        errno = rc;
        return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &entry->next_allowed);

    entry_count++;
    return entry;
}


// obtém ou cria a entrada para um IP. As entradas permanecem validas durante a execução do servidor
static qos_entry *get_entry(const char *ip){
    if (!valid_ipv4(ip)) {
        errno = EINVAL;
        return NULL;
    }

    pthread_mutex_lock(&table_mutex);

    qos_entry *entry = find_entry_locked(ip);

    if (entry == NULL) {
        entry = create_entry_locked(ip);
    }

    pthread_mutex_unlock(&table_mutex);

    return entry;
}


// Carrega um arquivo no formato -> IP taxa_kbps
int qos_init(const char *config_path){
    FILE *file = fopen(config_path, "r");

    if (file == NULL) {
        perror("Erro ao abrir arquivo de QoS");
        return -1;
    }

    char line[256];
    int line_number = 0;
    int result = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        char *comment = strchr(line, '#');

        if (comment != NULL) {
            *comment = '\0';
        }

        char *p = line;

        while (isspace((unsigned char)*p)) {
            p++;
        }

        if (*p == '\0') {
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        long rate;
        char extra;

        if (sscanf(p, "%15s %ld %c", ip, &rate, &extra) != 2 ||
            !valid_ipv4(ip) ||
            rate <= 0 ||
            rate > UINT_MAX) {

            fprintf(stderr, "Configuração de QoS inválida na linha %d\n", line_number);

            result = -1;
            break;
        }

        pthread_mutex_lock(&table_mutex);

        qos_entry *entry = find_entry_locked(ip);

        if (entry == NULL) {
            entry = create_entry_locked(ip);
        }

        if (entry != NULL) {
            entry->rate_kbps = (unsigned int)rate;
        }

        pthread_mutex_unlock(&table_mutex);

        if (entry == NULL) {
            fprintf(stderr, "Não foi possível registrar o IP da linha %d\n", line_number);

            result = -1;
            break;
        }
    }

    if (ferror(file)) {
        perror("Erro ao ler arquivo de QoS");
        result = -1;
    }

    fclose(file);

    if (result == 0) {
        printf("QoS - Configuração carregada com sucesso.\n");
    }

    return result;
}


//consulta a taxa configurada. IPs desconhecidos recebem taxa padrão
int qos_get_rate_kbps(const char *client_ip, unsigned int *rate_kbps){
    if (rate_kbps == NULL) {
        errno = EINVAL;
        return -1;
    }

    qos_entry *entry = get_entry(client_ip);

    if (entry == NULL) {
        return -1;
    }

    *rate_kbps = entry->rate_kbps;

    return 0;
}



//Implementar limitação de taxa na transmissão dos objetos
//soma uma quantidade de segundos a um instante monotônico
static struct timespec add_seconds(struct timespec value, double seconds){
    time_t whole_seconds = (time_t)seconds;

    long nanoseconds = (long) ((seconds - (double)whole_seconds) * 1000000000.0);

    value.tv_sec += whole_seconds;
    value.tv_nsec += nanoseconds;

    if (value.tv_nsec >= 1000000000L) {
        value.tv_sec += value.tv_nsec / 1000000000L;
        value.tv_nsec %= 1000000000L;
    }

    return value;
}


//aguarda até determinado instante usando o relógio monotônico
static void sleep_until(struct timespec deadline){
    while (1) {
        struct timespec now;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return;
        }

        time_t seconds = deadline.tv_sec - now.tv_sec;
        long nanoseconds = deadline.tv_nsec - now.tv_nsec;

        if (nanoseconds < 0) {
            seconds--;
            nanoseconds += 1000000000L;
        }

        if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) {
            return;
        }

        struct timespec remaining = {
            .tv_sec = seconds,
            .tv_nsec = nanoseconds
        };

        if (nanosleep(&remaining, NULL) == 0) {
            continue;
        }

        if (errno != EINTR) {
            return;
        }
    }
}


//envia todos os bytes, tratnado envios parciais
int send_all_count(int socket,const void *data,size_t length,size_t *sent_total){
    const char *bytes = data;
    *sent_total = 0;

    while (*sent_total < length) {
        ssize_t sent = send(socket,bytes + *sent_total,length - *sent_total,MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (sent == 0) {
            errno = EPIPE;
            return -1;
        }

        *sent_total += (size_t)sent;
    }

    return 0;
}


//limita a transmissão por IP, compartilhando o limite entre todas as conexões daquele endereço
int qos_send_limited(int client_socket,const void *data,size_t length,const char *client_ip){
    if (length == 0) {
        return 0;
    }

    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }

    qos_entry *entry = get_entry(client_ip);

    if (entry == NULL) {
        return -1;
    }

    pthread_mutex_lock(&entry->tx_mutex);

    //respeita o intervalo reservado pelo envio anterior
    sleep_until(entry->next_allowed);

    size_t bytes_sent = 0;

    int result = send_all_count(client_socket,data,length,&bytes_sent);

    if (bytes_sent > 0) {
        struct timespec finished;

        clock_gettime(CLOCK_MONOTONIC, &finished);

        double seconds = ((double)bytes_sent * 8.0) / ((double)entry->rate_kbps * 1000.0);

        entry->next_allowed = add_seconds(finished, seconds);

        //mantém o bloqueio durante o intervalo de transmissão para que outra conexão do mesmo IP não ultrapasse o limite
        sleep_until(entry->next_allowed);
    }

    pthread_mutex_unlock(&entry->tx_mutex);

    return result;
}