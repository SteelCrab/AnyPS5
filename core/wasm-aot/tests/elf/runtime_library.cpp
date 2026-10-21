extern "C" {
unsigned long guest_strlen(const char*) __asm__("j4ViWNHEgww");
int guest_abs(int) __asm__("Ye20uNnlglA");
void guest_qsort(void*, unsigned long, unsigned long, int (*)(const void*, const void*)) __asm__("AEJdIVZTEmo");
int guest_puts(const char*) __asm__("YQ0navp+YIc");
extern int guest_isthreaded __asm__("q1OvUam0BJU");
}

__asm__(".type q1OvUam0BJU,@object");

namespace {

long initialized;
thread_local long counter = 5;

struct Initializer {
    Initializer() { initialized = guest_abs(-1000); }
} initializer;

int Compare(const void* first, const void* second) {
    const auto a = *static_cast<const long*>(first);
    const auto b = *static_cast<const long*>(second);
    return (a > b) - (a < b);
}

}  // namespace

extern "C" {

long run_initialized() { return initialized; }
long run_imports(long value) { return guest_strlen("runtime") + guest_abs(-static_cast<int>(value)); }
long run_callback() {
    long values[] = {9, 3, 7, -4};
    guest_qsort(values, 4, sizeof(long), Compare);
    return values[0] * 1000 + values[1] * 100 + values[2] * 10 + values[3];
}
long run_tls() { return ++counter; }
long run_data() { return guest_isthreaded; }
long run_print() { return guest_puts("WASM_RUNTIME_SMOKE_PASS"); }

}
