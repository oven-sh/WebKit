/*
 * Copyright (c) 2026 Anthropic, PBC. All rights reserved.
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
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef PAS_THREAD_SUSPENDER_H
#define PAS_THREAD_SUSPENDER_H

#include "pas_utils.h"

PAS_BEGIN_EXTERN_C;

typedef void* pas_embedder_thread_handle;

/* Embedder-provided thread suspension. Installed once at process startup, before any
   thread creates a TLC. The scavenger uses this on platforms without a native counted
   suspend (i.e., not Darwin) to force-stop allocators on threads that have gone idle and
   will never reach an allocation slow path to honor a cooperative stop request.

   Contract:
   - current_thread() is called from the thread that owns a TLC, during TLC creation.
     Returning NULL is allowed; that TLC's allocators will only ever be stopped
     cooperatively.
   - begin_suspend()/end_suspend() are called from the scavenger thread while it holds
     pas_thread_suspend_lock, the heap lock, and the target node's scavenger_lock. They
     are called as a balanced pair; end_suspend() is called exactly once per successful
     begin_suspend().
   - Implementations MUST NOT allocate via libpas or take any libpas lock.
   - begin_suspend() must serialize against the embedder's other suspension users (e.g.,
     GC) so that at most one suspender is active process-wide. If it acquires an embedder
     lock to do so, that lock is inner to all libpas locks at this callsite, and the
     embedder must never take a libpas lock while holding it. */
struct pas_thread_suspender {
    pas_embedder_thread_handle (*current_thread)(void);
    bool (*begin_suspend)(pas_embedder_thread_handle);
    void (*end_suspend)(pas_embedder_thread_handle);
    /* Optional; may be NULL. Called once when the TLC that owns the handle is destroyed,
       from the owning thread, with no libpas locks held. */
    void (*release_handle)(pas_embedder_thread_handle);
};
typedef struct pas_thread_suspender pas_thread_suspender;

PAS_API extern const pas_thread_suspender* pas_thread_suspender_instance;

PAS_API void pas_install_thread_suspender(const pas_thread_suspender* suspender);

#if PAS_ENABLE_TESTING
/* When set, platforms with a native suspend (Darwin) route force-stop through the
   installed embedder instead, so the embedder path can be exercised locally. */
PAS_API extern bool pas_thread_suspender_override_native;
#endif

PAS_END_EXTERN_C;

#endif /* PAS_THREAD_SUSPENDER_H */
