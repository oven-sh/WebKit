export function toLocaleString(/* locale, options */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (length == 0)
        return "";

    var string = "";
    for (var i = 0; i < length; ++i) {
        if (i > 0)
            string += ",";
        var element = this[i];
        if (typeof element === "number" || typeof element === "bigint")
            string += $toString(element.toLocaleString($argument(0), $argument(1)));
    }

    return string;
}

export function at(index) {
    "use strict";

    var length = $typedArrayLength(this);

    var k = $toIntegerOrInfinity(index);
    if (k < 0)
        k += length;

    return (k >= 0 && k < length) ? this[$toLength(k)] : undefined;
}

export function forEach(callback /*, thisArg */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.forEach callback must be a function");

    var thisArg = $argument(1);
    for (var i = 0; i < length; ++i)
        callback.$call(thisArg, this[i], i, this);
}

export function every(callback /*, thisArg */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.every callback must be a function");

    var thisArg = $argument(1);
    for (var i = 0; i < length; ++i) {
        if (!callback.$call(thisArg, this[i], i, this))
            return false;
    }
    return true;
}

export function some(callback /*, thisArg */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.some callback must be a function");

    var thisArg = $argument(1);
    for (var i = 0; i < length; ++i) {
        if (callback.$call(thisArg, this[i], i, this))
            return true;
    }
    return false;
}

export function find(callback /*, thisArg */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.find callback must be a function");

    var thisArg = $argument(1);
    for (var i = 0; i < length; ++i) {
        var element = this[i];
        if (callback.$call(thisArg, element, i, this))
            return element;
    }
    return undefined;
}

export function findIndex(callback /*, thisArg */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.findIndex callback must be a function");

    var thisArg = $argument(1);
    for (var i = 0; i < length; ++i) {
        if (callback.$call(thisArg, this[i], i, this))
            return i;
    }
    return -1;
}

export function findLast(callback /*, thisArg */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.findLast callback must be a function");

    var thisArg = $argument(1);
    for (var i = length - 1; i >= 0; --i) {
        var element = this[i];
        if (callback.$call(thisArg, element, i, this))
            return element;
    }
    return undefined;
}

export function findLastIndex(callback /*, thisArg */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.findLastIndex callback must be a function");

    var thisArg = $argument(1);
    for (var i = length - 1; i >= 0; --i) {
        if (callback.$call(thisArg, this[i], i, this))
            return i;
    }
    return -1;
}

export function reduce(callback /*, initialValue */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.reduce callback must be a function");

    var argumentCount = $argumentCount();
    if (length === 0 && argumentCount < 2)
        $throwTypeError("TypedArray.prototype.reduce of empty array with no initial value");

    var accumulator, i = 0;
    if (argumentCount > 1)
        accumulator = $argument(1);
    else
        accumulator = this[i++];

    for (; i < length; ++i)
        accumulator = callback.$call(undefined, accumulator, this[i], i, this);
    return accumulator;
}

export function reduceRight(callback /*, initialValue */) {
    "use strict";

    var length = $typedArrayLength(this);

    if (!$isCallable(callback))
        $throwTypeError("TypedArray.prototype.reduceRight callback must be a function");

    var argumentCount = $argumentCount();
    if (length === 0 && argumentCount < 2)
        $throwTypeError("TypedArray.prototype.reduceRight of empty array with no initial value");

    var accumulator, i = length - 1;
    if (argumentCount > 1)
        accumulator = $argument(1);
    else
        accumulator = this[i--];

    for (; i >= 0; --i)
        accumulator = callback.$call(undefined, accumulator, this[i], i, this);
    return accumulator;
}
