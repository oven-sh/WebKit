$visibility = "Private";
export function next() {
    "use strict";

    if (!$isIteratorHelper(this))
        $throwTypeError("|this| should be an iterator helper");

    const generator = $getIteratorHelperInternalField(this, $iteratorHelperFieldGenerator);
    const step = $stepGenerator(generator, $resumeModeNext, undefined, $generatorStepCheckIsGenerator);
    const result = $resultAt(step, $stepGeneratorResultValue);
    const status = $resultAt(step, $stepGeneratorResultStatus);
    if (status === $generatorStepStatusNotStepped)
        return $resumeGenerator(generator, $resumeModeNext, undefined);
    return $makeIteratorResult(result, status === $generatorStepStatusCompleted);
}

$overriddenName = "return";
export function return_() {
    "use strict";

    if (!$isIteratorHelper(this))
        $throwTypeError("|this| should be an iterator helper");

    var generator = $getIteratorHelperInternalField(this, $iteratorHelperFieldGenerator);
    if (!$isGeneratorSuspendedAtStart(generator)) {
        const step = $stepGenerator(generator, $resumeModeReturn, undefined, $generatorStepCheckIsGenerator);
        const result = $resultAt(step, $stepGeneratorResultValue);
        const status = $resultAt(step, $stepGeneratorResultStatus);
        if (status === $generatorStepStatusNotStepped)
            return $resumeGenerator(generator, $resumeModeReturn, undefined);
        return $makeIteratorResult(result, status === $generatorStepStatusCompleted);
    }

    $resumeGenerator(generator, $resumeModeReturn, undefined);
    var underlyingIterator = $getIteratorHelperInternalField(this, $iteratorHelperFieldUnderlyingIterator);
    if (underlyingIterator !== null)
        $iteratorGenericClose(underlyingIterator);
    return $makeIteratorResult(undefined, true);
}
