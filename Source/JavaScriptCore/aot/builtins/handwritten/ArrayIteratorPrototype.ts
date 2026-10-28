$visibility = "Private";
export function next() {
    "use strict";

    if (!$isArrayIterator(this))
        $throwTypeError("%ArrayIteratorPrototype%.next requires that |this| be an Array Iterator instance");

    const step = $stepArrayIterator(this);
    return $makeIteratorResult($resultAt(step, $stepArrayIteratorResultValue), $resultAt(step, $stepArrayIteratorResultDone));
}
