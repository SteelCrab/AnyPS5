extern int printf(const char* format, ...);
extern int atexit(void (*function)(void));
extern int __cxa_atexit(void (*function)(void*), void* argument, void* dso);
extern void __cxa_finalize(void* dso);
extern void exit(int status);
extern void _Exit(int status);
extern void _exit(int status);
extern void abort(void);
extern int getargc(void);
extern const char** getargv(void);
extern int sysctlbyname(const char* name, void* output, unsigned long* size, const void* replacement, unsigned long replacementSize);
extern int sysctl(const int* name, unsigned count, void* output, unsigned long* size, const void* replacement, unsigned long replacementSize);
extern int pthread_create(unsigned long* thread, const void* attributes, void* (*function)(void*), void* argument);
extern int sem_init(unsigned long* semaphore, int shared, unsigned count);
extern int sem_wait(unsigned long* semaphore);
extern unsigned sleep(unsigned seconds);

unsigned trace;
unsigned initialized;
unsigned anchor;
unsigned long semaphore;

void Last(void) {
    if (trace != 7) _exit(99);
    trace |= 8;
    printf(" A");
}

void Destructor(void* argument) {
    if (argument != &anchor || (trace & 4)) _exit(99);
    trace |= 4;
    printf(" B");
}

void Added(void) {
    trace |= 2;
    printf(" D");
}

void First(void) {
    trace |= 1;
    printf(" C");
    if (atexit(Added) != 0) _exit(99);
}

void Initialize(void) {
    initialized = 1;
    if (atexit(Last) != 0 || __cxa_atexit(Destructor, &anchor, &anchor) != 0 || atexit(First) != 0) _exit(99);
}

void Finalize(void) {
    if (trace != 15) _exit(98);
    printf(" F");
}

void* Worker(void* argument) {
    if (argument == &anchor) exit(9);
    _Exit(11);
    return 0;
}

unsigned long GuestMain(void) {
    if (!initialized) _Exit(97);
    int cores = 0, numeric = 0;
    int mib[2] = {6, 3};
    unsigned long size = sizeof(cores);
    if (sysctlbyname("hw.ncpu", &cores, &size, 0, 0) != 0 || size != sizeof(cores) || cores <= 0) _Exit(96);
    size = sizeof(numeric);
    if (sysctl(mib, 2, &numeric, &size, 0, 0) != 0 || numeric != cores) _Exit(96);
    const char mode = getargc() == 2 ? getargv()[1][0] : 'r';
    if (mode == 'd') {
        __cxa_finalize(&anchor);
        __cxa_finalize(&anchor);
    }
    printf(" body");
    if (mode == 'e') exit(259);
    if (mode == 'q') _Exit(5);
    if (mode == 'x') _exit(7);
    if (mode == 'a') abort();
    if (mode == 'w' || mode == 'i' || mode == 's') {
        unsigned long thread = 0;
        if (sem_init(&semaphore, 0, 0) != 0 || pthread_create(&thread, 0, Worker, mode == 'i' ? 0 : &anchor) != 0) _Exit(95);
        if (mode == 's') sleep(60);
        sem_wait(&semaphore);
        _Exit(94);
    }
    return 42;
}
