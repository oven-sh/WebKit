//@ runDefault("--useZombieMode=1", "--sweepSynchronously=1")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldBeArray(actual, expected) {
    shouldBe(actual.length, expected.length);
    for (let i = 0; i < expected.length; ++i)
        shouldBe(actual[i], expected[i]);
}

function collect() {
    fullGC();
    const fillers = [];
    for (let i = 0; i < 10000; ++i)
        fillers.push({ index: i });
    edenGC();
}

const holderCount = 100;
const entryCount = 20;

function testAfterCollection(createHolder, checkHolder) {
    const holders = [];
    for (let i = 0; i < holderCount; ++i)
        holders.push(createHolder());
    collect();
    for (const holder of holders)
        checkHolder(holder);
}

function testReactions(createHolder, settle, expectedLog) {
    const log = [];
    testAfterCollection(() => createHolder(log), holder => {
        log.length = 0;
        settle(holder, log);
        drainMicrotasks();
        shouldBe(log.join(), expectedLog);
    });
}

function testArrayLiteral(createArray, expected) {
    shouldBeArray(createArray(), expected);
    collect();
    shouldBeArray(createArray(), expected);
}

function createGetterSetter() {
    const state = { value: 42 };
    return Object.defineProperty({}, "x", {
        get() { return state.value; },
        set(value) { state.value = value; },
    });
}

testAfterCollection(createGetterSetter, holder => {
    shouldBe(holder.x, 42);
    holder.x = 7;
    shouldBe(holder.x, 7);
});

function createCalleeWithScope() {
    const captured = { value: 42 };
    return () => captured.value;
}

testAfterCollection(createCalleeWithScope, closure => shouldBe(closure(), 42));

function createFunctionWithExecutable() {
    return new Function("return 42;");
}

testAfterCollection(createFunctionWithExecutable, callee => shouldBe(callee(), 42));

function createFunctionWithRareData() {
    const Box = new Function("this.value = 42;");
    new Box();
    return Box;
}

testAfterCollection(createFunctionWithRareData, Box => shouldBe(new Box().value, 42));

function strings() {
    return ["left", "right"];
}

testArrayLiteral(strings, ["left", "right"]);

function createMapStorage() {
    const map = new Map();
    for (let i = 0; i < entryCount; ++i)
        map.set(i, { value: i });
    return map;
}

testAfterCollection(createMapStorage, map => {
    shouldBe(map.size, entryCount);
    for (let i = 0; i < entryCount; ++i)
        shouldBe(map.get(i).value, i);
});

function createSetStorage() {
    const set = new Set();
    for (let i = 0; i < entryCount; ++i)
        set.add({ value: i });
    return set;
}

testAfterCollection(createSetStorage, set => {
    let index = 0;
    for (const element of set)
        shouldBe(element.value, index++);
    shouldBe(index, entryCount);
});

// 0x00007ffffffe0000 is a cell pointer if a double butterfly is visited as values.
function doubles() {
    return [6.9533558013592e-310, 1.5];
}

testArrayLiteral(doubles, [6.9533558013592e-310, 1.5]);

function integers() {
    return [1, 2, 3];
}

testArrayLiteral(integers, [1, 2, 3]);

function createRope() {
    const left = "left".toUpperCase();
    const middle = "middle".toUpperCase();
    const right = "right".toUpperCase();
    return `${left}${middle}${right}`;
}

testAfterCollection(createRope, rope => {
    shouldBe(isRope(rope), true);
    shouldBe(rope, "LEFTMIDDLERIGHT");
});

function createSubstring() {
    return "a long base string".toUpperCase().substring(2, 11);
}

testAfterCollection(createSubstring, substring => {
    shouldBe(isRope(substring), true);
    shouldBe(substring, "LONG BASE");
});

function createFunctionWithFields(log) {
    let resolve;
    new Promise(resolveFunction => { resolve = resolveFunction; }).then(value => { log.push(value); });
    return resolve;
}

testReactions(createFunctionWithFields, resolve => resolve(5), "5");

function createFulfilledPromise() {
    return Promise.resolve({ value: 42 });
}

testReactions(createFulfilledPromise, (promise, log) => { promise.then(result => { log.push(result.value); }); }, "42");

function createSlimReactionChain(log) {
    let resolve;
    const promise = new Promise(resolveFunction => { resolve = resolveFunction; });
    promise.then(value => { log.push(`first ${value}`); });
    promise.then(value => { log.push(`second ${value}`); });
    promise.then(value => { log.push(`third ${value}`); });
    return resolve;
}

testReactions(createSlimReactionChain, resolve => resolve(5), "first 5,second 5,third 5");

function createFullReaction(log) {
    let resolve;
    let reject;
    const promise = new Promise((resolveFunction, rejectFunction) => {
        resolve = resolveFunction;
        reject = rejectFunction;
    });
    promise.then(value => {
        log.push(`fulfilled ${value}`);
        return value + 1;
    }, reason => {
        log.push(`rejected ${reason}`);
        return reason + 2;
    }).then(value => { log.push(`derived ${value}`); });
    return { resolve, reject };
}

testReactions(createFullReaction, ({ resolve }) => resolve(5), "fulfilled 5,derived 6");
testReactions(createFullReaction, ({ reject }) => reject(5), "rejected 5,derived 7");

function createFullReactionWithContext(log) {
    let resolve;
    const promise = new Promise(resolveFunction => { resolve = resolveFunction; });
    $vm.setAsyncContext({ value: 42 });
    promise.then(() => { log.push($vm.asyncContext().value); });
    $vm.setAsyncContext(undefined);
    return resolve;
}

testReactions(createFullReactionWithContext, resolve => resolve(5), "42");
