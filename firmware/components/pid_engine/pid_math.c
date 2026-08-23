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

uint8_t pid_math_throttle_pct(uint8_t a)
{
    return (uint8_t)(((uint32_t)a * 100) / 255);
}

uint32_t pid_math_fuel_rail_pressure_kpa(uint8_t a, uint8_t b)
{
    return 10u * ((uint32_t)a * 256 + b);
}

float pid_math_fuel_rate_lph(uint8_t a, uint8_t b)
{
    return ((float)a * 256 + b) / 20.0f;
}

int pid_math_mil_on(uint8_t a)
{
    return (a & 0x80) != 0;
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

int pid_math_decode_dtc(uint8_t hi, uint8_t lo, char out[6])
{
    if (hi == 0 && lo == 0) {
        return 0; // "0000" es relleno de "sin codigo", no un DTC real
    }
    static const char sys_chars[4] = { 'P', 'C', 'B', 'U' };
    static const char hex_digits[16] = "0123456789ABCDEF";

    out[0] = sys_chars[(hi >> 6) & 0x03];
    out[1] = hex_digits[(hi >> 4) & 0x03]; // solo 0-3 segun el estandar
    out[2] = hex_digits[hi & 0x0F];
    out[3] = hex_digits[(lo >> 4) & 0x0F];
    out[4] = hex_digits[lo & 0x0F];
    out[5] = '\0';
    return 1;
}

int pid_math_parse_dtc_list(const uint8_t *bytes, int n, char out_codes[][6], int max_codes)
{
    if (n <= 0) return 0;
    int i = (bytes[0] == 0x43 || bytes[0] == 0x47) ? 1 : 0; // saltar header modo 03/07 si esta
    int count = 0;
    for (; i + 1 < n && count < max_codes; i += 2) {
        if (pid_math_decode_dtc(bytes[i], bytes[i + 1], out_codes[count])) {
            count++;
        }
    }
    return count;
}
