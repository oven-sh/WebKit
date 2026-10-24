/* Copyright (C) 2026 Peter Steinberger. See test-exit.c for license terms. */

#include <mimalloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed: %s\n", #expression); \
        _exit(EXIT_FAILURE); \
    } \
} while (0)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static _Thread_local int pauseNextAssociation;
static pthread_key_t pausedKey;
static int associationPaused;
static int releaseAssociation;
static int deletedWhilePaused;

// Only the test's allocator object renames these calls. All synchronization and
// the real pthread operations in this file use the platform functions directly.
int mimalloc_test_setspecific(pthread_key_t key, const void* value)
{
    if (!pauseNextAssociation)
        return pthread_setspecific(key, value);
    pauseNextAssociation = 0;
    CHECK(!pthread_mutex_lock(&lock));
    pausedKey = key;
    associationPaused = 1;
    CHECK(!pthread_cond_signal(&condition));
    while (!releaseAssociation)
        CHECK(!pthread_cond_wait(&condition, &lock));
    int result = pthread_setspecific(key, value);
    associationPaused = 0;
    CHECK(!pthread_mutex_unlock(&lock));
    return result;
}

int mimalloc_test_key_delete(pthread_key_t key)
{
    CHECK(!pthread_mutex_lock(&lock));
    if (associationPaused && key == pausedKey) {
        deletedWhilePaused = 1;
        releaseAssociation = 1;
        CHECK(!pthread_cond_signal(&condition));
    }
    CHECK(!pthread_mutex_unlock(&lock));
    return pthread_key_delete(key);
}

static void* allocate(void* unused)
{
    (void)unused;
    pauseNextAssociation = 1;
    void* allocation = mi_malloc(64);
    CHECK(allocation);
    mi_free(allocation);
    return NULL;
}

// The guarded allocator calls this only after it has closed admission and
// observed an active setter. An unguarded allocator reaches the deletion hook
// instead, which records the violation and releases the setter there.
void mi_test_thread_key_wait(void)
{
    CHECK(!pthread_mutex_lock(&lock));
    releaseAssociation = 1;
    CHECK(!pthread_cond_signal(&condition));
    CHECK(!pthread_mutex_unlock(&lock));
}

static void checkExit(void)
{
    mi_process_done();
    CHECK(!pthread_join(worker, NULL));
    CHECK(!deletedWhilePaused);
    puts("mimalloc in-flight exit TLS ownership: PASS");
}

int main(void)
{
    void* allocation = mi_malloc(16);
    CHECK(allocation);
    mi_free(allocation);
    CHECK(!pthread_mutex_lock(&lock));
    CHECK(!pthread_create(&worker, NULL, allocate, NULL));
    while (!associationPaused)
        CHECK(!pthread_cond_wait(&condition, &lock));
    CHECK(!pthread_mutex_unlock(&lock));
    CHECK(!atexit(checkExit));
    return 0;
}
