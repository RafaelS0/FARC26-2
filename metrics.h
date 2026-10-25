#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>
#include <time.h>

#define METRICS_MIN_DURATION_MS 1.0

typedef enum {
    TRANSFER_NONE = 0,
    TRANSFER_COMPLETE,
    TRANSFER_INTERRUPTED
} transfer_status;

typedef struct {
    struct timespec request_time;
    struct timespec first_byte_time;
    struct timespec end_time;
    size_t object_size;
    size_t bytes_sent;
    transfer_status status;
} transfer_record;

typedef struct {
    double send_ms;
    double server_ms;
    int has_bandwidth;
    double bandwidth_kbps;
    int has_delay;
    double delay_ms;
} transfer_metrics;

typedef struct {
    int has_rtt;
    double rtt_ms;
    int has_delay;
    double delay_ms;
    int has_bandwidth;
    double bandwidth_kbps;
    unsigned long objects;
    unsigned long interrupted;
    unsigned long long total_bytes;
    transfer_status last_status;
} client_metrics;

double metrics_elapsed_ms(const struct timespec *start, const struct timespec *end);
int metrics_bandwidth_kbps(size_t bytes, double duration_ms, double *kbps);
double metrics_end_to_end_delay_ms(double server_ms, double rtt_ms);

int metrics_sample_rtt_ms(int socket, double *rtt_ms);
int metrics_update_rtt(client_metrics *metrics, int socket);

void metrics_apply_transfer(client_metrics *metrics, const transfer_record *record, transfer_metrics *out);

const char *metrics_status_text(transfer_status status);
void metrics_format(char *buf, size_t size, int available, double value, int decimals, const char *unit);

#endif
