//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTDataStubs=0")
(function () {
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function checkThrows(f, type, what) {
    try {
        f();
    } catch (error) {
        if (!(error instanceof type))
            throw new Error(what + ": threw " + error);
        return;
    }
    throw new Error(what + ": did not throw");
}

const base = {
    value: 2,
    get viaGetter() { return this.own * 10; },
};
const object = {
    __proto__: base,
    own: 7,
    value: "shadows",
    readsValue() { return super.value; },
    readsGetter() { return super.viaGetter; },
    readsMissing() { return super.missing; },
};
for (let i = 0; i < 20; i++) {
    check(object.readsValue(), 2, "a value of the prototype");
    check(object.readsGetter(), 70, "a getter of the prototype, which sees the receiver");
    check(object.readsMissing(), undefined, "what the prototype does not have");
}

base.value = 3;
check(object.readsValue(), 3, "after the value changes");
base.missing = "found";
check(object.readsMissing(), "found", "after the property is added");
Object.defineProperty(base, "value", { get() { return this.own + 1; }, configurable: true });
check(object.readsValue(), 8, "after the value becomes a getter");
Object.defineProperty(base, "viaGetter", { value: "plain", configurable: true });
check(object.readsGetter(), "plain", "after the getter becomes a value");

const other = { value: "other", viaGetter: "other too" };
Object.setPrototypeOf(object, other);
for (let i = 0; i < 20; i++) {
    check(object.readsValue(), "other", "after the prototype changes");
    check(object.readsGetter(), "other too", "after the prototype changes, what was a getter");
    check(object.readsMissing(), undefined, "after the prototype changes, what it does not have");
}

Object.setPrototypeOf(object, null);
for (let i = 0; i < 3; i++) {
    checkThrows(() => object.readsValue(), TypeError, "without a prototype");
    checkThrows(() => object.readsGetter(), TypeError, "without a prototype, what was a getter");
    checkThrows(() => object.readsMissing(), TypeError, "without a prototype, what nothing has");
}
Object.setPrototypeOf(object, base);
check(object.readsValue(), 8, "with the prototype again");

class Parent {
    get doubled() { return this.n * 2; }
    static label = "parent";
}
Parent.prototype.shared = "on the prototype";
class Child extends Parent {
    constructor(n) { super(); this.n = n; }
    get doubled() { return -1; }
    readsDoubled() { return super.doubled; }
    readsShared() { return super.shared; }
    static readsLabel() { return super.label; }
}
let sum = 0;
for (let i = 0; i < 20; i++)
    sum += new Child(i).readsDoubled();
check(sum, 380, "a getter of the parent class");
check(new Child(1).readsShared(), "on the prototype", "a value on the prototype of the parent class");
check(Child.readsLabel(), "parent", "a static field of the parent class");
Object.setPrototypeOf(Child.prototype, null);
checkThrows(() => new Child(1).readsShared(), TypeError, "a class whose prototype has none");
})();
