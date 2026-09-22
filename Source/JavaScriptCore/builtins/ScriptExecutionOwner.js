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

// What a function of script tail-calls when it is called from outside its script execution owner
// (BytecodeGenerator::emitEnterScriptExecutionOwner): the same call, made with the owner current. The previous owner and
// async context come back however the call ends.

@linkTimeConstant
function callInScriptExecutionOwner(owner, callee, thisValue, argumentValues)
{
    "use strict";

    var asyncContextData = @asyncContextData;
    var previousAsyncContext = @getInternalField(asyncContextData, 0);
    var previousOwner = @getInternalField(asyncContextData, 1);
    @putInternalField(asyncContextData, 1, owner);
    try {
        return callee.@apply(thisValue, argumentValues);
    } finally {
        @putInternalField(asyncContextData, 0, previousAsyncContext);
        @putInternalField(asyncContextData, 1, previousOwner);
    }
}

@linkTimeConstant
function constructInScriptExecutionOwner(owner, callee, newTarget, argumentValues)
{
    "use strict";

    var asyncContextData = @asyncContextData;
    var previousAsyncContext = @getInternalField(asyncContextData, 0);
    var previousOwner = @getInternalField(asyncContextData, 1);
    @putInternalField(asyncContextData, 1, owner);
    try {
        return @constructWithNewTarget(callee, newTarget, argumentValues);
    } finally {
        @putInternalField(asyncContextData, 0, previousAsyncContext);
        @putInternalField(asyncContextData, 1, previousOwner);
    }
}
