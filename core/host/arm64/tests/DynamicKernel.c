#include <stdarg.h>

extern void* malloc(unsigned long size);
extern void free(void* address);
extern char* strcpy(char* destination, const char* source);
extern unsigned long strlen(const char* string);
extern void* memmove(void* destination, const void* source, unsigned long size);
extern unsigned long long strtoull(const char* string, char** end, int base);
extern double strtod(const char* string, char** end);
extern int vsnprintf(char* output, unsigned long size, const char* format, va_list arguments);
extern int snprintf(char* output, unsigned long size, const char* format, ...);
extern double pow(double left, double right);
extern double exp2(double value);
extern double scalbn(double value, int exponent);
extern float cosf(float value);
extern long lround(double value);

unsigned long initialized;

void Initialize(void) {
    initialized = 13;
}

int Format(char* output, unsigned long size, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(output, size, format, arguments);
    va_end(arguments);
    return result;
}

unsigned long GuestMain(void) {
    if (initialized != 13) return 1;
    char* bytes = malloc(64);
    if (!bytes) return 2;
    strcpy(bytes, "native");
    if (strlen(bytes) != 6) return 3;
    memmove(bytes + 1, bytes, 6);
    bytes[7] = 0;
    if (bytes[0] != 'n' || bytes[1] != 'n' || bytes[6] != 'e') return 4;
    char* end;
    unsigned long value = strtoull("29x", &end, 10);
    if (*end != 'x') return 5;
    union {
        double value;
        unsigned long bits;
    } floating;
    floating.value = strtod("16.25", 0);
    if (floating.bits != 0x4030400000000000UL) return 6;
    floating.value = pow(2.0, 5.0);
    if (floating.bits != 0x4040000000000000UL) return 10;
    floating.value = exp2(-3.0);
    if (floating.bits != 0x3fc0000000000000UL) return 11;
    floating.value = scalbn(1.5, 3);
    if (floating.bits != 0x4028000000000000UL || lround(-2.5) != -3) return 12;
    union { float value; unsigned int bits; } narrow;
    narrow.value = cosf(0.0f);
    if (narrow.bits != 0x3f800000U) return 13;
    char formatted[64];
    int printed = 0;
    if (Format(formatted, sizeof(formatted), "%s %u %.2f %llx%n", "M5", 29, 16.25, 0x123ULL, &printed) != 15) return 7;
    if (printed != 15 || formatted[0] != 'M' || formatted[14] != '3' || formatted[15] != 0) return 8;
    if (snprintf(formatted, sizeof(formatted), "%04x", 42) != 4 || formatted[3] != 'a') return 9;
    free(bytes);
    return value + initialized;
}
