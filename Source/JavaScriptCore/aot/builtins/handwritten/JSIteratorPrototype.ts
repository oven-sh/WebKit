// As in builtins/JSIteratorPrototype.js, but for the steps. Where that file calls `next`, reads `done` off the answer and then `value` unless it is done, this one asks iteratorStepValue() for the three at once,
// which makes no answer for an iterator of the engine. What it hands back is asked whether it is the mark of a closed iterator in front of anything else.

export function map(mapper)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.map requires that |this| be an Object.");

    if (!$isCallable(mapper)) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.map callback must be a function.");
        }
    }

    var iterated = this;
    var iteratedNextMethod = iterated.next;

    var generator = (function*() {
        var counter = 0;
        for (;;) {
            var value = $iteratorStepValue(iteratedNextMethod, iterated);
            if ($isCollectionIteratorClosedMark(value))
                return;

            $ifAbruptCloseIterator(iterated, (
                yield mapper(value, counter++)
            ));
        }
    })();

    return $iteratorHelperCreate(generator, iterated);
}

export function filter(predicate)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.filter requires that |this| be an Object.");

    if (!$isCallable(predicate)) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.filter callback must be a function.");
        }
    }

    var iterated = this;
    var iteratedNextMethod = iterated.next;

    var generator = (function*() {
        var counter = 0;
        for (;;) {
            var value = $iteratorStepValue(iteratedNextMethod, iterated);
            if ($isCollectionIteratorClosedMark(value))
                return;

            $ifAbruptCloseIterator(iterated, (
                predicate(value, counter++) && (yield value)
            ));
        }
    })();

    return $iteratorHelperCreate(generator, iterated);
}

export function take(limit)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.take requires that |this| be an Object.");

    var numLimit;
    $ifAbruptCloseIterator(this, numLimit = $toNumber(limit));
    if (numLimit !== numLimit) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwRangeError("Iterator.prototype.take argument must not be NaN.");
        }
    }

    if ($isFinite(numLimit) && numLimit > $MAX_SAFE_INTEGER) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwRangeError("Iterator.prototype.take argument must be non-negative safe integer or +Infinity");
        }
    }

    var intLimit = $toIntegerOrInfinity(numLimit);
    if (intLimit < 0) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwRangeError("Iterator.prototype.take argument must be non-negative safe integer or +Infinity");
        }
    }

    var iterated = this;
    var iteratedNextMethod = iterated.next;

    var generator = (function*() {
        var remaining = intLimit;
        for (;;) {
            if (remaining === 0) {
                $iteratorGenericClose(iterated);
                return;
            }

            if (remaining !== $Infinity)
                remaining--;

            var value = $iteratorStepValue(iteratedNextMethod, iterated);
            if ($isCollectionIteratorClosedMark(value))
                return;

            $ifAbruptCloseIterator(iterated, (
                yield value
            ));
        }
    })();

    return $iteratorHelperCreate(generator, iterated);
}

export function drop(limit)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.drop requires that |this| be an Object.");

    var numLimit;
    $ifAbruptCloseIterator(this, (numLimit = $toNumber(limit)));
    if (numLimit !== numLimit) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwRangeError("Iterator.prototype.drop argument must not be NaN.");
        }
    }

    if ($isFinite(numLimit) && numLimit > $MAX_SAFE_INTEGER) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwRangeError("Iterator.prototype.drop argument must be non-negative safe integer or +Infinity");
        }
    }

    var intLimit = $toIntegerOrInfinity(numLimit);
    if (intLimit < 0) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwRangeError("Iterator.prototype.drop argument must be non-negative safe integer or +Infinity");
        }
    }

    var iterated = this;
    var iteratedNextMethod = iterated.next;

    var generator = (function*() {
        var remaining = intLimit;
        for (;;) {
            // Of an answer that is dropped `value` is never read.
            if (remaining > 0) {
                if ($iteratorGenericNext(iteratedNextMethod, iterated).done)
                    return;

                if (remaining !== $Infinity)
                    remaining--;
            } else {
                var value = $iteratorStepValue(iteratedNextMethod, iterated);
                if ($isCollectionIteratorClosedMark(value))
                    return;

                $ifAbruptCloseIterator(iterated, (
                    yield value
                ));
            }
        }
    })();

    return $iteratorHelperCreate(generator, iterated);
}

export function flatMap(mapper)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.flatMap requires that |this| be an Object.");

    if (!$isCallable(mapper)) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.flatMap callback must be a function.");
        }
    }

    var iterated = this;
    var iteratedNextMethod = iterated.next;

    var generator = (function*() {
        var counter = 0;
        for (;;) {
            var value = $iteratorStepValue(iteratedNextMethod, iterated);
            if ($isCollectionIteratorClosedMark(value))
                return;

            var innerIterator;
            var innerNextMethod;
            $ifAbruptCloseIterator(iterated, (
                innerIterator = $getIteratorFlattenable(mapper(value, counter++), /* rejectStrings: */ true),
                innerNextMethod = innerIterator.next
            ));

            for (;;) {
                var innerValue;
                $ifAbruptCloseIterator(iterated, (
                    innerValue = $iteratorStepValue(innerNextMethod, innerIterator)
                ));
                if ($isCollectionIteratorClosedMark(innerValue))
                    break;

                $ifAbruptCloseIterator(iterated, $ifAbruptCloseIterator(innerIterator, (
                    yield innerValue
                )));
            }
        }
    })();

    return $iteratorHelperCreate(generator, iterated);
}

export function some(predicate)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.some requires that |this| be an Object.");

    if (!$isCallable(predicate)) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.some callback must be a function.");
        }
    }

    var iterated = this;
    var iteratedNextMethod = iterated.next;
    var count = 0;
    for (;;) {
        var item = $iteratorStepValue(iteratedNextMethod, iterated);
        if ($isCollectionIteratorClosedMark(item))
            return false;

        var matched;
        $ifAbruptCloseIterator(iterated, (
            matched = predicate(item, count++)
        ));
        if (matched) {
            $iteratorGenericClose(iterated);
            return true;
        }
    }
}

export function every(predicate)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.every requires that |this| be an Object.");

    if (!$isCallable(predicate)) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.every callback must be a function.");
        }
    }

    var iterated = this;
    var iteratedNextMethod = iterated.next;
    var count = 0;
    for (;;) {
        var item = $iteratorStepValue(iteratedNextMethod, iterated);
        if ($isCollectionIteratorClosedMark(item))
            return true;

        var matched;
        $ifAbruptCloseIterator(iterated, (
            matched = predicate(item, count++)
        ));
        if (!matched) {
            $iteratorGenericClose(iterated);
            return false;
        }
    }
}

export function find(predicate)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.find requires that |this| be an Object.");

    if (!$isCallable(predicate)) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.find callback must be a function.");
        }
    }

    var iterated = this;
    var iteratedNextMethod = iterated.next;
    var count = 0;
    for (;;) {
        var item = $iteratorStepValue(iteratedNextMethod, iterated);
        if ($isCollectionIteratorClosedMark(item))
            return undefined;

        var matched;
        $ifAbruptCloseIterator(iterated, (
            matched = predicate(item, count++)
        ));
        if (matched) {
            $iteratorGenericClose(iterated);
            return item;
        }
    }
}

export function reduce(reducer /*, initialValue */)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.reduce requires that |this| be an Object.");

    if (!$isCallable(reducer)) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.reduce reducer argument must be a function.");
        }
    }

    var iterated = this;
    var iteratedNextMethod = this.next;

    var accumulator;
    var counter = 0;
    if ($argumentCount() <= 1) {
        accumulator = $iteratorStepValue(iteratedNextMethod, iterated);
        if ($isCollectionIteratorClosedMark(accumulator))
            $throwTypeError("Iterator.prototype.reduce requires an initial value or an iterator that is not done.");
        counter = 1;
    } else
        accumulator = $argument(1);

    for (;;) {
        var value = $iteratorStepValue(iteratedNextMethod, iterated);
        if ($isCollectionIteratorClosedMark(value))
            break;

        $ifAbruptCloseIterator(iterated, (
            accumulator = reducer(accumulator, value, counter++)
        ));
    }

    return accumulator;
}

export function chunks(chunkSize)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.chunks requires that |this| be an Object.");

    if (typeof chunkSize !== "number" || chunkSize % 1 !== 0) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.chunks requires that first argument be an integral Number.");
        }
    }

    if (chunkSize < 1 || chunkSize > 4294967295) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwRangeError("Iterator.prototype.chunks requires that first argument be between 1 and 2**32 - 1.");
        }
    }

    var iterated = this;
    var iteratedNextMethod = this.next;

    var generator = (function*() {
        var buffer = [];

        for (;;) {
            var value = $iteratorStepValue(iteratedNextMethod, iterated);
            if ($isCollectionIteratorClosedMark(value)) {
                if (buffer.length)
                    yield buffer;
                return;
            }

            $arrayPush(buffer, value);

            if (buffer.length === chunkSize) {
                $ifAbruptCloseIterator(iterated, (
                    yield buffer
                ));

                buffer = [];
            }
        }
    })();

    return $iteratorHelperCreate(generator, iterated);
}

export function windows(windowSize /*, undersized */)
{
    "use strict";

    if (!$isObject(this))
        $throwTypeError("Iterator.prototype.windows requires that |this| be an Object.");

    if (typeof windowSize !== "number" || windowSize % 1 !== 0) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.windows requires that first argument be an integral Number.");
        }
    }

    if (windowSize < 1 || windowSize > 4294967295) {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwRangeError("Iterator.prototype.windows requires that first argument be between 1 and 2**32 - 1.");
        }
    }

    var undersized = $argument(1);
    if (undersized === undefined)
        undersized = "only-full";
    else if (undersized !== "only-full" && undersized !== "allow-partial") {
        try {
            $iteratorGenericClose(this);
        } finally {
            $throwTypeError("Iterator.prototype.windows requires that second argument be \"only-full\" or \"allow-partial\".");
        }
    }

    var iterated = this;
    var iteratedNextMethod = this.next;

    var generator = (function*() {
        var buffer: any[] = [];

        for (;;) {
            var value = $iteratorStepValue(iteratedNextMethod, iterated);
            if ($isCollectionIteratorClosedMark(value)) {
                if (undersized === "allow-partial" && buffer.length && buffer.length < windowSize)
                    yield buffer;
                return;
            }

            if (buffer.length === windowSize) {
                for (var i = 0; i < buffer.length - 1; ++i)
                    buffer[i] = buffer[i + 1];
                buffer[buffer.length - 1] = value;
            } else
                $arrayPush(buffer, value);

            if (buffer.length === windowSize) {
                var copy = $newArrayWithSize(buffer.length);
                for (var i = 0; i < buffer.length; ++i)
                    copy[i] = buffer[i];

                $ifAbruptCloseIterator(iterated, (
                    yield copy
                ));
            }
        }
    })();

    return $iteratorHelperCreate(generator, iterated);
}

$overriddenName = "[Symbol.dispose]";
export function dispose()
{
    "use strict";

    var returnMethod = this.return;
    if (!$isUndefinedOrNull(returnMethod))
        returnMethod.$call(this);
}
