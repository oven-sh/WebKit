//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function repeat(f) {
    for (let i = 0; i < 100; i++)
        f(i);
}
function throwsTypeError(f, what) {
    try {
        f();
    } catch (error) {
        check(error instanceof TypeError, true, what);
        return;
    }
    throw new Error(what + ": did not throw");
}

class Base { kind() { return "base"; } }
class Derived extends Base { kind() { return "derived of " + super.kind(); } }
class DerivedTwice extends Derived { }
class WithFields { a = 1; #b = 2; sum() { return this.a + this.#b; } }
class DerivedWithFields extends WithFields { c = 3; total() { return this.sum() + this.c; } }
class WithPrivateMethod { #method() { return "private"; } call() { return this.#method(); } }
class Takes { constructor(x, y = 5, ...rest) { this.x = x; this.y = y; this.rest = rest.join(); this.count = arguments.length; } }
class Forwards extends Takes { }
class ForwardsTwice extends Forwards { }
class OfError extends Error { }
class OfArray extends Array { }
class OfMap extends Map { }
class OfPromise extends Promise { }
class OfFunction extends Function { }
class OfNull extends null { }
class ObservesNewTarget { target = new.target; }
class Throws { constructor() { throw new RangeError("thrown"); } }
class ForwardsToThrows extends Throws { }
class ReturnsOther { constructor() { return { other: true }; } }
class ForwardsToReturnsOther extends ReturnsOther { field = "set on what was returned"; }
function Plain(x) { this.x = x; }
class OfPlain extends Plain { }

repeat(i => {
    check(new Base().kind(), "base", "a base class");
    check(new Derived().kind(), "derived of base", "a derived class");
    check(new DerivedTwice().kind(), "derived of base", "a class derived twice");
    check(Object.getPrototypeOf(new DerivedTwice), DerivedTwice.prototype, "the prototype");
    check(new WithFields().sum(), 3, "fields");
    check(new DerivedWithFields().total(), 6, "fields of a derived class");
    check(new WithPrivateMethod().call(), "private", "a private method");
    check(new Forwards(i).x, i, "an argument");
    check(new Forwards(i).y, 5, "a default parameter");
    check(new Forwards(i, 2).y, 2, "two arguments");
    check(new Forwards().count, 0, "no argument");
    check(new ForwardsTwice(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12).rest, "3,4,5,6,7,8,9,10,11,12", "many arguments");
    check(new ForwardsTwice(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12).count, 12, "the number of many arguments");
    check(new Forwards(...[1, 2, 3]).rest, "3", "arguments that are spread");
    check(new OfError("message").message, "message", "a class that extends Error");
    check(new OfError("message") instanceof OfError, true, "an instance of a class that extends Error");
    check(new OfArray(3).length, 3, "a class that extends Array");
    check(OfArray.of(1, 2) instanceof OfArray, true, "Array.of on a class that extends Array");
    check(new OfArray(1, 2, 3).map(x => x) instanceof OfArray, true, "the species of a class that extends Array");
    check(new OfMap([["k", i]]).get("k"), i, "a class that extends Map");
    check(new OfPromise(() => { }) instanceof OfPromise, true, "a class that extends Promise");
    check(new OfFunction("return 5")(), 5, "a class that extends Function");
    check(new ObservesNewTarget().target, undefined, "new.target in a field");
    check(new OfPlain(i).x, i, "a class that extends a function");
    check(new ForwardsToReturnsOther().other, true, "a parent that returns another object");
    check(new ForwardsToReturnsOther().field, "set on what was returned", "a field on what the parent returned");
    check(Reflect.construct(Derived, [], DerivedTwice) instanceof DerivedTwice, true, "another new.target");
    check(Reflect.construct(Forwards, [i], Base) instanceof Base, true, "an unrelated new.target");
    check(Reflect.construct(Forwards, [i], Base).x, i, "an argument with an unrelated new.target");
});

for (const C of [Base, Derived, Forwards, WithFields, OfError])
    repeat(() => throwsTypeError(() => C(), "calling " + C.name));
repeat(() => throwsTypeError(() => Base.call({ }), "calling with a receiver"));
repeat(() => throwsTypeError(() => new OfNull, "a class that extends null"));

repeat(() => {
    try {
        new ForwardsToThrows;
    } catch (error) {
        check(error instanceof RangeError, true, "what the parent throws");
        check(error.message, "thrown", "the message of what the parent throws");
        check(error.stack.includes("Throws"), true, "the parent in the stack trace");
        return;
    }
    throw new Error("the parent did not throw");
});

check(Base.name, "Base", "the name");
check(Base.length, 0, "the length of a base class");
check(Derived.length, 0, "the length of a derived class");
check(Base.prototype.constructor, Base, "prototype.constructor");
check(typeof Base.toString(), "string", "toString");
check(Object.getPrototypeOf(Derived), Base, "the prototype of a derived class");
check(Object.getOwnPropertyNames(Base).sort().join(), "length,name,prototype", "the properties");

{
    const anonymous = [class { }, class extends Base { }];
    repeat(() => check(new anonymous[1]().kind(), "base", "a class without a name"));
    check(anonymous[0].name, "", "the name of a class without one");
    const classes = [];
    for (let i = 0; i < 20; i++)
        classes.push(class extends Takes { which() { return i; } });
    repeat(i => check(new classes[i % 20](i).which(), i % 20, "classes made by one expression"));
    repeat(i => check(new classes[i % 20](i).x, i, "an argument of classes made by one expression"));
    check(classes[0] === classes[1], false, "classes made by one expression differ");
}

{
    class Reparented extends Base { }
    repeat(() => check(new Reparented().kind(), "base", "before the parent changes"));
    Object.setPrototypeOf(Reparented, Takes);
    repeat(i => check(new Reparented(i).x, i, "after the parent changes"));
    Object.setPrototypeOf(Reparented, Object.getPrototypeOf(Base));
    repeat(() => throwsTypeError(() => new Reparented, "after the parent becomes something that cannot construct"));
}

for (let round = 0; round < 4; round++) {
    (function () {
        class ShortLived extends Takes { }
        repeat(i => check(new ShortLived(i).x, i, "a class that is about to die"));
    })();
    fullGC();
}
