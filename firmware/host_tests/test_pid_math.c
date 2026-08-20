/**
 * Test de host para pid_math (sin ESP-IDF, sin hardware).
 *
 * Compilar y correr:
 *   gcc -I../components/pid_engine/include -o test_pid_math \
 *       test_pid_math.c ../components/pid_engine/pid_math.c
 *   ./test_pid_math
 *
 * Este es el tipo de test que SÍ podemos tener corriendo en CI (GitHub Actions)
 * desde el día 1, sin necesitar un runner con ESP32 conectado.
 */
#include "pid_math.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;

#define ASSERT_EQ_INT(desc, expected, actual) do { \
    long e = (long)(expected), a = (long)(actual); \
    if (e != a) { \
        printf("FAIL: %s -> esperado %ld, obtuvo %ld\n", desc, e, a); \
        g_failures++; \
    } else { \
        printf("OK:   %s\n", desc); \
    } \
} while (0)

int main(void)
{
    // RPM: PID 0x0C, ejemplo real de la spec SAE J1979: A=0x1A B=0xF8 -> ((26*256)+248)/4 = 1726
    ASSERT_EQ_INT("RPM 0x1A 0xF8 -> 1726", 1726, pid_math_rpm(0x1A, 0xF8));
    ASSERT_EQ_INT("RPM 0x00 0x00 -> 0 (motor apagado)", 0, pid_math_rpm(0x00, 0x00));
    ASSERT_EQ_INT("RPM 0xFF 0xFF -> 16383 (max teorico)", 16383, pid_math_rpm(0xFF, 0xFF));

    // Velocidad: PID 0x0D, 1 byte directo en km/h
    ASSERT_EQ_INT("Velocidad 0x00 -> 0 km/h", 0, pid_math_speed_kmh(0x00));
    ASSERT_EQ_INT("Velocidad 0x64 -> 100 km/h", 100, pid_math_speed_kmh(0x64));

    // Temperatura: PID 0x05 / 0x0F, A-40
    ASSERT_EQ_INT("Temp 0x00 -> -40 C", -40, pid_math_temp_c(0x00));
    ASSERT_EQ_INT("Temp 0x28 -> 0 C", 0, pid_math_temp_c(0x28));
    ASSERT_EQ_INT("Temp 0x5A -> 50 C (motor a temperatura normal)", 50, pid_math_temp_c(0x5A));

    // Carga motor: PID 0x04, A*100/255
    ASSERT_EQ_INT("Carga 0x00 -> 0%", 0, pid_math_engine_load_pct(0x00));
    ASSERT_EQ_INT("Carga 0xFF -> 100%", 100, pid_math_engine_load_pct(0xFF));
    ASSERT_EQ_INT("Carga 0x80 -> 50%", 50, pid_math_engine_load_pct(0x80));

    // MAP: PID 0x0B, 1 byte directo en kPa
    ASSERT_EQ_INT("MAP 0x65 -> 101 kPa (~atmosferica)", 101, pid_math_map_kpa(0x65));

    // Acelerador: PID 0x11, misma formula que carga motor
    ASSERT_EQ_INT("Acelerador 0x00 -> 0%", 0, pid_math_throttle_pct(0x00));
    ASSERT_EQ_INT("Acelerador 0xFF -> 100%", 100, pid_math_throttle_pct(0xFF));

    // Presion de riel (diesel common-rail): PID 0x23, 10*((A*256)+B) kPa
    ASSERT_EQ_INT("Riel 0x00 0x00 -> 0 kPa", 0, pid_math_fuel_rail_pressure_kpa(0x00, 0x00));
    // A=0x4E B=0x20 -> (78*256+32)=20000*10=200000 kPa (~2000 bar, tipico de un common-rail moderno)
    ASSERT_EQ_INT("Riel 0x4E 0x20 -> 200000 kPa", 200000, pid_math_fuel_rail_pressure_kpa(0x4E, 0x20));

    // Caudal de combustible: PID 0x5E, ((A*256)+B)/20 L/h
    {
        float lph = pid_math_fuel_rate_lph(0x00, 0x64); // 100/20 = 5.0
        if (fabsf(lph - 5.0f) > 0.01f) {
            printf("FAIL: fuel_rate_lph valor -> esperado 5.0, obtuvo %f\n", lph);
            g_failures++;
        } else {
            printf("OK:   fuel_rate_lph 0x00 0x64 == 5.0\n");
        }
    }

    // Check engine (MIL): PID 0x01, bit 7 de A
    ASSERT_EQ_INT("MIL bit7=1 -> encendido", 1, pid_math_mil_on(0x80));
    ASSERT_EQ_INT("MIL bit7=0 -> apagado", 0, pid_math_mil_on(0x07));

    // Parseo de texto hex ASCII -> bytes (respuesta ELM327 típica)
    {
        uint8_t bytes[8];
        int n = pid_math_ascii_hex_to_bytes("41 0C 1A F8", bytes, sizeof(bytes));
        ASSERT_EQ_INT("ascii_hex_to_bytes longitud", 4, n);
        ASSERT_EQ_INT("ascii_hex_to_bytes byte[0]==0x41", 0x41, bytes[0]);
        ASSERT_EQ_INT("ascii_hex_to_bytes byte[1]==0x0C", 0x0C, bytes[1]);
        ASSERT_EQ_INT("ascii_hex_to_bytes byte[2]==0x1A", 0x1A, bytes[2]);
        ASSERT_EQ_INT("ascii_hex_to_bytes byte[3]==0xF8", 0xF8, bytes[3]);
    }
    {
        // Con el prompt '>' pegado al final, como llega realmente del adaptador
        uint8_t bytes[8];
        int n = pid_math_ascii_hex_to_bytes("41 0D 32\r\r>", bytes, sizeof(bytes));
        ASSERT_EQ_INT("ascii_hex_to_bytes corta en '>'", 3, n);
    }
    {
        // Texto invalido (byte incompleto) debe devolver -1, no reventar
        uint8_t bytes[8];
        int n = pid_math_ascii_hex_to_bytes("41 0", bytes, sizeof(bytes));
        ASSERT_EQ_INT("ascii_hex_to_bytes byte incompleto -> -1", -1, n);
    }

    // Voltaje de bateria (respuesta de "ATRV")
    {
        float volts = 0;
        int ok = pid_math_parse_atrv("12.6V", &volts);
        ASSERT_EQ_INT("parse_atrv OK", 1, ok);
        if (fabsf(volts - 12.6f) > 0.01f) {
            printf("FAIL: parse_atrv valor -> esperado 12.6, obtuvo %f\n", volts);
            g_failures++;
        } else {
            printf("OK:   parse_atrv valor == 12.6\n");
        }
    }
    {
        float volts = 0;
        int ok = pid_math_parse_atrv("basura", &volts);
        ASSERT_EQ_INT("parse_atrv texto invalido -> 0", 0, ok);
    }

    printf("\n%s: %d fallas\n", g_failures == 0 ? "TODO OK" : "HAY FALLAS", g_failures);
    return g_failures == 0 ? 0 : 1;
}
