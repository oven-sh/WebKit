/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// Under USE_EXTERNAL_MIMALLOC the static archives call into the consumer's mimalloc
// (oven-sh/mimalloc), which has entry points the vendored copy does not. An executable in this
// repository that links the archives against the vendored mimalloc-obj (the jsc shell, testFFI)
// supplies those entry points here, in terms of the vendored API.

#include "config.h"

#include <bmalloc/mimalloc.h>

// The consumer's hook for a thread that goes idle: collect the heap, discard the free runs inside
// still-used pages, hand the arena purge to a scavenger thread. The vendored mimalloc can only do
// the first part.
extern "C" void mi_on_thread_idle(void) noexcept
{
    mi_theap_collect(mi_theap_get_default(), /* force */ false);
}

// The consumer's mimalloc remembers the request of a thread that last found no memory. The vendored one does not.
extern "C" bool mi_last_failed_request(struct mi_failed_request_s*) noexcept
{
    return false;
}

// A value above 1 skips the teardown at process exit, like the consumer's MI_NO_PROCESS_DETACH.
__attribute__((constructor)) static void skipProcessTeardown()
{
    mi_option_set_default(mi_option_destroy_on_exit, 2);
}
