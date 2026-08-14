#include "pid_math.h"
#include <ctype.h>
#include <stdlib.h>

int pid_math_ascii_hex_to_bytes(const char *ascii, uint8_t *out, size_t out_max)
{
    size_t out_len = 0;
    size_t i = 0;
    size_t in_len = 0;
    while (ascii[in_len] != '\0') in_len++; // strlen sin incluir string.h aca

    while (i < in_len && out_len < out_max) {
        while (i < in_len && isspace((unsigned char)ascii[i])) i++;
        if (i >= in_len || ascii[i] == '>') break;
        if (i + 1 >= in_len) return -1;

        char hi = ascii[i];
        char lo = ascii[i + 1];
        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo)) {
            return -1;
        }
        char byte_str[3] = { hi, lo, '\0' };
        out[out_len++] = (uint8_t)strtol(byte_str, NULL, 16);
        i += 2;
    }
    return (int)out_len;
}

uint16_t pid_math_rpm(uint8_t a, uint8_t b)
{
    return (uint16_t)(((uint16_t)a * 256 + b) / 4);
}

uint8_t pid_math_speed_kmh(uint8_t a)
{
    return a;
}

int16_t pid_math_temp_c(uint8_t a)
{
    return (int16_t)a - 40;
}

uint8_t pid_math_engine_load_pct(uint8_t a)
{
    return (uint8_t)(((uint32_t)a * 100) / 255);
}

uint8_t pid_math_map_kpa(uint8_t a)
{
    return a;
}

int pid_math_parse_atrv(const char *text, float *out_volts)
{
    char *endptr = NULL;
    float volts = strtof(text, &endptr);
    if (endptr == text) {
        return 0;
    }
    *out_volts = volts;
    return 1;
}
