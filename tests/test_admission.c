#define _GNU_SOURCE
#include "admission.h"
#include "config.h"
#include "qos.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int checks = 0;
static int failures = 0;
static pthread_mutex_t check_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CHECK(cond)                                                              \
    do {                                                                         \
        pthread_mutex_lock(&check_mutex);                                        \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "  FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                        \
        pthread_mutex_unlock(&check_mutex);                                      \
    } while (0)

static admission_stats stats(void)
{
    admission_stats s;
    admission_get_stats(&s);
    return s;
}

static void test_config_parse(void)
{
    printf("[teste] validação do parâmetro de vazão\n");

    const struct { const char *text; unsigned int expected; } valid[] = {
        {"1", 1}, {"550", 550}, {"10000", 10000}, {"0007", 7}, {"4294967295", UINT_MAX},
    };

    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        unsigned int kbps = 0;
        CHECK(config_parse_capacity(valid[i].text, &kbps, NULL) == 0);
        CHECK(kbps == valid[i].expected);
    }

    const char *invalid[] = {
        "", "0", "00", "-1", "-0", "-5000", "-abc", "abc", "12abc", "1.5", "1e3",
        " 100", "100 ", "+5", "10 kbps", "4294967296", "99999999999999999999999",
    };

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        unsigned int kbps = 12345;
        const char *error = NULL;
        CHECK(config_parse_capacity(invalid[i], &kbps, &error) == -1);
        CHECK(error != NULL && *error != '\0');
        CHECK(kbps == 12345);
    }

    const char *error = NULL;
    unsigned int kbps;
    config_parse_capacity("-10", &kbps, &error);
    CHECK(error != NULL && strstr(error, "negativa") != NULL);
    config_parse_capacity("0", &kbps, &error);
    CHECK(error != NULL && strstr(error, "maior que zero") != NULL);

    CHECK(config_parse_capacity(NULL, &kbps, NULL) == -1);
}

static void test_config_args(void)
{
    printf("[teste] argumentos de main() e disponibilização do limite\n");

    FILE *saved = stderr;
    stderr = fopen("/dev/null", "w");

    config_set_max_capacity_kbps(0);

    char *none[] = {"server", NULL};
    CHECK(config_load_args(1, none) == -1);
    CHECK(config_get_max_capacity_kbps() == 0);

    char *extra[] = {"server", "100", "200", NULL};
    CHECK(config_load_args(3, extra) == -1);

    char *negative[] = {"server", "-100", NULL};
    CHECK(config_load_args(2, negative) == -1);
    CHECK(config_get_max_capacity_kbps() == 0);

    char *zero[] = {"server", "0", NULL};
    CHECK(config_load_args(2, zero) == -1);

    char *text[] = {"server", "dez", NULL};
    CHECK(config_load_args(2, text) == -1);

    fclose(stderr);
    stderr = saved;

    char *ok[] = {"server", "2500", NULL};
    CHECK(config_load_args(2, ok) == 0);
    CHECK(config_get_max_capacity_kbps() == 2500);

    FILE *out = stdout;
    stdout = fopen("/dev/null", "w");
    char *help[] = {"server", "--help", NULL};
    CHECK(config_load_args(2, help) == 1);
    fclose(stdout);
    stdout = out;

    CHECK(admission_init(config_get_max_capacity_kbps()) == 0);
    CHECK(stats().capacity_kbps == 2500);
    CHECK(admission_init(0) == -1);
}

static void test_sequential(void)
{
    printf("[teste] reserva, compartilhamento por IP e liberação\n");

    const char *A = "192.0.2.1", *B = "192.0.2.2", *C = "192.0.2.3";
    unsigned int rate = 0;

    CHECK(admission_init(2500) == 0);

    CHECK(admission_try_admit(A, &rate) == ADMISSION_OK);
    CHECK(rate == QOS_DEFAULT_RATE_KBPS);
    CHECK(admission_try_admit(B, NULL) == ADMISSION_OK);
    CHECK(stats().reserved_kbps == 2000);
    CHECK(stats().available_kbps == 500);

    CHECK(admission_try_admit(C, NULL) == ADMISSION_NO_CAPACITY);
    CHECK(stats().reserved_kbps == 2000);
    CHECK(stats().active_ips == 2);

    CHECK(admission_try_admit(A, &rate) == ADMISSION_OK);
    CHECK(rate == QOS_DEFAULT_RATE_KBPS);
    CHECK(stats().reserved_kbps == 2000);
    CHECK(stats().active_connections == 3);
    CHECK(stats().active_ips == 2);

    admission_release(A);
    CHECK(stats().reserved_kbps == 2000);
    CHECK(admission_try_admit(C, NULL) == ADMISSION_NO_CAPACITY);

    admission_release(A);
    CHECK(stats().reserved_kbps == 1000);
    CHECK(stats().available_kbps == 1500);
    CHECK(admission_try_admit(C, NULL) == ADMISSION_OK);
    CHECK(stats().reserved_kbps == 2000);

    admission_release("192.0.2.99");
    admission_release(A);
    admission_release(NULL);
    CHECK(stats().reserved_kbps == 2000);
    CHECK(stats().active_connections == 2);

    admission_release(B);
    admission_release(C);
    CHECK(stats().reserved_kbps == 0);
    CHECK(stats().active_ips == 0);
    CHECK(stats().active_connections == 0);
    CHECK(stats().peak_reserved_kbps == 2000);
    CHECK(stats().peak_reserved_kbps <= stats().capacity_kbps);
}

static void test_boundaries(void)
{
    printf("[teste] limite exato, taxa maior que a capacidade e entradas inválidas\n");

    CHECK(admission_init(2000) == 0);
    CHECK(admission_try_admit("192.0.2.10", NULL) == ADMISSION_OK);
    CHECK(admission_try_admit("192.0.2.11", NULL) == ADMISSION_OK);
    CHECK(stats().available_kbps == 0);
    CHECK(admission_try_admit("192.0.2.12", NULL) == ADMISSION_NO_CAPACITY);
    CHECK(stats().reserved_kbps == 2000);

    CHECK(admission_init(2000) == 0);
    CHECK(admission_try_admit("10.0.2.1", NULL) == ADMISSION_RATE_EXCEEDS_CAPACITY);
    CHECK(stats().reserved_kbps == 0);
    CHECK(stats().active_ips == 0);

    CHECK(admission_init(2500) == 0);
    CHECK(admission_try_admit("10.0.2.1", NULL) == ADMISSION_OK);
    CHECK(stats().available_kbps == 0);
    admission_release("10.0.2.1");

    CHECK(admission_try_admit("não-é-ip", NULL) == ADMISSION_ERROR);
    CHECK(admission_try_admit("", NULL) == ADMISSION_ERROR);
    CHECK(admission_try_admit(NULL, NULL) == ADMISSION_ERROR);
    CHECK(stats().reserved_kbps == 0);
}

#define CONC_CAPACITY   10000
#define CONC_IP_RATE    700
#define CONC_IPS        40
#define CONC_MAX_IPS    (CONC_CAPACITY / CONC_IP_RATE)
#define CONC_THREADS    64
#define CONC_ROUNDS     3000

typedef struct {
    int id;
    admission_result first_result;
} conc_arg;

static pthread_barrier_t barrier;
static conc_arg conc_args[CONC_THREADS];
static unsigned long stress_admitted = 0, stress_refused = 0;
static pthread_mutex_t stress_mutex = PTHREAD_MUTEX_INITIALIZER;

static void ip_for(int index, char *buf, size_t size)
{
    snprintf(buf, size, "10.0.1.%d", index + 1);
}

static void *conc_worker(void *p)
{
    conc_arg *arg = p;
    char ip[32];
    unsigned int seed = (unsigned int)arg->id * 7919u + 17u;

    ip_for(arg->id % CONC_IPS, ip, sizeof(ip));

    pthread_barrier_wait(&barrier);
    arg->first_result = admission_try_admit(ip, NULL);
    pthread_barrier_wait(&barrier);

    if (arg->id == 0) {
        admission_stats s = stats();
        int admitted_threads = 0;
        int ip_seen[CONC_IPS] = {0};
        int distinct = 0;

        for (int i = 0; i < CONC_THREADS; i++) {
            if (conc_args[i].first_result == ADMISSION_OK) {
                admitted_threads++;
                if (!ip_seen[i % CONC_IPS]) {
                    ip_seen[i % CONC_IPS] = 1;
                    distinct++;
                }
            } else {
                CHECK(conc_args[i].first_result == ADMISSION_NO_CAPACITY);
            }
        }

        CHECK(distinct == CONC_MAX_IPS);
        CHECK(s.reserved_kbps == (unsigned)(distinct * CONC_IP_RATE));
        CHECK(s.reserved_kbps <= CONC_CAPACITY);
        CHECK(s.active_ips == (size_t)distinct);
        CHECK(s.active_connections == (unsigned long)admitted_threads);
        CHECK(admitted_threads > distinct);
    }

    pthread_barrier_wait(&barrier);

    if (arg->first_result == ADMISSION_OK) {
        admission_release(ip);
    }

    pthread_barrier_wait(&barrier);

    unsigned long admitted = 0, refused = 0;

    for (int round = 0; round < CONC_ROUNDS; round++) {
        ip_for((int)(rand_r(&seed) % CONC_IPS), ip, sizeof(ip));

        if (admission_try_admit(ip, NULL) == ADMISSION_OK) {
            admitted++;

            admission_stats s = stats();
            CHECK(s.reserved_kbps <= s.capacity_kbps);
            CHECK(s.reserved_kbps % CONC_IP_RATE == 0);

            if (rand_r(&seed) % 4 == 0) {
                usleep(rand_r(&seed) % 200);
            }

            admission_release(ip);
        } else {
            refused++;
        }
    }

    pthread_mutex_lock(&stress_mutex);
    stress_admitted += admitted;
    stress_refused += refused;
    pthread_mutex_unlock(&stress_mutex);

    return NULL;
}

static void test_concurrent(void)
{
    printf("[teste] %d threads concorrentes disputando %d kbps (%d IPs de %d kbps)\n",
           CONC_THREADS, CONC_CAPACITY, CONC_IPS, CONC_IP_RATE);

    CHECK(admission_init(CONC_CAPACITY) == 0);
    pthread_barrier_init(&barrier, NULL, CONC_THREADS);

    pthread_t threads[CONC_THREADS];

    for (int i = 0; i < CONC_THREADS; i++) {
        conc_args[i].id = i;
        CHECK(pthread_create(&threads[i], NULL, conc_worker, &conc_args[i]) == 0);
    }

    for (int i = 0; i < CONC_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&barrier);

    admission_stats s = stats();

    CHECK(s.reserved_kbps == 0);
    CHECK(s.available_kbps == CONC_CAPACITY);
    CHECK(s.active_ips == 0);
    CHECK(s.active_connections == 0);
    CHECK(s.peak_reserved_kbps <= CONC_CAPACITY);
    CHECK(s.peak_reserved_kbps > 0);
    CHECK(stress_admitted > 0 && stress_refused > 0);

    printf("        estresse: %lu admitidas, %lu recusadas, pico reservado %u/%u kbps\n",
           stress_admitted, stress_refused, s.peak_reserved_kbps, s.capacity_kbps);
}

static void write_qos_config(char *path_template)
{
    int fd = mkstemp(path_template);
    if (fd < 0) {
        perror("mkstemp");
        exit(EXIT_FAILURE);
    }

    FILE *f = fdopen(fd, "w");
    fprintf(f, "10.0.2.1 2500\n");
    for (int i = 1; i <= CONC_IPS; i++) {
        fprintf(f, "10.0.1.%d %d\n", i, CONC_IP_RATE);
    }
    fclose(f);
}

int main(void)
{
    char qos_path[] = "/tmp/qos_test_XXXXXX";
    write_qos_config(qos_path);

    int rc = qos_init(qos_path);
    unlink(qos_path);

    if (rc != 0) {
        fprintf(stderr, "não foi possível carregar a configuração de QoS de teste\n");
        return EXIT_FAILURE;
    }

    test_config_parse();
    test_config_args();
    test_sequential();
    test_boundaries();
    test_concurrent();

    printf("\n%d verificações, %d falha(s)\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
