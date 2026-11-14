function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }
function throws(f, type, m) {
    try { f(); } catch (e) {
        assert(e instanceof type, m + ': wrong error ' + e);
        return;
    }
    assert(false, m + ': did not throw');
}

struct S {
    x;
    constructor(x) { this.x = x; }
    m(a, b) { return [this.x, a, b]; }
    get g() { return this.x * 2; }
    set s(v) { this.x = v; }
    get both() { return this.x; }
    set both(v) { this.x = v + 1; }
    static make(x) { return new S(x); }
    static twice(s) { return s.m(1, 2); }
    [Symbol.toPrimitive]() { return this.x; }
    *items() { yield this.x; yield this.x + 1; }
}
struct T { y; get h() { return 1; } n() { return this.y; } }

const s = new S(5);

// Instances work, with the arguments and the receiver passed through.
assert(s.m(1, 2).join() === "5,1,2", "method call");
assert(s.g === 10, "getter");
s.s = 7;
assert(s.x === 7, "setter");
s.both = 1;
assert(s.both === 2, "getter and setter of one accessor");
assert(+s === 2, "symbol-keyed method");
assert([...s.items()].join() === "2,3", "generator method");
assert(S.make(3).m().join() === "3,,", "static method");
assert(S.twice(s).join() === "2,1,2", "static method calling an instance method");

// A plain method keeps its name, has no length of its own and the prototype of the original. Only plain methods
// (`name(parameters) { body }`) are branded: accessors, generators and async methods are ordinary functions.
assert(S.prototype.m.name === "m" && !Object.prototype.hasOwnProperty.call(S.prototype.m, "length"), "method name and no own length");
assert(Object.getPrototypeOf(S.prototype.m) === Function.prototype, "method prototype");
assert(Object.getPrototypeOf(S.prototype.items) === Object.getPrototypeOf(function* () { }), "a generator method is not wrapped");
const getter = Object.getOwnPropertyDescriptor(S.prototype, "g").get;
const setter = Object.getOwnPropertyDescriptor(S.prototype, "s").set;
assert(getter.name === "get g" && getter.length === 0, "getter name and length: " + getter.name);
assert(setter.name === "set s" && setter.length === 1, "setter name and length: " + setter.name);
assert(S.prototype[Symbol.toPrimitive].name === "[Symbol.toPrimitive]", "symbol method name");
assert(typeof S.prototype.m === "function", "still a function");
const method = Object.getOwnPropertyDescriptor(S.prototype, "m");
assert(!method.enumerable && method.writable && !method.configurable, "the prototype is sealed, not frozen");

// Anything else is rejected.
throws(() => S.prototype.m.call({}), TypeError, "a plain object");
throws(() => S.prototype.m.call(Object.create(S.prototype)), TypeError, "an object that only inherits");
throws(() => S.prototype.m.call(undefined), TypeError, "undefined");
throws(() => S.prototype.m.call(null), TypeError, "null");
throws(() => S.prototype.m.call(1), TypeError, "a number");
throws(() => S.prototype.m.call("str"), TypeError, "a string");
throws(() => S.prototype.m.call(S.prototype), TypeError, "the prototype itself");
throws(() => S.prototype.m.call(S), TypeError, "the constructor");
throws(() => S.prototype.m.call([]), TypeError, "an array");
throws(() => S.prototype.m.call(new Proxy(s, {})), TypeError, "a proxy of an instance");
throws(() => S.prototype.m.call(new T()), TypeError, "an instance of another struct");
throws(() => T.prototype.n.call(s), TypeError, "the other way round");
// Accessors and generator methods are not generic-checked: any receiver works.
assert(getter.call({ x: 4 }) === 8, "a getter on a plain object");
const target = { x: 0 };
setter.call(target, 6);
assert(target.x === 6, "a setter on a plain object");
assert([...S.prototype.items.call({ x: 1 })].join() === "1,2", "a generator method on a plain object");
assert(Number.isNaN(S.prototype.g), "reading an accessor off the prototype");
assert(Number.isNaN(Object.create(S.prototype).g), "reading an accessor off an inheriting object");
{ const o = Object.create(S.prototype); o.s = 1; assert(o.x === 1, "a setter off an inheriting object"); }
throws(() => S.prototype[Symbol.toPrimitive].call({}), TypeError, "a symbol method");
throws(() => S.twice({ m: S.prototype.m }), TypeError, "the method as a property of a plain object");
assert(S.prototype.m.call(s, 4).join() === "2,4,", "call with an instance");
assert(S.prototype.m.apply(s, [4, 5]).join() === "2,4,5", "apply with an instance");
assert(S.prototype.m.bind(s)(4).join() === "2,4,", "bind with an instance");
throws(() => S.prototype.m.bind({})(), TypeError, "bind with a plain object");
assert(T.prototype.n.call(new T()) === undefined, "the other struct's own instance");

// The brand is not a property.
assert(Object.keys(s).join() === "x", "Object.keys");
assert(Object.getOwnPropertyNames(s).join() === "x", "getOwnPropertyNames");
assert(Reflect.ownKeys(s).join() === "x", "ownKeys");
assert(Object.getOwnPropertySymbols(s).length === 0, "no symbols");
assert(JSON.stringify(s) === '{"x":2}', "JSON.stringify");
assert(Object.entries(s).join() === "x,2", "entries");
const copy = { ...s };
assert(Object.keys(copy).join() === "x" && Object.getOwnPropertySymbols(copy).length === 0, "spread");
const seen = [];
for (const key in s)
    seen.push(key);
assert(seen.join() === "x", "for-in");
assert(Object.isSealed(s) && !Object.isExtensible(s), "still sealed");
assert(Object.isSealed(new T()) && Object.keys(new T()).join() === "y", "another struct");
assert(Object.isSealed(S.prototype), "the prototype is sealed");

// Subclasses.
struct U extends S {
    z;
    constructor(x, z) { super(x); this.z = z; }
    m(a, b) { return ["U", ...super.m(a, b)]; }
    get g() { return super.g + 1; }
    only() { return this.z; }
}
const u = new U(1, 9);
assert(u instanceof S && u instanceof U, "instanceof");
assert(u.m(1, 2).join() === "U,1,1,2", "super.method() works");
assert(u.g === 3, "super getter works");
assert(u.only() === 9, "own method");
assert(S.prototype.m.call(u, 1).join() === "1,1,", "a base method on a subclass instance");
assert(Object.getOwnPropertyDescriptor(S.prototype, "g").get.call(u) === 2, "a base getter on a subclass instance");
assert(Object.keys(u).join() === "x,z", "subclass keys");
assert(JSON.stringify(u) === '{"x":1,"z":9}', "subclass JSON");
throws(() => U.prototype.only.call(s), TypeError, "a subclass method on a base instance");
throws(() => U.prototype.only.call({}), TypeError, "a subclass method on a plain object");
throws(() => U.prototype.m.call({}, 1), TypeError, "an overriding method on a plain object");

// Three levels, and a subclass without a constructor.
struct V extends U { w; wonly() { return this.w; } }
const v = new V();
v.x = 1; v.z = 2; v.w = 3;
assert(v.only() === 2 && v.m(0, 0).join() === "U,1,0,0" && v.g === 3, "a grandchild");
assert(S.prototype.m.call(v).join() === "1,,", "the grandparent's method on a grandchild");
throws(() => V.prototype.wonly.call(new U(1, 2)), TypeError, "a parent instance is not a child");
assert(U.prototype.only.call(v) === 2, "a parent method on a child instance");

// Methods that are not called as methods.
const detached = s.m;
throws(() => detached(), TypeError, "a detached method");
assert(detached.call(s).join() === "2,,", "a detached method with a receiver");
const extracted = Object.getOwnPropertyDescriptor(S.prototype, "g").get;
assert(extracted.call(s) === 4, "an extracted getter with a receiver");

// Methods as callbacks.
assert([1, 2].map(s.m, s).length === 2, "a method as a callback with a receiver");
throws(() => [1].map(s.m), TypeError, "a method as a callback without one");
