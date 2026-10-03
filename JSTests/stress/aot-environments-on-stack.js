//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTScopeAsCallee=1", "--useAOTEnvironmentsOnStack=1")
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
const onStack = "environment-on-stack";
function identity(x) { return x; }
noInline(identity);
function big(x) { let s = 0; for (let i = 0; i < 3; i++) s += x; for (let i = 0; i < 3; i++) s -= x; for (let i = 0; i < 2; i++) s += 0; return s; }

function sharesVariables(n) {
    let total = n, calls = 0;
    function add(x) { calls++; total += x + big(x); return total; }
    function addTwice(x) { add(x); return add(x); }
    return addTwice(1) + add(2) * 10 + calls * 100;
}
function recursesWithOwnEnvironment(n) {
    let mine = n;
    function read() { return mine + big(mine); }
    const below = n ? recursesWithOwnEnvironment(n - 1) : 0;
    return read() + below * 10;
}
function holdsOnlyReference(n) {
    let kept = { v: n }, list = [n, n + 1];
    function churn() { let last; for (let i = 0; i < 2000; i++) last = { i, pad: [i, i, i] }; return last.i + big(0); }
    function read() { return kept.v + list[1] + big(0); }
    function replace() { kept = { v: kept.v + 1 }; list = [0, list[1] + 1]; return big(0); }
    churn(); replace(); churn();
    if (n % 16 === 0)
        gc();
    return read();
}
function inLoop(limit) {
    let total = 0;
    for (let i = 0; i < limit; i++) {
        let square = i * i;
        function read() { return square + i + big(i); }
        total += read();
    }
    return total;
}
function throwsThrough(n) {
    let state = "before";
    function fails() { state = "failing"; big(1); throw new Error("thrown " + n); }
    function wraps() { fails(); return "not reached"; }
    return wraps() + state;
}
function catchesOutside(n) {
    try {
        return throwsThrough(n);
    } catch (error) {
        return error.message;
    }
}
function returnsCallOfNested(n) {
    "use strict";
    let base = n;
    function last(x) { return base + x + big(x); }
    return last(1);
}
function nestsTwoLevels(a) {
    let x = a;
    function middle(b) {
        let y = b;
        function inner(c) { return x * 100 + y * 10 + c + big(c); }
        return inner(1) + inner(2);
    }
    return middle(3) + middle(4);
}
for (const f of [sharesVariables, recursesWithOwnEnvironment, holdsOnlyReference, inLoop, throwsThrough, returnsCallOfNested, nestsTwoLevels]) {
    noInline(f);
    applies(f, onStack);
}

function outlivesThroughOuterVariable() {
    let saved;
    function makes() { let x = 41; function g() { return x + 1 + big(x); } saved = g; return big(0); }
    makes();
    identity(1);
    return saved();
}
function outlivesByReturn() {
    function makes() { let x = 5; function g() { return x + big(x); } return g; }
    const g = makes();
    identity(2);
    return g();
}
function oneOfTwoEscapes() {
    let x = 7;
    function stays() { return x + big(x); }
    function leaves() { return x * 2; }
    return [stays(), leaves];
}
function passedAsCallback() {
    let x = 3;
    function callback(y) { return x + y + big(y); }
    function takes(f) { return f(1) + big(0); }
    return takes(callback);
}
function innerClosureEscapes() {
    let x = 9;
    function middle() { return () => x; }
    return middle();
}
function* generatorBody() { let x = 1; function read() { return x + big(x); } yield read(); x++; yield read(); }
async function asyncBody() { let x = 1; function read() { return x + big(x); } await null; return read(); }
function makesInOuter() { return outlivesThroughOuterVariable; }
for (const f of [oneOfTwoEscapes, innerClosureEscapes, generatorBody, asyncBody]) {
    noInline(f);
    doesNotApply(f, onStack);
}

for (let i = 0; i < 200; i++) {
    check(sharesVariables(1), 3 + 5 * 10 + 3 * 100, "functions that share variables in the frame");
    check(recursesWithOwnEnvironment(3), 3 + 2 * 10 + 1 * 100 + 0, "each activation has its own environment");
    check(holdsOnlyReference(i), i + 1 + i + 2, "objects that only the frame refers to, across collections");
    check(inLoop(4), 0 + 2 + 6 + 12, "an environment for each iteration");
    check(catchesOutside(i), "thrown " + i, "an exception that unwinds frames with environments");
    check(returnsCallOfNested(5), 6, "a call in tail position that is given the environment");
    check(nestsTwoLevels(1), 131 + 132 + 141 + 142, "an environment in the frame whose parent is one");
    check(outlivesThroughOuterVariable(), 42, "a function that is kept in a variable of an outer function");
    check(outlivesByReturn(), 5, "a function that is returned to a known caller");
    check(oneOfTwoEscapes()[0] + oneOfTwoEscapes()[1](), 21, "two functions, one of which is given away");
    check(passedAsCallback(), 4, "a function that is passed to a known function");
    check(innerClosureEscapes()(), 9, "a closure of a function without an object that is given away");
    check([...generatorBody()].join(), "1,2", "a generator");
}
let awaited;
asyncBody().then(value => { awaited = value; });
drainMicrotasks();
check(awaited, 1, "an async function");
