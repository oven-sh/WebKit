$visibility = "Private";
export function next() {
    "use strict";

    if (!$isIteratorHelper(this))
        $throwTypeError("|this| should be an iterator helper");

    return $resumeGenerator($getIteratorHelperInternalField(this, $iteratorHelperFieldGenerator), $resumeModeNext, undefined);
}

$overriddenName = "return";
export function return_() {
    "use strict";

    if (!$isIteratorHelper(this))
        $throwTypeError("|this| should be an iterator helper");

    var generator = $getIteratorHelperInternalField(this, $iteratorHelperFieldGenerator);
    if (!$isGeneratorSuspendedAtStart(generator))
        return $resumeGenerator(generator, $resumeModeReturn, undefined);

    $resumeGenerator(generator, $resumeModeReturn, undefined);
    var underlyingIterator = $getIteratorHelperInternalField(this, $iteratorHelperFieldUnderlyingIterator);
    if (underlyingIterator !== null)
        $iteratorGenericClose(underlyingIterator);
    return $makeIteratorResult(undefined, true);
}
