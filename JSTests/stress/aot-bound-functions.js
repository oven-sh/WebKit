//@ runDefault("--compileMainScriptAheadOfTime=1")

function shouldBe(actual, expected, what)
{
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

function shouldThrow(run, constructor, what)
{
    let error = null;
    try {
        run();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof constructor))
        throw new Error(what + ": " + error + " instead of a " + constructor.name);
}

(function () {
    function describes(a, b, c, d, e)
    {
        return String(this) + ":" + [a, b, c, d, e].join() + ":" + arguments.length;
    }

    function bindsNothing(f) { return f.bind(); }
    function bindsThis(f, receiver) { return f.bind(receiver); }
    function bindsOne(f, receiver, a) { return f.bind(receiver, a); }
    function bindsTwo(f, receiver, a, b) { return f.bind(receiver, a, b); }
    function bindsThree(f, receiver, a, b, c) { return f.bind(receiver, a, b, c); }
    function bindsFour(f, receiver, a, b, c, d) { return f.bind(receiver, a, b, c, d); }

    function callsWithNone(f) { return f(); }
    function callsWithOne(f, a) { return f(a); }
    function callsWithTwo(f, a, b) { return f(a, b); }
    function callsWithFour(f, a, b, c, d) { return f(a, b, c, d); }
    function callsAsMethod(o, a) { return o.method(a); }

    for (let i = 0; i < 300; ++i) {
        shouldBe(callsWithNone(bindsThis(describes, "r")), "r:,,,,:0", "this alone, no arguments");
        shouldBe(callsWithOne(bindsThis(describes, "r"), i), "r:" + i + ",,,,:1", "this alone, one argument");
        shouldBe(callsWithTwo(bindsThis(describes, "r"), i, 2), "r:" + i + ",2,,,:2", "this alone, two arguments");
        shouldBe(callsWithFour(bindsThis(describes, "r"), 1, 2, 3, i), "r:1,2,3," + i + ",:4", "this alone, four arguments");
        shouldBe(callsWithNone(bindsOne(describes, "r", i)), "r:" + i + ",,,,:1", "one bound argument");
        shouldBe(callsWithTwo(bindsOne(describes, "r", "x"), i, 2), "r:x," + i + ",2,,:3", "one bound argument and two");
        shouldBe(callsWithOne(bindsTwo(describes, "r", "x", "y"), i), "r:x,y," + i + ",,:3", "two bound arguments and one");
        shouldBe(callsWithTwo(bindsThree(describes, "r", "x", "y", "z"), i, 5), "r:x,y,z," + i + ",5:5", "three bound arguments and two");
        shouldBe(callsWithFour(bindsThree(describes, "r", "x", "y", "z"), 4, 5, 6, i), "r:x,y,z,4,5:7", "three bound arguments and four");
        shouldBe(callsWithOne(bindsFour(describes, "r", "w", "x", "y", "z"), i), "r:w,x,y,z," + i + ":5", "four bound arguments and one");
        shouldBe(callsWithFour(bindsFour(describes, "r", "w", "x", "y", "z"), i, 6, 7, 8), "r:w,x,y,z," + i + ":8", "four bound arguments and four");
        shouldBe(callsAsMethod({ method: bindsThis(describes, "r") }, i), "r:" + i + ",,,,:1", "the receiver of the call is ignored");
        shouldBe(callsWithOne(bindsOne(bindsOne(describes, "inner", "a"), "outer", "b"), i), "inner:a,b," + i + ",,:3", "a bound function of a bound function");
        shouldBe(callsWithNone(bindsThis(bindsThis(bindsThis(describes, "first"), "second"), "third")), "first:,,,,:0", "three times bound");
    }

    function strict() { "use strict"; return this; }
    function sloppy() { return this; }
    const arrow = () => typeof this;
    for (let i = 0; i < 100; ++i) {
        shouldBe(callsWithNone(bindsNothing(strict)), undefined, "bind without arguments, strict");
        shouldBe(callsWithNone(bindsNothing(sloppy)), globalThis, "bind without arguments, sloppy");
        shouldBe(callsWithNone(bindsThis(strict, null)), null, "null as this, strict");
        shouldBe(callsWithNone(bindsThis(sloppy, null)), globalThis, "null as this, sloppy");
        shouldBe(callsWithNone(bindsThis(strict, i)), i, "a number as this, strict");
        shouldBe(typeof callsWithNone(bindsThis(sloppy, i)), "object", "a number as this, sloppy");
        shouldBe(callsWithNone(bindsThis(arrow, {})), callsWithNone(arrow), "an arrow function keeps its this");
    }
})();

(function () {
    function three(a, b, c) { return a + b + c; }
    function binds(f, ...rest) { return f.bind(null, ...rest); }
    function bindsThis(f) { return f.bind(null); }
    function bindsTwo(f) { return f.bind(null, 1, 2); }

    for (let i = 0; i < 100; ++i) {
        const bound = bindsThis(three);
        shouldBe(bound.name, "bound three", "name");
        shouldBe(bound.length, 3, "length");
        shouldBe(bindsTwo(three).length, 1, "length with two bound arguments");
        shouldBe(binds(three, 1, 2, 3, 4).length, 0, "length does not go below zero");
        shouldBe(bindsThis(bound).name, "bound bound three", "name of a bound function of a bound function");
        shouldBe(bindsTwo(bindsTwo(three)).length, 0, "length of a bound function of a bound function");
        shouldBe("prototype" in bound, false, "no prototype property");
        shouldBe(Object.getPrototypeOf(bound), Function.prototype, "prototype");
        shouldBe(Object.getOwnPropertyNames(bound).sort().join(), "length,name", "own properties");
        shouldBe(typeof bound, "function", "typeof");
        shouldBe(String(bound), "function three() { [native code] }", "text");
    }

    function renamed() { }
    Object.defineProperty(renamed, "name", { value: "other" });
    function resized(a, b) { }
    Object.defineProperty(resized, "length", { value: 7 });
    function unnamed() { }
    Object.defineProperty(unnamed, "name", { value: 5 });
    function withoutLength(a) { }
    delete withoutLength.length;
    function withProperty() { }
    withProperty.extra = 1;
    let reads = 0;
    function observed() { }
    Object.defineProperty(observed, "name", { get() { reads++; return "seen"; } });
    for (let i = 0; i < 100; ++i) {
        shouldBe(bindsThis(renamed).name, "bound other", "a name that was redefined");
        shouldBe(bindsTwo(resized).length, 5, "a length that was redefined");
        shouldBe(bindsThis(unnamed).name, "bound ", "a name that is not a string");
        shouldBe(bindsThis(withoutLength).length, 0, "a length that was deleted");
        shouldBe(bindsThis(withProperty).name, "bound withProperty", "a function with a property of its own");
        shouldBe(bindsThis(withProperty).extra, undefined, "properties are not copied");
        shouldBe(bindsThis(observed).name, "bound seen", "a name behind a getter");
        shouldBe(reads, i + 1, "the getter runs once, when the function is bound");
    }

    function late() { }
    const boundEarly = bindsThis(late);
    Object.defineProperty(late, "name", { value: "changed" });
    shouldBe(boundEarly.name, "bound late", "the name is the one at the time of binding");
    shouldBe(bindsThis(late).name, "bound changed", "a later bind sees the new name");

    const parent = Object.create(Function.prototype);
    parent.marker = "inherited";
    function reparented() { return 1; }
    Object.setPrototypeOf(reparented, parent);
    function orphan() { return 2; }
    Object.setPrototypeOf(orphan, null);
    const bind = Function.prototype.bind;
    function bindsIndirectly(f) { return bind.call(f, null); }
    for (let i = 0; i < 100; ++i) {
        shouldBe(Object.getPrototypeOf(bindsThis(reparented)), parent, "the prototype of the target is kept");
        shouldBe(bindsThis(reparented).marker, "inherited", "and is used");
        shouldBe(bindsThis(reparented)(), 1, "and the function works");
        shouldBe(Object.getPrototypeOf(bindsIndirectly(orphan)), null, "a target without a prototype");
        shouldBe(bindsIndirectly(orphan)(), 2, "and the function works");
    }
})();

(function () {
    function bindsThis(f, receiver) { return f.bind(receiver); }
    function bindsOne(f, receiver, a) { return f.bind(receiver, a); }
    function constructs(f, a) { return new f(a); }
    function calls(f, a) { return f(a); }

    class Point {
        constructor(x, y)
        {
            this.x = x;
            this.y = y;
            this.target = new.target;
        }
    }
    class Derived extends Point { }
    function Old(a) { this.a = a; this.target = new.target; }

    for (let i = 0; i < 200; ++i) {
        const BoundPoint = bindsOne(Point, "ignored", 1);
        const point = constructs(BoundPoint, i);
        shouldBe(point.x + ":" + point.y, "1:" + i, "a class constructed through a bound function");
        shouldBe(point.target, Point, "new.target is the target");
        shouldBe(point instanceof Point, true, "instanceof the class");
        shouldBe(point instanceof BoundPoint, true, "instanceof the bound function");
        shouldBe({} instanceof BoundPoint, false, "something else is no instance");
        shouldThrow(() => calls(BoundPoint, i), TypeError, "a bound class cannot be called");
        shouldBe(constructs(bindsThis(Old, "ignored"), i).a, i, "a function constructed through a bound function");
        shouldBe(constructs(bindsThis(Old, "ignored"), i).target, Old, "new.target of a function");
        shouldBe(constructs(bindsThis(bindsThis(Derived, 1), 2), i).target, Derived, "twice bound, constructed");
        shouldBe(Reflect.construct(BoundPoint, [i], Derived).target, Derived, "another new.target is kept");
        shouldBe(Object.getPrototypeOf(Reflect.construct(BoundPoint, [i], Derived)), Derived.prototype, "and decides the prototype");
        shouldThrow(() => constructs(bindsThis(() => 1, null), i), TypeError, "a bound arrow function is no constructor");
        shouldThrow(() => constructs(bindsThis({ method() { } }.method, null), i), TypeError, "a bound method is no constructor");
    }
})();

(function () {
    function bindsThis(f, receiver) { return f.bind(receiver); }
    function bindsOne(f, receiver, a) { return f.bind(receiver, a); }
    const bind = Function.prototype.bind;
    function bindsAnything(value) { return bind.call(value, null); }

    for (let i = 0; i < 100; ++i) {
        shouldBe(bindsOne(Math.max, null, 5)(i % 10), Math.max(5, i % 10), "a host function");
        shouldBe(bindsThis(Array.prototype.join, [1, 2, i])("-"), "1-2-" + i, "a host method");
        shouldBe(bindsThis(Array.prototype.map, [1, 2])(x => x + i).join(), (1 + i) + "," + (2 + i), "a built-in written in JavaScript");
        shouldBe(bindsOne(String, null, i)(), String(i), "a constructor of the engine, called");
        shouldBe(new (bindsOne(Array, null, 3))().length, 3, "a constructor of the engine, constructed");
        shouldBe(bindsThis(new Proxy(function (a) { return a + 1; }, {}), null)(i), i + 1, "a callable proxy");
        shouldBe(bindsThis(bind, function (a) { return this + a; })("t")(i), "t" + i, "bind, bound");
        shouldBe(bindsThis(Function.prototype.call, function () { return this; })(i) == i, true, "call, bound");
        for (const value of [undefined, null, 1, "text", {}, [], Symbol.iterator, new Proxy({}, {})])
            shouldThrow(() => bindsAnything(value), TypeError, "what cannot be called cannot be bound");
    }

    function* generates(a) { yield this + a; }
    async function waits(a) { return this + a; }
    let resolved = 0;
    for (let i = 0; i < 100; ++i) {
        shouldBe(bindsOne(generates, "g", i)().next().value, "g" + i, "a generator function");
        bindsOne(waits, "w", i)().then(value => { shouldBe(value, "w" + i, "an async function"); resolved++; });
    }
    drainMicrotasks();
    shouldBe(resolved, 100, "all async functions ran");

    const shadowed = function () { return "target"; };
    shadowed.bind = function () { return () => "own bind"; };
    const inherits = function () { return "target"; };
    Object.setPrototypeOf(inherits, { bind() { return () => "inherited bind"; } });
    const notAFunction = { bind(receiver) { return receiver; } };
    for (let i = 0; i < 100; ++i) {
        shouldBe(bindsThis(shadowed, null)(), "own bind", "a bind of its own");
        shouldBe(bindsThis(inherits, null)(), "inherited bind", "another bind on the prototype");
        shouldBe(bindsThis(notAFunction, i), i, "an object with a method of that name");
    }
})();

(function () {
    function bindsThis(f, receiver) { return f.bind(receiver); }
    function bindsThree(f, receiver, a, b, c) { return f.bind(receiver, a, b, c); }
    function bindsFive(f, receiver, a, b, c, d, e) { return f.bind(receiver, a, b, c, d, e); }
    function sums(a, b, c, d, e) { return this.base + a.value + b.value + c.value + (d ? d.value : 0) + (e ? e.value : 0); }

    const kept = [];
    for (let i = 0; i < 2000; ++i) {
        kept.push(bindsThree(sums, { base: i }, { value: 1 }, { value: 2 }, { value: 3 }));
        kept.push(bindsFive(sums, { base: i }, { value: 1 }, { value: 2 }, { value: 3 }, { value: 4 }, { value: 5 }));
        if (i % 500 === 499)
            gc();
    }
    gc();
    for (let i = 0; i < 2000; ++i) {
        shouldBe(kept[2 * i](), i + 6, "what three bound arguments hold survives collections");
        shouldBe(kept[2 * i + 1](), i + 15, "what five bound arguments hold survives collections");
    }

    const holder = { value: 1 };
    const object = {};
    Object.defineProperty(object, "fromBoundGetter", { get: bindsThis(function () { return this.value; }, holder) });
    function readsIt(o) { return o.fromBoundGetter; }
    for (let i = 0; i < 300; ++i) {
        holder.value = i;
        shouldBe(readsIt(object), i, "a bound function as a getter");
    }
    holder.other = 1;
    delete holder.value;
    shouldBe(readsIt(object), undefined, "after the bound this changed its shape");
    holder.value = "again";
    shouldBe(readsIt(object), "again", "and again");
})();

(function () {
    function bindsThis(f, receiver) { return f.bind(receiver); }
    function throwsHere() { throw new Error("thrown"); }
    function callsBound(f) { return f(); }
    function outer() { return callsBound(bindsThis(bindsThis(throwsHere, 1), 2)); }
    for (let i = 0; i < 50; ++i) {
        let stack = "";
        try {
            outer();
        } catch (e) {
            stack = e.stack;
        }
        const names = stack.split("\n").map(line => line.split("@")[0]).slice(0, 3).join();
        shouldBe(names, "throwsHere,callsBound,outer", "bound functions do not show in stack traces");
    }

    let depth = 0;
    let recurses = null;
    function goesOn() { depth++; return recurses() + 1; }
    recurses = bindsThis(goesOn, null);
    for (let i = 0; i < 3; ++i) {
        depth = 0;
        shouldThrow(() => recurses(), RangeError, "endless recursion through a bound function");
        shouldBe(depth > 100, true, "after some depth");
    }

    let chain = function (a) { return a; };
    for (let i = 0; i < 2000; ++i)
        chain = bindsThis(chain, i);
    shouldBe(chain("deep"), "deep", "a long chain of bound functions");
})();
