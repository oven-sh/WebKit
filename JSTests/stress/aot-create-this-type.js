//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(f.name);
    if (!remarks && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
const isFinalObject = "create-this-is-final-object";

function Plain(x) { this.x = x; this.isTruthy = this ? "yes" : "no"; this.kind = typeof this; this.isNullish = this == null; }
function ReturnsArray() { this.a = 1; return [this.a]; }
function ReturnsNumber() { this.a = 1; return 5; }
class Base { constructor(x) { this.x = x; this.kind = typeof this; } }
class Derived extends Base { constructor(x) { super(x); this.y = x + 1; } }
class ExtendsArray extends Array { constructor() { super(); this.tag = typeof this; } }
function notConstructed(x) { return typeof this + x; }
const arrow = x => typeof x + "!";
const holder = { method(x) { return typeof this + x; } };
for (let f of [Plain, ReturnsArray, ReturnsNumber, Base, Derived, ExtendsArray, notConstructed, arrow, holder.method])
    noInline(f);

function describe(object) { return [Object.prototype.toString.call(object), Array.isArray(object), object.x, object.isTruthy, object.kind, object.isNullish].join(); }
check(describe(new Plain(1)), "[object Object],false,1,yes,object,false", "a constructed object");
check(Object.getPrototypeOf(new Plain(1)), Plain.prototype, "its prototype");
check(describe(Reflect.construct(Plain, [2], Array)), "[object Object],false,2,yes,object,false", "an object constructed for Array");
check(Object.getPrototypeOf(Reflect.construct(Plain, [2], Array)), Array.prototype, "the prototype of an object constructed for Array");
check(describe(Reflect.construct(Plain, [3], Map)), "[object Map],false,3,yes,object,false", "an object constructed for Map");
check(describe(Reflect.construct(Plain, [4], new Proxy(Plain, { }))), "[object Object],false,4,yes,object,false", "an object constructed for a proxy");
check(describe(Reflect.construct(Plain, [5], Plain.bind(null))), "[object Object],false,5,yes,object,false", "an object constructed for a bound function");
function WithoutPrototype() { }
WithoutPrototype.prototype = 5;
check(Object.getPrototypeOf(Reflect.construct(Plain, [6], WithoutPrototype)), Object.prototype, "a prototype that is no object");
check(Array.isArray(new ReturnsArray), true, "a constructor that returns an array");
check(new ReturnsNumber().a, 1, "a constructor that returns a number");
check([new Base(1).x, new Base(1).kind, new Derived(1).x, new Derived(1).y, new Derived(1).kind].join(), "1,object,1,2,object", "classes");
check(Object.getPrototypeOf(new Derived(1)), Derived.prototype, "the prototype of an instance of a derived class");
check([Array.isArray(new ExtendsArray), new ExtendsArray().tag, new ExtendsArray().length].join(), "true,object,0", "a class that extends Array");
check(notConstructed.call(5, "!"), "object!", "a function that is called");
check(typeof new notConstructed("!"), "object", "the same function, constructed");
check(arrow(5) + holder.method("!"), "number!object!", "an arrow function and a method");
for (let f of [Plain, ReturnsArray, ReturnsNumber, Base, notConstructed])
    applies(f, isFinalObject);
for (let f of [Derived, ExtendsArray, arrow, holder.method])
    doesNotApply(f, isFinalObject);
doesNotApply(Plain, "calls:ToBoolean");
