#ifndef CONFIG_H
#define CONFIG_H

#include <stdio.h>

int config_parse_capacity(const char *text, unsigned int *kbps, const char **error);

int config_load_args(int argc, char *argv[]);

void config_print_usage(FILE *out, const char *program);

void config_set_max_capacity_kbps(unsigned int kbps);
unsigned int config_get_max_capacity_kbps(void);

#endif
