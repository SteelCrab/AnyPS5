typedef unsigned long Word;
extern int pthread_create(Word*, const Word*, void* (*)(void*), void*);
extern int pthread_join(Word, void**);
extern int pthread_attr_init(Word*);
extern int pthread_attr_setdetachstate(Word*, int);
extern int pthread_attr_destroy(Word*);
extern Word pthread_self(void);
extern int pthread_once(void*, void (*)(void));
extern int pthread_key_create(int*, void (*)(void*));
extern int pthread_key_delete(int);
extern int pthread_setspecific(int, const void*);
extern void* pthread_getspecific(int);
extern int pthread_mutex_lock(Word*);
extern int pthread_mutex_unlock(Word*);
extern int pthread_mutex_destroy(Word*);
extern int pthread_cond_wait(Word*, Word*);
extern int pthread_cond_broadcast(Word*);
extern int pthread_cond_destroy(Word*);
extern int pthread_rwlock_wrlock(Word*);
extern int pthread_rwlock_unlock(Word*);
extern int pthread_rwlock_destroy(Word*);
extern int sem_init(Word*, int, unsigned);
extern int sem_post(Word*);
extern int sem_wait(Word*);
extern int sem_destroy(Word*);
extern void* malloc(Word);
extern void free(void*);
extern int* __error(void);

static Word mutex, condition, rwlock, semaphore;
static struct { int state; Word mutex; } once, nestedOnce;
static int ready, counter, initialized, destructors, destructorSum, key;

static void NestedInitialize(void) { ++initialized; }
static void DestroyKey(void* pointer) {
    Word value = (Word)pointer;
    if (pthread_getspecific(key) != (void*)0) return;
    pthread_rwlock_wrlock(&rwlock);
    ++destructors;
    destructorSum += (int)value;
    pthread_rwlock_unlock(&rwlock);
    if (value == 4) pthread_setspecific(key, (void*)68);
}
static void Initialize(void) {
    pthread_once(&nestedOnce, NestedInitialize);
    pthread_key_create(&key, DestroyKey);
}

static void* Worker(void* pointer) {
    Word value = (Word)pointer;
#if defined(THREAD_FAILURE) || defined(THREAD_DETACHED_FAILURE)
    volatile long zero = 0;
    return (void*)(Word)(21 / zero);
#elif defined(THREAD_STOP)
    sem_wait(&semaphore);
    return pointer;
#else
    if (pthread_self() == 0 || pthread_once(&once, Initialize) != 0) return (void*)1;
    pthread_setspecific(key, pointer);
    int* error = __error();
    *error = (int)value + 70;
    for (int i = 0; i < 200; ++i) {
        unsigned char* data = (unsigned char*)malloc(17);
        if (data == (void*)0) return (void*)2;
        data[0] = (unsigned char)value;
        data[16] = (unsigned char)(value + 2);
        if (data[0] != value || data[16] != value + 2) return (void*)3;
        free(data);
        pthread_mutex_lock(&mutex);
        ++counter;
        pthread_mutex_unlock(&mutex);
    }
    pthread_mutex_lock(&mutex);
    ++ready;
    pthread_cond_broadcast(&condition);
    while (ready != 4) pthread_cond_wait(&condition, &mutex);
    pthread_mutex_unlock(&mutex);
    if (*error != (int)value + 70 || pthread_getspecific(key) != pointer) return (void*)4;
    sem_post(&semaphore);
    return (void*)(value + 16);
#endif
}

Word GuestMain(void) {
    Word threads[4];
    int* error = __error();
    *error = 11;
    if (sem_init(&semaphore, 0, 0) != 0) return 1;
#if defined(THREAD_STOP)
    Word attr = 0;
    if (pthread_attr_init(&attr) != 0 || pthread_attr_setdetachstate(&attr, 1) != 0) return 2;
    if (pthread_create(&threads[0], &attr, Worker, (void*)1) != 0) return 3;
    pthread_attr_destroy(&attr);
    return 42;
#elif defined(THREAD_DETACHED_FAILURE)
    Word attr = 0;
    if (pthread_attr_init(&attr) != 0 || pthread_attr_setdetachstate(&attr, 1) != 0) return 2;
    if (pthread_create(&threads[0], &attr, Worker, (void*)1) != 0) return 3;
    pthread_attr_destroy(&attr);
    for (;;) {
        volatile int* value = &counter;
        ++*value;
    }
#elif defined(THREAD_FAILURE)
    if (pthread_create(&threads[0], (void*)0, Worker, (void*)1) != 0) return 2;
    pthread_join(threads[0], (void*)0);
    return 7;
#else
    for (Word i = 0; i < 4; ++i) if (pthread_create(&threads[i], (void*)0, Worker, (void*)(i + 1)) != 0) return 2;
    for (Word i = 0; i < 4; ++i) {
        void* result = (void*)0;
        if (pthread_join(threads[i], &result) != 0 || (Word)result != i + 17) return 3;
        if (sem_wait(&semaphore) != 0) return 4;
    }
    if (counter != 800 || ready != 4 || initialized != 1 || destructors != 5 || destructorSum != 78 || *error != 11) return 5;
    if (pthread_key_delete(key) != 0 || pthread_mutex_destroy(&mutex) != 0 || pthread_cond_destroy(&condition) != 0 || pthread_rwlock_destroy(&rwlock) != 0 || sem_destroy(&semaphore) != 0) return 6;
    return 42;
#endif
}
