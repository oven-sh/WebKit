/*
 * Copyright (C) 2015 Yusuke Suzuki <utatane.tea@gmail.com>.
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

function try(callback /*, ...args */)
{
    "use strict";

    if (!@isObject(this))
        @throwTypeError("|this| is not an object");

    var args = [];
    for (var i = 1; i < @argumentCount(); i++)
        @putByValDirect(args, i - 1, arguments[i]);

    var promiseCapability = @newPromiseCapability(this);
    try {
        var value = callback.@apply(@undefined, args);
        promiseCapability.resolve.@call(@undefined, value);
    } catch (error) {
        promiseCapability.reject.@call(@undefined, error);
    }

    return promiseCapability.promise;
}

// https://tc39.es/proposal-await-dictionary/#sec-promise.allkeyed
function allKeyed(promises)
{
    "use strict";

    var promiseCapability = @newPromiseCapability(this);
    var promiseResolve;
    try {
        promiseResolve = this.resolve;
        if (!@isCallable(promiseResolve))
            @throwTypeError("Promise resolve is not a function");
        if (!@isObject(promises))
            @throwTypeError("Promise.allKeyed requires that its argument is an object");
    } catch (error) {
        promiseCapability.reject.@call(@undefined, error);
        return promiseCapability.promise;
    }

    var keys = [];
    var values = [];
    var remainingElementsCount = 1;
    var index = 0;

    function newResolveElement(currentIndex)
    {
        var alreadyCalled = false;
        return (argument) => {
            if (alreadyCalled)
                return @undefined;
            alreadyCalled = true;
            @putByValDirect(values, currentIndex, argument);
            if (!--remainingElementsCount) {
                var result = @Object.@create(null);
                for (var i = 0; i < keys.length; ++i)
                    @putByValDirect(result, keys[i], values[i]);
                promiseCapability.resolve.@call(@undefined, result);
            }
            return @undefined;
        };
    }

    try {
        var allKeys = @Object.@ownKeys(promises);
        for (var k = 0, length = allKeys.length; k < length; ++k) {
            var key = allKeys[k];
            var desc = @Object.@getOwnPropertyDescriptor(promises, key);
            if (desc === @undefined || !desc.enumerable)
                continue;
            var value = promises[key];
            @putByValDirect(keys, index, key);
            @putByValDirect(values, index, @undefined);
            var nextPromise = promiseResolve.@call(this, value);
            var resolveElement = newResolveElement(index);
            ++remainingElementsCount;
            nextPromise.then(resolveElement, promiseCapability.reject);
            ++index;
        }
        if (!--remainingElementsCount) {
            var result = @Object.@create(null);
            for (var i = 0; i < keys.length; ++i)
                @putByValDirect(result, keys[i], values[i]);
            promiseCapability.resolve.@call(@undefined, result);
        }
    } catch (error) {
        promiseCapability.reject.@call(@undefined, error);
    }

    return promiseCapability.promise;
}

// https://tc39.es/proposal-await-dictionary/#sec-promise.allsettledkeyed
function allSettledKeyed(promises)
{
    "use strict";

    var promiseCapability = @newPromiseCapability(this);
    var promiseResolve;
    try {
        promiseResolve = this.resolve;
        if (!@isCallable(promiseResolve))
            @throwTypeError("Promise resolve is not a function");
        if (!@isObject(promises))
            @throwTypeError("Promise.allSettledKeyed requires that its argument is an object");
    } catch (error) {
        promiseCapability.reject.@call(@undefined, error);
        return promiseCapability.promise;
    }

    var keys = [];
    var values = [];
    var remainingElementsCount = 1;
    var index = 0;

    function newResolveRejectElements(currentIndex)
    {
        var alreadyCalled = false;
        return [
            (0, (argument) => {
                if (alreadyCalled)
                    return @undefined;
                alreadyCalled = true;
                var entry = { status: "fulfilled", value: argument };
                @putByValDirect(values, currentIndex, entry);
                if (!--remainingElementsCount) {
                    var result = @Object.@create(null);
                    for (var i = 0; i < keys.length; ++i)
                        @putByValDirect(result, keys[i], values[i]);
                    promiseCapability.resolve.@call(@undefined, result);
                }
                return @undefined;
            }),
            (0, (argument) => {
                if (alreadyCalled)
                    return @undefined;
                alreadyCalled = true;
                var entry = { status: "rejected", reason: argument };
                @putByValDirect(values, currentIndex, entry);
                if (!--remainingElementsCount) {
                    var result = @Object.@create(null);
                    for (var i = 0; i < keys.length; ++i)
                        @putByValDirect(result, keys[i], values[i]);
                    promiseCapability.resolve.@call(@undefined, result);
                }
                return @undefined;
            }),
        ];
    }

    try {
        var allKeys = @Object.@ownKeys(promises);
        for (var k = 0, length = allKeys.length; k < length; ++k) {
            var key = allKeys[k];
            var desc = @Object.@getOwnPropertyDescriptor(promises, key);
            if (desc === @undefined || !desc.enumerable)
                continue;
            var value = promises[key];
            @putByValDirect(keys, index, key);
            @putByValDirect(values, index, @undefined);
            var nextPromise = promiseResolve.@call(this, value);
            var elements = newResolveRejectElements(index);
            ++remainingElementsCount;
            nextPromise.then(elements[0], elements[1]);
            ++index;
        }
        if (!--remainingElementsCount) {
            var result = @Object.@create(null);
            for (var i = 0; i < keys.length; ++i)
                @putByValDirect(result, keys[i], values[i]);
            promiseCapability.resolve.@call(@undefined, result);
        }
    } catch (error) {
        promiseCapability.reject.@call(@undefined, error);
    }

    return promiseCapability.promise;
}

@nakedConstructor
function Promise(executor)
{
    "use strict";

    if (!@isCallable(executor))
        @throwTypeError("Promise constructor takes a function argument");

    var promise = @createPromise(this);
    var capturedPromise = promise;

    try {
        executor(
            (resolution) => {
                return @resolvePromiseWithFirstResolvingFunctionCallCheck(capturedPromise, resolution);
            },
            (reason) => {
                return @rejectPromiseWithFirstResolvingFunctionCallCheck(capturedPromise, reason);
            });
    } catch (error) {
        @rejectPromiseWithFirstResolvingFunctionCallCheck(promise, error);
    }

    return promise;
}
