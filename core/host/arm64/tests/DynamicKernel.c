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
extern void* aligned_alloc(unsigned long alignment, unsigned long size);
extern int sscanf(const char* input, const char* format, ...);
extern int vsscanf(const char* input, const char* format, va_list arguments);
extern void qsort(void* base, unsigned long count, unsigned long size, int (*compare)(const void*, const void*));
extern void* bsearch(const void* key, const void* base, unsigned long count, unsigned long size, int (*compare)(const void*, const void*));
extern int getargc(void);
extern const char** getargv(void);
extern int setenv(const char* name, const char* value, int overwrite);
extern char* getenv(const char* name);
extern int unsetenv(const char* name);
extern char* getcwd(char* output, unsigned long size);
extern long sysconf(int name);
extern long random(void);
extern void srandom(unsigned int seed);
extern char* setlocale(int category, const char* name);
extern unsigned long mbrtowc(unsigned int* output, const char* input, unsigned long count, void* state);
extern int __mb_cur_max;

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

int Scan(const char* input, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vsscanf(input, format, arguments);
    va_end(arguments);
    return result;
}

int Compare(const void* left, const void* right) {
    return *(const int*)left - *(const int*)right;
}

int Posix(void) {
    const char** arguments = getargv();
    if ((getargc() != 1 && getargc() != 3) || !arguments || !arguments[0] || strlen(arguments[0]) == 0 || arguments[getargc()] != 0) return 14;
    if (getargc() == 3 && (arguments[1][0] != 'a' || arguments[2][5] != ' ')) return 15;
    if (setenv("ANYPS5_DYNAMIC_ENV", "guest", 1) != 0) return 16;
    const char* environment = getenv("ANYPS5_DYNAMIC_ENV");
    if (!environment || environment[0] != 'g' || unsetenv("ANYPS5_DYNAMIC_ENV") != 0 || getenv("ANYPS5_DYNAMIC_ENV") != 0) return 17;
    char* directory = getcwd(0, 0);
    if (!directory || strlen(directory) == 0 || sysconf(47) != 16384) return 18;
    free(directory);
    void* aligned = aligned_alloc(64, 128);
    if (!aligned || ((unsigned long)aligned & 63) != 0) return 19;
    free(aligned);
    int numbers[5] = {9, -2, 5, 0, 3};
    qsort(numbers, 5, sizeof(int), Compare);
    int key = 5;
    if (numbers[0] != -2 || numbers[4] != 9 || bsearch(&key, numbers, 5, sizeof(int), Compare) != &numbers[3]) return 20;
    int a, b, c, d, e, f, count = 0;
    if (sscanf("1 2 3 4 5 6", "%d%d%d%d%d%d%n", &a, &b, &c, &d, &e, &f, &count) != 6 || a != 1 || f != 6 || count != 11) return 21;
    if (Scan("7 8 9 10 11 12", "%d%d%d%d%d%d", &a, &b, &c, &d, &e, &f) != 6 || a != 7 || f != 12) return 22;
    union { double value; unsigned long bits; } floating;
    if (sscanf("1.25", "%lf", &floating.value) != 1 || floating.bits != 0x3ff4000000000000UL) return 23;
    if (!setlocale(2, "en_US.UTF-8") || __mb_cur_max < 4) return 24;
    unsigned long state[16] = {0};
    unsigned int character = 0;
    if (mbrtowc(&character, "\xe2", 1, state) != (unsigned long)-2 || mbrtowc(&character, "\x82\xac", 2, state) != 2 || character != 0x20ac) return 25;
    if (sscanf("\xf0\x9f\x99\x82", "%lc", &character) != 1 || character != 0x1f642) return 26;
    srandom(0);
    if (random() != 577655601 || random() != 1248161417) return 27;
    srandom(123);
    if (random() != 128959393 || random() != 1692901013) return 28;
    return 0;
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
    int posix = Posix();
    if (posix != 0) return (unsigned long)posix;
    return value + initialized;
}
