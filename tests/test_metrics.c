#define _GNU_SOURCE
#include "admission.h"
#include "client_manager.h"
#include "metrics.h"
#include "monitor.h"
#include "qos.h"

#include <arpa/inet.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

static struct timespec ts(time_t sec, long nsec)
{
    struct timespec t = {sec, nsec};
    return t;
}

static void test_conversions(void)
{
    printf("[teste] conversões de tempo e largura de banda\n");

    struct timespec a = ts(1, 500000000), b = ts(3, 250000000);
    CHECK(NEAR(metrics_elapsed_ms(&a, &b), 1750.0, 1e-9));

    struct timespec c = ts(1, 900000000), d = ts(2, 100000000);
    CHECK(NEAR(metrics_elapsed_ms(&c, &d), 200.0, 1e-9));

    CHECK(NEAR(metrics_elapsed_ms(&a, &a), 0.0, 1e-12));
    CHECK(NEAR(metrics_elapsed_ms(&b, &a), 0.0, 1e-12));

    struct timespec e = ts(10, 0), f = ts(10, 123456);
    CHECK(NEAR(metrics_elapsed_ms(&e, &f), 0.123456, 1e-9));

    double kbps = 0;
    CHECK(metrics_bandwidth_kbps(125000, 1000.0, &kbps) == 0);
    CHECK(NEAR(kbps, 1000.0, 1e-9));

    CHECK(metrics_bandwidth_kbps(1000, 8.0, &kbps) == 0);
    CHECK(NEAR(kbps, 1000.0, 1e-9));

    CHECK(metrics_bandwidth_kbps(1000000, 1000.0, &kbps) == 0);
    CHECK(NEAR(kbps, 8000.0, 1e-9));

    CHECK(metrics_bandwidth_kbps(68750, 1000.0, &kbps) == 0);
    CHECK(NEAR(kbps, 550.0, 1e-9));

    CHECK(metrics_bandwidth_kbps(125, METRICS_MIN_DURATION_MS, &kbps) == 0);
    CHECK(NEAR(kbps, 1000.0, 1e-9));

    CHECK(metrics_bandwidth_kbps(0, 1000.0, &kbps) == -1);
    CHECK(metrics_bandwidth_kbps(18, 0.02, &kbps) == -1);
    CHECK(metrics_bandwidth_kbps(18, 0.0, &kbps) == -1);
    CHECK(metrics_bandwidth_kbps(100, 1000.0, NULL) == -1);

    CHECK(NEAR(metrics_end_to_end_delay_ms(800.0, 0.5), 800.5, 1e-9));
    CHECK(NEAR(metrics_end_to_end_delay_ms(0.0, 20.0), 20.0, 1e-9));
}

static transfer_record make_record(transfer_status status, size_t size, size_t sent,
                                   long request_ns, long first_ns, long end_ns)
{
    transfer_record r;
    memset(&r, 0, sizeof(r));
    r.request_time = ts(10, request_ns);
    r.first_byte_time = ts(10, first_ns);
    r.end_time = ts(10 + end_ns / 1000000000L, end_ns % 1000000000L);
    r.object_size = size;
    r.bytes_sent = sent;
    r.status = status;
    return r;
}

static void test_apply_transfer(void)
{
    printf("[teste] transferências completas, interrompidas e curtas\n");

    client_metrics m;
    transfer_metrics tm;
    memset(&m, 0, sizeof(m));

    transfer_record first = make_record(TRANSFER_COMPLETE, 100000, 100000, 0, 2000000, 802000000L);
    metrics_apply_transfer(&m, &first, &tm);
    CHECK(NEAR(tm.send_ms, 800.0, 1e-6));
    CHECK(NEAR(tm.server_ms, 802.0, 1e-6));
    CHECK(tm.has_bandwidth && NEAR(tm.bandwidth_kbps, 1000.0, 1e-6));
    CHECK(!tm.has_delay);
    CHECK(!m.has_delay);
    CHECK(m.has_bandwidth);
    CHECK(m.objects == 1 && m.interrupted == 0 && m.total_bytes == 100000);

    m.has_rtt = 1;
    m.rtt_ms = 20.0;
    transfer_record second = make_record(TRANSFER_COMPLETE, 100000, 100000, 0, 2000000, 802000000L);
    metrics_apply_transfer(&m, &second, &tm);
    CHECK(tm.has_delay && NEAR(tm.delay_ms, 822.0, 1e-6));
    CHECK(m.has_delay && NEAR(m.delay_ms, 822.0, 1e-6));
    CHECK(m.objects == 2 && m.total_bytes == 200000);
    CHECK(m.last_status == TRANSFER_COMPLETE);

    transfer_record cut = make_record(TRANSFER_INTERRUPTED, 200000, 25000, 0, 1000000, 201000000L);
    metrics_apply_transfer(&m, &cut, &tm);
    CHECK(tm.has_bandwidth && NEAR(tm.bandwidth_kbps, 1000.0, 1e-6));
    CHECK(!tm.has_delay);
    CHECK(NEAR(m.delay_ms, 822.0, 1e-6));
    CHECK(m.objects == 2 && m.interrupted == 1);
    CHECK(m.total_bytes == 225000);
    CHECK(m.last_status == TRANSFER_INTERRUPTED);

    transfer_record tiny = make_record(TRANSFER_COMPLETE, 18, 18, 0, 100000, 120000);
    double before = m.bandwidth_kbps;
    metrics_apply_transfer(&m, &tiny, &tm);
    CHECK(!tm.has_bandwidth);
    CHECK(NEAR(m.bandwidth_kbps, before, 1e-12));
    CHECK(m.objects == 3 && m.total_bytes == 225018);

    transfer_record none = make_record(TRANSFER_NONE, 0, 0, 0, 0, 0);
    client_metrics snapshot = m;
    metrics_apply_transfer(&m, &none, NULL);
    CHECK(memcmp(&snapshot, &m, sizeof(m)) == 0);

    client_metrics fresh;
    memset(&fresh, 0, sizeof(fresh));
    transfer_record empty = make_record(TRANSFER_COMPLETE, 0, 0, 0, 0, 500000);
    metrics_apply_transfer(&fresh, &empty, &tm);
    CHECK(!tm.has_bandwidth);
    CHECK(fresh.objects == 1 && fresh.total_bytes == 0);
}

static void test_format(void)
{
    printf("[teste] formatação com unidades e métricas indisponíveis\n");

    char buf[64];

    metrics_format(buf, sizeof(buf), 0, 123.0, 3, "ms");
    CHECK(strcmp(buf, "n/d") == 0);

    metrics_format(buf, sizeof(buf), 1, 998.9876, 3, "kbps");
    CHECK(strcmp(buf, "998.988 kbps") == 0);

    metrics_format(buf, sizeof(buf), 1, 0.0456, 3, NULL);
    CHECK(strcmp(buf, "0.046") == 0);

    CHECK(strcmp(metrics_status_text(TRANSFER_COMPLETE), "completo") == 0);
    CHECK(strcmp(metrics_status_text(TRANSFER_INTERRUPTED), "interrompido") == 0);
    CHECK(strcmp(metrics_status_text(TRANSFER_NONE), "n/d") == 0);
}

static void test_rtt_sampling(void)
{
    printf("[teste] amostragem de RTT em conexão TCP real\n");

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(listener, 1) == 0);

    socklen_t len = sizeof(addr);
    getsockname(listener, (struct sockaddr *)&addr, &len);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(client, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    int server = accept(listener, NULL, NULL);
    CHECK(server >= 0);

    char byte = 'x';
    CHECK(write(server, &byte, 1) == 1);
    CHECK(read(client, &byte, 1) == 1);
    CHECK(write(client, &byte, 1) == 1);
    CHECK(read(server, &byte, 1) == 1);
    usleep(20000);

    double rtt = -1;
    CHECK(metrics_sample_rtt_ms(server, &rtt) == 0);
    CHECK(rtt > 0.0 && rtt < 1000.0);

    client_metrics m;
    memset(&m, 0, sizeof(m));
    CHECK(metrics_update_rtt(&m, server) == 0);
    CHECK(m.has_rtt && m.rtt_ms > 0.0);

    CHECK(metrics_sample_rtt_ms(-1, &rtt) == -1);
    client_metrics untouched;
    memset(&untouched, 0, sizeof(untouched));
    CHECK(metrics_update_rtt(&untouched, -1) == -1);
    CHECK(!untouched.has_rtt);

    close(client);
    close(server);
    close(listener);
}

static int add_client(const char *ip, int port)
{
    client_info info;
    memset(&info, 0, sizeof(info));
    info.socket = port;
    info.port = port;
    snprintf(info.ip, sizeof(info.ip), "%s", ip);
    return register_client(&info);
}

static void test_snapshot(void)
{
    printf("[teste] snapshot de clientes com métricas\n");

    size_t total = 0;
    client_view views[8];
    CHECK(client_snapshot(views, 8, &total) == 0);
    CHECK(total == 0);

    int s1 = add_client("192.0.2.1", 5001);
    int s2 = add_client("192.0.2.2", 5002);
    int s3 = add_client("192.0.2.3", 5003);
    CHECK(s1 >= 0 && s2 >= 0 && s3 >= 0);

    client_metrics m;
    memset(&m, 0, sizeof(m));
    m.has_rtt = 1;
    m.rtt_ms = 0.25;
    m.has_bandwidth = 1;
    m.bandwidth_kbps = 998.5;
    m.has_delay = 1;
    m.delay_ms = 801.25;
    m.objects = 2;
    m.interrupted = 1;
    m.last_status = TRANSFER_INTERRUPTED;
    client_publish_metrics(s1, &m);
    set_busy(s2, 1);

    CHECK(client_snapshot(views, 8, &total) == 3);
    CHECK(total == 3);
    CHECK(strcmp(views[0].ip, "192.0.2.1") == 0 && views[0].port == 5001);
    CHECK(!views[0].busy && views[1].busy);
    CHECK(views[0].idle_remaining_sec > 0 && views[0].idle_remaining_sec <= TIMEOUT_SEC);
    CHECK(views[0].metrics.has_rtt && NEAR(views[0].metrics.rtt_ms, 0.25, 1e-12));
    CHECK(views[0].metrics.objects == 2 && views[0].metrics.interrupted == 1);
    CHECK(!views[2].metrics.has_rtt && !views[2].metrics.has_bandwidth && !views[2].metrics.has_delay);
    CHECK(views[2].metrics.last_status == TRANSFER_NONE);

    CHECK(client_snapshot(views, 2, &total) == 2);
    CHECK(total == 3);

    CHECK(client_snapshot(NULL, 0, &total) == 0);
    CHECK(total == 3);

    unregister_client(s1);
    CHECK(client_snapshot(views, 8, &total) == 2);
    CHECK(total == 2);

    int reused = add_client("192.0.2.9", 5009);
    CHECK(reused == s1);
    CHECK(client_snapshot(views, 8, &total) == 3);
    CHECK(views[0].metrics.objects == 0 && !views[0].metrics.has_rtt);

    client_publish_metrics(-1, &m);
    client_publish_metrics(MAX_CLIENTS, &m);
    client_publish_metrics(s1, NULL);

    unregister_client(s1);
    unregister_client(s2);
    unregister_client(s3);
    CHECK(client_snapshot(views, 8, &total) == 0 && total == 0);
}

static void test_render(void)
{
    printf("[teste] renderização do monitor\n");

    CHECK(admission_init(5000) == 0);

    static char frame[MONITOR_FRAME_SIZE];

    size_t len = monitor_render(frame, sizeof(frame), 0);
    CHECK(len == strlen(frame));
    CHECK(strstr(frame, "=== Monitor do servidor ===") != NULL);
    CHECK(strstr(frame, "total 5000 kbps") != NULL);
    CHECK(strstr(frame, "clientes ativos: 0") != NULL);
    CHECK(strstr(frame, "nenhum cliente conectado") != NULL);
    CHECK(strstr(frame, "\033") == NULL);

    int s1 = add_client("192.0.2.1", 6001);
    int s2 = add_client("192.0.2.2", 6002);

    client_metrics m;
    memset(&m, 0, sizeof(m));
    m.has_rtt = 1;
    m.rtt_ms = 0.045;
    m.has_delay = 1;
    m.delay_ms = 812.34;
    m.has_bandwidth = 1;
    m.bandwidth_kbps = 998.76;
    m.objects = 3;
    m.interrupted = 1;
    m.last_status = TRANSFER_COMPLETE;
    client_publish_metrics(s1, &m);
    set_busy(s1, 1);

    monitor_render(frame, sizeof(frame), 0);
    CHECK(strstr(frame, "clientes ativos: 2") != NULL);
    CHECK(strstr(frame, "RTT (ms)") != NULL);
    CHECK(strstr(frame, "ATRASO (ms)") != NULL);
    CHECK(strstr(frame, "BANDA (kbps)") != NULL);

    char *row1 = strstr(frame, "192.0.2.1 ");
    char *row2 = strstr(frame, "192.0.2.2 ");
    CHECK(row1 != NULL && row2 != NULL);

    if (row1 != NULL && row2 != NULL) {
        char line1[256], line2[256];
        snprintf(line1, sizeof(line1), "%.*s", (int)strcspn(row1, "\n"), row1);
        snprintf(line2, sizeof(line2), "%.*s", (int)strcspn(row2, "\n"), row2);

        CHECK(strstr(line1, "6001") != NULL);
        CHECK(strstr(line1, "atendendo") != NULL);
        CHECK(strstr(line1, "0.045") != NULL);
        CHECK(strstr(line1, "812.3") != NULL);
        CHECK(strstr(line1, "998.8") != NULL);
        CHECK(strstr(line1, "3/1") != NULL);
        CHECK(strstr(line1, "completo") != NULL);

        CHECK(strstr(line2, "6002") != NULL);
        CHECK(strstr(line2, "ocioso") != NULL);
        CHECK(strstr(line2, "n/d") != NULL);
        CHECK(strstr(line2, "0/0") != NULL);
    }

    len = monitor_render(frame, sizeof(frame), 1);
    CHECK(strncmp(frame, "\033[H", 3) == 0);
    CHECK(strstr(frame, "\033[K\n") != NULL);
    CHECK(strstr(frame, "\033[J") != NULL);
    CHECK(len == strlen(frame));

    char tiny[64];
    len = monitor_render(tiny, sizeof(tiny), 0);
    CHECK(len < sizeof(tiny) && len == strlen(tiny));
    CHECK(monitor_render(tiny, 0, 0) == 0);
    CHECK(monitor_render(NULL, 10, 0) == 0);

    int extra[MONITOR_MAX_ROWS + 5];
    int count = 0;
    for (int i = 0; i < MONITOR_MAX_ROWS + 3; i++) {
        extra[count] = add_client("198.51.100.7", 7000 + i);
        if (extra[count] >= 0) count++;
    }
    monitor_render(frame, sizeof(frame), 0);
    CHECK(strstr(frame, "e mais 5 cliente(s) não exibido(s)") != NULL);

    for (int i = 0; i < count; i++) unregister_client(extra[i]);
    unregister_client(s1);
    unregister_client(s2);
}

#define WRITERS 8
#define ROUNDS 40000

static volatile int writers_done = 0;

static void *writer(void *p)
{
    int id = (int)(long)p;
    char ip[INET_ADDRSTRLEN];
    snprintf(ip, sizeof(ip), "203.0.113.%u", (unsigned)(unsigned char)(id + 1));

    for (int i = 1; i <= ROUNDS; i++) {
        int slot = add_client(ip, 8000 + id);
        client_metrics m;
        memset(&m, 0, sizeof(m));
        m.objects = (unsigned long)i;
        m.total_bytes = (unsigned long long)i * 10;
        client_publish_metrics(slot, &m);
        set_busy(slot, i % 2);
        unregister_client(slot);
    }

    __atomic_fetch_add(&writers_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *reader(void *p)
{
    (void)p;
    static __thread char frame[MONITOR_FRAME_SIZE];
    unsigned long snapshots = 0;

    while (__atomic_load_n(&writers_done, __ATOMIC_ACQUIRE) < WRITERS) {
        client_view views[MONITOR_MAX_ROWS];
        size_t total = 0;
        size_t n = client_snapshot(views, MONITOR_MAX_ROWS, &total);

        CHECK(n <= total);

        for (size_t i = 0; i < n; i++) {
            CHECK(views[i].ip[0] != '\0');
            CHECK(views[i].metrics.total_bytes == (unsigned long long)views[i].metrics.objects * 10);
        }

        monitor_render(frame, sizeof(frame), 0);
        snapshots++;
    }

    CHECK(snapshots > 0);
    return NULL;
}

static void test_concurrent_snapshot(void)
{
    printf("[teste] %d threads atualizando métricas enquanto o monitor lê\n", WRITERS);

    pthread_t writers[WRITERS], reader_thread;
    pthread_create(&reader_thread, NULL, reader, NULL);

    for (long i = 0; i < WRITERS; i++) {
        pthread_create(&writers[i], NULL, writer, (void *)i);
    }

    for (int i = 0; i < WRITERS; i++) {
        pthread_join(writers[i], NULL);
    }

    pthread_join(reader_thread, NULL);

    size_t total = 0;
    client_snapshot(NULL, 0, &total);
    CHECK(total == 0);
}

int main(void)
{
    char qos_path[] = "/tmp/qos_metrics_XXXXXX";
    int fd = mkstemp(qos_path);
    if (fd < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    FILE *f = fdopen(fd, "w");
    fprintf(f, "192.0.2.1 1000\n");
    fclose(f);
    int rc = qos_init(qos_path);
    unlink(qos_path);
    if (rc != 0) {
        return EXIT_FAILURE;
    }

    test_conversions();
    test_apply_transfer();
    test_format();
    test_rtt_sampling();
    test_snapshot();
    test_render();
    test_concurrent_snapshot();

    printf("\n%d verificações, %d falha(s)\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
