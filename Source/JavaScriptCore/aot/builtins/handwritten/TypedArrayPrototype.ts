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
