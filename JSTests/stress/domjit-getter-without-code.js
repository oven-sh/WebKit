function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)}, got ${String(actual)}`);
}

const object = $vm.createDOMJITGetterNoEffectsObject();

function readTwice(object) { return object.getterWithoutCode + object.getterWithoutCode; }
noInline(readTwice);

function readAroundCall(object, callback) {
    const first = object.getterWithoutCode;
    callback();
    return first + object.getterWithoutCode;
}
noInline(readAroundCall);

function readByValue(object, name) { return object[name]; }
noInline(readByValue);

function nothing() { }
noInline(nothing);

const callsOf = (run) => {
    const before = object.timesCalled;
    run();
    return object.timesCalled - before;
};

for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(readTwice(object), 84);
    shouldBe(readByValue(object, "getterWithoutCode"), 42);
    shouldBe(callsOf(() => shouldBe(readAroundCall(object, nothing), 84)), 2);
}

// The inline caches have to be filled before the function is optimized.
const { useConcurrentJIT, useDOMJIT, forceICFailure, thresholdForOptimizeAfterWarmUp } = jscOptions();
if (numberOfDFGCompiles(readTwice) && $vm.useDFGJIT() && useDOMJIT && !useConcurrentJIT && !forceICFailure && thresholdForOptimizeAfterWarmUp >= 1000)
    shouldBe(callsOf(() => readTwice(object)), 1);

let error;
try {
    readTwice(Object.create(object));
} catch (e) {
    error = e;
}
shouldBe(error instanceof TypeError, true);
