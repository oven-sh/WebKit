/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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

// https://tc39.es/proposal-iterator-helpers/#sec-%iteratorhelperprototype%.next
function next()
{
    "use strict";

    if (!@isIteratorHelper(this))
        @throwTypeError("|this| should be an iterator helper");

    var generator = @getIteratorHelperInternalField(this, @iteratorHelperFieldGenerator);
    // SPEC-ungil §N.5 claim (annex N7 row R8) — see GeneratorPrototype.js
    // next(): GIL-off the check-then-resume must be ONE atomic claim;
    // @generatorResume's epilogue publishes against the claim token.
    var state;
    if (@gilOffProcess)
        state = @claimGeneratorResume(generator);
    else
        state = @getGeneratorInternalField(generator, @generatorFieldState);
    if (state === @GeneratorStateExecuting)
        @throwTypeError("Generator is executing");

    if (@gilOffProcess) {
        // §N.5 claim-leak guard — see GeneratorPrototype.js next(): publish
        // the claim if the call into @generatorResume throws before its
        // protected region (e.g. prologue stack-overflow), else the helper's
        // generator is bricked as permanently "executing". Idempotent: the
        // CAS only ever clears OUR token.
        try {
            return @generatorResume(generator, state, @undefined, @GeneratorResumeModeNormal);
        } catch (error) {
            @publishGeneratorResume(generator);
            throw error;
        }
    }
    return @generatorResume(generator, state, @undefined, @GeneratorResumeModeNormal);
}

// https://tc39.es/proposal-iterator-helpers/#sec-%iteratorhelperprototype%.return
function return()
{
    "use strict";

    if (!@isIteratorHelper(this))
        @throwTypeError("|this| should be an iterator helper");

    var generator = @getIteratorHelperInternalField(this, @iteratorHelperFieldGenerator);
    // §N.5 claim — see next() above.
    var state;
    if (@gilOffProcess)
        state = @claimGeneratorResume(generator);
    else
        state = @getGeneratorInternalField(generator, @generatorFieldState);
    if (state === @GeneratorStateInit) {
        // GIL-off we hold the claim (the field holds our token); this store
        // is the unclaim-to-terminal (Completed is never claimed).
        @putGeneratorInternalField(generator, @generatorFieldState, @GeneratorStateCompleted);

        var underlyingIterator = @getIteratorHelperInternalField(this, @iteratorHelperFieldUnderlyingIterator);
        if (underlyingIterator !== null)
            @iteratorGenericClose(underlyingIterator);

        return { value: @undefined, done: true };
    }

    if (state === @GeneratorStateExecuting)
        @throwTypeError("Generator is executing");

    if (@gilOffProcess) {
        // §N.5 claim-leak guard — see next() above.
        try {
            return @generatorResume(generator, state, @undefined, @GeneratorResumeModeReturn);
        } catch (error) {
            @publishGeneratorResume(generator);
            throw error;
        }
    }
    return @generatorResume(generator, state, @undefined, @GeneratorResumeModeReturn);
}
