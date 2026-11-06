$visibility = "Private";
export function next(value) {
    "use strict";

    const step = $stepGenerator(this, $resumeModeNext, value, $generatorStepCheckIsGenerator);
    let result = $resultAt(step, $stepGeneratorResultValue);
    const status = $resultAt(step, $stepGeneratorResultStatus);
    let done = status === $generatorStepStatusCompleted;
    if (status === $generatorStepStatusNotStepped) {
        const answer = $resumeGenerator(this, $resumeModeNext, value);
        result = answer.value;
        done = answer.done === true;
    }
    return $makeIteratorResult(result, done);
}

$overriddenName = "return";
$visibility = "Private";
export function return_(value) {
    "use strict";

    const step = $stepGenerator(this, $resumeModeReturn, value, $generatorStepCheckIsGenerator);
    let result = $resultAt(step, $stepGeneratorResultValue);
    const status = $resultAt(step, $stepGeneratorResultStatus);
    let done = status === $generatorStepStatusCompleted;
    if (status === $generatorStepStatusNotStepped) {
        const answer = $resumeGenerator(this, $resumeModeReturn, value);
        result = answer.value;
        done = answer.done === true;
    }
    return $makeIteratorResult(result, done);
}

$overriddenName = "throw";
$visibility = "Private";
export function throw_(exception) {
    "use strict";

    const step = $stepGenerator(this, $resumeModeThrow, exception, $generatorStepCheckIsGenerator);
    let result = $resultAt(step, $stepGeneratorResultValue);
    const status = $resultAt(step, $stepGeneratorResultStatus);
    let done = status === $generatorStepStatusCompleted;
    if (status === $generatorStepStatusNotStepped) {
        const answer = $resumeGenerator(this, $resumeModeThrow, exception);
        result = answer.value;
        done = answer.done === true;
    }
    return $makeIteratorResult(result, done);
}
