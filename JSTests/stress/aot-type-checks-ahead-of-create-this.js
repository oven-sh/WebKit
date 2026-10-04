//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1")
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
const planned = "planned-construction", checksAhead = "checks-type-ahead-of-create-this";

let log = [];
function observed(target, handler = { }) {
    return new Proxy(target, {
        get(t, key, receiver) {
            log.push(String(key));
            return handler.get ? handler.get(t, key, receiver) : Reflect.get(t, key, receiver);
        }
    });
}
function construct(C, args, newTarget) {
    log = [];
    try {
        let made = Reflect.construct(C, args, newTarget);
        log.push(Object.keys(made).map(key => key + "=" + made[key]).join("&"));
    } catch (error) {
        log.push(typeof error === "object" ? error.constructor.name : error);
    }
    return log.join();
}

function OneField(a) { this.x = $$t(a, 8); }
function TwoFields(a, b) {
    this.x = $$t(a, 8);
    this.y = $$t(b, 16);
}
function ChecksFirst(a, b) {
    $$t(a, 8);
    $$t(b, 16);
    this.x = a;
    this.y = b;
}
function AcceptsCallables(f) { this.f = $$t(f, 128); }
function Unchecked(a) { this.x = a; }
function AlwaysPasses() { this.x = $$t(1, 8); }

check(construct(OneField, [1], OneField), "x=1", "a plain new.target, the check passes");
check(construct(OneField, ["no"], OneField), "TypeError", "a plain new.target, the check fails");
check(construct(OneField, [1], observed(OneField)), "prototype,x=1", "the prototype is read once when the check passes");
check(construct(OneField, ["no"], observed(OneField)), "prototype,TypeError", "the prototype is read before the check fails");

check(construct(TwoFields, [1, "s"], observed(TwoFields)), "prototype,x=1&y=s", "both checks pass");
check(construct(TwoFields, ["no", "s"], observed(TwoFields)), "prototype,TypeError", "the first check fails");
check(construct(TwoFields, [1, 2], observed(TwoFields)), "prototype,TypeError", "the second check fails");
check(construct(ChecksFirst, [1, "s"], observed(ChecksFirst)), "prototype,x=1&y=s", "both checks pass ahead of the stores");
check(construct(ChecksFirst, [1, 2], observed(ChecksFirst)), "prototype,TypeError", "a check ahead of the stores fails");

check(construct(OneField, ["no"], observed(OneField, { get() { throw "from the trap"; } })), "prototype,from the trap", "reading the prototype throws first");
check(construct(OneField, [1], observed(OneField, { get() { throw "from the trap"; } })), "prototype,from the trap", "reading the prototype throws and the check would pass");
check(construct(OneField, ["no"], observed(OneField, { get() { return 5; } })), "prototype,TypeError", "the prototype is not an object");

{
    let { proxy, revoke } = Proxy.revocable(OneField, { get(t, key) { log.push(String(key)); revoke(); return undefined; } });
    check(construct(OneField, ["no"], proxy), "prototype,TypeError", "the realm of a revoked proxy is asked for");
}

{
    let bound = OneField.bind(null);
    Object.defineProperty(bound, "prototype", { get() { log.push("prototype of the bound function"); return OneField.prototype; } });
    check(construct(OneField, ["no"], bound), "prototype of the bound function,TypeError", "a bound function with an accessor");
    check(construct(OneField, [2], bound), "prototype of the bound function,x=2", "a bound function with an accessor, the check passes");
}

check(construct(AcceptsCallables, [new Proxy(function () { }, { })], observed(AcceptsCallables)).startsWith("prototype,f="), true, "the slow path of the check accepts a callable proxy: the prototype is read once");
check(construct(AcceptsCallables, [{ }], observed(AcceptsCallables)), "prototype,TypeError", "the slow path of the check rejects an object");

class Base { constructor(a) { this.x = $$t(a, 8); } }
class Derived extends Base { }
check(construct(Base, ["no"], observed(Derived)), "prototype,TypeError", "a class, the check fails");
check(construct(Base, [3], observed(Derived)), "prototype,x=3", "a class, the check passes");
check(Reflect.construct(Base, [3], Derived) instanceof Derived, true, "the prototype of new.target is used");

check(construct(Unchecked, [1], observed(Unchecked)), "prototype,x=1", "no check");
check(construct(AlwaysPasses, [], observed(AlwaysPasses)), "prototype,x=1", "a check that is known to pass");

applies(OneField, planned, checksAhead);
applies(TwoFields, planned, checksAhead);
applies(ChecksFirst, planned, checksAhead);
applies(AcceptsCallables, planned, checksAhead);
applies(Unchecked, planned);
doesNotApply(Unchecked, checksAhead);
applies(AlwaysPasses, planned);
doesNotApply(AlwaysPasses, checksAhead);
