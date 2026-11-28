export function from(items /*, mapFn, thisArg */)
{
    "use strict";

    var mapFn = $argument(1);

    var thisArg;

    if (mapFn !== undefined) {
        if (!$isCallable(mapFn))
            $throwTypeError("Array.from requires that the second argument, when provided, be a function");

        thisArg = $argument(2);
    }

    var arrayLike = $toObject(items, "Array.from requires an array-like object - not null or undefined");

    if (mapFn === undefined) {
        var fastResult = $arrayFromFastWithoutMapFn(this, arrayLike);
        if (fastResult)
            return fastResult;
    }

    var iteratorMethod = items.$$iterator;
    if (!$isUndefinedOrNull(iteratorMethod)) {
        if (!$isCallable(iteratorMethod))
            $throwTypeError("Array.from requires that the property of the first argument, items[Symbol.iterator], when exists, be a function");

        var result = this !== $Array && $isConstructor(this) ? new this() : [];
        if (!$isObject(result))
            $throwTypeError("Array.from requires that |this| constructs an object");

        var k = 0;
        var iterator = iteratorMethod.$call(items);
        if (!$isObject(iterator))
            $throwTypeError("Iterator result interface is not an object.");
        var nextMethod = iterator.next;

        // The next of a generator asks by itself whether it was given one. Nobody sees the object that a call of it answers with.
        if (nextMethod === $generatorProtoNext) {
            for (;;) {
                var step = $stepGenerator(iterator, $resumeModeNext, undefined, $generatorStepCheckIsGenerator);
                var yielded = $resultAt(step, $stepGeneratorResultValue);
                var status = $resultAt(step, $stepGeneratorResultStatus);
                // It is running, has ended, or is no generator: the method answers.
                if (status === $generatorStepStatusNotStepped)
                    break;
                if (status === $generatorStepStatusCompleted) {
                    result.length = k;
                    return result;
                }
                if (k >= $MAX_SAFE_INTEGER)
                    $ifAbruptCloseIterator(iterator, $throwTypeError("Length exceeded the maximum array length"));
                var answer = yielded;
                if (mapFn !== undefined)
                    $ifAbruptCloseIterator(iterator, (answer = thisArg === undefined ? mapFn(yielded, k) : mapFn.$call(thisArg, yielded, k)));
                if (!$isJSArray(result) || !$tryPutIndex(result, k, answer))
                    $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, answer));
                k += 1;
            }
        }

        // Nobody sees the object that a call of the own method of an iterator of the engine answers with.
        if (nextMethod === $setIteratorProtoNext && $isSetIterator(iterator)) {
            for (;;) {
                const step = $stepSetIterator(iterator);
                const stepped = $resultAt(step, $stepArrayIteratorResultValue);
                if ($resultAt(step, $stepArrayIteratorResultDone)) {
                    result.length = k;
                    return result;
                }
                if (k >= $MAX_SAFE_INTEGER)
                    $ifAbruptCloseIterator(iterator, $throwTypeError("Length exceeded the maximum array length"));
                var value = stepped;
                if (mapFn !== undefined)
                    $ifAbruptCloseIterator(iterator, (value = thisArg === undefined ? mapFn(stepped, k) : mapFn.$call(thisArg, stepped, k)));
                if (!$isJSArray(result) || !$tryPutIndex(result, k, value))
                    $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, value));
                k += 1;
            }
        }

        if (nextMethod === $arrayIteratorProtoNext && $isArrayIterator(iterator)) {
            // The values of a list are read by index, and the length is read for each of them.
            if (iteratorMethod === $arrayProtoValues && mapFn !== undefined && $isJSArray(items) && $isJSArray(result)) {
                for (; k < items.length; k++) {
                    var element = $hasContiguousIndex(items, k) ? $getOwnContiguousIndex(items, k) : items[k];
                    var made;
                    $ifAbruptCloseIterator(iterator, (made = thisArg === undefined ? mapFn(element, k) : mapFn.$call(thisArg, element, k)));
                    if (!$tryPutIndex(result, k, made))
                        $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, made));
                }
                result.length = k;
                return result;
            }
            if (iteratorMethod === $typedArrayProtoValues && $isTypedArrayView(items) && $isJSArray(result)) {
                for (; k < $typedArrayLength(items); k++) {
                    var number = items[k];
                    if (mapFn !== undefined)
                        $ifAbruptCloseIterator(iterator, (number = thisArg === undefined ? mapFn(number, k) : mapFn.$call(thisArg, number, k)));
                    if (!$tryPutIndex(result, k, number))
                        $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, number));
                }
                result.length = k;
                return result;
            }
            for (;;) {
                const step = $stepArrayIterator(iterator);
                const stepped = $resultAt(step, $stepArrayIteratorResultValue);
                if ($resultAt(step, $stepArrayIteratorResultDone)) {
                    result.length = k;
                    return result;
                }
                if (k >= $MAX_SAFE_INTEGER)
                    $ifAbruptCloseIterator(iterator, $throwTypeError("Length exceeded the maximum array length"));
                var value = stepped;
                if (mapFn !== undefined)
                    $ifAbruptCloseIterator(iterator, (value = thisArg === undefined ? mapFn(stepped, k) : mapFn.$call(thisArg, stepped, k)));
                if (!$isJSArray(result) || !$tryPutIndex(result, k, value))
                    $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, value));
                k += 1;
            }
        }

        if (nextMethod === $mapIteratorProtoNext && $isMapIterator(iterator)) {
            for (;;) {
                const step = $stepMapIterator(iterator);
                const stepped = $resultAt(step, $stepArrayIteratorResultValue);
                if ($resultAt(step, $stepArrayIteratorResultDone)) {
                    result.length = k;
                    return result;
                }
                if (k >= $MAX_SAFE_INTEGER)
                    $ifAbruptCloseIterator(iterator, $throwTypeError("Length exceeded the maximum array length"));
                var value = stepped;
                if (mapFn !== undefined)
                    $ifAbruptCloseIterator(iterator, (value = thisArg === undefined ? mapFn(stepped, k) : mapFn.$call(thisArg, stepped, k)));
                if (!$isJSArray(result) || !$tryPutIndex(result, k, value))
                    $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, value));
                k += 1;
            }
        }

        if (nextMethod === $regExpStringIteratorProtoNext && $isRegExpStringIterator(iterator)) {
            for (;;) {
                var match = $stepMatchIterator(iterator);
                if (match === null) {
                    result.length = k;
                    return result;
                }
                if (k >= $MAX_SAFE_INTEGER)
                    $ifAbruptCloseIterator(iterator, $throwTypeError("Length exceeded the maximum array length"));
                if (mapFn !== undefined)
                    $ifAbruptCloseIterator(iterator, (match = thisArg === undefined ? mapFn(match, k) : mapFn.$call(thisArg, match, k)));
                if (!$isJSArray(result) || !$tryPutIndex(result, k, match))
                    $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, match));
                k += 1;
            }
        }

        for (;;) {
            var next = $iteratorGenericNext(nextMethod, iterator);
            if (next.done)
                break;
            var value = next.value;
            if (k >= $MAX_SAFE_INTEGER)
                $ifAbruptCloseIterator(iterator, $throwTypeError("Length exceeded the maximum array length"));
            if (mapFn === undefined)
                $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, value));
            else
                $ifAbruptCloseIterator(iterator, $putByValDirect(result, k, thisArg === undefined ? mapFn(value, k) : mapFn.$call(thisArg, value, k)));
            k += 1;
        }

        result.length = k;
        return result;
    }

    var arrayLikeLength = $toLength(arrayLike.length);

    var result = this !== $Array && $isConstructor(this) ? new this(arrayLikeLength) : $newArrayWithSize(arrayLikeLength);
    if (!$isObject(result))
        $throwTypeError("Array.from requires that |this| constructs an object");

    var k = 0;
    while (k < arrayLikeLength) {
        var value = arrayLike[k];
        if (mapFn === undefined)
            $putByValDirect(result, k, value);
        else
            $putByValDirect(result, k, thisArg === undefined ? mapFn(value, k) : mapFn.$call(thisArg, value, k));
        k += 1;
    }

    result.length = arrayLikeLength;
    return result;
}

$linkTimeConstant;
$visibility = "PrivateRecursive";
export async function defaultAsyncFromAsyncIterator(iterator, mapFn, thisArg)
{
    "use strict";

    if (!$isObject(iterator))
        $throwTypeError("Iterator result interface is not an object.");

    var nextMethod = iterator.next;

    var result = this !== $Array && $isConstructor(this) ? new this() : [];
    if (!$isObject(result))
        $throwTypeError("Array.fromAsync requires that |this| constructs an object");

    var k = 0;
    for (;;) {
        var nextResult = await nextMethod.$call(iterator);
        if (!$isObject(nextResult))
            $throwTypeError("Iterator result interface is not an object.");
        if (nextResult.done)
            break;

        var value = nextResult.value;
        try {
            if (k >= $MAX_SAFE_INTEGER)
                $throwTypeError("Length exceeded the maximum array length");
            if (mapFn === undefined)
                $putByValDirect(result, k, value);
            else
                $putByValDirect(result, k, await (thisArg === undefined ? mapFn(value, k) : mapFn.$call(thisArg, value, k)));
        } catch (error) {
            try {
                var returnMethod = iterator.return;
                if (!$isUndefinedOrNull(returnMethod)) {
                    var returnResult = await returnMethod.$call(iterator);
                    if (!$isObject(returnResult))
                        $throwTypeError("Iterator result interface is not an object.");
                }
            } catch { }
            throw error;
        }
        k += 1;
    }

    result.length = k;
    return result;
}

$linkTimeConstant;
$visibility = "PrivateRecursive";
export async function defaultAsyncFromAsyncArrayLike(asyncItems, mapFn, thisArg)
{
    "use strict";

    var arrayLike = $toObject(asyncItems, "Array.fromAsync requires an array-like object - not null or undefined");

    var arrayLikeLength = $toLength(arrayLike.length);

    var result = this !== $Array && $isConstructor(this) ? new this(arrayLikeLength) : $newArrayWithSize(arrayLikeLength);
    if (!$isObject(result))
        $throwTypeError("Array.fromAsync requires that |this| constructs an object");

    var k = 0;
    while (k < arrayLikeLength) {
        var value = await arrayLike[k];
        if (mapFn === undefined)
            $putByValDirect(result, k, value);
        else
            $putByValDirect(result, k, await (thisArg === undefined ? mapFn(value, k) : mapFn.$call(thisArg, value, k)));
        k += 1;
    }

    result.length = arrayLikeLength;
    return result;
}

export function fromAsync(asyncItems  /*, mapFn, thisArg */)
{
    "use strict";

    try {
        var mapFn = $argument(1);

        var thisArg;

        if (mapFn !== undefined) {
            if (!$isCallable(mapFn))
                $throwTypeError("Array.fromAsync requires that the second argument, when provided, be a function");

            thisArg = $argument(2);
        }

        var usingSyncIterator;
        var usingAsyncIterator = asyncItems.$$asyncIterator;
        if (!$isUndefinedOrNull(usingAsyncIterator)) {
            if (!$isCallable(usingAsyncIterator))
                $throwTypeError("Array.fromAsync requires that the property of the first argument, items[Symbol.asyncIterator], when exists, be a function");
        } else {
            usingSyncIterator = asyncItems.$$iterator;
            if (!$isUndefinedOrNull(usingSyncIterator)) {
                if (!$isCallable(usingSyncIterator))
                    $throwTypeError("Array.fromAsync requires that the property of the first argument, items[Symbol.iterator], when exists, be a function");
            }
        }

        if (!$isUndefinedOrNull(usingAsyncIterator)) {
            var iterator = usingAsyncIterator.$call(asyncItems);
            return $defaultAsyncFromAsyncIterator.$call(this, iterator, mapFn, thisArg);
        }

        if (!$isUndefinedOrNull(usingSyncIterator)) {
            var iterator = usingSyncIterator.$call(asyncItems);
            return $defaultAsyncFromAsyncIterator.$call(this, $asyncFromSyncIteratorCreate(iterator), mapFn, thisArg);
        }

        return $defaultAsyncFromAsyncArrayLike.$call(this, asyncItems, mapFn, thisArg);
    } catch (reason) {
        var promise = $newPromise();
        $rejectPromiseWithFirstResolvingFunctionCallCheck(promise, reason);
        return promise;
    }
}
