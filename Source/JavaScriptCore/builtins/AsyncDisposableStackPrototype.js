/*
 * Copyright (C) 2025 Sosuke Suzuki <aosukeke@gmail.com>.
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

// https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.adopt
function adopt(value, onAsyncDispose)
{
    'use strict';

    if (!@isAsyncDisposableStack(this))
        @throwTypeError("AsyncDisposableStack.prototype.adopt requires that |this| be a AsyncDisposableStack object");

    if (@getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldState) === @AsyncDisposableStackStateDisposed)
        throw new @ReferenceError("AsyncDisposableStack.prototype.adopt requires that |this| be a pending AsyncDisposableStack object");

    if (!@isCallable(onAsyncDispose))
        @throwTypeError("AsyncDisposableStack.prototype.adopt requires that onAsyncDispose argument be a callable");

    var closure = () => onAsyncDispose.@call(@undefined, value);
    @addDisposableResource(@getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldCapability), @undefined, /* isAsync */ true, closure);

    return value;
}

// https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.defer
@overriddenName="defer"
function deferMethod(onAsyncDispose)
{
    'use strict';

    if (!@isAsyncDisposableStack(this))
        @throwTypeError("AsyncDisposableStack.prototype.defer requires that |this| be a AsyncDisposableStack object");

    if (@getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldState) === @AsyncDisposableStackStateDisposed)
        throw new @ReferenceError("AsyncDisposableStack.prototype.defer requires that |this| be a pending AsyncDisposableStack object");

    if (!@isCallable(onAsyncDispose))
        @throwTypeError("AsyncDisposableStack.prototype.defer requires that onAsyncDispose argument be a callable");

    @addDisposableResource(@getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldCapability), @undefined, /* isAsync */ true, onAsyncDispose);

    return @undefined;
}

// https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.disposeAsync
function disposeAsync()
{
    'use strict';

    var promise = @newPromise();

    if (!@isAsyncDisposableStack(this)) {
        @rejectPromiseWithFirstResolvingFunctionCallCheck(promise, @makeTypeError("AsyncDisposableStack.prototype.disposeAsync requires that |this| be a AsyncDisposableStack object"));
        return promise;
    }

    if (@getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldState) === @AsyncDisposableStackStateDisposed) {
        @resolvePromiseWithFirstResolvingFunctionCallCheck(promise, @undefined);
        return promise;
    }

    @putAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldState, @AsyncDisposableStackStateDisposed);

    var stack = @getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldCapability);
    @assert(@isArray(stack));

    var self = this;
    var i = stack.length;
    var thrown = false;
    var suppressed;
    var needsAwait = false;
    var hasAwaited = false;

    var handleError = (result) => {
        told = @undefined;
        if (thrown)
            suppressed = new @SuppressedError(result, suppressed);
        else {
            thrown = true;
            suppressed = result;
        }
        loop();
    };

    var finish = () => {
        if (thrown)
            @rejectPromiseWithFirstResolvingFunctionCallCheck(promise, suppressed);
        else
            @resolvePromiseWithFirstResolvingFunctionCallCheck(promise, @undefined);
    };

    var told;
    var loop = () => {
        if (told !== @undefined) {
            var stillThrown = @isDisposableStack(told) ? @getDisposableStackInternalField(told, @disposableStackFieldWasThrown) : @getAsyncDisposableStackInternalField(told, @asyncDisposableStackFieldWasThrown);
            told = @undefined;
            if (!stillThrown) {
                if (thrown) {
                    thrown = false;
                    suppressed = @undefined;
                } else
                    @putAsyncDisposableStackInternalField(self, @asyncDisposableStackFieldWasThrown, false);
            }
        }
        while (i) {
            var resource = stack[--i];
            if (resource.method === @undefined) {
                needsAwait = true;
                continue;
            }
            var result;
            try {
                if (resource.method === @pythonExitContext) {
                    // It is told what has been thrown: here, or failing that by the block that is using this.
                    if (thrown) {
                        if (resource.method.@call(resource.value, true, suppressed)) {
                            thrown = false;
                            suppressed = @undefined;
                        }
                    } else if (resource.method.@call(resource.value, @getAsyncDisposableStackInternalField(self, @asyncDisposableStackFieldWasThrown), @getAsyncDisposableStackInternalField(self, @asyncDisposableStackFieldThrown)))
                        @putAsyncDisposableStackInternalField(self, @asyncDisposableStackFieldWasThrown, false);
                } else {
                    // There may be such a one in a stack that is in this one, so that is told the same. Whether it was dealt with is seen when it is done.
                    var wasThrown = thrown || @getAsyncDisposableStackInternalField(self, @asyncDisposableStackFieldWasThrown);
                    var what = thrown ? suppressed : @getAsyncDisposableStackInternalField(self, @asyncDisposableStackFieldThrown);
                    if (@isDisposableStack(resource.value)) {
                        @putDisposableStackInternalField(resource.value, @disposableStackFieldWasThrown, wasThrown);
                        @putDisposableStackInternalField(resource.value, @disposableStackFieldThrown, what);
                        told = wasThrown ? resource.value : @undefined;
                    } else if (@isAsyncDisposableStack(resource.value)) {
                        @putAsyncDisposableStackInternalField(resource.value, @asyncDisposableStackFieldWasThrown, wasThrown);
                        @putAsyncDisposableStackInternalField(resource.value, @asyncDisposableStackFieldThrown, what);
                        told = wasThrown ? resource.value : @undefined;
                    }
                    result = resource.method.@call(resource.value);
                }
            } catch (error) {
                told = @undefined;
                if (thrown)
                    suppressed = new @SuppressedError(error, suppressed);
                else {
                    thrown = true;
                    suppressed = error;
                }
                continue;
            }
            hasAwaited = true;
            @promiseResolve(@Promise, result).@then(loop, handleError);
            return;
        }
        if (needsAwait && !hasAwaited)
            @promiseResolve(@Promise, @undefined).@then(finish, finish);
        else
            finish();
    };

    loop();

    @putAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldCapability, []);

    return promise;
}

// https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.move
function move()
{
    'use strict';

    if (!@isAsyncDisposableStack(this))
        @throwTypeError("AsyncDisposableStack.prototype.move requires that |this| be a AsyncDisposableStack object");

    if (@getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldState) === @AsyncDisposableStackStateDisposed)
        throw new @ReferenceError("AsyncDisposableStack.prototype.move requires that |this| be a pending AsyncDisposableStack object");

    var newAsyncDisposableStack = new @AsyncDisposableStack();
    @putAsyncDisposableStackInternalField(newAsyncDisposableStack, @asyncDisposableStackFieldCapability, @getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldCapability));

    @putAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldCapability, []);
    @putAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldState, @AsyncDisposableStackStateDisposed);

    return newAsyncDisposableStack;
}

// https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype.use
function use(value)
{
    'use strict';

    if (!@isAsyncDisposableStack(this))
        @throwTypeError("AsyncDisposableStack.prototype.use requires that |this| be a AsyncDisposableStack object");

    if (@getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldState) === @AsyncDisposableStackStateDisposed)
        throw new @ReferenceError("AsyncDisposableStack.prototype.use requires that |this| be a pending AsyncDisposableStack object");

    return @addDisposableResource(@getAsyncDisposableStackInternalField(this, @asyncDisposableStackFieldCapability), value, /* isAsync */ true);
}
