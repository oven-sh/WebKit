$visibility = "Private";
export function next() {
    "use strict";

    if (!$isRegExpStringIterator(this)) {
        if (!$isObject(this))
            $throwTypeError("%RegExpStringIteratorPrototype%.next requires |this| to be an Object");
        $throwTypeError("%RegExpStringIteratorPrototype%.next requires |this| to be a RegExp String Iterator instance");
    }

    const match = $stepMatchIterator(this);
    return match === null ? $makeIteratorResult(undefined, true) : $makeIteratorResult(match, false);
}
