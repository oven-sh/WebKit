/*
 * Copyright (C) 2015-2016 Yusuke Suzuki <utatane.tea@gmail.com>.
 * Copyright (C) 2016-2024 Apple Inc. All rights reserved.
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

// This implements https://tc39.es/ecma262/#sec-generatorresume and https://tc39.es/ecma262/#sec-generatorresumeabrupt
// with exception of [[GeneratorBrand]] check and handling of [[GeneratorState]] equal to *completed*.
@linkTimeConstant
function generatorResume(generator, state, value, resumeMode)
{
    "use strict";

    var done = true;
    if (state !== @GeneratorStateCompleted) {
        if (@gilOffProcess) {
            // SPEC-ungil §N.5: the caller already claimed the resume — the
            // state field holds OUR thread's claim token (see
            // @claimGeneratorResume). After the body call, done is decided
            // by the publish CAS token -> Completed: success <=> the body
            // never unclaimed (ran to completion / threw). A plain
            // state re-read would race a rival's fresh claim after the
            // body's yield-side unclaim and fabricate done:true.
            try {
                var value = @getGeneratorInternalField(generator, @generatorFieldNext).@call(@getGeneratorInternalField(generator, @generatorFieldThis), generator, state, value, resumeMode, @getGeneratorInternalField(generator, @generatorFieldFrame));
            } catch (error) {
                @publishGeneratorResume(generator);
                throw error;
            }
            done = @publishGeneratorResume(generator);
        } else {
            @putGeneratorInternalField(generator, @generatorFieldState, @GeneratorStateExecuting);

            try {
                var value = @getGeneratorInternalField(generator, @generatorFieldNext).@call(@getGeneratorInternalField(generator, @generatorFieldThis), generator, state, value, resumeMode, @getGeneratorInternalField(generator, @generatorFieldFrame));
            } catch (error) {
                @putGeneratorInternalField(generator, @generatorFieldState, @GeneratorStateCompleted);
                throw error;
            }

            done = @getGeneratorInternalField(generator, @generatorFieldState) === @GeneratorStateExecuting;
            if (done)
                @putGeneratorInternalField(generator, @generatorFieldState, @GeneratorStateCompleted);
        }
    }
    return { value, done };
}

function next(value)
{
    "use strict";

    if (!@isGenerator(this))
        @throwTypeError("|this| should be a generator");

    // SPEC-ungil §N.5 (annex N7 row R7): GIL-off, the resume head's
    // check-then-store is ONE atomic claim — @claimGeneratorResume CASes
    // SuspendedX -> Executing on the state field and returns the observed
    // pre-claim state (Executing/Completed are returned WITHOUT claiming;
    // dispatch below is on that re-read). Flag-off / GIL-on keeps the landed
    // plain read; @gilOffProcess is a bytecode-time constant, so every tier
    // folds the branch. @generatorResume's Executing store is redundant
    // under the claim (same value) and remains the claiming store GIL-on.
    var state;
    if (@gilOffProcess)
        state = @claimGeneratorResume(this);
    else
        state = @getGeneratorInternalField(this, @generatorFieldState);
    if (state === @GeneratorStateExecuting)
        @throwTypeError("Generator is executing");

    if (state === @GeneratorStateCompleted)
        value = @undefined;

    if (@gilOffProcess) {
        // §N.5 claim-leak guard: if the CALL into @generatorResume itself
        // throws before its protected try is entered (realistic case: a
        // stack-overflow RangeError from its prologue stack check), the
        // claim token would be left in the State field forever — every
        // later claim on every thread then reads the canonical Executing
        // and the generator is permanently bricked. Publish-on-throw here
        // is idempotent against @generatorResume's own catch arm/epilogue:
        // the publish CAS (ourToken -> Completed) simply fails if the token
        // is already gone, and can never clear a rival's token.
        try {
            return @generatorResume(this, state, value, @GeneratorResumeModeNormal);
        } catch (error) {
            @publishGeneratorResume(this);
            throw error;
        }
    }
    return @generatorResume(this, state, value, @GeneratorResumeModeNormal);
}

function return(value)
{
    "use strict";

    if (!@isGenerator(this))
        @throwTypeError("|this| should be a generator");

    // §N.5 claim — see next() above.
    var state;
    if (@gilOffProcess)
        state = @claimGeneratorResume(this);
    else
        state = @getGeneratorInternalField(this, @generatorFieldState);
    if (state === @GeneratorStateExecuting)
        @throwTypeError("Generator is executing");

    if (@gilOffProcess) {
        // §N.5 claim-leak guard — see next() above.
        try {
            return @generatorResume(this, state, value, @GeneratorResumeModeReturn);
        } catch (error) {
            @publishGeneratorResume(this);
            throw error;
        }
    }
    return @generatorResume(this, state, value, @GeneratorResumeModeReturn);
}

function throw(exception)
{
    "use strict";

    if (!@isGenerator(this))
        @throwTypeError("|this| should be a generator");

    // §N.5 claim — see next() above. Completed is terminal and returned
    // unclaimed, so the rethrow arm below holds no claim.
    var state;
    if (@gilOffProcess)
        state = @claimGeneratorResume(this);
    else
        state = @getGeneratorInternalField(this, @generatorFieldState);
    if (state === @GeneratorStateExecuting)
        @throwTypeError("Generator is executing");

    if (state === @GeneratorStateCompleted)
        throw exception;

    if (@gilOffProcess) {
        // §N.5 claim-leak guard — see next() above.
        try {
            return @generatorResume(this, state, exception, @GeneratorResumeModeThrow);
        } catch (error) {
            @publishGeneratorResume(this);
            throw error;
        }
    }
    return @generatorResume(this, state, exception, @GeneratorResumeModeThrow);
}
