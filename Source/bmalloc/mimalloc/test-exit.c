/*
 * Copyright (C) 2026 Peter Steinberger.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the above copyright notice,
 * this condition and the following disclaimer are retained.
 * THIS SOFTWARE IS PROVIDED AS IS, WITHOUT WARRANTY OF ANY KIND.
 */

// Regression for the pthread-key collision described in oven-sh/WebKit#698.
// Run after main returns so allocator teardown and a live thread overlap.
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
static pthread_t thread;
static int allocatorStopped;
static int sentinel;

static void* allocateAfterShutdown(void* unused)
{
    (void)unused;
    CHECK(!pthread_mutex_lock(&lock));
    while (!allocatorStopped)
        CHECK(!pthread_cond_wait(&condition, &lock));
    CHECK(!pthread_mutex_unlock(&lock));

    // The first new key may reuse mimalloc's deleted key, just as ParkingLot does.
    pthread_key_t key;
    CHECK(!pthread_key_create(&key, NULL));
    CHECK(!pthread_setspecific(key, &sentinel));
    void* allocation = mi_malloc(64);
    CHECK(allocation);
    CHECK(pthread_getspecific(key) == &sentinel);
    mi_free(allocation);
    CHECK(pthread_getspecific(key) == &sentinel);
    CHECK(!pthread_key_delete(key));
    return NULL;
}

static void checkExit(void)
{
    // Calling the same public shutdown function here avoids relying on link-order
    // dependent destructor priorities. The automatic allocator destructor is idempotent.
    mi_process_done();
    CHECK(!pthread_mutex_lock(&lock));
    allocatorStopped = 1;
    CHECK(!pthread_cond_signal(&condition));
    CHECK(!pthread_mutex_unlock(&lock));
    CHECK(!pthread_join(thread, NULL));
    puts("mimalloc exit TLS ownership: PASS");
}

int main(void)
{
    // Initialize the allocator on the main thread, leaving the worker's first
    // allocation until after shutdown has deleted the allocator's pthread key.
    void* allocation = mi_malloc(16);
    CHECK(allocation);
    mi_free(allocation);
    CHECK(!pthread_create(&thread, NULL, allocateAfterShutdown, NULL));
    CHECK(!atexit(checkExit));
    return 0;
}
