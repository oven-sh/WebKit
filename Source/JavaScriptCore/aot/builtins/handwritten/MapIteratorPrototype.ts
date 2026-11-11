$visibility = "Private";
export function next() {
    "use strict";

    if (!$isMapIterator(this))
        $throwTypeError("%MapIteratorPrototype%.next requires that |this| be a Map Iterator instance");

    const step = $stepMapIterator(this);
    return $makeIteratorResult($resultAt(step, $stepArrayIteratorResultValue), $resultAt(step, $stepArrayIteratorResultDone));
}
