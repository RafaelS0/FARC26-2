#include "config.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>

_Static_assert(UINT_MAX == 4294967295U,
               "a mensagem de erro de limite assume unsigned int de 32 bits");

static unsigned int max_capacity_kbps = 0;

static int all_digits(const char *s)
{
    if (*s == '\0') {
        return 0;
    }

    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s)) {
            return 0;
        }
    }

    return 1;
}

int config_parse_capacity(const char *text, unsigned int *kbps, const char **error)
{
    const char *ignored;

    if (error == NULL) {
        error = &ignored;
    }

    if (text == NULL || kbps == NULL) {
        *error = "argumento ausente";
        return -1;
    }

    if (*text == '\0') {
        *error = "valor vazio";
        return -1;
    }

    if (text[0] == '-' && all_digits(text + 1)) {
        *error = "a vazão não pode ser negativa";
        return -1;
    }

    if (!all_digits(text)) {
        *error = "use apenas um número inteiro positivo, em kbps "
                 "(sem sinal, espaços, casas decimais ou unidade)";
        return -1;
    }

    unsigned long long value = 0;

    for (const char *p = text; *p != '\0'; p++) {
        value = value * 10 + (unsigned long long)(*p - '0');

        if (value > UINT_MAX) {
            *error = "valor acima do máximo suportado (4294967295 kbps)";
            return -1;
        }
    }

    if (value == 0) {
        *error = "a vazão deve ser maior que zero";
        return -1;
    }

    *kbps = (unsigned int)value;
    return 0;
}

void config_print_usage(FILE *out, const char *program)
{
    fprintf(out,
            "Uso: %s <vazao_maxima_kbps>\n"
            "\n"
            "  vazao_maxima_kbps  vazão total máxima do servidor, em kbps\n"
            "                     (quilobits por segundo). Inteiro maior que zero.\n"
            "\n"
            "Exemplo: %s 10000    (servidor com 10 Mbps)\n",
            program, program);
}

int config_load_args(int argc, char *argv[])
{
    const char *program = (argc > 0 && argv[0] != NULL) ? argv[0] : "server";

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        config_print_usage(stdout, program);
        return 1;
    }

    if (argc != 2) {
        fprintf(stderr,
                "Erro: informe exatamente um parâmetro, a vazão máxima do servidor (recebidos %d).\n\n",
                argc > 0 ? argc - 1 : 0);
        config_print_usage(stderr, program);
        return -1;
    }

    unsigned int kbps;
    const char *error;

    if (config_parse_capacity(argv[1], &kbps, &error) != 0) {
        fprintf(stderr, "Erro: vazão máxima inválida (\"%s\"): %s.\n\n", argv[1], error);
        config_print_usage(stderr, program);
        return -1;
    }

    config_set_max_capacity_kbps(kbps);
    return 0;
}

void config_set_max_capacity_kbps(unsigned int kbps)
{
    max_capacity_kbps = kbps;
}

unsigned int config_get_max_capacity_kbps(void)
{
    return max_capacity_kbps;
}
