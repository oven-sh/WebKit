function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`FAIL: expected '${expected}' actual '${actual}'`);
}

// https://tc39.es/ecma262/#sec-promise.withResolvers creates the data properties in the order
// "promise", "resolve", "reject", and that order is observable through [[OwnPropertyKeys]].
{
    let result = Promise.withResolvers();
    shouldBe(Object.keys(result).join(), "promise,resolve,reject");
    shouldBe(Reflect.ownKeys(result).join(), "promise,resolve,reject");
    shouldBe(JSON.stringify(result, (key, value) => typeof value === "function" ? "function" : value), `{"promise":{},"resolve":"function","reject":"function"}`);
    shouldBe(Object.getPrototypeOf(result), Object.prototype);

    for (let key of ["promise", "resolve", "reject"]) {
        let descriptor = Object.getOwnPropertyDescriptor(result, key);
        shouldBe(descriptor.writable, true);
        shouldBe(descriptor.enumerable, true);
        shouldBe(descriptor.configurable, true);
    }

    shouldBe(result.promise instanceof Promise, true);
    shouldBe(typeof result.resolve, "function");
    shouldBe(typeof result.reject, "function");
}

// The same holds for a subclass and for a foreign constructor, which take the slow NewPromiseCapability path.
{
    class Derived extends Promise { }
    let result = Promise.withResolvers.call(Derived);
    shouldBe(Object.keys(result).join(), "promise,resolve,reject");
    shouldBe(result.promise instanceof Derived, true);
}

{
    let calls = [];
    function NotAPromise(executor) {
        executor((value) => calls.push(["resolve", value]), (reason) => calls.push(["reject", reason]));
    }
    let result = Promise.withResolvers.call(NotAPromise);
    shouldBe(Object.keys(result).join(), "promise,resolve,reject");
    shouldBe(result.promise instanceof NotAPromise, true);
    result.resolve(1);
    result.reject(2);
    shouldBe(JSON.stringify(calls), `[["resolve",1],["reject",2]]`);
}

// The values still land in the right slots after the reorder.
{
    let { promise, resolve, reject } = Promise.withResolvers();
    let settled = [];
    promise.then((value) => settled.push(["fulfilled", value]), (reason) => settled.push(["rejected", reason]));
    resolve(42);
    reject(new Error("ignored"));
    drainMicrotasks();
    shouldBe(JSON.stringify(settled), `[["fulfilled",42]]`);
}

{
    let { promise, resolve, reject } = Promise.withResolvers();
    let settled = [];
    promise.then((value) => settled.push(["fulfilled", value]), (reason) => settled.push(["rejected", reason]));
    reject(42);
    resolve("ignored");
    drainMicrotasks();
    shouldBe(JSON.stringify(settled), `[["rejected",42]]`);
}
