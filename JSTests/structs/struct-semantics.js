// Struct bodies as class bodies, the constructor method as an initializer that is called (never constructed), super()
// as a call of the parent's constructor method, and the integrity levels of the constructor and the prototype.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, type, m) {
    try { fn(); } catch (e) { assert(e instanceof type, m + " threw " + e); return; }
    throw new Error("did not throw: " + m);
}

// Field initializers, computed names, private names and methods, static fields and blocks.
{
    const k = "dyn", sym = Symbol("s");
    struct S {
        a = 1;
        [k] = 2;
        [sym] = 7;
        #p = 2;
        static s = 3;
        static { S.t = 4; }
        static #sp = 5;
        static getSp() { return S.#sp; }
        get p() { return this.#p; }
        #m() { return this.a + this.#p; }
        callM() { return this.#m(); }
        late = this.a + 10;
    }
    const s = new S();
    assert(s.a === 1 && s.dyn === 2 && s[sym] === 7 && s.p === 2 && s.late === 11, "initializers");
    assert(S.s === 3 && S.t === 4 && S.getSp() === 5, "static elements");
    assert(Object.isSealed(s) && !Object.isExtensible(s), "sealed");
    assert(s.callM() === 3, "a private method");
    assert(Object.keys(s).join() === "a,dyn,late", "private fields are not keys");
    assert(Reflect.ownKeys(s).length === 4 && Reflect.ownKeys(s)[3] === sym, "the symbol key comes last");

    struct Order {
        first = this.second;
        second = 5;
        seen = Object.keys(this).join();
    }
    const o = new Order();
    assert(o.first === undefined && o.second === 5 && o.seen === "first,second,seen", "every field exists before an initializer runs");

    struct Adds { x = (this.y = 1); }
    throws(() => new Adds(), TypeError, "an initializer cannot add a property");
    struct Thrower { x = (() => { throw new RangeError("boom"); })(); }
    throws(() => new Thrower(), RangeError, "an initializer can throw");

    const log = [];
    struct P1 { a = log.push("a"); constructor() { log.push("P1 body"); } }
    struct P2 extends P1 { b = log.push("b"); constructor() { log.push("P2 before"); super(); log.push("P2 after"); } }
    new P2();
    assert(log.join() === "a,b,P2 before,P1 body,P2 after", "the initializers of the chain, parent first, then the constructors: " + log.join());

    let count = 0;
    struct Once { [(count++, "c")] = 1; }
    new Once();
    new Once();
    assert(count === 1, "a computed field name is evaluated once, when the struct is defined");

    struct PA { #x = 1; getX() { return this.#x; } }
    struct PB extends PA { #y = 2; getY() { return this.#y; } }
    const pb = new PB();
    assert(pb.getX() === 1 && pb.getY() === 2, "private fields of a chain");
    struct PM { #m() { return 5; } call() { return this.#m(); } }
    struct PM2 extends PM { n() { return this.call(); } }
    assert(new PM().call() === 5 && new PM2().n() === 5, "a private method is installed on an instance of a derived struct too");
}

// Fields named by array indices come first in the key order.
{
    struct Idx { 1; a; 0; }
    assert(Reflect.ownKeys(new Idx()).join() === "0,1,a", "index-named fields: " + Reflect.ownKeys(new Idx()).join());
    struct Idx2 { 2 = "two"; [1] = "one"; }
    const idx2 = new Idx2();
    assert(idx2[2] === "two" && idx2[1] === "one" && Reflect.ownKeys(idx2).join() === "1,2", "index-named fields with initializers");
    throws(() => { "use strict"; delete idx2[1]; }, TypeError, "an index-named field cannot be deleted");
    throws(() => { "use strict"; idx2[9] = 1; }, TypeError, "an index cannot be added");
}

// The constructor method is called with the instance as its this value from its first line, and it is not
// constructed: new.target is undefined in it, and the constructor function has length 0.
{
    struct A { a; constructor(x) { this.a = x; } }
    struct B extends A { b; constructor(x) { this.b = 1; super(x); super(x + 1); } }
    const b = new B(2);
    assert(b.a === 3 && b.b === 1, "this is usable before super(), which can be called again");
    struct C extends A { constructor() { } }
    assert(new C().a === undefined, "super() is not required");
    struct D extends A { }
    assert(new D(5).a === undefined, "no constructor method: the constructor of the parent is not run");

    struct NT { t = 1; constructor(a, b) { this.t = new.target; } }
    assert(NT.length === 0 && new NT().t === undefined, "length 0 and no new.target");
    struct Base0 { constructor() { super(); } }
    throws(() => new Base0(), TypeError, "super() in a base struct has no constructor to call");

    struct N extends null { a; }
    assert(Object.getPrototypeOf(N.prototype) === null && Object.getPrototypeOf(N) === Function.prototype, "extends null links");
    assert("a" in new N() && Object.isSealed(new N()), "extends null constructs");
    struct N2 extends null { constructor() { super(); } }
    throws(() => new N2(), TypeError, "super() with extends null");

    struct Sup { x; }
    struct Sub { y; }
    throws(() => Reflect.construct(Sup, [], Sub), TypeError, "another new.target");
    assert(Object.keys(Reflect.construct(Sup, [], Sup)).join() === "x", "the struct as new.target");
    class Extender extends Sup { }
    throws(() => new Extender(), TypeError, "a class cannot extend a struct");
}

// The result of new is the instance, whatever the constructor method returns.
{
    const other = {};
    struct R { x; constructor() { return other; } }
    const r = new R();
    assert(r !== other && Object.isSealed(r) && "x" in r, "a returned object is dropped");
    struct K extends R { y; }
    assert(Object.keys(new K()).join() === "x,y", "a struct that extends one whose constructor returns an object");
}

// Integrity: the prototype of an unshared struct is sealed, its constructor is not. A shared struct constructor is.
{
    struct S { m() { } }
    assert(Object.isExtensible(S) && Object.isSealed(S.prototype) && !Object.isFrozen(S.prototype), "unshared");
    assert(Object.getOwnPropertyDescriptor(S.prototype, "m").writable === true, "a method is writable");
    S.extra = 1;
    assert(S.extra === 1, "a static property can be added to the constructor");
    shared struct T { static f() { } }
    assert(Object.isSealed(T) && !Object.isExtensible(T) && !Object.isFrozen(T), "shared: sealed, not frozen");
    assert(T.prototype === null, "a shared struct has no prototype");
    throws(() => new T() instanceof T, TypeError, "instanceof needs a prototype");
    assert(Object.getPrototypeOf(new T()) === null, "instances have a null prototype");
}

// Shared structs take initializers and computed names, and static private names.
{
    shared struct SI { a = 1; b = "x"; ["c" + 1] = 3; static #h = 1; static get h() { return SI.#h; } }
    const si = new SI();
    assert(si.a === 1 && si.b === "x" && si.c1 === 3 && SI.h === 1, "shared initializers");
    assert(Object.keys(si).join() === "a,b,c1" && Object.isSealed(si), "shared layout");
    throws(() => { shared struct Bad { o = {}; } new Bad(); }, TypeError, "an initializer value that cannot be shared");
}

// The layout of a struct is internal state: tampering with Array.prototype and Object.prototype does not reach it.
{
    const savedPush = Array.prototype.push;
    const trace = [];
    Array.prototype.push = function () { trace.push("push"); return 0; };
    for (let i = 0; i < 4; i++) {
        Object.defineProperty(Array.prototype, i, { configurable: true, set(v) { trace.push("set" + i); }, get() { return "evil"; } });
        Object.defineProperty(Object.prototype, i, { configurable: true, set(v) { trace.push("oset" + i); }, get() { return "evil"; } });
    }
    let made;
    try {
        struct Base { a; b = 1; [Symbol.for("k")]; }
        struct Derived extends Base { c; a; }
        shared struct Sh { x; y = 2; }
        made = [new Derived(), new Sh(), new Base()];
    } finally {
        Array.prototype.push = savedPush;
        for (let i = 0; i < 4; i++) {
            delete Array.prototype[i];
            delete Object.prototype[i];
        }
    }
    assert(Object.keys(made[0]).join() === "a,b,c" && made[0].b === 1, "a derived struct built while Array.prototype was tampered with");
    assert(Object.keys(made[1]).join() === "x,y" && made[1].y === 2, "a shared struct built while Array.prototype was tampered with");
    assert(Reflect.ownKeys(made[2]).length === 3, "the symbol key");
    assert(trace.length === 0, "nothing observed the layout: " + trace.join());
}

// Private fields and methods of the chain exist before any initializer runs, so a parent's initializer can reach
// those of a child through a method.
{
    struct Early { a = this.#p; #p = 5; }
    assert(new Early().a === undefined, "a private field that is declared later is undefined, not a TypeError");
    struct P0 { seen = this.probe(); probe() { return "p0"; } }
    struct P1 extends P0 { #y = 2; probe() { return this.#y; } }
    assert(new P1().seen === undefined, "the private field of a child exists when the initializer of the parent runs");
    struct P2 extends P0 { #m() { return 7; } probe() { return this.#m(); } }
    assert(new P2().seen === 7, "the private method of a child is installed when the initializer of the parent runs");
    struct P3 extends P0 { #y = 3; get y() { return this.#y; } }
    assert(new P3().y === 3, "the initializer of a private field runs");
}

// super() works in an arrow function of the constructor method.
{
    struct A { a; constructor(x) { this.a = x; } }
    struct B extends A { constructor(x) { const f = () => super(x + 1); f(); } }
    assert(new B(1).a === 2, "super() in an arrow function");
    struct C extends A { constructor(x) { const f = () => () => { super(x * 2); return this; }; assert(f()() === this, "this in an arrow function"); } }
    assert(new C(4).a === 8, "super() in a nested arrow function");
    struct D extends A { constructor(x) { [1, 2].forEach(n => super(x + n)); } }
    assert(new D(10).a === 12, "super() in a callback, called twice");
}

// Function.prototype.toString answers the source text of the declaration.
{
    struct Src { x; m() { return 1; } }
    shared struct SharedSrc { y; static s() { } }
    assert(String(Src) === "struct Src { x; m() { return 1; } }", "struct source: " + String(Src));
    assert(String(SharedSrc) === "shared struct SharedSrc { y; static s() { } }", "shared struct source: " + String(SharedSrc));
    assert(Function.prototype.toString.call(Src) === String(Src), "Function.prototype.toString");
}

// A private method is branded like the other methods.
{
    struct PMB { #m() { return 1; } static getM(o) { return o.#m; } call() { return this.#m(); } }
    struct PMD extends PMB { }
    const m = PMB.getM(new PMB());
    assert(m.call(new PMB()) === 1 && m.call(new PMD()) === 1, "a private method on instances");
    throws(() => m.call({}), TypeError, "a private method on a plain object");
    throws(() => m.call(PMB.prototype), TypeError, "a private method on the prototype");
    assert(m.name === "#m" && !Object.prototype.hasOwnProperty.call(m, "length"), "its name and length");
}

// The prototype of a struct has no constructor property.
{
    struct NoCtor { x; }
    assert(!Object.prototype.hasOwnProperty.call(NoCtor.prototype, "constructor"), "MakeConstructor(F, false, proto) does not add one");
    assert(Object.getOwnPropertyNames(NoCtor.prototype).length === 0, "an empty prototype");
}

// The chain is read at construction time, super() calls the constructor method of the initializer's prototype, and the
// arguments of super() are evaluated before its constructor is checked.
{
    struct PA { a; constructor() { this.a = "pa"; } }
    struct PB { b; constructor() { this.b = "pb"; } }
    struct C extends PA { c; }
    assert(Object.keys(new C()).join() === "a,c", "the parent at definition");
    Object.setPrototypeOf(C, PB);
    assert(Object.keys(new C()).join() === "b,c", "the parent that the constructor has now supplies the fields");
    struct D { d; }
    struct E extends null { e; }
    Object.setPrototypeOf(E, D);
    assert(Object.keys(new E()).join() === "d,e", "a struct that extends null and was given a parent");
    struct F { f; }
    Object.setPrototypeOf(F, PB);
    assert(Object.keys(new F()).join() === "f", "a struct without a heritage does not walk to a parent that it was given");

    struct SA { x; constructor() { this.x = "SA"; } }
    struct SB { x; constructor() { this.x = "SB"; } }
    struct SC extends SA { constructor() { super(); } }
    Object.setPrototypeOf(SC, SB);
    assert(new SC().x === "SA", "super() calls the constructor of the parent of the initializer, not of the constructor now");

    let evaluated = 0;
    struct N extends null { constructor() { super(evaluated++); } }
    throws(() => new N(), TypeError, "no super constructor");
    assert(evaluated === 1, "the arguments were evaluated first: " + evaluated);
}
