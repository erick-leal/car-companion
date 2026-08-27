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

    // Decodificar DTC: P0301 = fallo de encendido cilindro 1 (ejemplo real y muy comun)
    // hi=0x03 -> sistema P (bits7:6=00), digito1=0 (bits5:4=00), digito2=3 (bits3:0=0011)
    // lo=0x01 -> digito3=0, digito4=1
    {
        char code[6];
        int ok = pid_math_decode_dtc(0x03, 0x01, code);
        ASSERT_EQ_INT("decode_dtc devuelve 1 para codigo real", 1, ok);
        if (strcmp(code, "P0301") != 0) {
            printf("FAIL: decode_dtc 0x03 0x01 -> esperado P0301, obtuvo %s\n", code);
            g_failures++;
        } else {
            printf("OK:   decode_dtc 0x03 0x01 == P0301\n");
        }
    }
    // C1201, B0001, U0100 — un codigo de cada sistema, para confirmar los 4 prefijos
    {
        char code[6];
        pid_math_decode_dtc(0x52, 0x01, code); // hi=0101 0010 -> C(01) digito1=1(01) digito2=2(0010)
        if (strcmp(code, "C1201") != 0) { printf("FAIL: decode_dtc C -> %s\n", code); g_failures++; }
        else printf("OK:   decode_dtc sistema C -> C1201\n");

        pid_math_decode_dtc(0x80, 0x01, code); // hi=1000 0000 -> B(10) digito1=0 digito2=0
        if (strcmp(code, "B0001") != 0) { printf("FAIL: decode_dtc B -> %s\n", code); g_failures++; }
        else printf("OK:   decode_dtc sistema B -> B0001\n");

        pid_math_decode_dtc(0xC1, 0x00, code); // hi=1100 0001 -> U(11) digito1=0 digito2=1
        if (strcmp(code, "U0100") != 0) { printf("FAIL: decode_dtc U -> %s\n", code); g_failures++; }
        else printf("OK:   decode_dtc sistema U -> U0100\n");
    }
    ASSERT_EQ_INT("decode_dtc 0x00 0x00 (relleno) -> 0", 0, pid_math_decode_dtc(0, 0, (char[6]){0}));

    // Lista completa: respuesta tipica de "43 03 01 00 00 01 71" (header 43,
    // codigo P0301, relleno 0000, codigo P0171 -> 2 codigos reales)
    {
        uint8_t bytes[8];
        int n = pid_math_ascii_hex_to_bytes("43 03 01 00 00 01 71", bytes, sizeof(bytes));
        ASSERT_EQ_INT("dtc_list ascii_hex_to_bytes longitud", 7, n);

        char codes[8][6];
        int count = pid_math_parse_dtc_list(bytes, n, codes, 8);
        ASSERT_EQ_INT("dtc_list cantidad de codigos reales", 2, count);
        if (count == 2) {
            if (strcmp(codes[0], "P0301") != 0) { printf("FAIL: dtc_list[0] -> %s\n", codes[0]); g_failures++; }
            else printf("OK:   dtc_list[0] == P0301\n");
            if (strcmp(codes[1], "P0171") != 0) { printf("FAIL: dtc_list[1] -> %s\n", codes[1]); g_failures++; }
            else printf("OK:   dtc_list[1] == P0171\n");
        }
    }
    // Sin fallas: "43 00 00" -> 0 codigos
    {
        uint8_t bytes[8];
        int n = pid_math_ascii_hex_to_bytes("43 00 00", bytes, sizeof(bytes));
        char codes[8][6];
        int count = pid_math_parse_dtc_list(bytes, n, codes, 8);
        ASSERT_EQ_INT("dtc_list sin fallas -> 0 codigos", 0, count);
    }

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

    // strip_frame_prefixes: ELM327 agrega "N:" (indice de linea) en respuestas
    // multi-frame como el VIN -- hay que sacarlo antes de ascii_hex_to_bytes.
    {
        char text[128];
        strcpy(text, "0:49 02 01 31 48 47 43\r1:4D 38 32 36 33 33 41 30\r2:30 34 33 35 32\r>");
        pid_math_strip_frame_prefixes(text);
        int ok = strcmp(text, "49 02 01 31 48 47 43\r4D 38 32 36 33 33 41 30\r30 34 33 35 32\r>") == 0;
        ASSERT_EQ_INT("strip_frame_prefixes saca los indices de linea", 1, ok);
    }
    {
        char text[32];
        strcpy(text, "41 0C 1A F8"); // respuesta normal (un solo frame), no debe tocarse
        pid_math_strip_frame_prefixes(text);
        int ok = strcmp(text, "41 0C 1A F8") == 0;
        ASSERT_EQ_INT("strip_frame_prefixes sin prefijos no cambia nada", 1, ok);
    }

    // VIN (modo 09 PID 02): respuesta multi-frame real reconstruida a mano,
    // VIN de ejemplo "1HGCM82633A004352" (formato SAE J1979 valido, 17 chars)
    {
        char text[128];
        strcpy(text, "0:49 02 01 31 48 47 43\r1:4D 38 32 36 33 33 41 30\r2:30 34 33 35 32\r>");
        pid_math_strip_frame_prefixes(text);
        uint8_t bytes[32];
        int n = pid_math_ascii_hex_to_bytes(text, bytes, sizeof(bytes));
        char vin[18];
        int ok = pid_math_parse_vin(bytes, n, vin);
        ASSERT_EQ_INT("parse_vin junta el VIN completo", 1, ok);
        int match = strcmp(vin, "1HGCM82633A004352") == 0;
        ASSERT_EQ_INT("parse_vin VIN == 1HGCM82633A004352", 1, match);
    }
    {
        // Respuesta incompleta (adaptador/vehiculo no soporta modo 09, o se
        // corto en el medio) -> 0, no un VIN a medias
        uint8_t bytes[8] = {0x49, 0x02, 0x01, 0x31, 0x48};
        char vin[18];
        int ok = pid_math_parse_vin(bytes, 5, vin);
        ASSERT_EQ_INT("parse_vin respuesta incompleta -> 0", 0, ok);
    }

    printf("\n%s: %d fallas\n", g_failures == 0 ? "TODO OK" : "HAY FALLAS", g_failures);
    return g_failures == 0 ? 0 : 1;
}
