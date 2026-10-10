//@ runDefault("--compileMainScriptAheadOfTime=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function KeepsTarget() {
    let who = new.target || 1;
    return function () { return typeof (who + 1); };
}
noInline(KeepsTarget);
check(KeepsTarget()(), "number", "called");
check((new KeepsTarget())(), "string", "constructed");

function AsksInArrow() { return () => new.target === undefined; }
noInline(AsksInArrow);
check(AsksInArrow()(), true, "new.target of a call, in an arrow function");
check((new AsksInArrow())(), false, "new.target of a construction, in an arrow function");

function KeepsThis(n) {
    let self = this === undefined || this === globalThis ? n : this;
    let reads = function () { return self; };
    let writes = function (value) { self = value; };
    return { reads, writes, direct() { return self; } };
}
noInline(KeepsThis);
let called = KeepsThis(7);
check(called.reads(), 7, "called: a number");
let constructed = new KeepsThis(7);
check(typeof constructed.reads(), "object", "constructed: this");
constructed.writes("text");
check(constructed.reads(), "text", "written by a closure of the constructed one");
check(constructed.direct(), "text", "read by another closure");
called.writes(2.5);
check(called.reads() + 1, 3.5, "written by a closure of the called one");

function CountsBoth() {
    let count = new.target ? 0.5 : 1;
    function bump() { count += new.target ? 1 : count; return count; }
    return bump;
}
noInline(CountsBoth);
check(CountsBoth()(), 2, "called");
check((new CountsBoth())(), 1, "constructed");
check(new (new CountsBoth())() instanceof Object, true, "the closure constructed");
