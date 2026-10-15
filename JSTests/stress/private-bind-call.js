//@ requireOptions("--exposePrivateIdentifiers=1")
"use strict";

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldThrowTypeError(run) {
    let error;
    try {
        run();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error(`expected a TypeError but got ${error}`);
}

function target(a, b, c) {
    return [this, a, b, c].join();
}

// Neither the public bind nor one of the function's own is used.
let handed = 0;
Function.prototype.bind = function () { handed++; };
target.bind = function () { handed++; };

class Base { method() { } }
class Derived extends Base {
    bindOfSuper() { return super.@bind(1); }
}

function test() {
    const bound = target.@bind("t", 1);
    shouldBe(bound.name, "bound target");
    shouldBe(bound.length, 2);
    shouldBe(bound(2, 3), "t,1,2,3");

    shouldBe(target.@bind()(1, 2, 3), ",1,2,3");
    shouldBe(target.@bind("t", ...[1, 2])(3), "t,1,2,3");
    shouldBe(target.@bind(...["t", 1, 2, 3])(), "t,1,2,3");
    shouldBe(target.@bind?.("t", 1)(2, 3), "t,1,2,3");

    let argumentsEvaluated = 0;
    shouldBe((void 0)?.@bind(argumentsEvaluated++), undefined);
    shouldBe(null?.@bind(argumentsEvaluated++), undefined);
    shouldBe(argumentsEvaluated, 0);
    shouldBe(target?.@bind("t", 1)(2, 3), "t,1,2,3");

    const order = [];
    (order.push("receiver"), target).@bind(order.push("first"), order.push("second"));
    shouldBe(order.join(), "receiver,first,second");
}
noInline(test);

for (let i = 0; i < testLoopCount; i++)
    test();

{
    // Nothing is looked up on the receiver.
    shouldBe(Object.setPrototypeOf(function () { return 1; }, null).@bind()(), 1);
    const proxy = new Proxy(target, {
        get(object, key, receiver) {
            if (key === "bind")
                throw new Error("looked up");
            return Reflect.get(object, key, receiver);
        },
    });
    shouldBe(proxy.@bind("t", 1)(2, 3), "t,1,2,3");

    shouldThrowTypeError(() => ({}).@bind());
    shouldThrowTypeError(() => (void 0).@bind());
    shouldThrowTypeError(() => new Derived().bindOfSuper());
}

shouldBe(handed, 0);
