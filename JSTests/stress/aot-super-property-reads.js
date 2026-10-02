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
class Base { constructor(v) { this.v = v; } get() { return this.v; } get twice() { return this.v * 2; } static make() { return "base"; } }
class Derived extends Base { get() { return super.get() + 1; } get twice() { return super.twice + 1; } missing() { return super.nothing; } static make() { return super.make() + "+derived"; } }
class Other { get() { return "other"; } get twice() { return "other twice"; } }
let object = { __proto__: { hello() { return "hello " + this.name; } }, name: "object", hello() { return super.hello() + "!"; } };
function callGet(d) { return d.get(); }
function readTwice(d) { return d.twice; }
function prototypeOf(v) { return Object.getPrototypeOf(v); }
for (let round = 0; round < 3; ++round) {
    check(callGet(new Derived(round)), round + 1, "a method of the superclass");
    check(readTwice(new Derived(round)), round * 2 + 1, "a getter of the superclass gets this");
    check(new Derived(1).missing(), undefined, "nothing there");
    check(Derived.make(), "base+derived", "a static method");
    check(object.hello(), "hello object!", "in an object literal");
}
Base.prototype.get = function () { return 100; };
check(callGet(new Derived(1)), 101, "the method was replaced");
Object.setPrototypeOf(Derived.prototype, Other.prototype);
check(callGet(new Derived(1)), "other1", "the prototype of the home object was replaced");
check(readTwice(new Derived(1)), "other twice1", "and its getter");
check(prototypeOf({}), Object.prototype, "of an object");
check(prototypeOf([]), Array.prototype, "of an array");
check(prototypeOf(Object.create(null)), null, "null");
check(prototypeOf(function () { }), Function.prototype, "of a function");
check(prototypeOf("s"), String.prototype, "of a string");
check(prototypeOf(1), Number.prototype, "of a number");
check(prototypeOf(new Proxy({}, { getPrototypeOf() { return Array.prototype; } })), Array.prototype, "of a proxy");
let threw = false;
try { prototypeOf(null); } catch (e) { threw = e instanceof TypeError; }
check(threw, true, "of null");

class Computed extends Base { get(key) { return super[key](); } }
let viaSuper = Object.getOwnPropertyDescriptor(Derived.prototype, "get").value;
let viaComputedSuper = Object.getOwnPropertyDescriptor(Computed.prototype, "get").value;
function named(f, name) { return { name, __proto__: null, f }; }
check(new Computed(7).get("get"), 100, "a computed name");
let remarks = aotRemarks("get");
if (remarks) {
    check(remarks.includes("cached-read-with-this:get"), true, "super.get is cached");
    check(remarks.includes("inline-get-prototype-of"), true, "the prototype of the home object is read inline");
    check(aotRemarks("missing").includes("cached-read-with-this:nothing"), true, "super.nothing is cached");
    check(aotRemarks("callGet").some(remark => remark.startsWith("cached-read-with-this")), false, "d.get() is not a read with another this");
    check(aotRemarks("prototypeOf").includes("inline-get-prototype-of"), false, "Object.getPrototypeOf() is a call");
}
