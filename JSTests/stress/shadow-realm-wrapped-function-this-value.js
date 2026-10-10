//@ requireOptions("--useShadowRealm=1")

// https://tc39.es/proposal-shadowrealm/#sec-ordinary-wrapped-function-call
// A wrapped function passes |this| through GetWrappedValue, like an argument: a primitive crosses as is, a callable
// is wrapped for the target realm, and any other object throws a TypeError from the caller realm before the target runs.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message ? message + ": " : ""}expected ${String(expected)} but got ${String(actual)}`);
}

function shouldThrowTypeErrorFromThisRealm(func, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error(`${message}: expected a TypeError but got ${String(error)}`);
    shouldBe($.globalObjectFor(error), globalThis, `${message}: realm of the TypeError`);
}

// With the JIT on, only the first call of a plain function target takes remoteFunctionCallForJSFunction. After that the
// remoteFunctionCallGenerator thunk wraps the arguments and |this| itself, so a few rounds cover both. The last block
// uses testLoopCount to let the targets tier up as well.
const rounds = 3;

const realm = new ShadowRealm();
realm.evaluate(`globalThis.calls = 0`);
const callsInRealm = realm.evaluate(`() => globalThis.calls`);

const describeThisSource = `(function describeThis() {
    "use strict";
    globalThis.calls++;
    if (this === undefined || this === null)
        return String(this);
    if (typeof this === "function")
        return "function:" + (Object.getPrototypeOf(this) === Function.prototype) + ":" + this();
    if (typeof this === "symbol")
        return "symbol:" + this.description;
    return typeof this + ":" + String(this);
})`;

// A JSFunction target takes remoteFunctionCallForJSFunction or the remoteFunctionCallGenerator thunk. The others take
// remoteFunctionCallGeneric.
const targets = [
    ["function", realm.evaluate(describeThisSource), null],
    ["proxy", realm.evaluate(`new Proxy(${describeThisSource}, {})`), null],
    ["proxy with an apply trap", realm.evaluate(`new Proxy(${describeThisSource}, { apply(target, thisValue, args) { return Reflect.apply(target, thisValue, args); } })`), null],
    ["bound function", realm.evaluate(`${describeThisSource}.bind("bound")`), "string:bound"],
];

function outer() { return "outer"; }

for (let i = 0; i < rounds; ++i) {
    for (const [kind, wrapped, fixedThis] of targets) {
        const expect = (thisValue, expected) => {
            const before = callsInRealm();
            shouldBe(wrapped.call(thisValue), fixedThis ?? expected, `${kind} target, call()`);
            shouldBe(Reflect.apply(wrapped, thisValue, []), fixedThis ?? expected, `${kind} target, Reflect.apply`);
            shouldBe(wrapped.bind(thisValue)(), fixedThis ?? expected, `${kind} target, bind()`);
            shouldBe(callsInRealm(), before + 3, `${kind} target, calls`);
        };

        expect(undefined, "undefined");
        expect(null, "null");
        expect(5, "number:5");
        expect(-0, "number:0");
        expect("s", "string:s");
        expect(true, "boolean:true");
        expect(10n, "bigint:10");
        expect(Symbol("d"), "symbol:d");
        expect(Symbol.iterator, "symbol:Symbol.iterator");
        expect(outer, "function:true:outer");
        expect(() => "arrow", "function:true:arrow");

        for (const thisValue of [{ }, [], new Proxy({}, {}), new String("boxed"), Object(Symbol("boxed")), globalThis, Object.prototype, new Date(0), /re/, new Error("e")]) {
            const before = callsInRealm();
            shouldThrowTypeErrorFromThisRealm(() => wrapped.call(thisValue), `${kind} target, call() with a non-callable object`);
            shouldThrowTypeErrorFromThisRealm(() => Reflect.apply(wrapped, thisValue, []), `${kind} target, Reflect.apply with a non-callable object`);
            shouldThrowTypeErrorFromThisRealm(() => wrapped.bind(thisValue)(), `${kind} target, bind() with a non-callable object`);
            shouldThrowTypeErrorFromThisRealm(() => ({ method: wrapped }).method(), `${kind} target, called as a method`);
            shouldBe(callsInRealm(), before, `${kind} target must not run when |this| cannot be wrapped`);
        }

        // A revoked Proxy is not callable any more... but IsCallable only looks at [[Call]], which a revoked callable Proxy keeps.
        {
            const { proxy, revoke } = Proxy.revocable(function () { return "revoked"; }, {});
            revoke();
            shouldThrowTypeErrorFromThisRealm(() => wrapped.call(proxy), `${kind} target, revoked callable Proxy as |this|`);
        }
    }
}

// The wrapped function itself as |this|: JSRemoteFunction::tryCreate unwraps it and wraps its target again, for the
// shadow realm this time. So the realm sees a callable of its own, and calling that runs describeThis once more with
// |this| = undefined.
{
    const wrapped = targets[0][1];
    for (let i = 0; i < rounds; ++i) {
        const before = callsInRealm();
        shouldBe(wrapped.call(wrapped), "function:true:undefined");
        shouldBe(callsInRealm(), before + 2);
    }
}

// A sloppy-mode target sees its own global object for undefined and null, and a wrapper object of its own realm for
// any other primitive.
{
    const sloppy = realm.evaluate(`(function () {
        if (this === globalThis)
            return "globalThis";
        if (typeof this === "function")
            return "function:" + this();
        return typeof this + ":" + (this instanceof Number || this instanceof String || this instanceof Symbol) + ":" + this.toString();
    })`);
    for (let i = 0; i < rounds; ++i) {
        shouldBe(sloppy(), "globalThis");
        shouldBe(sloppy.call(undefined), "globalThis");
        shouldBe(sloppy.call(null), "globalThis");
        shouldBe(sloppy.call(3), "object:true:3");
        shouldBe(sloppy.call("s"), "object:true:s");
        shouldBe(sloppy.call(Symbol("q")), "object:true:Symbol(q)");
        shouldBe(sloppy.call(outer), "function:outer");
        shouldThrowTypeErrorFromThisRealm(() => sloppy.call({}), "sloppy target, call() with an object");
    }
}

// An arrow function target ignores |this|, but the wrapper still cannot let an object through.
{
    const arrow = realm.evaluate(`() => "arrow"`);
    for (let i = 0; i < rounds; ++i) {
        shouldBe(arrow.call(1), "arrow");
        shouldBe(arrow.call(outer), "arrow");
        shouldThrowTypeErrorFromThisRealm(() => arrow.call({}), "arrow target, call() with an object");
    }
}

// The other direction: the shadow realm calls a wrapped function of this realm.
{
    let calls = 0;
    function describeThis() {
        "use strict";
        calls++;
        if (this === undefined || this === null)
            return String(this);
        if (typeof this === "function")
            return "function:" + (Object.getPrototypeOf(this) === Function.prototype) + ":" + this();
        return typeof this + ":" + String(this);
    }
    const callWith = realm.evaluate(`(function callWith(f, kind) {
        switch (kind) {
        case "none": return f();
        case "number": return f.call(42);
        case "string": return Reflect.apply(f, "s", []);
        case "callable": return f.call(() => "inner");
        case "bound callable": return f.bind(function () { return "inner bound"; })();
        case "object":
        case "array":
        case "global":
        case "method": {
            try {
                if (kind === "object")
                    f.call({});
                else if (kind === "array")
                    f.apply([], []);
                else if (kind === "global")
                    f.call(globalThis);
                else
                    ({ f }).f();
            } catch (e) {
                return "threw " + e.constructor.name + " of the shadow realm: " + (e instanceof TypeError);
            }
            return "did not throw";
        }
        }
    })`);
    for (let i = 0; i < rounds; ++i) {
        shouldBe(callWith(describeThis, "none"), "undefined");
        shouldBe(callWith(describeThis, "number"), "number:42");
        shouldBe(callWith(describeThis, "string"), "string:s");
        shouldBe(callWith(describeThis, "callable"), "function:true:inner");
        shouldBe(callWith(describeThis, "bound callable"), "function:true:inner bound");
        const before = calls;
        shouldBe(callWith(describeThis, "object"), "threw TypeError of the shadow realm: true");
        shouldBe(callWith(describeThis, "array"), "threw TypeError of the shadow realm: true");
        shouldBe(callWith(describeThis, "global"), "threw TypeError of the shadow realm: true");
        shouldBe(callWith(describeThis, "method"), "threw TypeError of the shadow realm: true");
        shouldBe(calls, before, "the target must not run when |this| cannot be wrapped");
    }
}

// Arguments land in the right slots for every argument count, with |this| next to them, on every tier.
{
    const collect = realm.evaluate(`"use strict"; (function (...args) { return String(this) + "|" + args.join(",") })`);
    const collectViaProxy = realm.evaluate(`"use strict"; new Proxy(function (...args) { return String(this) + "|" + args.join(",") }, {})`);
    const hundred = new Array(100).fill(9);
    const hundredJoined = hundred.join(",");
    function object() { return "O"; }
    for (let i = 0; i < testLoopCount; ++i) {
        for (const f of [collect, collectViaProxy]) {
            shouldBe(f(), "undefined|");
            shouldBe(f.call("T"), "T|");
            shouldBe(f.call("T", 1), "T|1");
            shouldBe(f.call(2, 1, 2), "2|1,2");
            shouldBe(f.call(true, 1, 2, 3), "true|1,2,3");
            shouldBe(f.call("T", 1, 2, 3, 4, 5, 6, 7), "T|1,2,3,4,5,6,7");
            shouldBe(f(1, 2, 3, 4, 5, 6, 7, 8), "undefined|1,2,3,4,5,6,7,8");
            if (!(i % 64)) {
                shouldBe(f.apply("T", hundred), "T|" + hundredJoined);
                shouldBe(f.call(object, 1, object).replace(/function [^|,]*/g, "F"), "F|1,F");
                let error = null;
                try {
                    f.call({}, 1, 2);
                } catch (e) {
                    error = e;
                }
                shouldBe(error instanceof TypeError, true, "object |this| on a warm target");
            }
        }
    }
}

// Wrapping order is observable through the "length" and "name" reads of WrappedFunctionCreate: each argument in
// order, then |this|.
{
    const log = [];
    function observed(tag) {
        const f = function () { return tag; };
        Object.defineProperty(f, "name", { get() { log.push(tag + ".name"); return tag; } });
        Object.defineProperty(f, "length", { get() { log.push(tag + ".length"); return 0; } });
        return f;
    }
    const takesTwo = realm.evaluate(`(function (a, b) { "use strict"; return [this(), a(), b()].join() })`);
    const takesTwoViaProxy = realm.evaluate(`new Proxy(function (a, b) { "use strict"; return [this(), a(), b()].join() }, {})`);
    for (let i = 0; i < rounds; ++i) {
        for (const f of [takesTwo, takesTwoViaProxy]) {
            log.length = 0;
            shouldBe(f.call(observed("this"), observed("a"), observed("b")), "this,a,b");
            shouldBe(log.join(), "a.length,a.name,b.length,b.name,this.length,this.name");

            // When an argument cannot be wrapped, |this| is not looked at and the target does not run.
            log.length = 0;
            shouldThrowTypeErrorFromThisRealm(() => f.call(observed("this"), observed("a"), {}), "non-callable last argument");
            shouldBe(log.join(), "a.length,a.name");
        }
    }
}
