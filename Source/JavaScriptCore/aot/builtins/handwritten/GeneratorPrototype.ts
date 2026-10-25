$visibility = "Private";
export function next(value) {
    "use strict";

    const step = $stepGenerator(this, $resumeModeNext, value, $generatorStepCheckIsGenerator);
    const result = $resultAt(step, $stepGeneratorResultValue);
    const status = $resultAt(step, $stepGeneratorResultStatus);
    if (status === $generatorStepStatusNotStepped)
        return $resumeGenerator(this, $resumeModeNext, value);
    return $makeIteratorResult(result, status === $generatorStepStatusCompleted);
}

$overriddenName = "return";
$visibility = "Private";
export function return_(value) {
    "use strict";

    const step = $stepGenerator(this, $resumeModeReturn, value, $generatorStepCheckIsGenerator);
    const result = $resultAt(step, $stepGeneratorResultValue);
    const status = $resultAt(step, $stepGeneratorResultStatus);
    if (status === $generatorStepStatusNotStepped)
        return $resumeGenerator(this, $resumeModeReturn, value);
    return $makeIteratorResult(result, status === $generatorStepStatusCompleted);
}

$overriddenName = "throw";
$visibility = "Private";
export function throw_(exception) {
    "use strict";

    const step = $stepGenerator(this, $resumeModeThrow, exception, $generatorStepCheckIsGenerator);
    const result = $resultAt(step, $stepGeneratorResultValue);
    const status = $resultAt(step, $stepGeneratorResultStatus);
    if (status === $generatorStepStatusNotStepped)
        return $resumeGenerator(this, $resumeModeThrow, exception);
    return $makeIteratorResult(result, status === $generatorStepStatusCompleted);
}
