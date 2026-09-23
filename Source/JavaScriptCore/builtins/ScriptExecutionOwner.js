/*
 * Copyright (C) 2026 Codeblog Corp. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// The Promise constructor's executor call and resolving functions (PromiseConstructor.js) once the global object has
// script execution owners: whoever calls them, the resolving functions settle the promise as the owner that was
// current when it was made.
@linkTimeConstant
function runPromiseExecutorInScriptExecutionOwner(promise, executor)
{
    "use strict";

    var asyncContextData = @asyncContextData;
    var madeBy = @getInternalField(asyncContextData, 1);

    try {
        executor(
            (resolution) => {
                return @settlePromiseInScriptExecutionOwner(madeBy, promise, resolution, true);
            },
            (reason) => {
                return @settlePromiseInScriptExecutionOwner(madeBy, promise, reason, false);
            });
    } catch (error) {
        @rejectPromiseWithFirstResolvingFunctionCallCheck(promise, error);
    }

    return promise;
}

@linkTimeConstant
function settlePromiseInScriptExecutionOwner(owner, promise, value, isResolve)
{
    "use strict";

    var asyncContextData = @asyncContextData;
    var previousOwner = @getInternalField(asyncContextData, 1);
    if (previousOwner === owner) {
        if (isResolve)
            return @resolvePromiseWithFirstResolvingFunctionCallCheck(promise, value);
        return @rejectPromiseWithFirstResolvingFunctionCallCheck(promise, value);
    }

    @putInternalField(asyncContextData, 1, owner);
    try {
        if (isResolve)
            return @resolvePromiseWithFirstResolvingFunctionCallCheck(promise, value);
        return @rejectPromiseWithFirstResolvingFunctionCallCheck(promise, value);
    } finally {
        @putInternalField(asyncContextData, 1, previousOwner);
    }
}
