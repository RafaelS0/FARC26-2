
#include "rtt_monitor.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define MAX_RTT_CLIENTS 100
#define EXPERIMENT_ID_SIZE 64

typedef struct {
    char experiment_id[EXPERIMENT_ID_SIZE];
    struct timespec html_timestamp;
    int active;
} rtt_entry;

static rtt_entry entries[MAX_RTT_CLIENTS];
static pthread_mutex_t rtt_mutex = PTHREAD_MUTEX_INITIALIZER;

void rtt_register_html(const char *experiment_id,
                       const struct timespec *timestamp)
{
    if (experiment_id == NULL || timestamp == NULL ||
        experiment_id[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&rtt_mutex);

    for (int i = 0; i < MAX_RTT_CLIENTS; i++) {
        if (entries[i].active &&
            strcmp(entries[i].experiment_id, experiment_id) == 0) {
            entries[i].html_timestamp = *timestamp;
            pthread_mutex_unlock(&rtt_mutex);
            return;
        }
    }

    for (int i = 0; i < MAX_RTT_CLIENTS; i++) {
        if (!entries[i].active) {
            snprintf(entries[i].experiment_id,
                     sizeof(entries[i].experiment_id),
                     "%s", experiment_id);

            entries[i].html_timestamp = *timestamp;
            entries[i].active = 1;

            printf("[RTT] Experimento %s: HTML registrado.\n",
                   experiment_id);

            pthread_mutex_unlock(&rtt_mutex);
            return;
        }
    }

    fprintf(stderr, "[RTT] Limite de experimentos atingido.\n");
    pthread_mutex_unlock(&rtt_mutex);
}


void rtt_register_object(const char *experiment_id,
                         const struct timespec *timestamp)
{
    if (experiment_id == NULL || timestamp == NULL ||
        experiment_id[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&rtt_mutex);

    for (int i = 0; i < MAX_RTT_CLIENTS; i++) {
        if (entries[i].active &&
            strcmp(entries[i].experiment_id, experiment_id) == 0) {

            double elapsed_ms =
                (timestamp->tv_sec - entries[i].html_timestamp.tv_sec)
                    * 1000.0
                + (timestamp->tv_nsec -
                   entries[i].html_timestamp.tv_nsec) / 1000000.0;

            printf("[RTT] Experimento %s: intervalo HTML-objeto = %.3f ms\n",
                   experiment_id, elapsed_ms);

            entries[i].active = 0;

            pthread_mutex_unlock(&rtt_mutex);
            return;
        }
    }

    printf("[RTT] Nenhum HTML pendente para o experimento %s.\n",
           experiment_id);

    pthread_mutex_unlock(&rtt_mutex);
}
