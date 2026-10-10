//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--definePlainInstanceFieldsInConstructor=1")

function remarksOf(name) {
    return aotRemarks(name);
}
function applies(name, pattern) {
    let remarks = remarksOf(name);
    if (remarks && !remarks.includes(pattern))
        throw new Error(pattern + " does not apply to " + name + ": " + remarks.join(" "));
}
function doesNotApply(name, pattern) {
    let remarks = remarksOf(name);
    if (remarks && remarks.includes(pattern))
        throw new Error(pattern + " applies to " + name + ": " + remarks.join(" "));
}
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

let log = [];
let seen;

class ChecksBetweenStores {
    left;
    right;
    constructor(l, r) {
        $$t(l, 514);
        $$t(r, 514);
        this.left = $$t(l, 514);
        this.right = $$t(r, 514);
    }
}
class ChecksAheadOfStores {
    constructor(l, r) {
        $$t(l, 514);
        $$t(r, 514);
        this.left = l;
        this.right = r;
    }
}
class AssignsNewPropertyAheadOfCheck {
    constructor(a, b) {
        this.first = a;
        $$t(b, 8);
        this.second = b;
    }
}
class ChecksThisBetweenStores {
    first;
    second;
    constructor(a) {
        $$t(this, 512);
        this.first = a;
        this.second = a;
    }
}
class CatchesCheck {
    first;
    second;
    constructor(a) {
        this.first = 1;
        try {
            $$t(a, 8);
            this.second = a;
        } catch (error) {
            seen = this.first;
        }
    }
}

function outcomeOf(f) {
    try {
        f();
        return "returned";
    } catch (error) {
        return error.constructor.name;
    }
}

for (let i = 0; i < 100; ++i) {
    let leaf = new ChecksBetweenStores(null, null);
    check(leaf.left, null, "left of a leaf");
    check(leaf.right, null, "right of a leaf");
    let inner = new ChecksBetweenStores(leaf, null);
    check(inner.left, leaf, "left");
    check(inner.right, null, "right");
    check(Object.keys(inner).join(), "left,right", "keys");
    check(outcomeOf(() => new ChecksBetweenStores(1, null)), "TypeError", "a number as left");
    check(outcomeOf(() => new ChecksBetweenStores(null, "x")), "TypeError", "a string as right");
    check(outcomeOf(() => new ChecksBetweenStores(undefined, null)), "TypeError", "undefined as left");

    let ahead = new ChecksAheadOfStores(leaf, null);
    check(ahead.left, leaf, "left, checked ahead");
    check(Object.keys(ahead).join(), "left,right", "keys, checked ahead");

    let assigned = new AssignsNewPropertyAheadOfCheck(leaf, 2);
    check(assigned.first, leaf, "first");
    check(assigned.second, 2, "second");
    check(outcomeOf(() => new AssignsNewPropertyAheadOfCheck(leaf, "x")), "TypeError", "a string as second");

    let checked = new ChecksThisBetweenStores(3);
    check(checked.first + checked.second, 6, "fields after a check of this");

    seen = 0;
    let caught = new CatchesCheck("x");
    check(seen, 1, "the field that the handler reads");
    check(caught.second, undefined, "the field after the failed check");
    check(new CatchesCheck(5).second, 5, "the field after the check");
}

let newTarget = new Proxy(function () { }, {
    get(target, name, receiver) {
        if (name === "prototype")
            log.push("prototype");
        return Reflect.get(target, name, receiver);
    }
});
log = [];
log.push(outcomeOf(() => Reflect.construct(ChecksBetweenStores, [1, null], newTarget)));
check(log.join(), "prototype,TypeError", "the prototype is read ahead of the failing check");

Object.defineProperty(ChecksBetweenStores.prototype, "left", { set(value) { log.push("setter of left"); }, configurable: true });
log = [];
check(new ChecksBetweenStores(null, null).left, null, "left is the instance's own");
log.push(outcomeOf(() => new ChecksBetweenStores(null, 1)));
check(log.join(), "TypeError", "an assignment to a field that is defined runs no setter");

Object.defineProperty(AssignsNewPropertyAheadOfCheck.prototype, "first", { set(value) { log.push("setter of first"); }, configurable: true });
log = [];
log.push(outcomeOf(() => new AssignsNewPropertyAheadOfCheck(1, "x")));
check(log.join(), "setter of first,TypeError", "the setter runs ahead of the failing check");

function storesOutsideOfPlan(name) {
    let remarks = remarksOf(name);
    return !remarks || remarks.includes("calls:PutById") || remarks.includes("calls:operationAOTPutById");
}
for (let name of ["ChecksBetweenStores", "ChecksAheadOfStores"]) {
    applies(name, "planned-construction");
    applies(name, "checks-type-ahead-of-create-this");
    doesNotApply(name, "calls:PutById");
    doesNotApply(name, "calls:operationAOTPutById");
}
for (let name of ["AssignsNewPropertyAheadOfCheck", "ChecksThisBetweenStores", "CatchesCheck"])
    check(storesOutsideOfPlan(name), true, name + " stores outside of the plan");
