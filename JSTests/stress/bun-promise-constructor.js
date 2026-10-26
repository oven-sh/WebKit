function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldThrowTypeError(run, message) {
    let error = null;
    try {
        run();
    } catch (thrown) {
        error = thrown;
    }
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError but got ${error}`);
    shouldBe(error.message, message);
}

function settlementOf(promise) {
    let settlement = "pending";
    promise.then(value => { settlement = `fulfilled with ${value}`; }, reason => { settlement = `rejected with ${reason}`; });
    drainMicrotasks();
    return settlement;
}

const missingNewMessage = "Cannot call a constructor Promise without |new|";
const invalidExecutorMessage = "Promise constructor takes a function argument";
const iterationCount = 1e5;

class ForwardingPromise extends Promise {
    constructor(executor) {
        super(executor);
    }
}

class DefaultConstructorPromise extends Promise { }

class ArgumentlessPromise extends Promise {
    constructor() {
        super();
    }
}

function testFunctionProperties() {
    shouldBe(Promise.length, 1);
    shouldBe(Promise.name, "Promise");

    let resolve;
    let reject;
    let executorThis = null;
    let argumentCount = 0;
    new Promise(function (resolveFunction, rejectFunction) {
        "use strict";
        resolve = resolveFunction;
        reject = rejectFunction;
        executorThis = this;
        argumentCount = arguments.length;
    });
    shouldBe(resolve.length, 1);
    shouldBe(reject.length, 1);
    shouldBe(resolve.name, "");
    shouldBe(reject.name, "");
    shouldBe(executorThis, undefined);
    shouldBe(argumentCount, 2);
}

function testCallWithoutNew() {
    shouldThrowTypeError(() => Promise(), missingNewMessage);
    shouldThrowTypeError(() => Promise(() => { }), missingNewMessage);
    shouldThrowTypeError(() => Promise.call(Promise.resolve(), () => { }), missingNewMessage);
}

function createLoggingNewTarget(log) {
    return new Proxy(function () { }, {
        get(target, key, receiver) {
            log.push(`get ${String(key)}`);
            return Reflect.get(target, key, receiver);
        },
    });
}

function testOperationOrder() {
    const log = [];
    const newTarget = createLoggingNewTarget(log);
    shouldThrowTypeError(() => new Promise(1), invalidExecutorMessage);
    shouldThrowTypeError(() => new Promise, invalidExecutorMessage);
    shouldThrowTypeError(() => Reflect.construct(Promise, [1], newTarget), invalidExecutorMessage);
    shouldBe(log.length, 0);

    const promise = Reflect.construct(Promise, [() => { log.push("executor"); }], newTarget);
    shouldBe(log.join(), "get prototype,executor");
    shouldBe(Object.getPrototypeOf(promise), newTarget.prototype);

    let didRunExecutor = false;
    const throwingNewTarget = new Proxy(function () { }, {
        get() {
            throw new TypeError("prototype getter");
        },
    });
    shouldThrowTypeError(() => Reflect.construct(Promise, [() => { didRunExecutor = true; }], throwingNewTarget), "prototype getter");
    shouldBe(didRunExecutor, false);
}

function testThrowingExecutor() {
    shouldBe(settlementOf(new Promise(() => { throw "early"; })), "rejected with early");
    shouldBe(settlementOf(new Promise(resolve => { resolve("first"); throw "late"; })), "fulfilled with first");
    shouldBe(settlementOf(new Promise((resolve, reject) => { reject("first"); throw "late"; })), "rejected with first");

    let resolveInner;
    const inner = new Promise(resolve => { resolveInner = resolve; });
    const outer = new Promise(resolve => { resolve(inner); throw "late"; });
    shouldBe(settlementOf(outer), "pending");
    resolveInner("inner");
    shouldBe(settlementOf(outer), "fulfilled with inner");
}

function testResolvingFunctionsRunOnce() {
    shouldBe(settlementOf(new Promise(resolve => { resolve("first"); resolve("second"); })), "fulfilled with first");
    shouldBe(settlementOf(new Promise((resolve, reject) => { resolve("first"); reject("second"); })), "fulfilled with first");
    shouldBe(settlementOf(new Promise((resolve, reject) => { reject("first"); resolve("second"); })), "rejected with first");
    shouldBe(settlementOf(new Promise((resolve, reject) => { reject("first"); reject("second"); })), "rejected with first");
}

function testSubclasses() {
    for (const constructor of [ForwardingPromise, DefaultConstructorPromise]) {
        const promise = new constructor(resolve => resolve("derived"));
        shouldBe(Object.getPrototypeOf(promise), constructor.prototype);
        shouldBe(promise instanceof Promise, true);
        shouldBe(promise.then(() => { }) instanceof constructor, true);
        shouldBe(settlementOf(promise), "fulfilled with derived");
        shouldBe(settlementOf(new constructor(() => { throw "early"; })), "rejected with early");
    }
    shouldThrowTypeError(() => new ArgumentlessPromise, invalidExecutorMessage);
    shouldThrowTypeError(() => new DefaultConstructorPromise, invalidExecutorMessage);
}

function createPromise(executor) {
    return new Promise(executor);
}
noInline(createPromise);

function createDerivedPromise(executor) {
    return new ForwardingPromise(executor);
}
noInline(createDerivedPromise);

function testOptimizedCallers() {
    let sum = 0;
    const add = value => { sum += value; };
    for (let i = 0; i < iterationCount; ++i) {
        createPromise(resolve => resolve(i)).then(add);
        createDerivedPromise(resolve => resolve(i)).then(add);
        if (!(i % 1000)) {
            shouldThrowTypeError(() => createPromise(i), invalidExecutorMessage);
            shouldThrowTypeError(() => createDerivedPromise(i), invalidExecutorMessage);
            drainMicrotasks();
        }
    }
    drainMicrotasks();
    shouldBe(sum, iterationCount * (iterationCount - 1));
}

testFunctionProperties();
testCallWithoutNew();
testOperationOrder();
testThrowingExecutor();
testResolvingFunctionsRunOnce();
testSubclasses();
testOptimizedCallers();
