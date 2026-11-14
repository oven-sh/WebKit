function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

let checkedErrors = 0;

function rejectionOf(promise) {
    let rejected = false;
    let reason;
    promise.then(() => { }, (error) => {
        rejected = true;
        reason = error;
    });
    drainMicrotasks();
    shouldBe(rejected, true, "the promise is rejected");
    return reason;
}

function shouldHaveNoOwnCause(label, error, constructor) {
    shouldBe(Object.getPrototypeOf(error), constructor.prototype, `${label}: prototype`);
    shouldBe(Object.hasOwn(error, "cause"), false, `${label}: own cause`);
    shouldBe("cause" in error, false, `${label}: "cause" in error`);
    checkedErrors++;
}

function shouldHaveErrors(label, error, length) {
    let descriptor = Object.getOwnPropertyDescriptor(error, "errors");
    shouldBe(descriptor.writable, true, `${label}: errors is writable`);
    shouldBe(descriptor.enumerable, false, `${label}: errors is enumerable`);
    shouldBe(descriptor.configurable, true, `${label}: errors is configurable`);
    shouldBe(Array.isArray(descriptor.value), true, `${label}: errors is an array`);
    shouldBe(descriptor.value.length, length, `${label}: errors.length`);
}

class DerivedPromise extends Promise { }

// A promise with an own "then" gets a reject element function instead of the internal job.
function rejectedPromiseWithOwnThen() {
    let promise = Promise.reject(1);
    promise.then = Promise.prototype.then;
    return promise;
}

// Each row is named after the function that creates the AggregateError.
let promiseAnyRows = [
    ["promiseAnyResolveJob", () => Promise.any([Promise.reject(1), Promise.reject(2)]), 2],
    ["promiseConstructorFuncAny", () => Promise.any([]), 0],
    ["promiseAnyRejectFunction", () => Promise.any([rejectedPromiseWithOwnThen()]), 1],
    ["promiseAnySlow", () => DerivedPromise.any([]), 0],
    ["promiseAnySlowRejectFunction", () => DerivedPromise.any([DerivedPromise.reject(1)]), 1],
];

for (let [label, run, length] of promiseAnyRows) {
    let error = rejectionOf(run());
    shouldHaveNoOwnCause(label, error, AggregateError);
    shouldHaveErrors(label, error, length);
}

// A promise resolved with itself, from a resolving function and from a reaction job.
{
    let resolve;
    let promise = new Promise((resolveFunction) => { resolve = resolveFunction; });
    resolve(promise);
    shouldHaveNoOwnCause("resolving function", rejectionOf(promise), TypeError);
}
{
    let promise = Promise.resolve().then(() => promise);
    shouldHaveNoOwnCause("reaction job", rejectionOf(promise), TypeError);
}

shouldBe(checkedErrors, 7, "errors checked");

// A cause that the options give is an own property, also when it is undefined.
for (let error of [new AggregateError([1], "message", { cause: undefined }), new Error("message", { cause: undefined })]) {
    let descriptor = Object.getOwnPropertyDescriptor(error, "cause");
    shouldBe(descriptor.value, undefined, "explicit cause: value");
    shouldBe(descriptor.enumerable, false, "explicit cause: enumerable");
}
shouldBe(Object.hasOwn(new AggregateError([1]), "cause"), false, "no options: own cause");
