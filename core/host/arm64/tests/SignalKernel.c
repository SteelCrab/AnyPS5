typedef struct { unsigned bits[4]; } SignalSet;
typedef void (*Handler)(int);
typedef struct { Handler handler; unsigned flags; SignalSet mask; } Action;
extern int sigemptyset(SignalSet* set);
extern int sigaddset(SignalSet* set, int signal);
extern int sigismember(const SignalSet* set, int signal);
extern int sigpending(SignalSet* set);
extern int sigaction(int signal, const Action* action, Action* previous);
extern Handler signal(int value, Handler handler);
extern int raise(int signal);
extern int kill(int pid, int signal);
extern int getpid(void);
extern int pthread_sigmask(int how, const SignalSet* mask, SignalSet* previous);
extern int pthread_kill(unsigned long thread, int signal);
extern int pthread_create(unsigned long* thread, const void* attributes, void* (*function)(void*), void* argument);
extern int pthread_join(unsigned long thread, void** result);
extern unsigned long pthread_self(void);
extern int pthread_getschedparam(unsigned long thread, int* policy, int* priority);
extern int pthread_setschedparam(unsigned long thread, int policy, const int* priority);
extern int sched_get_priority_min(int policy);
extern int sched_get_priority_max(int policy);
extern int sem_init(unsigned long* semaphore, int shared, unsigned count);
extern int sem_wait(unsigned long* semaphore);
extern int sem_post(unsigned long* semaphore);
extern int getargc(void);
extern const char** getargv(void);
extern void _exit(int status);
extern void abort(void);
extern long write(int descriptor, const void* bytes, unsigned long length);
extern int printf(const char* format, ...);

volatile unsigned count, depth, maximumDepth, repeat, trace;
unsigned long ready, release;

void Check(int value) { if (!value) _exit(99); }

void Catch(int value) {
    Check(value == 30);
    ++count;
    ++depth;
    if (depth > maximumDepth) maximumDepth = depth;
    if (repeat && count == 1) Check(raise(30) == 0);
    --depth;
}

void Secondary(int value) {
    Check(value == 31 && trace == 2);
    trace = 3;
}

void Masked(int value) {
    Check(value == 30 && trace == 0);
    trace = 1;
    Check(raise(31) == 0 && trace == 1);
    trace = 2;
}

void AbortHandler(int value) {
    Check(value == 6);
    Check(write(1, "caught-abort", 12) == 12);
}

void ExitHandler(int value) {
    Check(value == 6);
    _exit(37);
}

void* Worker(void* argument) {
    int policy, priority;
    Check(pthread_getschedparam(pthread_self(), &policy, &priority) == 0);
    Check(priority >= sched_get_priority_min(policy) && priority <= sched_get_priority_max(policy));
    Check(pthread_setschedparam(pthread_self(), policy, &priority) == 0);
    SignalSet mask, pending;
    Check(pthread_sigmask(0, 0, &mask) == 0 && sigismember(&mask, 30) == 1);
    Check(raise(30) == 0 && count == 0);
    Check(sem_post(&ready) == 0 && sem_wait(&release) == 0);
    Check(sigpending(&pending) == 0 && sigismember(&pending, 30) == 1);
    Check(pthread_sigmask(2, &mask, 0) == 0 && count == 1);
    return argument;
}

void* TerminatingWorker(void* argument) {
    (void)argument;
    raise(15);
    return 0;
}

unsigned long GuestMain(void) {
    const char mode = getargc() == 2 ? getargv()[1][0] : 'r';
    if (mode == 'k') { printf("pending-output"); kill(getpid(), 15); return 99; }
    if (mode == 't') {
        unsigned long thread;
        Check(sem_init(&ready, 0, 0) == 0 && pthread_create(&thread, 0, TerminatingWorker, 0) == 0);
        sem_wait(&ready);
        return 99;
    }
    if (mode == 'a' || mode == 'i' || mode == 'e') {
        Check(signal(6, mode == 'a' ? AbortHandler : mode == 'e' ? ExitHandler : (Handler)1) == 0);
        printf("pending-output");
        abort();
        return 99;
    }
    SignalSet set, pending;
    Action action = {Catch, 0x10, {{0, 0, 0, 0}}}, previous;
    Check(sigemptyset(&set) == 0 && sigaddset(&set, 30) == 0);
    Check(signal(30, Catch) == 0 && pthread_sigmask(1, &set, 0) == 0);
    Check(raise(30) == 0 && raise(30) == 0 && count == 0);
    Check(sigpending(&pending) == 0 && sigismember(&pending, 30) == 1);
    Check(pthread_sigmask(2, &set, 0) == 0 && count == 1);
    count = 0;
    repeat = 1;
    Check(raise(30) == 0 && count == 2 && maximumDepth == 1);
    Check(sigaction(30, &action, &previous) == 0 && previous.flags == 2 && previous.handler == Catch);
    count = maximumDepth = 0;
    Check(raise(30) == 0 && count == 2 && maximumDepth == 2);
    repeat = 0;
    action.handler = Masked;
    action.flags = 0;
    Check(sigaddset(&action.mask, 31) == 0 && sigaction(30, &action, 0) == 0 && signal(31, Secondary) == 0);
    Check(raise(30) == 0 && trace == 3);
    Check(signal(30, Catch) == Masked);
    count = 0;
    Check(pthread_sigmask(1, &set, 0) == 0 && sem_init(&ready, 0, 0) == 0 && sem_init(&release, 0, 0) == 0);
    unsigned long thread;
    Check(pthread_create(&thread, 0, Worker, (void*)42) == 0 && sem_wait(&ready) == 0);
    Check(pthread_sigmask(2, &set, 0) == 0 && count == 0);
    Check(pthread_kill(thread, 30) == 0 && count == 0 && sem_post(&release) == 0);
    void* result = 0;
    Check(pthread_join(thread, &result) == 0 && result == (void*)42 && count == 1);
    Check(pthread_kill(thread, 0) == 3);
    return 42;
}
