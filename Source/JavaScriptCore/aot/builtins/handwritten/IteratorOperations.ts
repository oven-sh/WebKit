$linkTimeConstant;
$alwaysInline;
export function iteratorGenericNext(nextMethod, iterator) {
    "use strict";

    var result = nextMethod.$call(iterator);
    if (!$isObject(result))
        $throwTypeError("Iterator result interface is not an object.");
    return result;
}

$linkTimeConstant;
$alwaysInline;
export function iteratorGenericClose(iterator) {
    "use strict";

    var returnMethod = iterator.return;
    if ($isUndefinedOrNull(returnMethod))
        return;
    if (!$isObject(returnMethod.$call(iterator)))
        $throwTypeError("Iterator result interface is not an object.");
}

// What the iterator hands out: iteratorGenericNext(), a read of `done` and, unless that holds, a read of `value`.
// Where the iterator is done: the mark of a closed iterator, which no program ever holds, so that no value can be mistaken for it.
// Where `nextMethod` is the own method of an iterator of the engine, nobody sees the object that a call of it answers with, and none is made.
// A loop that knows the method has a copy of the one arm that is left.
$linkTimeConstant;
$alwaysInline;
export function iteratorStepValue(nextMethod, iterator) {
    "use strict";

    if (nextMethod === $arrayIteratorProtoNext && $isArrayIterator(iterator)) {
        const step = $stepArrayIterator(iterator);
        const value = $resultAt(step, $stepArrayIteratorResultValue);
        return $resultAt(step, $stepArrayIteratorResultDone) ? $collectionIteratorClosedMark() : value;
    }
    if (nextMethod === $mapIteratorProtoNext && $isMapIterator(iterator)) {
        const step = $stepMapIterator(iterator);
        const value = $resultAt(step, $stepArrayIteratorResultValue);
        return $resultAt(step, $stepArrayIteratorResultDone) ? $collectionIteratorClosedMark() : value;
    }
    if (nextMethod === $setIteratorProtoNext && $isSetIterator(iterator)) {
        const step = $stepSetIterator(iterator);
        const value = $resultAt(step, $stepArrayIteratorResultValue);
        return $resultAt(step, $stepArrayIteratorResultDone) ? $collectionIteratorClosedMark() : value;
    }
    // As `next` of a generator does it, which asks by itself whether it was given one.
    if (nextMethod === $generatorProtoNext) {
        const step = $stepGenerator(iterator, $resumeModeNext, undefined, $generatorStepCheckIsGenerator);
        const value = $resultAt(step, $stepGeneratorResultValue);
        const status = $resultAt(step, $stepGeneratorResultStatus);
        if (status === $generatorStepStatusNotStepped) {
            const answer = $resumeGenerator(iterator, $resumeModeNext, undefined);
            return answer.done === true ? $collectionIteratorClosedMark() : answer.value;
        }
        return status === $generatorStepStatusCompleted ? $collectionIteratorClosedMark() : value;
    }
    if (nextMethod === $regExpStringIteratorProtoNext && $isRegExpStringIterator(iterator)) {
        const match = $stepMatchIterator(iterator);
        return match === null ? $collectionIteratorClosedMark() : match;
    }
    const result = $iteratorGenericNext(nextMethod, iterator);
    return result.done ? $collectionIteratorClosedMark() : result.value;
}
