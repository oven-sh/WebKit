// As in GeneratorPrototype.ts, every path makes the result in one place.
$visibility = "Private";
export function next() {
    "use strict";

    if (!$isIteratorHelper(this))
        $throwTypeError("|this| should be an iterator helper");

    const generator = $getIteratorHelperInternalField(this, $iteratorHelperFieldGenerator);
    const step = $stepGenerator(generator, $resumeModeNext, undefined, $generatorStepCheckIsGenerator);
    let result = $resultAt(step, $stepGeneratorResultValue);
    const status = $resultAt(step, $stepGeneratorResultStatus);
    let done = status === $generatorStepStatusCompleted;
    if (status === $generatorStepStatusNotStepped) {
        const answer = $resumeGenerator(generator, $resumeModeNext, undefined);
        result = answer.value;
        done = answer.done === true;
    }
    return $makeIteratorResult(result, done);
}

$overriddenName = "return";
export function return_() {
    "use strict";

    if (!$isIteratorHelper(this))
        $throwTypeError("|this| should be an iterator helper");

    var generator = $getIteratorHelperInternalField(this, $iteratorHelperFieldGenerator);
    if (!$isGeneratorSuspendedAtStart(generator)) {
        const step = $stepGenerator(generator, $resumeModeReturn, undefined, $generatorStepCheckIsGenerator);
        let result = $resultAt(step, $stepGeneratorResultValue);
        const status = $resultAt(step, $stepGeneratorResultStatus);
        let done = status === $generatorStepStatusCompleted;
        if (status === $generatorStepStatusNotStepped) {
            const answer = $resumeGenerator(generator, $resumeModeReturn, undefined);
            result = answer.value;
            done = answer.done === true;
        }
        return $makeIteratorResult(result, done);
    }

    $resumeGenerator(generator, $resumeModeReturn, undefined);
    var underlyingIterator = $getIteratorHelperInternalField(this, $iteratorHelperFieldUnderlyingIterator);
    if (underlyingIterator !== null)
        $iteratorGenericClose(underlyingIterator);
    return $makeIteratorResult(undefined, true);
}
