#include "metrics.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

double metrics_elapsed_ms(const struct timespec *start, const struct timespec *end)
{
    double ms = (double)(end->tv_sec - start->tv_sec) * 1000.0
              + (double)(end->tv_nsec - start->tv_nsec) / 1000000.0;

    return ms > 0.0 ? ms : 0.0;
}

int metrics_bandwidth_kbps(size_t bytes, double duration_ms, double *kbps)
{
    if (kbps == NULL || bytes == 0 || duration_ms < METRICS_MIN_DURATION_MS) {
        return -1;
    }

    *kbps = ((double)bytes * 8.0) / duration_ms;
    return 0;
}

double metrics_end_to_end_delay_ms(double server_ms, double rtt_ms)
{
    return server_ms + rtt_ms;
}

int metrics_sample_rtt_ms(int socket, double *rtt_ms)
{
    struct tcp_info info;
    socklen_t length = sizeof(info);

    memset(&info, 0, sizeof(info));

    if (rtt_ms == NULL || getsockopt(socket, IPPROTO_TCP, TCP_INFO, &info, &length) != 0) {
        return -1;
    }

    if (info.tcpi_rtt == 0) {
        return -1;
    }

    *rtt_ms = (double)info.tcpi_rtt / 1000.0;
    return 0;
}

int metrics_update_rtt(client_metrics *metrics, int socket)
{
    double rtt_ms;

    if (metrics == NULL || metrics_sample_rtt_ms(socket, &rtt_ms) != 0) {
        return -1;
    }

    metrics->has_rtt = 1;
    metrics->rtt_ms = rtt_ms;
    return 0;
}

void metrics_apply_transfer(client_metrics *metrics, const transfer_record *record, transfer_metrics *out)
{
    transfer_metrics result;
    memset(&result, 0, sizeof(result));

    if (record->status != TRANSFER_NONE) {
        result.send_ms = metrics_elapsed_ms(&record->first_byte_time, &record->end_time);
        result.server_ms = metrics_elapsed_ms(&record->request_time, &record->end_time);

        if (metrics_bandwidth_kbps(record->bytes_sent, result.send_ms, &result.bandwidth_kbps) == 0) {
            result.has_bandwidth = 1;
        }

        if (record->status == TRANSFER_COMPLETE && metrics->has_rtt) {
            result.delay_ms = metrics_end_to_end_delay_ms(result.server_ms, metrics->rtt_ms);
            result.has_delay = 1;
        }

        if (record->status == TRANSFER_COMPLETE) {
            metrics->objects++;
        } else {
            metrics->interrupted++;
        }

        metrics->total_bytes += record->bytes_sent;
        metrics->last_status = record->status;

        if (result.has_bandwidth) {
            metrics->has_bandwidth = 1;
            metrics->bandwidth_kbps = result.bandwidth_kbps;
        }

        if (result.has_delay) {
            metrics->has_delay = 1;
            metrics->delay_ms = result.delay_ms;
        }
    }

    if (out != NULL) {
        *out = result;
    }
}

const char *metrics_status_text(transfer_status status)
{
    switch (status) {
    case TRANSFER_COMPLETE:
        return "completo";
    case TRANSFER_INTERRUPTED:
        return "interrompido";
    default:
        return "n/d";
    }
}

void metrics_format(char *buf, size_t size, int available, double value, int decimals, const char *unit)
{
    if (!available) {
        snprintf(buf, size, "n/d");
    } else if (unit != NULL && *unit != '\0') {
        snprintf(buf, size, "%.*f %s", decimals, value, unit);
    } else {
        snprintf(buf, size, "%.*f", decimals, value);
    }
}
