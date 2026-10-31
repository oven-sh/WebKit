$visibility = "Private";
export function next() {
    "use strict";

    if (!$isSetIterator(this))
        $throwTypeError("%SetIteratorPrototype%.next requires that |this| be a Set Iterator instance");

    const step = $stepSetIterator(this);
    return $makeIteratorResult($resultAt(step, $stepArrayIteratorResultValue), $resultAt(step, $stepArrayIteratorResultDone));
}
