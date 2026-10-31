//@ defaultRun; run("save-everything", "--useGeneratorSavePruning=false")

// A generator or async function that suspends saves only the locals that may have been written since it was entered or
// resumed. Each case is one way for a write to reach a suspend point, with no other way for it to get there.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function opaque(value) { return value; }
noInline(opaque);

function throwIf(condition)
{
    if (condition)
        throw "thrown";
}
noInline(throwIf);

function values(generator)
{
    let result = [];
    for (;;) {
        let { value, done } = generator.next();
        result.push(value);
        if (done)
            return result.join();
    }
}

function* neverWrittenAgain()
{
    let a = opaque(1);
    yield 0;
    yield 0;
    yield 0;
    return a;
}

function* inTheSameBlock()
{
    let a = opaque(1);
    yield 0;
    a = 2;
    yield 0;
    return a;
}

function* inAnEarlierBlock(condition)
{
    let a = opaque(1);
    yield 0;
    a = 2;
    if (condition)
        opaque(0);
    yield 0;
    return a;
}

function* onOneOfTwoPaths(condition)
{
    let a = opaque(1);
    let b = opaque(1);
    yield 0;
    if (condition)
        a = 2;
    else
        b = 2;
    yield 0;
    return `${a}${b}`;
}

function* inAnEarlierIteration(count)
{
    let a = opaque(1);
    yield 0;
    for (let i = 0; i < count; i++) {
        yield 0;
        a = a + 1;
    }
    return a;
}

function* inATryBlockThatThrows()
{
    let a = opaque(1);
    yield 0;
    try {
        a = 2;
        throwIf(true);
    } catch {
        yield 0;
    }
    return a;
}

function* lateInATryBlockThatThrows(condition)
{
    let a = opaque(1);
    yield 0;
    try {
        if (condition)
            opaque(0);
        a = 2;
        throwIf(true);
    } catch {
        yield 0;
    }
    return a;
}

function* inTheInnerOfTwoTryBlocks()
{
    let a = opaque(1);
    yield 0;
    try {
        try {
            a = 2;
            throwIf(true);
        } catch {
            yield 0;
        }
    } catch {
    }
    return a;
}

function* inATryBlockWhoseHandlerThrows()
{
    let a = opaque(1);
    yield 0;
    try {
        try {
            a = 2;
            throwIf(true);
        } catch {
            throwIf(true);
        }
    } catch {
        yield 0;
    }
    return a;
}

// op_enter writes the scope register, which is what finds a captured variable.
let onEntry = (captured => function* () {
    yield captured;
    yield captured + 1;
    return captured + 2;
})(40);

// op_iterator_open writes the iterator and then can still throw, getting the next method.
function* partWayThroughAnInstruction(iterable)
{
    let sum = 0;
    for (let x of iterable) {
        yield x;
        sum += x;
        yield sum;
    }
    return sum;
}

function iterable()
{
    return {
        i: 0,
        [Symbol.iterator]() { return this; },
        next() { return { done: this.i >= 2, value: ++this.i }; },
    };
}

async function inAnAsyncFunction(condition)
{
    let a = opaque(1);
    await 0;
    try {
        if (condition)
            a = 2;
        throwIf(true);
    } catch {
        await 0;
    }
    return a;
}

function settle(promise)
{
    let result;
    promise.then(value => { result = value; });
    drainMicrotasks();
    return result;
}

for (let i = 0; i < testLoopCount / 10; i++) {
    shouldBe(values(neverWrittenAgain()), "0,0,0,1");
    shouldBe(values(inTheSameBlock()), "0,0,2");
    shouldBe(values(inAnEarlierBlock(i & 1)), "0,0,2");
    shouldBe(values(onOneOfTwoPaths(true)), "0,0,21");
    shouldBe(values(onOneOfTwoPaths(false)), "0,0,12");
    shouldBe(values(inAnEarlierIteration(3)), "0,0,0,0,4");
    shouldBe(values(inATryBlockThatThrows()), "0,0,2");
    shouldBe(values(lateInATryBlockThatThrows(i & 1)), "0,0,2");
    shouldBe(values(inTheInnerOfTwoTryBlocks()), "0,0,2");
    shouldBe(values(inATryBlockWhoseHandlerThrows()), "0,0,2");
    shouldBe(values(onEntry()), "40,41,42");
    shouldBe(values(partWayThroughAnInstruction(iterable())), "1,1,2,3,3");
    shouldBe(settle(inAnAsyncFunction(true)), 2);
    shouldBe(settle(inAnAsyncFunction(false)), 1);
}
