// As in builtins/JSIteratorConstructor.js, but for the steps. Where that file calls `next`, reads `done` off the answer and then `value` unless it is done, this one asks iteratorStepValue() for the three at once,
// which makes no answer for an iterator of the engine. What it hands back is asked whether it is the mark of a closed iterator in front of anything else.

$linkTimeConstant;
export function getIteratorFlattenable(obj, rejectStrings)
{
    "use strict";

    if (!$isObject(obj) && (rejectStrings || typeof obj !== "string"))
        $throwTypeError("GetIteratorFlattenable expects its first argument to be an object" + (rejectStrings ? "" : " or a string"));
    var method = obj.$$iterator;
    var iterator;
    if ($isUndefinedOrNull(method))
        iterator = obj;
    else
        iterator = method.$call(obj);
    if (!$isObject(iterator))
        $throwTypeError("Iterator is not an object");
    return iterator;
}

$linkTimeConstant;
export function getIteratorSync(obj)
{
    "use strict";

    var method = obj.$$iterator;
    if ($isUndefinedOrNull(method))
        $throwTypeError("Symbol.iterator is not defined");
    if (!$isCallable(method))
        $throwTypeError("Symbol.iterator is not callable");

    var iterator = method.$call(obj);
    if (!$isObject(iterator))
        $throwTypeError("Iterator is not an object");
    return iterator;
}

export function from(value)
{
    "use strict";

    var iterator = $getIteratorFlattenable(value, /* rejectStrings: */ false);
    var iteratorNextMethod = iterator.next;
    if ($instanceOf(iterator, $Iterator.prototype))
        return iterator;

    return $wrapForValidIteratorCreate(iterator, iteratorNextMethod);
}

export function concat(/* ...items */)
{
    "use strict";

    var argumentCount = $argumentCount();

    var openMethods: any[] = [];
    var iterables: any[] = [];
    for (var i = 0; i < argumentCount; ++i) {
        var iterable = arguments[i];
        if (!$isObject(iterable))
            $throwTypeError("Iterator.concat expects all arguments to be objects");

        var openMethod = iterable.$$iterator;
        if (!$isCallable(openMethod))
            $throwTypeError("Iterator.concat expects all arguments to be iterable");

        $arrayPush(openMethods, openMethod);
        $arrayPush(iterables, iterable);
    }

    var generator = (function*() {
        for (var i = 0; i < argumentCount; ++i) {
            var iterator = openMethods[i].$call(iterables[i]);
            if (!$isObject(iterator))
                $throwTypeError("Iterator.concat expects all arguments to be iterable");

            var nextMethod = iterator.next;

            for (;;) {
                var value = $iteratorStepValue(nextMethod, iterator);
                if ($isCollectionIteratorClosedMark(value))
                    break;

                $ifAbruptCloseIterator(iterator, (
                    yield value
                ));
            }
        }
    })();

    return $iteratorHelperCreate(generator, null);
}

$linkTimeConstant;
export function iteratorZip(iters, nextMethods, mode, padding, finishResults)
{
    "use strict";

    var iterCount = iters.length;

    var openIters = [];
    for (var i = 0; i < iterCount; ++i)
        $arrayPush(openIters, iters[i]);

    // The one underlying iterator of the helper: its return() closes every iterator that is still open, also where the helper is closed in front of its first step.
    var underlyingIterators = $Object.$create(null);
    underlyingIterators.return = function () {
        $iteratorCloseAllNormal(openIters);
        return { done: true, value: undefined };
    };

    var generator = (function*() {
        if (iterCount === 0)
            return;

        for (;;) {
            var results = [];

            for (var i = 0; i < iterCount; ++i) {
                var iter = iters[i];
                var result;

                if (iter === null)
                    result = padding[i];
                else {
                    var stepDone;
                    var stepValue;
                    try {
                        stepValue = $iteratorStepValue(nextMethods[i], iter);
                        stepDone = $isCollectionIteratorClosedMark(stepValue);
                    } catch (e) {
                        $removeFirstFromList(openIters, iter);
                        $closeAllIterators(openIters);
                        throw e;
                    }

                    if (stepDone) {
                        $removeFirstFromList(openIters, iter);

                        if (mode === "shortest") {
                            $iteratorCloseAllNormal(openIters);
                            return;
                        }

                        if (mode === "strict") {
                            if (i !== 0) {
                                $closeAllIterators(openIters);
                                $throwTypeError("Iterator.zip strict mode requires all iterables to have the same number of elements");
                            }

                            for (var k = 1; k < iterCount; ++k) {
                                var otherIter = iters[k];
                                var otherDone;
                                try {
                                    // Of this answer `value` is never read.
                                    otherDone = $iteratorGenericNext(nextMethods[k], otherIter).done;
                                } catch (e) {
                                    $removeFirstFromList(openIters, otherIter);
                                    $closeAllIterators(openIters);
                                    throw e;
                                }

                                if (otherDone)
                                    $removeFirstFromList(openIters, otherIter);
                                else {
                                    $closeAllIterators(openIters);
                                    $throwTypeError("Iterator.zip strict mode requires all iterables to have the same number of elements");
                                }
                            }

                            return;
                        }

                        if (openIters.length === 0)
                            return;
                        iters[i] = null;
                        result = padding[i];
                    } else
                        result = stepValue;
                }

                $arrayPush(results, result);
            }

            results = finishResults(results);

            $ifAbruptCloseIterator(underlyingIterators, (
                yield results
            ));
        }
    })();

    return $iteratorHelperCreate(generator, underlyingIterators);
}

$linkTimeConstant;
export function getOptionsObject(options)
{
    if (options === undefined)
        return $Object.$create(null);

    if ($isObject(options))
        return options;

    $throwTypeError("Expected object or null for options argument");
}

$linkTimeConstant;
export function closeAllIterators(openIters)
{
    "use strict";

    for (var i = openIters.length - 1; i >= 0; --i) {
        try {
            $iteratorGenericClose(openIters[i]);
        } catch (e) { }
    }
}

$linkTimeConstant;
export function iteratorCloseAllNormal(openIters)
{
    "use strict";

    var error;
    var hasError = false;
    for (var i = openIters.length - 1; i >= 0; --i) {
        try {
            $iteratorGenericClose(openIters[i]);
        } catch (e) {
            if (!hasError) {
                hasError = true;
                error = e;
            }
        }
    }
    if (hasError)
        throw error;
}

$linkTimeConstant;
export function removeFirstFromList(list, value)
{
    "use strict";

    var length = list.length;
    var shifting = false;
    for (var i = 0; i < length; ++i) {
        if (shifting)
            $putByValDirect(list, i - 1, list[i]);
        else if (list[i] === value)
            shifting = true;
    }
    if (shifting)
        list.length = length - 1;
}

export function zip(iterables /*, options */)
{
    "use strict";

    var options = $argument(1);

    if (!$isObject(iterables))
        $throwTypeError("Iterator.zip expects its first argument to be an object");

    options = $getOptionsObject(options);

    var mode = options.mode;
    if (mode === undefined)
        mode = "shortest";
    if (mode !== "shortest" && mode !== "longest" && mode !== "strict")
        $throwTypeError("Expected \"shortest\", \"longest\" or \"strict\" for options.mode");

    var paddingOption = undefined;
    if (mode === "longest") {
        paddingOption = options.padding;
        if (paddingOption !== undefined && !$isObject(paddingOption))
            $throwTypeError("Expected options.padding to be an object");
    }

    var iters = [];
    var nextMethods = [];
    var padding = [];

    var inputIter = $getIteratorSync(iterables);
    var inputIterNextMethod = inputIter.next;

    for (;;) {
        var done;
        var value;
        try {
            value = $iteratorStepValue(inputIterNextMethod, inputIter);
            done = $isCollectionIteratorClosedMark(value);
        } catch (e) {
            $closeAllIterators(iters);
            throw e;
        }

        if (done)
            break;

        var iter;
        var iterNextMethod;
        try {
            iter = $getIteratorFlattenable(value, /* rejectStrings: */ true);
            iterNextMethod = iter.next;
        } catch (e) {
            $closeAllIterators(iters);
            try {
                $iteratorGenericClose(inputIter);
            } catch (e2) { }
            throw e;
        }

        $arrayPush(iters, iter);
        $arrayPush(nextMethods, iterNextMethod);
    }

    var iterCount = iters.length;

    if (mode === "longest") {
        if (paddingOption === undefined) {
            for (var i = 0; i < iterCount; ++i)
                $arrayPush(padding, undefined);
        } else {
            var paddingIter;
            var paddingIterNextMethod;
            try {
                paddingIter = $getIteratorSync(paddingOption);
                paddingIterNextMethod = paddingIter.next;
            } catch (e) {
                $closeAllIterators(iters);
                throw e;
            }

            var usingIterator = true;

            for (var i = 0; i < iterCount; ++i) {
                if (usingIterator) {
                    var paddingDone;
                    var paddingValue;
                    try {
                        paddingValue = $iteratorStepValue(paddingIterNextMethod, paddingIter);
                        paddingDone = $isCollectionIteratorClosedMark(paddingValue);
                    } catch (e) {
                        $closeAllIterators(iters);
                        throw e;
                    }

                    if (paddingDone)
                        usingIterator = false;
                    else
                        $arrayPush(padding, paddingValue);
                }

                if (!usingIterator)
                    $arrayPush(padding, undefined);
            }

            if (usingIterator) {
                try {
                    $iteratorGenericClose(paddingIter);
                } catch (e) {
                    $closeAllIterators(iters);
                    throw e;
                }
            }
        }
    }

    var finishResults = (results) => {
        return results;
    };

    return $iteratorZip(iters, nextMethods, mode, padding, finishResults);
}

export function zipKeyed(iterables /*, options */)
{
    "use strict";

    var options = $argument(1);

    if (!$isObject(iterables))
        $throwTypeError("Iterator.zipKeyed expects its first argument to be an object");

    options = $getOptionsObject(options);

    var mode = options.mode;
    if (mode === undefined)
        mode = "shortest";
    if (mode !== "shortest" && mode !== "longest" && mode !== "strict")
        $throwTypeError("Expected \"shortest\", \"longest\" or \"strict\" for options.mode");

    var paddingOption = undefined;
    if (mode === "longest") {
        paddingOption = options.padding;
        if (paddingOption !== undefined && !$isObject(paddingOption))
            $throwTypeError("Expected options.padding to be an object");
    }

    var iters = [];
    var nextMethods = [];
    var padding = [];

    var allKeys = $ownKeys(iterables);
    var keys = [];

    var allKeysCount = allKeys.length;
    for (var k = 0; k < allKeysCount; ++k) {
        var key = allKeys[k];

        var desc;
        try {
            desc = $Object.$getOwnPropertyDescriptor(iterables, key);
        } catch (e) {
            $closeAllIterators(iters);
            throw e;
        }

        if (desc !== undefined && desc.enumerable) {
            var value;
            try {
                value = iterables[key];
            } catch (e) {
                $closeAllIterators(iters);
                throw e;
            }

            if (value !== undefined) {
                $arrayPush(keys, key);

                var iter;
                var iterNextMethod;
                try {
                    iter = $getIteratorFlattenable(value, /* rejectStrings: */ true);
                    iterNextMethod = iter.next;
                } catch (e) {
                    $closeAllIterators(iters);
                    throw e;
                }

                $arrayPush(iters, iter);
                $arrayPush(nextMethods, iterNextMethod);
            }
        }
    }

    var iterCount = iters.length;

    if (mode === "longest") {
        if (paddingOption === undefined) {
            for (var i = 0; i < iterCount; ++i)
                $arrayPush(padding, undefined);
        } else {
            for (var i = 0; i < iterCount; ++i) {
                var paddingValue;
                try {
                    paddingValue = paddingOption[keys[i]];
                } catch (e) {
                    $closeAllIterators(iters);
                    throw e;
                }
                $arrayPush(padding, paddingValue);
            }
        }
    }

    var finishResults = (results) => {
        var obj = $Object.$create(null);
        for (var i = 0; i < iterCount; ++i)
            $putByValDirect(obj, keys[i], results[i]);
        return obj;
    };

    return $iteratorZip(iters, nextMethods, mode, padding, finishResults);
}
