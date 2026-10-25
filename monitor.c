#include "monitor.h"
#include "admission.h"
#include "client_manager.h"
#include "log.h"
#include "metrics.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *buf;
    size_t size;
    size_t len;
} frame_buffer;

static void append(frame_buffer *frame, const char *fmt, ...)
{
    if (frame->len >= frame->size) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(frame->buf + frame->len, frame->size - frame->len, fmt, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    if ((size_t)written >= frame->size - frame->len) {
        frame->len = frame->size - 1;
    } else {
        frame->len += (size_t)written;
    }
}

size_t monitor_render(char *buf, size_t size, int use_ansi)
{
    if (buf == NULL || size == 0) {
        return 0;
    }

    buf[0] = '\0';

    frame_buffer frame = {buf, size, 0};
    const char *eol = use_ansi ? "\033[K\n" : "\n";

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    admission_stats capacity;
    admission_get_stats(&capacity);

    client_view rows[MONITOR_MAX_ROWS];
    size_t total = 0;
    size_t shown = client_snapshot(rows, MONITOR_MAX_ROWS, &total);

    if (use_ansi) {
        append(&frame, "\033[H");
    }

    append(&frame, "=== Monitor do servidor === %02d:%02d:%02d === atualização a cada %d ms%s",
           local.tm_hour, local.tm_min, local.tm_sec, MONITOR_INTERVAL_MS, eol);

    append(&frame, "Vazão: total %u kbps | reservada %u kbps | disponível %u kbps | clientes ativos: %zu%s",
           capacity.capacity_kbps, capacity.reserved_kbps, capacity.available_kbps, total, eol);

    append(&frame, "%s", eol);

    append(&frame, "%-15s %5s  %-20s %9s %12s %13s %8s  %s%s",
           "IP", "PORTA", "ESTADO", "RTT (ms)", "ATRASO (ms)", "BANDA (kbps)", "OBJ ok/int", "ÚLTIMO", eol);

    if (total == 0) {
        append(&frame, "(nenhum cliente conectado)%s", eol);
    }

    for (size_t i = 0; i < shown; i++) {
        const client_view *row = &rows[i];
        char port[16], state[32], rtt[32], delay[32], bandwidth[32], objects[48];

        snprintf(port, sizeof(port), "%d", row->port);

        if (row->busy) {
            snprintf(state, sizeof(state), "atendendo");
        } else {
            snprintf(state, sizeof(state), "ocioso (encerra %.0fs)", row->idle_remaining_sec);
        }

        metrics_format(rtt, sizeof(rtt), row->metrics.has_rtt, row->metrics.rtt_ms, 3, NULL);
        metrics_format(delay, sizeof(delay), row->metrics.has_delay, row->metrics.delay_ms, 1, NULL);
        metrics_format(bandwidth, sizeof(bandwidth), row->metrics.has_bandwidth, row->metrics.bandwidth_kbps, 1, NULL);
        snprintf(objects, sizeof(objects), "%lu/%lu", row->metrics.objects, row->metrics.interrupted);

        append(&frame, "%-15s %5s  %-20s %9s %12s %13s %8s  %s%s",
               row->ip, port, state, rtt, delay, bandwidth, objects,
               metrics_status_text(row->metrics.last_status), eol);
    }

    if (total > shown) {
        append(&frame, "... e mais %zu cliente(s) não exibido(s)%s", total - shown, eol);
    }

    append(&frame, "%s", eol);
    append(&frame, "Log detalhado em %s. Ctrl+C encerra o servidor.%s", LOG_FILE_PATH, eol);

    if (use_ansi) {
        append(&frame, "\033[J");
    } else {
        append(&frame, "\n");
    }

    return frame.len;
}

static void *monitor_loop(void *arg)
{
    (void)arg;

    static char frame[MONITOR_FRAME_SIZE];
    int use_ansi = isatty(STDOUT_FILENO);

    while (1) {
        size_t length = monitor_render(frame, sizeof(frame), use_ansi);

        fwrite(frame, 1, length, stdout);
        fflush(stdout);

        struct timespec interval = {
            .tv_sec = MONITOR_INTERVAL_MS / 1000,
            .tv_nsec = (long)(MONITOR_INTERVAL_MS % 1000) * 1000000L
        };
        nanosleep(&interval, NULL);
    }

    return NULL;
}

int monitor_start(void)
{
    pthread_t thread;

    if (pthread_create(&thread, NULL, monitor_loop, NULL) != 0) {
        return -1;
    }

    pthread_detach(thread);
    return 0;
}
