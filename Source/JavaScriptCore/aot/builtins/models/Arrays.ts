export function flat(depth) {
    "use strict";

    const result = [];
    $flatIntoArray(result, this, this.length, 0, depth === undefined ? 1 : $toIntegerOrInfinity(depth));
    return result;
}
