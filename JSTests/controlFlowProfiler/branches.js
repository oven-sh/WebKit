var branchExecutionCounts = $vm.branchExecutionCounts;

load("./driver/driver.js");

function foo() { }
function fail() { throw new Error("fail"); }

// `text` starts where the decision does: an `if` at its condition, and one that can only skip something at what it skips.
function branches(func, text, ...expected) {
    let actual = branchExecutionCounts(func, text);
    assert(`${actual}` === `${expected}`, `${func.name}: the branches of "${text}" ran [${actual}] times, not [${expected}]`);
}

function ifStatements(a, b) {
    if (a)
        foo();
    if (b) {
        foo();
    } else
        foo();
    if (a) foo(); else if (b) foo();
}
ifStatements(true, false);
ifStatements(true, true);
ifStatements(false, true);
branches(ifStatements, "a)\n", 2, 1);
branches(ifStatements, "b) {\n        foo", 2, 1);
branches(ifStatements, "a) foo()", 2, 1);
branches(ifStatements, "b) foo()", 1, 0);

// Neither branch is taken.
function ifConditionThrows(a) {
    try {
        if (a || fail())
            foo();
    } catch { }
}
ifConditionThrows(true);
ifConditionThrows(false);
branches(ifConditionThrows, "a || fail()", 1, 0);
branches(ifConditionThrows, " || fail()", 1, 1);
branches(ifConditionThrows, "try", 1, 1);

// What leaves the `if` does not count for the branch that is not in the text.
function ifLeaves(a) {
    for (let i = 0; i < 3; i++) {
        if (i === a) break;
        if (i === 0) continue;
        if (i === 1) return;
    }
}
ifLeaves(0);
ifLeaves(5);
branches(ifLeaves, "i === a", 1, 2);
branches(ifLeaves, "i === 0", 1, 1);
branches(ifLeaves, "i === 1", 1, 0);
branches(ifLeaves, "for", 3, 0);

function conditionals(a, b) {
    let r = a ? 1 : 2;
    r = a ? b ? 1 : 2 : 3;
    if (a ? b : !b)
        foo();
    return r;
}
conditionals(true, false);
conditionals(true, true);
conditionals(false, true);
branches(conditionals, "a ? 1", 2, 1);
branches(conditionals, "a ? b ? 1", 2, 1);
branches(conditionals, "b ? 1", 1, 1);
// The `?:` starts where the `if` does, and ends first.
branches(conditionals, "a ? b : !b", 2, 1, 1, 2);

function logical(a, b, c) {
    let r = a && b;
    r = a || b;
    r = c ?? b;
    a && foo();
    a || foo();
    r = a && b && c;
    if (a && b)
        foo();
    if (a || b)
        foo();
    if (c ?? b)
        foo();
    return r;
}
logical(1, 0, null);
logical(1, 1, 2);
logical(0, 1, undefined);
branches(logical, " && b;", 2, 1);
branches(logical, " || b;", 1, 2);
branches(logical, " ?? b;", 2, 1);
branches(logical, " && foo()", 2, 1);
branches(logical, " || foo()", 1, 2);
branches(logical, " && b && c", 2, 1);
branches(logical, " && c;", 1, 2);
branches(logical, " && b)", 2, 1);
branches(logical, " || b)", 1, 2);
branches(logical, " ?? b)", 2, 1);

function logicalAssignment(a, o) {
    a ||= 1;
    a &&= 2;
    o.x ??= 3;
    o["y"] ||= 4;
}
logicalAssignment(0, { x: 1 });
logicalAssignment(1, { y: 1 });
logicalAssignment(1, { });
branches(logicalAssignment, " 1;", 1, 2);
branches(logicalAssignment, " 2;", 3, 0);
branches(logicalAssignment, " 3;", 2, 1);
branches(logicalAssignment, " 4;", 2, 1);

function optionalChains(o, f) {
    let r = o?.a;
    r = o?.a?.b;
    r = o?.["a"];
    r = f?.();
    r = o?.a ?? 1;
    delete o?.c;
    return r;
}
optionalChains(null, null);
optionalChains({ }, foo);
optionalChains({ a: { } }, foo);
branches(optionalChains, "?.a;", 2, 1);
branches(optionalChains, "?.a?.b", 2, 1);
branches(optionalChains, "?.b", 1, 1);
branches(optionalChains, "?.[", 2, 1);
branches(optionalChains, "?.()", 2, 1);
branches(optionalChains, "?.a ??", 2, 1);
branches(optionalChains, " ?? 1", 2, 1);
branches(optionalChains, "?.c", 2, 1);

function defaultValues(a = 1, { b = 2 } = { }, [c = 3] = []) {
    let d, e;
    [d = 4] = [a];
    ({ e = 5 } = { e: b });
    for (let [f = 6] of [[c], []])
        foo();
}
defaultValues();
defaultValues(0, { b: 0 }, [0]);
defaultValues(undefined, { }, []);
branches(defaultValues, " 1,", 2, 1);
branches(defaultValues, " 2 }", 2, 1);
branches(defaultValues, " { },", 1, 2);
branches(defaultValues, " 3]", 2, 1);
branches(defaultValues, " []) {", 1, 2);
branches(defaultValues, " 4]", 0, 3);
branches(defaultValues, " 5 }", 0, 3);
branches(defaultValues, " 6]", 3, 3);

function loops(limit) {
    let i = 0;
    while (i < limit)
        i++;
    for (i = 0; i < limit; i++)
        foo();
    for (;;)
        break;
    i = 0;
    do
        i++;
    while (i < limit);
    for (let key in { a: 1, b: 2 })
        foo();
    for (let value of [])
        foo();
}
loops(0);
loops(3);
branches(loops, "while (i < limit)\n", 3, 2);
branches(loops, "for (i = 0", 3, 2);
branches(loops, "for (;;)");
branches(loops, "i < limit);", 2, 2);
branches(loops, "for (let key", 4);
branches(loops, "for (let value", 0);

// The condition is emitted twice, and `break` is not the condition being false.
function loopConditions(a) {
    let i = 0;
    while (i < 3 && a) {
        if (i++ === 1)
            break;
    }
    do {
        if (a)
            break;
    } while (i++ < 4 || a);
}
loopConditions(true);
loopConditions(false);
branches(loopConditions, "while (i < 3", 2, 1);
branches(loopConditions, " && a", 3, 0);
branches(loopConditions, "i++ < 4", 4, 1);
branches(loopConditions, " || a", 1, 4);

function switches(a) {
    switch (a) {
    case 0:
        foo();
    case 1:
    case 2:
        foo();
        break;
    default:
        foo();
    case 3:
        foo();
    }
    switch (a) {
    case 0:
        break;
    }
    switch (a) {
    case "a":
        foo();
    case foo():
        break;
    }
    switch (a) { }
    switch (a) {
    default:
    }
}
for (let a of [0, 0, 1, 2, 3, 4, "a"])
    switches(a);
branches(switches, "switch (a) {\n    case 0:\n        foo", 2, 1, 1, 2, 1);
branches(switches, "switch (a) {\n    case 0:\n        break", 2, 5);
branches(switches, "switch (a) {\n    case \"a\"", 1, 0, 6);
branches(switches, "switch (a) { }");
branches(switches, "switch (a) {\n    default", 7);

// The test of a `case` runs until one matches.
function caseTests(a, b) {
    switch (a) {
    case b && 1:
    case b?.x:
    case b ? 2 : 3:
        foo();
    }
}
caseTests(1, 1);
caseTests(3, 0);
caseTests(4, 1);
branches(caseTests, "switch", 1, 0, 1, 1);
branches(caseTests, " && 1", 2, 1);
branches(caseTests, "?.x", 2, 0);
branches(caseTests, "b ? 2", 1, 1);

function tries(a) {
    try {
        if (a === 1)
            fail();
        if (a === 2)
            return 2;
    } catch (error) {
        foo();
    }
    try {
        foo();
    } finally {
        foo();
    }
    try {
        fail();
    } catch ({ message = foo() }) {
    } finally {
        foo();
    }
    for (let i = 0; i < 2; i++) {
        try {
            continue;
        } catch { }
    }
}
tries(0);
tries(1);
tries(2);
branches(tries, "try {\n        if", 1, 2);
branches(tries, "try {\n        foo");
branches(tries, "try {\n        fail", 2, 0);
branches(tries, "try {\n            continue", 0, 4);

// It was caught.
function catchPatternThrows() {
    try {
        try {
            fail();
        } catch ({ message: { length: { none: { none } } } }) {
            foo();
        }
    } catch { }
}
catchPatternThrows();
branches(catchPatternThrows, "try {\n            fail", 1, 0);

function* generator(a) {
    let r = (yield 1) ? yield 2 : 3;
    if (yield r)
        foo();
}
let started = generator();
started.next();
let finished = generator();
finished.next();
finished.next(true);
finished.next();
finished.next(false);
branches(generator, "(yield 1)", 1, 0);
branches(generator, "yield r", 0, 1);

function classes() {
    class Fields {
        field = Fields.flag ? 1 : 2;
        static flag = foo() ?? true;
        static {
            Fields.flag &&= false;
        }
        constructor(a = 1) {
            this.a = a || 2;
        }
    }
    new Fields;
    new Fields(0);
}
classes();
branches(classes, "Fields.flag ? 1", 0, 2);
branches(classes, " ?? true", 1, 0);
branches(classes, " false", 1, 0);
branches(classes, " 1) {", 1, 1);
branches(classes, " || 2", 1, 1);

// branchExecutionCounts generates the bytecode of the functions that were not called.
function hasFunctions() {
    function notCalled(a, b = 1) {
        if (a)
            return a && b;
        let inner = () => a ? 1 : 2;
        class Inner {
            field = a ?? 1;
            method() {
                switch (a) {
                case 1:
                }
            }
        }
        return function* () {
            while (a)
                yield a?.b;
        };
    }
    return function calledLater(a) {
        return a ? 3 : 4;
    };
}
let calledLater = hasFunctions();
branches(hasFunctions, " 1) {", 0, 0);
branches(hasFunctions, "a)\n            return", 0, 0);
branches(hasFunctions, " && b", 0, 0);
branches(hasFunctions, "a ? 1", 0, 0);
branches(hasFunctions, " ?? 1", 0, 0);
branches(hasFunctions, "switch", 0, 0);
branches(hasFunctions, "while", 0, 0);
branches(hasFunctions, "?.b", 0, 0);

// A call uses that bytecode, and what ran keeps its counts.
branches(hasFunctions, "a ? 3", 0, 0);
calledLater(true);
branches(hasFunctions, "a ? 3", 1, 0);
calledLater(false);
branches(hasFunctions, "a ? 3", 1, 1);
branches(ifStatements, "a)\n", 2, 1);

function tiersUp(a) {
    if (a & 1)
        return a && 1;
    return a ? 2 : 3;
}
for (let i = 0; i < testLoopCount; i++)
    tiersUp(i);
branches(tiersUp, "a & 1", testLoopCount >> 1, testLoopCount - (testLoopCount >> 1));
branches(tiersUp, " && 1", testLoopCount >> 1, 0);
branches(tiersUp, "a ? 2", testLoopCount - (testLoopCount >> 1) - 1, 1);
