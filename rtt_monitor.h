#ifndef RTT_MONITOR_H
#define RTT_MONITOR_H

#include <time.h>

void rtt_register_html(const char *experiment_id,
                       const struct timespec *timestamp);

void rtt_register_object(const char *experiment_id,
                         const struct timespec *timestamp);

#endif