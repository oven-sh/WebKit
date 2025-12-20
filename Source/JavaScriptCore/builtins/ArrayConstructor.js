/*
 * Copyright (C) 2015, 2016 Apple Inc. All rights reserved.
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

function of(/* items... */)
{
    "use strict";

    
    const len = @argumentCount();
    const extended = this !== @Array && @isConstructor(this);
    const arr = extended ? new this(len) : @newArrayWithSize(len);

    const unrollable = len <= 2 ** 30;

    if (!unrollable) for (let o = 0; o < len; o++) @putByValDirect(arr, o, arguments[o]);

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, arguments[o]);
            @putByValDirect(arr, o + 1, arguments[o + 1]);
            @putByValDirect(arr, o + 2, arguments[o + 2]);
            @putByValDirect(arr, o + 3, arguments[o + 3]);
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, arguments[o]);
    }

    return (arr.length = len, arr);
    
}

function from(items /*, mapFn, thisArg */)
{
    "use strict";

    var mapFn = @argument(1);

    var thisArg;

    if (mapFn !== @undefined) {
        if (!@isCallable(mapFn))
            @throwTypeError("Array.from requires that the second argument, when provided, be a function");

        thisArg = @argument(2);
    }

    var arrayLike = @toObject(items, "Array.from requires an array-like object - not null or undefined");

    if (mapFn === @undefined) {
        var fastResult = @arrayFromFastFillWithUndefined(this, arrayLike);
        if (fastResult)
            return fastResult;
    }

    const iteratorMethod = items.@@iterator;
    if (!@isUndefinedOrNull(iteratorMethod)) return @bun_arrayFromIterator.@call(this, items, mapFn, thisArg, iteratorMethod);
    
    const len = @toLength(arrayLike.length);
    const fast = @bun_isLengthObject(arrayLike);
    const extended = this !== @Array && @isConstructor(this);
    const arr = extended ? new this(len) : @newArrayWithSize(len);

    if (mapFn === @undefined) {
        if (fast) @bun_arrayFillUndefined(arr, len);
        else @bun_arrayCopyFrom(arr, len, arrayLike);
    }

    else {
        if (thisArg === @undefined) {
            if (fast) @bun_arrayFillMapUndefined(arr, len, mapFn);
            else @bun_arrayCopyFromMap(arr, len, arrayLike, mapFn);
        }

        else {
            if (fast) @bun_arrayFillMapUndefinedThis(arr, len, mapFn, thisArg);
            else @bun_arrayCopyFromMapThis(arr, len, arrayLike, mapFn, thisArg);
        }
    }

    return (arr.length = len, arr);
    
}

@linkTimeConstant
@visibility=PrivateRecursive
function bun_isLengthObject(obj) {
    "use strict";

    if (@isArray(obj)) return false;
    if (@isProxyObject(obj)) return false;
    if (@isTypedArrayView(obj)) return false;

    // @isNaturalObject would be useful
    if (@Object.prototype !== @Object.@getPrototypeOf(obj)) return false;

    // needs @objectAllKeysCount(obj)
    const keys = @Object.@getOwnPropertyNames(obj);
    if (1 !== keys.length || keys[0] !== 'length') return false;

    return true;
}

@linkTimeConstant
@visibility=PrivateRecursive
function bun_arrayFillUndefined(arr, len) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, @undefined);

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, @undefined);
            @putByValDirect(arr, o + 1, @undefined);
            @putByValDirect(arr, o + 2, @undefined);
            @putByValDirect(arr, o + 3, @undefined);
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, @undefined);
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
function bun_arrayCopyFrom(arr, len, src) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, src[o]);

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, src[o]);
            @putByValDirect(arr, o + 1, src[o + 1]);
            @putByValDirect(arr, o + 2, src[o + 2]);
            @putByValDirect(arr, o + 3, src[o + 3]);
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, src[o]);
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
function bun_arrayFillMapUndefined(arr, len, map) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, map(@undefined, o));

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, map(@undefined, o));
            @putByValDirect(arr, o + 1, map(@undefined, o + 1));
            @putByValDirect(arr, o + 2, map(@undefined, o + 2));
            @putByValDirect(arr, o + 3, map(@undefined, o + 3));
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, map(@undefined, o));
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
function bun_arrayFillMapUndefinedThis(arr, len, map, thisArg) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, map.@call(thisArg, @undefined, o));

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, map.@call(thisArg, @undefined, o));
            @putByValDirect(arr, o + 1, map.@call(thisArg, @undefined, o + 1));
            @putByValDirect(arr, o + 2, map.@call(thisArg, @undefined, o + 2));
            @putByValDirect(arr, o + 3, map.@call(thisArg, @undefined, o + 3));
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, map.@call(thisArg, @undefined, o));
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
function bun_arrayCopyFromMap(arr, len, src, map) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, map(src[o], o));

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, map(src[o], o));
            @putByValDirect(arr, o + 1, map(src[o + 1], o + 1));
            @putByValDirect(arr, o + 2, map(src[o + 2], o + 2));
            @putByValDirect(arr, o + 3, map(src[o + 3], o + 3));
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, map(src[o], o));
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
function bun_arrayCopyFromMapThis(arr, len, src, map, thisArg) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, map.@call(thisArg, src[o], o));

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, map.@call(thisArg, src[o], o));
            @putByValDirect(arr, o + 1, map.@call(thisArg, src[o + 1], o + 1));
            @putByValDirect(arr, o + 2, map.@call(thisArg, src[o + 2], o + 2));
            @putByValDirect(arr, o + 3, map.@call(thisArg, src[o + 3], o + 3));
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, map.@call(thisArg, src[o], o));
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
function bun_arrayFromIterator(items, mapFn, thisArg, iteratorMethod) {
    "use strict"

    if (!@isCallable(iteratorMethod))
        @throwTypeError("Array.from requires that the property of the first argument, items[Symbol.iterator], when exists, be a function");

    const extended = this !== @Array && @isConstructor(this);

    let offset = 0;
    const arr = extended ? new this() : [];
    const iterator = iteratorMethod.@call(items);

    const wrapper = {
        @@iterator: function () { return iterator; }
    };

    if (mapFn === @undefined) {
        for (const value of wrapper) {
            if (offset === @MAX_SAFE_INTEGER) @throwTypeError("Length exceeded the maximum array length");
            @putByValDirect(arr, offset, value);

            offset++;
        }
    }

    else {
        if (thisArg === @undefined) {
            for (const value of wrapper) {
                if (offset === @MAX_SAFE_INTEGER) @throwTypeError("Length exceeded the maximum array length");
                @putByValDirect(arr, offset, mapFn(value, offset));

                offset++;
            }
        }

        else {
            for (const value of wrapper) {
                if (offset === @MAX_SAFE_INTEGER) @throwTypeError("Length exceeded the maximum array length");
                @putByValDirect(arr, offset, mapFn.@call(thisArg, value, offset));

                offset++;
            }
        }
    }

    return (arr.length = offset, arr);
}


function isArray(array)
{
    "use strict";

    if (@isJSArray(array) || @isDerivedArray(array))
        return true;
    if (!@isProxyObject(array))
        return false;
    return @isArraySlow(array);
}

@linkTimeConstant
@visibility=PrivateRecursive
async function defaultAsyncFromAsyncIterator(iterator, mapFn, thisArg)
{
    "use strict";

    
    const extended = this !== @Array && @isConstructor(this);

    let offset = 0;
    const arr = extended ? new this() : [];

    const wrapper = {
        @@asyncIterator: function () { return iterator; }
    };

    if (mapFn === @undefined) {
        for await (const value of wrapper) {
            if (offset === @MAX_SAFE_INTEGER) @throwTypeError("Length exceeded the maximum array length");
            @putByValDirect(arr, offset, value);

            offset++;
        }
    }

    else {
        if (thisArg === @undefined) {
            for await (const value of wrapper) {
                if (offset === @MAX_SAFE_INTEGER) @throwTypeError("Length exceeded the maximum array length");
                @putByValDirect(arr, offset, await mapFn(value, offset));

                offset++;
            }
        }

        else {
            for await (const value of wrapper) {
                if (offset === @MAX_SAFE_INTEGER) @throwTypeError("Length exceeded the maximum array length");
                @putByValDirect(arr, offset, await mapFn.@call(thisArg, value, offset));

                offset++;
            }
        }
    }

    return (arr.length = offset, arr);
    
}

@linkTimeConstant
@visibility=PrivateRecursive
async function defaultAsyncFromAsyncArrayLike(asyncItems, mapFn, thisArg)
{
    "use strict";

    var arrayLike = @toObject(asyncItems, "Array.fromAsync requires an array-like object - not null or undefined");

    
    const len = @toLength(arrayLike.length);
    const fast = @bun_isLengthObject(arrayLike);
    const extended = this !== @Array && @isConstructor(this);
    const arr = extended ? new this(len) : @newArrayWithSize(len);

    if (mapFn === @undefined) {
        if (fast) @bun_arrayFillUndefined(arr, len);
        else await @bun_async_arrayCopyFrom(arr, len, arrayLike);
    }
    
    else {
        if (thisArg === @undefined) {
            if (fast) await @bun_async_arrayFillMapUndefined(arr, len, mapFn);
            else await @bun_async_arrayCopyFromMap(arr, len, arrayLike, mapFn);
        }

        else {
            if (fast) await @bun_async_arrayFillMapUndefinedThis(arr, len, mapFn, thisArg);
            else await @bun_async_arrayCopyFromMapThis(arr, len, arrayLike, mapFn, thisArg);
        }
    }

    return (arr.length = len, arr);
    
}


@linkTimeConstant
@visibility=PrivateRecursive
async function bun_async_arrayCopyFrom(arr, len, src) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, await src[o]);

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, await src[o]);
            @putByValDirect(arr, o + 1, await src[o + 1]);
            @putByValDirect(arr, o + 2, await src[o + 2]);
            @putByValDirect(arr, o + 3, await src[o + 3]);
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, await src[o]);
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
async function bun_async_arrayFillMapUndefined(arr, len, map) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, await map(@undefined, o));

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, await map(@undefined, o));
            @putByValDirect(arr, o + 1, await map(@undefined, o + 1));
            @putByValDirect(arr, o + 2, await map(@undefined, o + 2));
            @putByValDirect(arr, o + 3, await map(@undefined, o + 3));
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, await map(@undefined, o));
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
async function bun_async_arrayFillMapUndefinedThis(arr, len, map, thisArg) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, await map.@call(thisArg, @undefined, o));

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, await map.@call(thisArg, @undefined, o));
            @putByValDirect(arr, o + 1, await map.@call(thisArg, @undefined, o + 1));
            @putByValDirect(arr, o + 2, await map.@call(thisArg, @undefined, o + 2));
            @putByValDirect(arr, o + 3, await map.@call(thisArg, @undefined, o + 3));
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, await map.@call(thisArg, @undefined, o));
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
async function bun_async_arrayCopyFromMap(arr, len, src, map) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, await map(await src[o], o));

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, await map(await src[o], o));
            @putByValDirect(arr, o + 1, await map(await src[o + 1], o + 1));
            @putByValDirect(arr, o + 2, await map(await src[o + 2], o + 2));
            @putByValDirect(arr, o + 3, await map(await src[o + 3], o + 3));
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, await map(await src[o], o));
    }
}

@linkTimeConstant
@visibility=PrivateRecursive
async function bun_async_arrayCopyFromMapThis(arr, len, src, map, thisArg) {
    "use strict";
    const unrollable = len <= 2 ** 30;

    if (!unrollable)
        for (let o = 0; o < len; o++) @putByValDirect(arr, o, await map.@call(thisArg, await src[o], o));

    else {
        const unrolled = len & ~3;

        for (let o = 0; o < unrolled; o += 4) {
            @putByValDirect(arr, o, await map.@call(thisArg, await src[o], o));
            @putByValDirect(arr, o + 1, await map.@call(thisArg, await src[o + 1], o + 1));
            @putByValDirect(arr, o + 2, await map.@call(thisArg, await src[o + 2], o + 2));
            @putByValDirect(arr, o + 3, await map.@call(thisArg, await src[o + 3], o + 3));
        }

        for (let o = unrolled; o < len; o++) @putByValDirect(arr, o, await map.@call(thisArg, await src[o], o));
    }
}


function fromAsync(asyncItems  /*, mapFn, thisArg */)
{
    "use strict";

    try {
        var mapFn = @argument(1);

        var thisArg;

        if (mapFn !== @undefined) {
            if (!@isCallable(mapFn))
                @throwTypeError("Array.fromAsync requires that the second argument, when provided, be a function");

            thisArg = @argument(2);
        }

        var usingSyncIterator;
        var usingAsyncIterator = asyncItems.@@asyncIterator;
        if (!@isUndefinedOrNull(usingAsyncIterator)) {
            if (!@isCallable(usingAsyncIterator))
                @throwTypeError("Array.fromAsync requires that the property of the first argument, items[Symbol.asyncIterator], when exists, be a function");
        } else {
            usingSyncIterator = asyncItems.@@iterator;
            if (!@isUndefinedOrNull(usingSyncIterator)) {
                if (!@isCallable(usingSyncIterator))
                    @throwTypeError("Array.fromAsync requires that the property of the first argument, items[Symbol.iterator], when exists, be a function");
            }
        }

        if (!@isUndefinedOrNull(usingAsyncIterator))
            return @defaultAsyncFromAsyncIterator.@call(this, usingAsyncIterator.@call(asyncItems), mapFn, thisArg);

        if (!@isUndefinedOrNull(usingSyncIterator)) {
            var iterator = usingSyncIterator.@call(asyncItems);
            return @defaultAsyncFromAsyncIterator.@call(this, @createAsyncFromSyncIterator(iterator, iterator.next), mapFn, thisArg);
        }

        return @defaultAsyncFromAsyncArrayLike.@call(this, asyncItems, mapFn, thisArg);
    } catch (reason) {
        var promise = @newPromise();
        @rejectPromiseWithFirstResolvingFunctionCallCheck(promise, reason);
        return promise;
    }
}
