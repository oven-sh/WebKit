/*
 * Copyright (C) 2017 Oleksandr Skachkov <gskachkov@gmail.com>.
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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

@linkTimeConstant
function asyncGeneratorResumeNext(generator, resumeMode)
{
    "use strict";

    @assert(@isAsyncGenerator(generator), "Generator is not an AsyncGenerator instance.");

    while (true) {
        var state = @getAsyncGeneratorInternalField(generator, @generatorFieldState);

        @assert(state !== @AsyncGeneratorStateExecuting, "Async generator should not be in executing state");

        if (state === @AsyncGeneratorStateAwaitingReturn)
            return;

        if (resumeMode === @AsyncGeneratorResumeModeEmpty)
            return;

        var resumeValue = @getAsyncGeneratorInternalField(generator, @asyncGeneratorFieldResumeValue);

        if (resumeMode !== @GeneratorResumeModeNormal) {
            if (state === @AsyncGeneratorStateInit) {
                @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateCompleted);
                state = @AsyncGeneratorStateCompleted;
            }

            if (resumeMode === @GeneratorResumeModeReturn) {
                if (state === @AsyncGeneratorStateCompleted) {
                    @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateAwaitingReturn);
                    return @resolveWithInternalMicrotaskForAsyncAwait(resumeValue, @InternalMicrotaskAsyncGeneratorResumeNext, generator);
                }

                if (state > 0 && (state & @AsyncGeneratorSuspendReasonMask) === @AsyncGeneratorSuspendReasonYield) {
                    state = (state & ~@AsyncGeneratorSuspendReasonMask) | @AsyncGeneratorSuspendReasonAwait;
                    @putAsyncGeneratorInternalField(generator, @generatorFieldState, state);
                    return @resolveWithInternalMicrotaskForAsyncAwait(resumeValue, @InternalMicrotaskAsyncGeneratorBodyCallReturn, generator);
                }
            } else {
                @assert(resumeMode === @GeneratorResumeModeThrow, "Async generator has wrong mode");
                if (state === @AsyncGeneratorStateCompleted) {
                    resumeMode = @asyncGeneratorQueueDequeueReject(generator, resumeValue);
                    continue;
                }
            }
        } else if (state === @AsyncGeneratorStateCompleted) {
            resumeMode = @asyncGeneratorQueueDequeueResolve(generator, { value: @undefined, done: true });
            continue;
        }

        var value = @undefined;

        @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateExecuting);

        try {
            value = @getAsyncGeneratorInternalField(generator, @generatorFieldNext).@call(@getAsyncGeneratorInternalField(generator, @generatorFieldThis), generator, state >> @AsyncGeneratorSuspendReasonShift, resumeValue, resumeMode, @getAsyncGeneratorInternalField(generator, @generatorFieldFrame));
            state = @getAsyncGeneratorInternalField(generator, @generatorFieldState);
            if (state === @AsyncGeneratorStateExecuting) {
                @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateCompleted);
                state = @AsyncGeneratorStateCompleted;
            }
        } catch (error) {
            @putAsyncGeneratorInternalField(generator, @generatorFieldState, @AsyncGeneratorStateCompleted);
            resumeMode = @asyncGeneratorQueueDequeueReject(generator, error);
            continue;
        }

        if (state > 0) {
            if ((state & @AsyncGeneratorSuspendReasonMask) === @AsyncGeneratorSuspendReasonAwait)
                return @resolveWithInternalMicrotaskForAsyncAwait(value, @InternalMicrotaskAsyncGeneratorBodyCallNormal, generator);

            state = (state & ~@AsyncGeneratorSuspendReasonMask) | @AsyncGeneratorSuspendReasonAwait;
            @putAsyncGeneratorInternalField(generator, @generatorFieldState, state);
            return @resolveWithInternalMicrotaskForAsyncAwait(value, @InternalMicrotaskAsyncGeneratorYieldAwaited, generator);
        }

        if (state === @AsyncGeneratorStateCompleted) {
            @assert(@getAsyncGeneratorInternalField(generator, @generatorFieldState) == @AsyncGeneratorStateCompleted);
            resumeMode = @asyncGeneratorQueueDequeueResolve(generator, { value, done: true });
            continue;
        }
        return;
    }
}

@linkTimeConstant
function asyncGeneratorEnqueueAndDrive(generator, value, mode, promise)
{
    "use strict";

    // SPEC-ungil §N.5 async resume-head claim (annex N7 row R7; MC-TEAR S6b /
    // MC-PRIM P5-async — CVE-AUDIT-RESULTS.md A4). GIL-off ONLY (every caller
    // is behind an `if (@gilOffProcess)` whose dead arm the bytecode generator
    // drops, so flag-off bytecode is the landed inline path verbatim —
    // next/return/throw keep their inline @asyncGeneratorQueueEnqueue +
    // @asyncGeneratorResumeNext sequence).
    //
    // AMENDED SHAPE (see the JSMicrotask.cpp block comment for the full
    // ruling): the previous landing held the rank-10a cellLock across the
    // body call — a frozen O2/I20 violation (allocates, polls safepoints,
    // self-re-locks via JSObject indexed/dictionary paths, GC-visit re-lock
    // deadlocks). The amended @claimAsyncGeneratorResume host hook does the
    // ENTIRE GIL-off path in C++: a SHORT cellLock span {DeferGC; lock;
    // wasEmpty; enqueue; unlock} (the §N default bounded shape, O1/O2-
    // compliant) and, iff wasEmpty, drives via the C++ asyncGeneratorResume-
    // Next so EVERY GIL-off dequeue is the locked nowEmpty edge in that TU.
    // The wasEmpty/nowEmpty queue-occupancy edges are mutually serialized by
    // the cell lock, giving at-most-one driver chain WITHOUT any claim word
    // the body's mid-drive State store can clobber, and WITHOUT any lock held
    // across the body.
    //
    // LOSER (wasEmpty=false — cross-thread OR same-thread re-entrant from
    // inside the body) ENQUEUES and returns the pending promise WITHOUT
    // driving and WITHOUT a TypeError: ECMA-262 AsyncGeneratorEnqueue, and
    // exactly the GIL-on observable behaviour. The previous landing's
    // tryLock-loser TypeError was a single-threaded semantic fork (sync
    // §N.5's TypeError licence is ECMA-262 GeneratorValidate; async
    // generators have no such step). No claim word means no claim-leak
    // hazard, so no try/catch / publish guard is needed; an exception inside
    // the C++ drive surfaces with the queue head still present and the next
    // continuation / enqueuer's wasEmpty edge picks up the chain.
    if (!@isAsyncGenerator(generator)) {
        // Match @asyncGeneratorQueueEnqueue's non-generator rejection so the
        // gilOff arm's observable behaviour is identical to GIL-on for a
        // wrong-|this| (the host hook unchecked-downcasts).
        @rejectPromiseWithFirstResolvingFunctionCallCheck(promise, @makeTypeError("|this| should be an async generator"));
        return promise;
    }
    @claimAsyncGeneratorResume(generator, value, mode, promise);
    return promise;
}

function next(value)
{
    "use strict";

    var promise = @newPromise();
    if (@gilOffProcess)
        return @asyncGeneratorEnqueueAndDrive(this, value, @GeneratorResumeModeNormal, promise);
    var resumeMode = @asyncGeneratorQueueEnqueue(this, value, @GeneratorResumeModeNormal, promise);
    if (resumeMode !== @AsyncGeneratorResumeModeEmpty)
        @asyncGeneratorResumeNext(this, resumeMode);

    return promise;
}

function return(value)
{
    "use strict";

    var promise = @newPromise();
    if (@gilOffProcess)
        return @asyncGeneratorEnqueueAndDrive(this, value, @GeneratorResumeModeReturn, promise);
    var resumeMode = @asyncGeneratorQueueEnqueue(this, value, @GeneratorResumeModeReturn, promise);
    if (resumeMode !== @AsyncGeneratorResumeModeEmpty)
        @asyncGeneratorResumeNext(this, resumeMode);

    return promise;
}

function throw(value)
{
    "use strict";

    var promise = @newPromise();
    if (@gilOffProcess)
        return @asyncGeneratorEnqueueAndDrive(this, value, @GeneratorResumeModeThrow, promise);
    var resumeMode = @asyncGeneratorQueueEnqueue(this, value, @GeneratorResumeModeThrow, promise);
    if (resumeMode !== @AsyncGeneratorResumeModeEmpty)
        @asyncGeneratorResumeNext(this, resumeMode);

    return promise;
}
