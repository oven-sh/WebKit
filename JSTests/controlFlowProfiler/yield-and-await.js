var basicBlockExecutionCount = $vm.basicBlockExecutionCount;

load("./driver/driver.js");

function foo() { return 1; }
var never = new Promise(() => { });

function* testYield() {
    foo(1);
    yield 1;
    foo(2);
    const x = yield 2;
    foo(3);
}
var generator = testYield();
generator.next();
checkBasicBlock(testYield, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testYield, "yield 1", ShouldHaveExecuted);
checkBasicBlock(testYield, "foo(2)", ShouldNotHaveExecuted);
checkBasicBlock(testYield, "foo(3)", ShouldNotHaveExecuted);
generator.next();
checkBasicBlock(testYield, "foo(2)", ShouldHaveExecuted);
checkBasicBlock(testYield, "yield 2", ShouldHaveExecuted);
checkBasicBlock(testYield, "foo(3)", ShouldNotHaveExecuted);
generator.next();
checkBasicBlock(testYield, "foo(3)", ShouldHaveExecuted);

function* testReturnAndThrow() {
    try {
        yield 1;
        foo(1);
    } finally {
        foo(2);
    }
    foo(3);
}
generator = testReturnAndThrow();
generator.next();
generator.return();
checkBasicBlock(testReturnAndThrow, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testReturnAndThrow, "foo(2)", ShouldHaveExecuted);
checkBasicBlock(testReturnAndThrow, "foo(3)", ShouldNotHaveExecuted);
generator = testReturnAndThrow();
generator.next();
try {
    generator.throw(1);
} catch { }
checkBasicBlock(testReturnAndThrow, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testReturnAndThrow, "foo(3)", ShouldNotHaveExecuted);

function* testDelegate() {
    yield* [1, 2];
    foo();
}
generator = testDelegate();
generator.next();
generator.next();
checkBasicBlock(testDelegate, "foo()", ShouldNotHaveExecuted);
generator.next();
checkBasicBlock(testDelegate, "foo()", ShouldHaveExecuted);

async function testAwait(promise) {
    foo(1);
    await promise;
    foo(2);
}
testAwait(never);
drainMicrotasks();
checkBasicBlock(testAwait, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testAwait, "await promise", ShouldHaveExecuted);
checkBasicBlock(testAwait, "foo(2)", ShouldNotHaveExecuted);
testAwait(1);
drainMicrotasks();
checkBasicBlock(testAwait, "foo(2)", ShouldHaveExecuted);
assert(basicBlockExecutionCount(testAwait, "foo(1)") === 2, "what is before the await ran twice");
assert(basicBlockExecutionCount(testAwait, "foo(2)") === 1, "what is after the await ran once");

async function testRejection() {
    try {
        await Promise.reject(1);
        foo(1);
    } catch {
        foo(2);
    }
    foo(3);
}
testRejection();
drainMicrotasks();
checkBasicBlock(testRejection, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testRejection, "foo(2)", ShouldHaveExecuted);
checkBasicBlock(testRejection, "foo(3)", ShouldHaveExecuted);

// Wherever it is in the statement.
async function testInAnExpression(x, o) {
    let a = x ? await never : 1;
    foo(1);
    o.a ||= x && foo(await never);
    foo(2);
}
testInAnExpression(false, { a: 1 });
drainMicrotasks();
checkBasicBlock(testInAnExpression, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testInAnExpression, "foo(2)", ShouldHaveExecuted);

async function testInTheArmThatRuns(x) {
    let a = x ? await never : 1;
    foo();
}
testInTheArmThatRuns(true);
drainMicrotasks();
checkBasicBlock(testInTheArmThatRuns, "foo()", ShouldNotHaveExecuted);

async function testKindsOfStatement() {
    var a = await 1;
    foo(1);
    class C extends (await Object) { }
    foo(2);
    label: await 1;
    foo(3);
    ({ a = await 1 } = { });
    foo(4);
    function hoisted() { }
}
testKindsOfStatement();
drainMicrotasks();
for (let i = 1; i <= 4; ++i)
    assert(basicBlockExecutionCount(testKindsOfStatement, "foo(" + i + ")") === 1, "foo(" + i + ") ran once");

// A statement that other statements are a part of has one basic block after it.
async function testInAStatementOfAStatement(x) {
    if (x) {
        await 1;
        foo(1);
    }
    foo(2);
    for (let i = 0; i < 2; ++i) {
        await 1;
        foo(3);
    }
    foo(4);
    switch (await x) {
    case true:
        await 1;
        foo(5);
    }
    foo(6);
    try {
        await 1;
    } finally {
        await 1;
    }
    foo(7);
    {
        await 1;
    }
    foo(8);
}
testInAStatementOfAStatement(true);
drainMicrotasks();
for (let i = 1; i <= 8; ++i)
    assert(basicBlockExecutionCount(testInAStatementOfAStatement, "foo(" + i + ")") === (i === 3 ? 2 : 1), "the count of foo(" + i + ")");

async function testInALoop(list) {
    for (const promise of list) {
        foo(1);
        await promise;
        foo(2);
    }
    foo(3);
}
testInALoop([1, 2, never, 3]);
drainMicrotasks();
assert(basicBlockExecutionCount(testInALoop, "foo(1)") === 3, "three turns start");
assert(basicBlockExecutionCount(testInALoop, "foo(2)") === 2, "two turns end");
checkBasicBlock(testInALoop, "foo(3)", ShouldNotHaveExecuted);

async function* testAsyncGenerator() {
    foo(1);
    yield 1;
    foo(2);
    await never;
    foo(3);
}
generator = testAsyncGenerator();
generator.next();
drainMicrotasks();
checkBasicBlock(testAsyncGenerator, "foo(1)", ShouldHaveExecuted);
checkBasicBlock(testAsyncGenerator, "foo(2)", ShouldNotHaveExecuted);
generator.next();
drainMicrotasks();
checkBasicBlock(testAsyncGenerator, "foo(2)", ShouldHaveExecuted);
checkBasicBlock(testAsyncGenerator, "foo(3)", ShouldNotHaveExecuted);

async function testForAwait() {
    for await (const x of [never])
        foo(1);
    foo(2);
}
testForAwait();
drainMicrotasks();
checkBasicBlock(testForAwait, "foo(1)", ShouldNotHaveExecuted);
checkBasicBlock(testForAwait, "foo(2)", ShouldNotHaveExecuted);

var testArrowFunction = async () => {
    await never;
    foo();
};
testArrowFunction();
drainMicrotasks();
checkBasicBlock(testArrowFunction, "await never", ShouldHaveExecuted);
checkBasicBlock(testArrowFunction, "foo()", ShouldNotHaveExecuted);
