//@ requireOptions("--useJSThreads=1")
// With useJSThreads the FTL uses handler ICs. On a miss the shared slow-path
// thunk calls the operation itself, so an exception it throws unwinds with the
// frame's call site index set to the IC's. DelBy and InstanceOf must register
// an unwind handler for that index, or the enclosing try/catch is skipped.

"use strict";

const thrown = new Error("thrown");

function check(name, op, good, bad, isExpected) {
    function test(o) {
        try {
            op(o);
        } catch (e) {
            return isExpected(e) ? 1 : 2;
        }
        return 0;
    }
    noInline(test);
    for (let i = 0; i < 1e5; ++i) {
        let result = test(good);
        if (result !== 0)
            throw new Error(`${name}: good returned ${result} at ${i}`);
        result = test(bad);
        if (result !== 1)
            throw new Error(`${name}: bad returned ${result} at ${i}`);
    }
}

const isThrown = (e) => e === thrown;
const throwingProxy = new Proxy({}, {
    deleteProperty() { throw thrown; },
    getPrototypeOf() { throw thrown; },
});

check("delete_by_id non-configurable",
    (o) => delete o.a,
    { a: 1 },
    Object.defineProperty({}, "a", { value: 1 }),
    (e) => e instanceof TypeError);

check("delete_by_id proxy", (o) => delete o.a, { a: 1 }, throwingProxy, isThrown);

let key = "a";
check("delete_by_val proxy", (o) => delete o[key], { a: 1 }, throwingProxy, isThrown);

function F() { }
check("instanceof proxy", (o) => o instanceof F, new F, throwingProxy, isThrown);
