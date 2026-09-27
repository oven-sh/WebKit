// An InternalFunction with the ReportsNoConstructData flag keeps its functionForConstruct for `new f()`,
// and is not a constructor to anything that asks before it constructs.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + String(expected) + " but got " + String(actual));
}

function shouldBeArray(actual, expected) {
    shouldBe(Array.isArray(actual), true);
    shouldBe(Object.getPrototypeOf(actual), Array.prototype);
    shouldBe(JSON.stringify(actual), expected);
}

function shouldThrowTypeError(func) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof TypeError))
        throw new Error("expected a TypeError but got " + String(error));
}

const f = $vm.createInternalFunctionWithoutConstructData();
const isConstructor = $vm.createBuiltin("(function (c) { return @isConstructor(c); })");

function constructWithNoArguments() { return new f(); }
function constructWithTwoArguments(a, b) { return new f(a, b); }
function constructWithSpread(args) { return new f(...args); }
function constructAny(constructor) { return new constructor(1); }
function callWithTwoArguments(a, b) { return f(a, b); }
function callWithSpread(args) { return f(...args); }
function queryIsConstructor() { return isConstructor(f); }
noInline(constructWithNoArguments);
noInline(constructWithTwoArguments);
noInline(constructWithSpread);
noInline(constructAny);
noInline(callWithTwoArguments);
noInline(callWithSpread);
noInline(queryIsConstructor);

class Plain { }

for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(constructWithNoArguments(), "new:0");
    shouldBe(constructWithTwoArguments(1, 2), "new:2");
    shouldBe(constructWithSpread([1, 2, 3]), "new:3");
    shouldBe(callWithTwoArguments(1, 2), "call:2");
    shouldBe(callWithSpread([1]), "call:1");
    shouldBe(queryIsConstructor(), false);

    // The same `new` site sees a class and the flagged function.
    shouldBe(constructAny(i & 1 ? f : Plain) instanceof Plain, !(i & 1));
}

class Species extends Array {
    static get [Symbol.species]() { return f; }
}

class Reparented extends Array {
    constructor() { super(); }
}
Object.setPrototypeOf(Reparented, f);

class ReparentedWithField extends Array {
    field = 1;
}
Object.setPrototypeOf(ReparentedWithField, f);

function doors() {
    shouldBe(typeof f, "function");
    shouldBe(isConstructor(f), false);
    shouldBe(isConstructor(f.bind(null)), false);
    shouldBe(isConstructor(new Proxy(f, {})), false);

    // A |this| that is not a constructor gets a plain array.
    shouldBeArray(Array.of.call(f, 1, 2, 3), "[1,2,3]");
    shouldBeArray(Array.of.call(f), "[]");
    shouldBeArray(Array.from.call(f, [1, 2, 3]), "[1,2,3]");
    shouldBeArray(Array.from.call(f, { length: 2, 0: "a", 1: "b" }), '["a","b"]');

    const species = new Species(1, 2, 3);
    shouldThrowTypeError(() => species.filter(() => true));
    shouldThrowTypeError(() => species.map(x => x));
    shouldThrowTypeError(() => species.flatMap(x => [x]));
    shouldThrowTypeError(() => species.slice());
    shouldThrowTypeError(() => species.splice(0, 1));
    shouldThrowTypeError(() => species.concat([4]));
    shouldThrowTypeError(() => species.flat());

    const ownConstructor = [1, 2, 3];
    ownConstructor.constructor = { [Symbol.species]: f };
    shouldThrowTypeError(() => ownConstructor.map(x => x));

    shouldThrowTypeError(() => Reflect.construct(f, []));
    shouldThrowTypeError(() => Reflect.construct(f, [], Object));
    shouldThrowTypeError(() => Reflect.construct(Object, [], f));
    shouldThrowTypeError(() => new (f.bind(null))());
    shouldThrowTypeError(() => new (new Proxy(f, {}))());
    shouldThrowTypeError(() => { class Derived extends f { } });
    shouldThrowTypeError(() => Promise.resolve.call(f, 1));
    shouldThrowTypeError(() => Uint8Array.from.call(f, []));
    shouldThrowTypeError(() => Uint8Array.of.call(f));

    // bind and Proxy keep the call.
    shouldBe(f.bind(null, 1)(2), "call:2");
    shouldBe(new Proxy(f, {})(1, 2, 3), "call:3");

    // `super()` reads no construct data. It runs functionForConstruct with the class as new.target, and
    // functionForConstruct of this function throws for a new.target that is not the function.
    shouldThrowTypeError(() => new Reparented());
    shouldThrowTypeError(() => new ReparentedWithField());
    shouldThrowTypeError(() => Array.of.call(Reparented, 1, 2, 3));
    shouldThrowTypeError(() => Reflect.construct(Reparented, []));
}
noInline(doors);

for (let i = 0; i < 1e3; ++i)
    doors();
