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
    return { value, done };
}

// What `for x in generator` does each time round, in Python: resumeGenerator() of python/PythonGenerators.cpp, for when there is nothing to send or to throw. It is here so that one piece of compiled code calls another.
// A generator of JavaScript's is gone through in the same way. What comes back is what was yielded, or nothing at all if there is no more.
@intrinsic=PythonGeneratorNextIntrinsic
@linkTimeConstant
function pythonGeneratorNext(generator)
{
    "use strict";

    var state = @getGeneratorInternalField(generator, @generatorFieldState);
    if (state < 0)
        return @pythonGeneratorNextSlow(generator);

    // What it was handling when it yielded, it is handling again, and beyond that whatever is being handled here.
    var handled = @pythonHandledExceptions;
    var callersOwn = @getInternalField(handled, 0);
    var callersOuter = @getInternalField(handled, 1);
    var generatorsOwn = @getByIdDirectPrivate(generator, "pythonHandled");
    @putInternalField(handled, 1, callersOwn === @undefined ? callersOuter : callersOwn);
    @putInternalField(handled, 0, generatorsOwn);

    @putGeneratorInternalField(generator, @generatorFieldState, @GeneratorStateExecuting);
    try {
        var value = @getGeneratorInternalField(generator, @generatorFieldNext).@call(@getGeneratorInternalField(generator, @generatorFieldThis), generator, state, @undefined, @GeneratorResumeModeNormal, @getGeneratorInternalField(generator, @generatorFieldFrame));
    } catch (error) {
        @putInternalField(handled, 0, callersOwn);
        @putInternalField(handled, 1, callersOuter);
        @putGeneratorInternalField(generator, @generatorFieldState, @GeneratorStateCompleted);
        @pythonGeneratorRaised(generator, error);
    }

    var nowHandling = @getInternalField(handled, 0);
    if (nowHandling !== generatorsOwn)
        @putByIdDirectPrivate(generator, "pythonHandled", nowHandling);
    @putInternalField(handled, 0, callersOwn);
    @putInternalField(handled, 1, callersOuter);

    // If it yielded, it said where to go on from.
    if (@getGeneratorInternalField(generator, @generatorFieldState) === @GeneratorStateExecuting) {
        @putGeneratorInternalField(generator, @generatorFieldState, @GeneratorStateCompleted);
        return @emptyValue();
    }
    return value;
}

function next(value)
{
    "use strict";

    if (!@isGenerator(this))
        @throwTypeError("|this| should be a generator");

    var state = @getGeneratorInternalField(this, @generatorFieldState);
    if (state === @GeneratorStateExecuting)
        @throwTypeError("Generator is executing");

    if (state === @GeneratorStateCompleted)
        value = @undefined;

    return @generatorResume(this, state, value, @GeneratorResumeModeNormal);
}

function return(value)
{
    "use strict";

    if (!@isGenerator(this))
        @throwTypeError("|this| should be a generator");

    var state = @getGeneratorInternalField(this, @generatorFieldState);
    if (state === @GeneratorStateExecuting)
        @throwTypeError("Generator is executing");

    return @generatorResume(this, state, value, @GeneratorResumeModeReturn);
}

function throw(exception)
{
    "use strict";

    if (!@isGenerator(this))
        @throwTypeError("|this| should be a generator");

    var state = @getGeneratorInternalField(this, @generatorFieldState);
    if (state === @GeneratorStateExecuting)
        @throwTypeError("Generator is executing");

    if (state === @GeneratorStateCompleted)
        throw exception;

    return @generatorResume(this, state, exception, @GeneratorResumeModeThrow);
}
