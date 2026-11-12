/*
 * Copyright (C) 2014-2017 Apple Inc. All rights reserved.
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

// builtins/ArrayPrototype.js, but for the methods that go up a list and call a function for each element: reduce every forEach filter map some find findIndex. EACH IS WRITTEN TWICE.
// <method>OfList IS UPSTREAM's TEXT. The rows of the table name it for a receiver that is known for a list (array-callbacks.builtins.ts): it is what is copied into a caller, where the walk knows the list and the callback.
// <method> IS WHAT A CALL BY VALUE ENTERS, knowing nothing. A RUN OVER A PLAIN VECTOR stands in it. $hasContiguousIndex() asks in one look whether the list still holds boxed values or integers one behind the other,
// whether the index lies below its length and whether the place is filled: the callback of the turn before may have changed any of the three. Behind a yes the element is the list's own, so nothing above the list is asked
// and the read runs no code. At the first no the loop as upstream writes it goes on from the same index: <method>From.
// The run is entered for a list that has an element: an empty list does not pay for what stands in front of a loop.
// What a method makes it stores by an operation that takes a list whose vector is writable and has the place; a definition and a store are the same there. Where that says no, the definition as upstream writes it.

$overriddenName = "reduce";
export function reduceOfList(callback /*, initialValue */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.reduce requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.reduce callback must be a function");

    var argumentCount = $argumentCount();
    if (length === 0 && argumentCount < 2)
        $throwTypeError("reduce of empty array with no initial value");

    var accumulator, k = 0;
    if (argumentCount > 1)
        accumulator = $argument(1);
    else {
        while (k < length && !(k in array))
            k += 1;
        if (k >= length)
            $throwTypeError("reduce of empty array with no initial value");
        accumulator = array[k++];
    }

    while (k < length) {
        if (k in array)
            accumulator = callback.$call($undefined, accumulator, array[k], k, array);
        k += 1;
    }
    return accumulator;
}

// Callbacks over one array that cannot be seen to run, in one loop: the walk copies these functions where it knows the callbacks (AOTWalkFusedChains.cpp). Nobody calls them.
// An array with a hole leaves the copy, and the calls run as written from the first one.
$linkTimeConstant;
export function mapArray(array, callback)
{
    "use strict";

    var length = $toLength(array.length);
    var result = $newArrayWithSpecies(length, array, length, undefined);
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var made = callback.$call($undefined, $getOwnContiguousIndex(array, i), i);
        if (!$tryPutByValDirect(result, i, made)) {
            // The site remembers the storage that the array has got by this store.
            $putByValDirect(result, i, made);
            break;
        }
    }
    if (i < length)
        $throwTypeError("Array.prototype.map callback must be a function");
    return result;
}

$linkTimeConstant;
export function filterArray(array, callback)
{
    "use strict";

    var length = $toLength(array.length);
    var result = [];
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        if (callback.$call($undefined, element, i))
            $appendElement(result, element);
    }
    if (i < length)
        $throwTypeError("Array.prototype.filter callback must be a function");
    return result;
}

$linkTimeConstant;
export function reduceArray(array, callback, accumulator)
{
    "use strict";

    var length = $toLength(array.length);
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++)
        accumulator = callback.$call($undefined, accumulator, $getOwnContiguousIndex(array, i), i);
    if (i < length)
        $throwTypeError("Array.prototype.reduce callback must be a function");
    return accumulator;
}

$linkTimeConstant;
export function reduceArrayFromFirst(array, callback, accumulator)
{
    "use strict";

    var length = $toLength(array.length);
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        accumulator = i ? callback.$call($undefined, accumulator, element, i) : element;
    }
    if (!i || i < length)
        $throwTypeError("reduce of empty array with no initial value");
    return accumulator;
}

$linkTimeConstant;
export function filterTruthyArray(array)
{
    "use strict";

    var length = $toLength(array.length);
    var result = [];
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        if (element)
            $appendElement(result, element);
    }
    if (i < length)
        $throwTypeError("Array.prototype.filter callback must be a function");
    return result;
}

$linkTimeConstant;
export function countTruthyInArray(array)
{
    "use strict";

    var length = $toLength(array.length);
    var count = 0;
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        if ($getOwnContiguousIndex(array, i))
            count++;
    }
    if (i < length)
        $throwTypeError("Array.prototype.filter callback must be a function");
    return count;
}

$linkTimeConstant;
export function someArray(array, callback)
{
    "use strict";

    var length = $toLength(array.length);
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        if (callback.$call($undefined, element, i))
            return true;
    }
    if (i < length)
        $throwTypeError("Array.prototype.some callback must be a function");
    return false;
}

$linkTimeConstant;
export function everyArray(array, callback)
{
    "use strict";

    var length = $toLength(array.length);
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        if (!callback.$call($undefined, element, i))
            return false;
    }
    if (i < length)
        $throwTypeError("Array.prototype.some callback must be a function");
    return true;
}

$linkTimeConstant;
export function findInArray(array, callback)
{
    "use strict";

    var length = $toLength(array.length);
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        if (callback.$call($undefined, element, i))
            return element;
    }
    if (i < length)
        $throwTypeError("Array.prototype.some callback must be a function");
    return $undefined;
}

$linkTimeConstant;
export function findIndexInArray(array, callback)
{
    "use strict";

    var length = $toLength(array.length);
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        if (callback.$call($undefined, element, i))
            return i;
    }
    if (i < length)
        $throwTypeError("Array.prototype.some callback must be a function");
    return -1;
}

// KindOfFusedChain names the bits of `links`.
$linkTimeConstant;
export function chainToValue(array, links, first, second, step, accumulator)
{
    "use strict";

    var length = $toLength(array.length);
    var i = 0;
    var position = 0;
    var nextIndex = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        if (links & 1) {
            if (!first.$call($undefined, element, i))
                continue;
        } else
            element = first.$call($undefined, element, i);
        if (links & 2) {
            var place = position++;
            if (links & 4) {
                if (!second.$call($undefined, element, place))
                    continue;
            } else
                element = second.$call($undefined, element, place);
        }
        accumulator = step.$call($undefined, accumulator, element, nextIndex);
        ++nextIndex;
    }
    if (i < length)
        $throwTypeError("Array.prototype.reduce callback must be a function");
    return accumulator;
}

$linkTimeConstant;
export function countInArray(array, callback)
{
    "use strict";

    var length = $toLength(array.length);
    var count = 0;
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        if (callback.$call($undefined, $getOwnContiguousIndex(array, i), i))
            count++;
    }
    if (i < length)
        $throwTypeError("Array.prototype.filter callback must be a function");
    return count;
}

$linkTimeConstant;
export function chainToList(array, links, first, second)
{
    "use strict";

    var length = $toLength(array.length);
    var i = 0;
    var position = 0;
    var result = links & 16 ? $newArrayWithSpecies(length, array, length, undefined) : [];
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var element = $getOwnContiguousIndex(array, i);
        if (links & 1) {
            if (!first.$call($undefined, element, i))
                continue;
        } else
            element = first.$call($undefined, element, i);
        if (links & 2) {
            var place = position++;
            if (links & 4) {
                if (!second.$call($undefined, element, place))
                    continue;
            } else
                element = second.$call($undefined, element, place);
        }
        if (links & 8) {
            if (!element)
                continue;
        }
        if (!(links & 16))
            $appendElement(result, element);
        else if (!$tryPutByValDirect(result, i, element)) {
            $putByValDirect(result, i, element);
            break;
        }
    }
    if (i < length)
        $throwTypeError("Array.prototype.filter callback must be a function");
    return result;
}

$linkTimeConstant;
export function reduceFrom(array, length, callback, accumulator, k)
{
    "use strict";

    if ($isJSArray(array)) {
        for (k >>>= 0; k < length; k += 1) {
            for (; k < length && $hasContiguousIndex(array, k); k += 1)
                accumulator = callback.$call($undefined, accumulator, $getOwnContiguousIndex(array, k), k, array);
            for (; k < length; k += 1) {
                var number = $getDoubleIndex(array, k);
                if (number !== number)
                    break;
                accumulator = callback.$call($undefined, accumulator, number, k, array);
            }
            if (k < length && k in array)
                accumulator = callback.$call($undefined, accumulator, array[k], k, array);
        }
        return accumulator;
    }

    while (k < length) {
        if (k in array)
            accumulator = callback.$call($undefined, accumulator, array[k], k, array);
        k += 1;
    }
    return accumulator;
}

export function reduce(callback /*, initialValue */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.reduce requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.reduce callback must be a function");

    var argumentCount = $argumentCount();
    if (length === 0 && argumentCount < 2)
        $throwTypeError("reduce of empty array with no initial value");

    var accumulator, k = 0;
    if (argumentCount > 1)
        accumulator = $argument(1);
    else {
        while (k < length && !(k in array))
            k += 1;
        if (k >= length)
            $throwTypeError("reduce of empty array with no initial value");
        accumulator = array[k++];
    }

    if (length > 0 && $isJSArray(array)) {
        for (; k < length && $hasContiguousIndex(array, k); k += 1)
            accumulator = callback.$call($undefined, accumulator, $getOwnContiguousIndex(array, k), k, array);
    }
    if (k >= length)
        return accumulator;
    var answer = $reduceFrom(array, length, callback, accumulator, k);
    return answer;
}

$linkTimeConstant;
export function reduceRightFrom(array, callback, accumulator, k)
{
    "use strict";

    if ($isJSArray(array)) {
        for (; k >= 0; k -= 1) {
            for (; k >= 0 && $hasContiguousIndex(array, k); k -= 1)
                accumulator = callback.$call($undefined, accumulator, $getOwnContiguousIndex(array, k), k, array);
            for (; k >= 0; k -= 1) {
                var number = $getDoubleIndex(array, k);
                if (number !== number)
                    break;
                accumulator = callback.$call($undefined, accumulator, number, k, array);
            }
            if (k >= 0 && k in array)
                accumulator = callback.$call($undefined, accumulator, array[k], k, array);
        }
        return accumulator;
    }

    while (k >= 0) {
        if (k in array)
            accumulator = callback.$call($undefined, accumulator, array[k], k, array);
        k -= 1;
    }
    return accumulator;
}

export function reduceRight(callback /*, initialValue */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.reduceRight requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.reduceRight callback must be a function");

    var accumulator, k = length - 1;
    if ($argumentCount() > 1)
        accumulator = $argument(1);
    else {
        while (k >= 0 && !(k in array))
            k -= 1;
        if (k < 0)
            $throwTypeError("reduceRight of empty array with no initial value");
        accumulator = array[k--];
    }

    if ($isJSArray(array)) {
        for (; k >= 0 && $hasContiguousIndex(array, k); k -= 1)
            accumulator = callback.$call($undefined, accumulator, $getOwnContiguousIndex(array, k), k, array);
    }
    if (k < 0)
        return accumulator;
    var answer = $reduceRightFrom(array, callback, accumulator, k);
    return answer;
}

$overriddenName = "every";
export function everyOfList(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.every requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.every callback must be a function");
    
    var thisArg = $argument(1);
    
    for (var i = 0; i < length; i++) {
        if (!(i in array))
            continue;
        if (!callback.$call(thisArg, array[i], i, array))
            return false;
    }
    
    return true;
}

$linkTimeConstant;
export function everyFrom(array, length, callback, thisArg, i)
{
    "use strict";

    if ($isJSArray(array)) {
        for (i >>>= 0; i < length; i++) {
            for (; i < length && $hasContiguousIndex(array, i); i++) {
                if (!callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array))
                    return false;
            }
            for (; i < length; i++) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                if (!callback.$call(thisArg, number, i, array))
                    return false;
            }
            if (i < length && i in array && !callback.$call(thisArg, array[i], i, array))
                return false;
        }
        return true;
    }

    for (; i < length; i++) {
        if (!(i in array))
            continue;
        if (!callback.$call(thisArg, array[i], i, array))
            return false;
    }
    
    return true;
}

export function every(callback /*, thisArg */)
{
    "use strict";

    var array = this;
    var length;
    if ($isJSArray(array))
        length = $toLength(array.length);
    else {
        array = $toObject(this, "Array.prototype.every requires that |this| not be null or undefined");
        length = $toLength(array.length);
    }

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.every callback must be a function");
    
    var thisArg = $argument(1);
    
    var i = 0;
    if (length > 0 && $isJSArray(array)) {
        for (; i < length && $hasContiguousIndex(array, i); i++) {
            if (!callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array))
                return false;
        }
    }
    if (i >= length)
        return true;
    var answer = $everyFrom(array, length, callback, thisArg, i);
    return answer;
}

$overriddenName = "forEach";
export function forEachOfList(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.forEach requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.forEach callback must be a function");
    
    var thisArg = $argument(1);
    
    for (var i = 0; i < length; i++) {
        if (i in array)
            callback.$call(thisArg, array[i], i, array);
    }
}

$linkTimeConstant;
export function forEachFrom(array, length, callback, thisArg, i)
{
    "use strict";

    if ($isJSArray(array)) {
        for (i >>>= 0; i < length; i++) {
            for (; i < length && $hasContiguousIndex(array, i); i++)
                callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array);
            for (; i < length; i++) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                callback.$call(thisArg, number, i, array);
            }
            if (i < length && i in array)
                callback.$call(thisArg, array[i], i, array);
        }
        return;
    }

    for (; i < length; i++) {
        if (i in array)
            callback.$call(thisArg, array[i], i, array);
    }
}

export function forEach(callback /*, thisArg */)
{
    "use strict";

    var array = this;
    var length;
    if ($isJSArray(array))
        length = $toLength(array.length);
    else {
        array = $toObject(this, "Array.prototype.forEach requires that |this| not be null or undefined");
        length = $toLength(array.length);
    }

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.forEach callback must be a function");
    
    var thisArg = $argument(1);
    
    var i = 0;
    if (length > 0 && $isJSArray(array)) {
        for (; i < length && $hasContiguousIndex(array, i); i++)
            callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array);
    }
    if (i >= length)
        return;
    $forEachFrom(array, length, callback, thisArg, i);
}

$alwaysInline;
$overriddenName = "filter";
export function filterOfList(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.filter requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.filter callback must be a function");
    
    var thisArg = $argument(1);
    // The row is for arrays of their own species: what is returned is made as a literal makes it.
    var result = [];

    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); i++) {
        var kept = $getOwnContiguousIndex(array, i);
        if (callback.$call(thisArg, kept, i, array))
            $appendElement(result, kept);
    }
    if (i < length)
        $filterFrom(array, length, callback, thisArg, result, i, result.length);
    return result;
}

$linkTimeConstant;
export function filterFrom(array, length, callback, thisArg, result, i, nextIndex)
{
    "use strict";

    if ($isJSArray(array) && $isJSArray(result)) {
        nextIndex >>>= 0;
        for (i >>>= 0; i < length; i++) {
            for (; i < length && $hasContiguousIndex(array, i); i++) {
                var kept = $getOwnContiguousIndex(array, i);
                if (callback.$call(thisArg, kept, i, array)) {
                    if (!$tryPutIndex(result, nextIndex, kept))
                        $putByValDirect(result, nextIndex, kept);
                    ++nextIndex;
                }
            }
            for (; i < length; i++) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                if (callback === $Boolean ? number !== 0 : callback.$call(thisArg, number, i, array)) {
                    if (!$tryPutIndex(result, nextIndex, number))
                        $putByValDirect(result, nextIndex, number);
                    ++nextIndex;
                }
            }
            if (i < length && i in array) {
                var element = array[i];
                if (callback.$call(thisArg, element, i, array)) {
                    if (!$tryPutIndex(result, nextIndex, element))
                        $putByValDirect(result, nextIndex, element);
                    ++nextIndex;
                }
            }
        }
        return result;
    }

    for (; i < length; i++) {
        if (!(i in array))
            continue;
        var current = array[i]
        if (callback.$call(thisArg, current, i, array)) {
            $putByValDirect(result, nextIndex, current);
            ++nextIndex;
        }
    }
    return result;
}

$linkTimeConstant;
$alwaysInline;
export function filterTruthy(array, length, result)
{
    "use strict";

    var nextIndex = 0;
    var i = 0;
    if ($isJSArray(array) && $isJSArray(result)) {
        var end = length >>> 0;
        for (; i < end && $hasContiguousIndex(array, i); i++) {
            var kept = $getOwnContiguousIndex(array, i);
            if (kept) {
                if (!$tryPutIndex(result, nextIndex, kept))
                    $putByValDirect(result, nextIndex, kept);
                ++nextIndex;
            }
        }
    }
    if (i < length)
        $filterFrom(array, length, $Boolean, undefined, result, i, nextIndex);
}

export function filter(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.filter requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.filter callback must be a function");
    
    var thisArg = $argument(1);
    var result = $newArrayWithSpecies(0, array, length);

    var nextIndex = 0;
    var i = nextIndex;
    if ($isJSArray(array) && $isJSArray(result)) {
        if (callback === $Boolean) {
            $filterTruthy(array, length, result);
            return result;
        }
        for (; i < length && $hasContiguousIndex(array, i); i++) {
            var kept = $getOwnContiguousIndex(array, i);
            if (callback.$call(thisArg, kept, i, array)) {
                if (!$tryPutIndex(result, nextIndex, kept))
                    $putByValDirect(result, nextIndex, kept);
                ++nextIndex;
            }
        }
    }
    if (i >= length)
        return result;
    // What is returned is what was made above, on either road: whoever copies the text knows it for that.
    $filterFrom(array, length, callback, thisArg, result, i, nextIndex);
    return result;
}

$overriddenName = "map";
export function mapOfList(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.map requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.map callback must be a function");
    
    var thisArg = $argument(1);
    var result = $newArrayWithSpecies(length, array, length, undefined);

    var i = 0;
    if (length > 0 && $isJSArray(array) && $isJSArray(result)) {
        for (; i < length && $hasContiguousIndex(array, i); i++) {
            var made = callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array);
            if (!$tryPutByValDirect(result, i, made)) {
                if (!$tryPutIndex(result, i, made))
                    $putByValDirect(result, i, made);
                i++;
                break;
            }
        }
    }
    if (i >= length)
        return result;
    $mapFrom(array, length, callback, thisArg, result, i);
    return result;
}

$linkTimeConstant;
export function mapFrom(array, length, callback, thisArg, result, i)
{
    "use strict";

    if ($isJSArray(array) && $isJSArray(result)) {
        for (i >>>= 0; i < length; i++) {
            for (; i < length && $hasContiguousIndex(array, i); i++) {
                var mapped = callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array);
                if (!$tryPutIndex(result, i, mapped))
                    $putByValDirect(result, i, mapped);
            }
            for (; i < length; i++) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                var made = callback.$call(thisArg, number, i, array);
                if (!$tryPutIndex(result, i, made))
                    $putByValDirect(result, i, made);
            }
            if (i < length && i in array) {
                var element = callback.$call(thisArg, array[i], i, array);
                if (!$tryPutIndex(result, i, element))
                    $putByValDirect(result, i, element);
            }
        }
        return result;
    }

    for (; i < length; i++) {
        if (!(i in array))
            continue;
        var mappedValue = callback.$call(thisArg, array[i], i, array);
        $putByValDirect(result, i, mappedValue);
    }
    return result;
}

export function map(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.map requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.map callback must be a function");
    
    var thisArg = $argument(1);
    var result = $newArrayWithSpecies(length, array, length, undefined);

    var i = 0;
    if (length > 0 && $isJSArray(array) && $isJSArray(result)) {
        for (; i < length && $hasContiguousIndex(array, i); i++) {
            var made = callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array);
            if (!$tryPutByValDirect(result, i, made) && !$tryPutIndex(result, i, made))
                $putByValDirect(result, i, made);
        }
    }
    if (i >= length)
        return result;
    $mapFrom(array, length, callback, thisArg, result, i);
    return result;
}

$overriddenName = "some";
export function someOfList(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.some requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.some callback must be a function");
    
    var thisArg = $argument(1);
    for (var i = 0; i < length; i++) {
        if (!(i in array))
            continue;
        if (callback.$call(thisArg, array[i], i, array))
            return true;
    }
    return false;
}

$linkTimeConstant;
export function someFrom(array, length, callback, thisArg, i)
{
    "use strict";

    if ($isJSArray(array)) {
        for (i >>>= 0; i < length; i++) {
            for (; i < length && $hasContiguousIndex(array, i); i++) {
                if (callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array))
                    return true;
            }
            for (; i < length; i++) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                if (callback.$call(thisArg, number, i, array))
                    return true;
            }
            if (i < length && i in array && callback.$call(thisArg, array[i], i, array))
                return true;
        }
        return false;
    }

    for (; i < length; i++) {
        if (!(i in array))
            continue;
        if (callback.$call(thisArg, array[i], i, array))
            return true;
    }
    return false;
}

export function some(callback /*, thisArg */)
{
    "use strict";

    var array = this;
    var length;
    if ($isJSArray(array))
        length = $toLength(array.length);
    else {
        array = $toObject(this, "Array.prototype.some requires that |this| not be null or undefined");
        length = $toLength(array.length);
    }

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.some callback must be a function");
    
    var thisArg = $argument(1);
    var i = 0;
    if (length > 0 && $isJSArray(array)) {
        for (; i < length && $hasContiguousIndex(array, i); i++) {
            if (callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array))
                return true;
        }
    }
    if (i >= length)
        return false;
    var answer = $someFrom(array, length, callback, thisArg, i);
    return answer;
}

$overriddenName = "find";
export function findOfList(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.find requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.find callback must be a function");
    
    var thisArg = $argument(1);
    for (var i = 0; i < length; i++) {
        var kValue = array[i];
        if (callback.$call(thisArg, kValue, i, array))
            return kValue;
    }
    return $undefined;
}

$linkTimeConstant;
export function findFrom(array, length, callback, thisArg, i)
{
    "use strict";

    if ($isJSArray(array)) {
        for (i >>>= 0; i < length; i++) {
            for (; i < length && $hasContiguousIndex(array, i); i++) {
                var own = $getOwnContiguousIndex(array, i);
                if (callback.$call(thisArg, own, i, array))
                    return own;
            }
            for (; i < length; i++) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                if (callback.$call(thisArg, number, i, array))
                    return number;
            }
            if (i < length) {
                var element = array[i];
                if (callback.$call(thisArg, element, i, array))
                    return element;
            }
        }
        return $undefined;
    }

    for (; i < length; i++) {
        var kValue = array[i];
        if (callback.$call(thisArg, kValue, i, array))
            return kValue;
    }
    return $undefined;
}

export function find(callback /*, thisArg */)
{
    "use strict";

    var array = this;
    var length;
    if ($isJSArray(array))
        length = $toLength(array.length);
    else {
        array = $toObject(this, "Array.prototype.find requires that |this| not be null or undefined");
        length = $toLength(array.length);
    }

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.find callback must be a function");
    
    var thisArg = $argument(1);
    var i = 0;
    if (length > 0 && $isJSArray(array)) {
        for (; i < length && $hasContiguousIndex(array, i); i++) {
            var own = $getOwnContiguousIndex(array, i);
            if (callback.$call(thisArg, own, i, array))
                return own;
        }
    }
    if (i >= length)
        return $undefined;
    var answer = $findFrom(array, length, callback, thisArg, i);
    return answer;
}

$linkTimeConstant;
export function findLastFrom(array, callback, thisArg, i)
{
    "use strict";

    if ($isJSArray(array)) {
        for (; i >= 0; i--) {
            for (; i >= 0 && $hasContiguousIndex(array, i); i--) {
                var own = $getOwnContiguousIndex(array, i);
                if (callback.$call(thisArg, own, i, array))
                    return own;
            }
            for (; i >= 0; i--) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                if (callback.$call(thisArg, number, i, array))
                    return number;
            }
            if (i >= 0) {
                var element = array[i];
                if (callback.$call(thisArg, element, i, array))
                    return element;
            }
        }
        return $undefined;
    }

    for (; i >= 0; i--) {
        var kValue = array[i];
        if (callback.$call(thisArg, kValue, i, array))
            return kValue;
    }
    return $undefined;
}

export function findLast(callback /*, thisArg */)
{
    "use strict";

    var array = this;
    var length;
    if ($isJSArray(array))
        length = $toLength(array.length);
    else {
        array = $toObject(this, "Array.prototype.findLast requires that |this| not be null or undefined");
        length = $toLength(array.length);
    }

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.findLast callback must be a function");

    var thisArg = $argument(1);
    var i = length - 1;
    if ($isJSArray(array)) {
        for (; i >= 0 && $hasContiguousIndex(array, i); i--) {
            var own = $getOwnContiguousIndex(array, i);
            if (callback.$call(thisArg, own, i, array))
                return own;
        }
    }
    if (i < 0)
        return $undefined;
    var answer = $findLastFrom(array, callback, thisArg, i);
    return answer;
}

$overriddenName = "findIndex";
export function findIndexOfList(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.findIndex requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.findIndex callback must be a function");
    
    var thisArg = $argument(1);
    for (var i = 0; i < length; i++) {
        if (callback.$call(thisArg, array[i], i, array))
            return i;
    }
    return -1;
}

$linkTimeConstant;
export function findIndexFrom(array, length, callback, thisArg, i)
{
    "use strict";

    if ($isJSArray(array)) {
        for (i >>>= 0; i < length; i++) {
            for (; i < length && $hasContiguousIndex(array, i); i++) {
                if (callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array))
                    return i;
            }
            for (; i < length; i++) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                if (callback.$call(thisArg, number, i, array))
                    return i;
            }
            if (i < length && callback.$call(thisArg, array[i], i, array))
                return i;
        }
        return -1;
    }

    for (; i < length; i++) {
        if (callback.$call(thisArg, array[i], i, array))
            return i;
    }
    return -1;
}

export function findIndex(callback /*, thisArg */)
{
    "use strict";

    var array = this;
    var length;
    if ($isJSArray(array))
        length = $toLength(array.length);
    else {
        array = $toObject(this, "Array.prototype.findIndex requires that |this| not be null or undefined");
        length = $toLength(array.length);
    }

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.findIndex callback must be a function");
    
    var thisArg = $argument(1);
    var i = 0;
    if (length > 0 && $isJSArray(array)) {
        for (; i < length && $hasContiguousIndex(array, i); i++) {
            if (callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array))
                return i;
        }
    }
    if (i >= length)
        return -1;
    var answer = $findIndexFrom(array, length, callback, thisArg, i);
    return answer;
}

$linkTimeConstant;
export function findLastIndexFrom(array, callback, thisArg, i)
{
    "use strict";

    if ($isJSArray(array)) {
        for (; i >= 0; i--) {
            for (; i >= 0 && $hasContiguousIndex(array, i); i--) {
                if (callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array))
                    return i;
            }
            for (; i >= 0; i--) {
                var number = $getDoubleIndex(array, i);
                if (number !== number)
                    break;
                if (callback.$call(thisArg, number, i, array))
                    return i;
            }
            if (i >= 0 && callback.$call(thisArg, array[i], i, array))
                return i;
        }
        return -1;
    }

    for (; i >= 0; i--) {
        if (callback.$call(thisArg, array[i], i, array))
            return i;
    }
    return -1;
}

export function findLastIndex(callback /*, thisArg */)
{
    "use strict";

    var array = this;
    var length;
    if ($isJSArray(array))
        length = $toLength(array.length);
    else {
        array = $toObject(this, "Array.prototype.findLastIndex requires that |this| not be null or undefined");
        length = $toLength(array.length);
    }

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.findLastIndex callback must be a function");

    var thisArg = $argument(1);
    var i = length - 1;
    if ($isJSArray(array)) {
        for (; i >= 0 && $hasContiguousIndex(array, i); i--) {
            if (callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array))
                return i;
        }
    }
    if (i < 0)
        return -1;
    var answer = $findLastIndexFrom(array, callback, thisArg, i);
    return answer;
}

$linkTimeConstant;
export function flatIntoArray(target, source, sourceLength, sourceIndex, targetIndex, depth)
{
    "use strict";

    targetIndex = $toLength(targetIndex);
    if (depth <= 0 && sourceLength > 0 && $isJSArray(source) && $isJSArray(target)) {
        for (; sourceIndex < sourceLength && $hasContiguousIndex(source, sourceIndex); ++sourceIndex) {
            var taken = $getOwnContiguousIndex(source, sourceIndex);
            if (!$tryPutIndex(target, targetIndex, taken)) {
                if (targetIndex >= $MAX_SAFE_INTEGER)
                    $throwTypeError("flatten array exceeds 2**53 - 1");
                $putByValDirect(target, targetIndex, taken);
            }
            ++targetIndex;
        }
        for (; sourceIndex < sourceLength; ++sourceIndex) {
            var number = $getDoubleIndex(source, sourceIndex);
            if (number !== number)
                break;
            if (!$tryPutIndex(target, targetIndex, number)) {
                if (targetIndex >= $MAX_SAFE_INTEGER)
                    $throwTypeError("flatten array exceeds 2**53 - 1");
                $putByValDirect(target, targetIndex, number);
            }
            ++targetIndex;
        }
    }
    for (; sourceIndex < sourceLength; ++sourceIndex) {
        if (sourceIndex in source) {
            var element = source[sourceIndex];
            if (depth > 0 && $isArray(element))
                targetIndex = $flatIntoArray(target, element, $toLength(element.length), 0, targetIndex, depth - 1);
            else {
                if (targetIndex >= $MAX_SAFE_INTEGER)
                    $throwTypeError("flatten array exceeds 2**53 - 1");
                $putByValDirect(target, targetIndex, element);
                ++targetIndex;
            }
        }
    }
    return targetIndex;
}

$linkTimeConstant;
export function flatIntoArrayWithCallback(target, source, sourceLength, sourceIndex, targetIndex, callback, thisArg)
{
    "use strict";

    if ($isJSArray(source) && $isJSArray(target)) {
        for (sourceIndex >>>= 0; sourceIndex < sourceLength; ++sourceIndex) {
            for (; sourceIndex < sourceLength && $hasContiguousIndex(source, sourceIndex); ++sourceIndex) {
                var made = callback.$call(thisArg, $getOwnContiguousIndex(source, sourceIndex), sourceIndex, source);
                if ($isArray(made))
                    targetIndex = $toLength($flatIntoArray(target, made, $toLength(made.length), 0, targetIndex, 0));
                else {
                    if (!$tryPutIndex(target, targetIndex, made))
                        $putByValDirect(target, targetIndex, made);
                    ++targetIndex;
                }
            }
            for (; sourceIndex < sourceLength; ++sourceIndex) {
                var number = $getDoubleIndex(source, sourceIndex);
                if (number !== number)
                    break;
                var madeOfNumber = callback.$call(thisArg, number, sourceIndex, source);
                if ($isArray(madeOfNumber))
                    targetIndex = $toLength($flatIntoArray(target, madeOfNumber, $toLength(madeOfNumber.length), 0, targetIndex, 0));
                else {
                    if (!$tryPutIndex(target, targetIndex, madeOfNumber))
                        $putByValDirect(target, targetIndex, madeOfNumber);
                    ++targetIndex;
                }
            }
            if (sourceIndex < sourceLength && sourceIndex in source) {
                var madeOfOther = callback.$call(thisArg, source[sourceIndex], sourceIndex, source);
                if ($isArray(madeOfOther))
                    targetIndex = $toLength($flatIntoArray(target, madeOfOther, $toLength(madeOfOther.length), 0, targetIndex, 0));
                else {
                    $putByValDirect(target, targetIndex, madeOfOther);
                    ++targetIndex;
                }
            }
        }
        return target;
    }
    for (; sourceIndex < sourceLength; ++sourceIndex) {
        if (sourceIndex in source) {
            var element = callback.$call(thisArg, source[sourceIndex], sourceIndex, source);
            if ($isArray(element))
                targetIndex = $flatIntoArray(target, element, $toLength(element.length), 0, targetIndex, 0);
            else {
                if (targetIndex >= $MAX_SAFE_INTEGER)
                    $throwTypeError("flatten array exceeds 2**53 - 1");
                $putByValDirect(target, targetIndex, element);
                ++targetIndex;
            }
        }
    }
    return target;
}

// What a callback of flatMap returned. A short array gives its elements at known places: one that is written in the callback need not be made.
$linkTimeConstant;
$alwaysInline;
export function appendFlattened(target, made, end)
{
    "use strict";

    if (!$isArray(made)) {
        if (!$tryPutIndex(target, end, made))
            $putByValDirect(target, end, made);
        return end + 1;
    }
    if ($isJSArray(made)) {
        var count = made.length;
        if (count === 0)
            return end;
        if (count < 4 && $hasContiguousIndex(made, 0) && (count === 1 || ($hasContiguousIndex(made, 1) && (count === 2 || $hasContiguousIndex(made, 2))))) {
            var first = $getOwnContiguousIndex(made, 0);
            if (!$tryPutIndex(target, end, first))
                $putByValDirect(target, end, first);
            if (count === 1)
                return end + 1;
            var second = $getOwnContiguousIndex(made, 1);
            if (!$tryPutIndex(target, end + 1, second))
                $putByValDirect(target, end + 1, second);
            if (count === 2)
                return end + 2;
            var third = $getOwnContiguousIndex(made, 2);
            if (!$tryPutIndex(target, end + 2, third))
                $putByValDirect(target, end + 2, third);
            return end + 3;
        }
    }
    var answer = $flatIntoArray(target, made, $toLength(made.length), 0, end, 0);
    return answer;
}

$linkTimeConstant;
export function flatMapOfPristineArray(array, callback, thisArg)
{
    "use strict";

    var length = array.length;
    var result = [];
    var targetIndex = 0;
    var i = 0;
    for (; i < length && $hasContiguousIndex(array, i); ++i) {
        var made = callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array);
        if ($isJSArray(made)) {
            var count = made.length;
            var k = 0;
            for (; k < count && $hasContiguousIndex(made, k); ++k) {
                var inner = $getOwnContiguousIndex(made, k);
                if (!$tryPutIndex(result, targetIndex, inner))
                    $putByValDirect(result, targetIndex, inner);
                ++targetIndex;
            }
            if (k < count)
                targetIndex = $toLength($flatIntoArray(result, made, count, k, targetIndex, 0));
        } else if ($isArray(made))
            targetIndex = $toLength($flatIntoArray(result, made, $toLength(made.length), 0, targetIndex, 0));
        else {
            if (!$tryPutIndex(result, targetIndex, made))
                $putByValDirect(result, targetIndex, made);
            ++targetIndex;
        }
    }
    if (i < length)
        $flatIntoArrayWithCallback(result, array, length, i, targetIndex, callback, thisArg);
    return result;
}

// The rows of the table name this one for a list that is as it was made. It is hidden as the two functions that flatMap calls are.
$overriddenName = "flatMap";
$visibility = "Private";
export function flatMapOfList(callback /*, thisArg */)
{
    "use strict";

    var array = $toObject(this, "Array.prototype.flatMap requires that |this| not be null or undefined");
    var length = $toLength(array.length);
    var thisArg = $argument(1);
    var result = [];
    var targetIndex = 0;
    var i = 0;
    if (length > 0 && $isJSArray(array)) {
        for (; i < length && $hasContiguousIndex(array, i); ++i)
            targetIndex = $toLength($appendFlattened(result, callback.$call(thisArg, $getOwnContiguousIndex(array, i), i, array), targetIndex));
    }
    if (i < length)
        $flatIntoArrayWithCallback(result, array, length, i, targetIndex, callback, thisArg);
    return result;
}

// Both ends are calls of hidden functions in tail position, as upstream's one: a stack trace that a callback takes lists no frame of flatMap, and the errors of flatMap itself do.
export function flatMap(callback)
{
    "use strict";

    if ($isPristineArray(this) && $isCallable(callback))
        return $flatMapOfPristineArray(this, callback, $argument(1));

    var array = $toObject(this, "Array.prototype.flatMap requires that |this| not be null or undefined");
    var length = $toLength(array.length);

    if (!$isCallable(callback))
        $throwTypeError("Array.prototype.flatMap callback must be a function");

    var thisArg = $argument(1);

    var result = $newArrayWithSpecies(0, array);

    return $flatIntoArrayWithCallback(result, array, length, 0, 0, callback, thisArg);
}

export function at(index)
{
    "use strict";

    var array = this;
    var length;
    if ($isJSArray(array)) {
        if (typeof index === "number" && (index | 0) === index) {
            var place = index | 0;
            var count = array.length;
            if (place < 0)
                place = (place + count) | 0;
            if (place >= count)
                return $undefined;
            if (place >= 0) {
                if ($hasContiguousIndex(array, place))
                    return $getOwnContiguousIndex(array, place);
                var number = $getDoubleIndex(array, place);
                if (number === number)
                    return number;
            }
        }
        length = $toLength(array.length);
    } else {
        array = $toObject(this, "Array.prototype.at requires that |this| not be null or undefined");
        length = $toLength(array.length);
    }

    var k = typeof index === "number" && (index | 0) === index ? index | 0 : $toIntegerOrInfinity(index);
    if (k < 0)
        k += length;

    return (k >= 0 && k < length) ? array[k] : $undefined;
}
