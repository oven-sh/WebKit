//@ requireOptions("--useAOT=1")
// Allocation, conversions and the caches behind property access, in code from the static compiler.

function shouldBe(actual, expected) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error("bad value: " + JSON.stringify(actual) + ", expected " + JSON.stringify(expected));
}
function shouldThrow(f, type) {
    let error;
    try { f(); } catch (e) { error = e; }
    if (!(error instanceof type))
        throw new Error("did not throw " + type.name + ": " + error);
}

// A property found on the prototype chain, and what can happen to it.
(function () {
    function getM(o) { return o.m; }
    class Base { m() { return "base"; } }
    class Middle extends Base { }
    class Leaf extends Middle { }
    const leaf = new Leaf;
    for (let i = 0; i < 10; ++i)
        shouldBe(getM(leaf)(), "base");
    Middle.prototype.m = () => "middle";
    for (let i = 0; i < 10; ++i)
        shouldBe(getM(leaf)(), "middle");
    Base.prototype.m = () => "unseen";
    shouldBe(getM(leaf)(), "middle");
    delete Middle.prototype.m;
    for (let i = 0; i < 10; ++i)
        shouldBe(getM(leaf)(), "unseen");
    Object.setPrototypeOf(Middle.prototype, { m: () => "other" });
    for (let i = 0; i < 10; ++i)
        shouldBe(getM(leaf)(), "other");
    leaf.m = () => "own";
    shouldBe(getM(leaf)(), "own");
    shouldBe(getM("string"), undefined);
    String.prototype.m = () => "string";
    for (let i = 0; i < 10; ++i)
        shouldBe(getM("string")(), "string");
    delete String.prototype.m;
    shouldBe(getM("string"), undefined);
})();

// A property that is nowhere, until it is.
(function () {
    function getMissing(o) { return o.missing; }
    const object = { a: 1 };
    for (let i = 0; i < 10; ++i)
        shouldBe(getMissing(object), undefined);
    Object.prototype.missing = 1;
    for (let i = 0; i < 10; ++i)
        shouldBe(getMissing(object), 1);
    delete Object.prototype.missing;
    for (let i = 0; i < 10; ++i)
        shouldBe(getMissing(object), undefined);
    shouldBe(getMissing(new Proxy({}, { get() { return 2; } })), 2);
    shouldThrow(() => getMissing(null), TypeError);
})();

// A new property, and a prototype that comes to have a say.
(function () {
    function Point(x) { this.x = x; this.y = 2; }
    function setZ(o) { o.z = 3; return o; }
    for (let i = 0; i < 10; ++i)
        shouldBe(setZ(new Point(1)), { x: 1, y: 2, z: 3 });
    let calls = 0;
    Object.defineProperty(Point.prototype, "z", { set(v) { calls++; }, configurable: true });
    for (let i = 0; i < 10; ++i)
        shouldBe(setZ(new Point(1)), { x: 1, y: 2 });
    shouldBe(calls, 10);
    Object.defineProperty(Point.prototype, "z", { value: 0, writable: false });
    shouldBe(setZ(new Point(1)), { x: 1, y: 2 });
    shouldThrow(() => { "use strict"; new Point(1).z = 3; }, TypeError);
    Object.defineProperty(Object.prototype, "y", { set(v) { calls++; }, configurable: true });
    shouldBe(new Point(1), { x: 1 });
    delete Object.prototype.y;
    shouldBe(new Point(1), { x: 1, y: 2 });
})();

// Sites that see many structures, by name and by value, with more properties than fit in an object.
(function () {
    function get(o) { return o.k; }
    function set(o, v) { o.k = v; }
    function getByVal(o, k) { return o[k]; }
    function putByVal(o, k, v) { o[k] = v; }
    const objects = [];
    for (let i = 0; i < 40; ++i) {
        const object = {};
        for (let j = 0; j < i; ++j)
            object["p" + j] = j;
        objects.push(object);
    }
    for (let round = 0; round < 4; ++round) {
        objects.forEach((object, i) => {
            shouldBe(get(object), round ? i + round - 1 : undefined);
            set(object, i + round);
            shouldBe(getByVal(object, "k"), i + round);
            putByVal(object, "q" + round, round);
            shouldBe(getByVal(object, "q" + round), round);
            shouldBe(getByVal(object, "p" + (i - 1)), i ? i - 1 : undefined);
        });
    }
    Object.defineProperty(Object.prototype, "fresh", { set(v) { this._fresh = v; }, configurable: true });
    objects.forEach(object => { putByVal(object, "fresh", 1); shouldBe(object._fresh, 1); shouldBe(Object.hasOwn(object, "fresh"), false); });
    delete Object.prototype.fresh;
    shouldBe(getByVal([1, 2], "length"), 2);
    shouldBe(getByVal(function f(a) { }, "name"), "f");
    shouldBe(getByVal({ 1: "one" }, "1"), "one");
    shouldBe(getByVal("abc", 1), "b");
    shouldBe(getByVal([1.5, , 2.5], 1), undefined);
    const array = [];
    for (let i = 0; i < 100; ++i)
        putByVal(array, i, i % 3 ? i : { i });
    shouldBe(array.length, 100);
    shouldBe(array[99], { i: 99 });
    putByVal(array, 200, 1);
    shouldBe(array.length, 201);
    shouldBe(150 in array, false);
})();

// Allocation.
(function () {
    function literal(a) { return { a, b: [a, a + 1], c: { d: a } }; }
    function closures(a) { let b = a; return [() => b++, function () { return b; }, function* () { yield b; }, async () => b]; }
    function Constructor() { this.a = 1; }
    function loopScopes() { const fs = []; for (let i = 0; i < 3; ++i) fs.push(() => i); return fs.map(f => f()); }
    for (let i = 0; i < 100; ++i) {
        shouldBe(literal(i), { a: i, b: [i, i + 1], c: { d: i } });
        const [increment, read, generator] = closures(i);
        increment();
        shouldBe([read(), generator().next().value, read.prototype.constructor === read, increment.prototype], [i + 1, i + 1, true, undefined]);
        shouldBe(new Constructor, { a: 1 });
        shouldBe(loopScopes(), [0, 1, 2]);
    }
    Constructor.prototype = { inherited: true };
    shouldBe(new Constructor().inherited, true);
    shouldBe(Reflect.construct(Constructor, [], Array) instanceof Array, true);
    shouldBe([..."ab", ...[1, 2], 3], ["a", "b", 1, 2, 3]);
    shouldBe([/a/g.flags, /b/.test("abc"), new Array(3).length, `x${1}y${{ toString() { return "z"; } }}`], ["g", true, 3, "x1yz"]);
})();

// Conversions and tests.
(function () {
    function types(...values) { return values.map(v => typeof v); }
    function sloppyThis() { return typeof this; }
    function strictThis() { "use strict"; return this; }
    function equal(a, b) { return [a == b, a === b, a != b, a !== b]; }
    class Private { #f = 1; static #s() { return 2; } #m() { return this.#f; } get() { return this.#m() + Private.#s(); } set(v) { this.#f = v; } static has(o) { return [#f in o, #m in o]; } }
    class HasInstance { static [Symbol.hasInstance](v) { return v === 1; } }
    for (let i = 0; i < 3; ++i) {
        shouldBe(types(1, "a", undefined, null, {}, () => { }, Symbol(), 1n, true), ["number", "string", "undefined", "object", "object", "function", "symbol", "bigint", "boolean"]);
        shouldBe([sloppyThis.call(1), sloppyThis.call(undefined), strictThis.call(1), strictThis.call(undefined), strictThis()], ["object", "object", 1, undefined, undefined]);
        shouldBe(equal("ab", "a" + String.fromCharCode(98)), [true, true, false, false]);
        shouldBe(equal("ab", "ac"), [false, false, true, true]);
        shouldBe(equal(1, 1.0), [true, true, false, false]);
        shouldBe(equal(0.5, 0.5), [true, true, false, false]);
        shouldBe(equal(NaN, NaN), [false, false, true, true]);
        shouldBe(equal(0, -0), [true, true, false, false]);
        shouldBe(equal(null, undefined), [true, false, false, true]);
        shouldBe(equal(null, 0), [false, false, true, true]);
        shouldBe(equal(null, {}), [false, false, true, true]);
        shouldBe(equal("1", 1), [true, false, false, true]);
        shouldBe(equal(1n, 1n), [true, true, false, false]);
        shouldBe(equal({}, {}), [false, false, true, true]);
        shouldBe(equal({ valueOf() { return 1; } }, 1), [true, false, false, true]);
        shouldBe(equal(true, 1), [true, false, false, true]);
        shouldBe(equal(Symbol.iterator, Symbol.iterator), [true, true, false, false]);
        const object = new Private;
        object.set(5);
        shouldBe([object.get(), Private.has(object), Private.has({})], [7, [true, true], [false, false]]);
        shouldThrow(() => Private.prototype.get.call({}), TypeError);
        shouldBe([1 instanceof HasInstance, [] instanceof Array, {} instanceof Array, 1 instanceof Object, (() => { }).bind() instanceof Function], [true, true, false, false, true]);
        shouldThrow(() => 1 instanceof 1, TypeError);
        shouldThrow(() => "a" in 1, TypeError);
        shouldThrow(() => { tdz; let tdz; }, ReferenceError);
        shouldThrow(() => { class X extends Object { constructor() { this.a = 1; } } new X; }, ReferenceError);
        shouldThrow(() => { const c = 1; c = 2; }, TypeError);
        shouldBe(["abc".length, [1, 2].length, { length: "l" }.length, (function (a, b) { }).length], [3, 2, "l", 2]);
        const accessors = { get a() { return 1; }, set a(v) { this.b = v; }, get ["c"]() { return 3; }, __proto__: { inherited: 4 } };
        accessors.a = 2;
        shouldBe([accessors.a, accessors.b, accessors.c, accessors.inherited, delete accessors.b, "b" in accessors], [1, 2, 3, 4, true, false]);
        const base = { m() { return this.v; }, set s(v) { this.w = v; } };
        const derived = { __proto__: base, v: 1, m() { super.s = 2; return [super.m(), super["m"](), this.w]; } };
        shouldBe(derived.m(), [1, 1, 2]);
        with ({ scoped: 1 }) shouldBe(scoped, 1);
    }
})();
