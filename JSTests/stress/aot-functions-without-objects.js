//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTScopeAsCallee=1")
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
const none = "no-function-object";
function identity(x) { return x; }
noInline(identity);

function callsDeclarations(n) {
    let total = n;
    function addsOne() { total += 1; return total; }
    function addsTwo() { total += 2; return addsOne(); }
    function big(x) { let s = 0; for (let i = 0; i < x; i++) s += i * total; for (let i = 0; i < x; i++) s -= i; for (let i = 0; i < x; i++) s += 1; return s; }
    return addsOne() + addsTwo() + big(3);
}
function recurses(n) {
    let calls = 0;
    function down(k) { calls++; return k ? down(k - 1) + 1 : 0; }
    function even(k) { calls++; return k ? odd(k - 1) : true; }
    function odd(k) { calls++; return k ? even(k - 1) : false; }
    return down(n) + "," + even(n) + "," + calls;
}
function nestsDeeply(a) {
    function middle(b) {
        function inner(c) { return a * 100 + b * 10 + c; }
        return inner(3) + inner(4);
    }
    return middle(1) + middle(2);
}
function callsArrow(n) {
    const twice = x => { let s = 0; for (let i = 0; i < 2; i++) s += x; for (let i = 0; i < n; i++) s += 1; for (let i = 0; i < 3; i++) s += 0; return s; };
    return twice(1) + twice(2);
}
function callsFromClosureThatEscapes(n) {
    function helper(x) { return x + n; }
    return () => helper(1) + helper(2);
}
function perActivation(n) {
    function own() { return n; }
    return n ? own() * 10 + perActivation(n - 1) : own();
}
function inBlockAndLoop(limit) {
    let total = 0;
    for (let i = 0; i < limit; i++) {
        function ofIteration() { return i * 2; }
        total += ofIteration();
    }
    return total;
}
function throwsFromNested() {
    function fails() { throw new Error("from " + "nested"); }
    try {
        fails();
    } catch (error) {
        return error.message + "|" + error.stack.split("\n").slice(0, 2).map(line => line.split("@")[0]).join(">");
    }
}
for (const [f, ...names] of [[callsDeclarations, "addsOne", "addsTwo", "big"], [recurses, "down", "even", "odd"], [nestsDeeply, "middle"], [callsArrow], [callsFromClosureThatEscapes, "helper"], [perActivation, "own"], [throwsFromNested, "fails"]]) {
    noInline(f);
    for (const name of names)
        applies(f, none + ":" + name);
}

function returnsIt() { function given() { return 1; } return given; }
function storesIt() { function kept() { return 2; } return { kept }; }
function passesIt() { function passed() { return 3; } return identity(passed)(); }
function asksWhatItIs() { function asked() { return 4; } return typeof asked + asked(); }
function readsItsName() { function named() { return 5; } return named.name + named.length + named(); }
function callsWithCall() { function called() { return this.v; } return called.call({ v: 6 }); }
function constructsIt() { function Made() { this.v = 7; } return new Made().v; }
function comparesIt() { function one() { return 8; } function other() { return 9; } return (one === other) + "," + (one === one) + one() + other(); }
function mapsWithIt() { function mapped(x) { return x + 1; } return [1, 2].map(mapped).join(); }
function readsItself() { function self() { return typeof self; } return self(); }
function usesArguments() { function counts() { return arguments.length; } return counts(1, 2, 3); }
function assignsIt(flag) { function first() { return 10; } let f = first; if (flag) f = () => 11; return f(); }
const negative = [[returnsIt, "given"], [storesIt, "kept"], [passesIt, "passed"], [asksWhatItIs, "asked"], [readsItsName, "named"], [callsWithCall, "called"], [constructsIt, "Made"], [comparesIt, "one", "other"], [mapsWithIt, "mapped"], [readsItself, "self"]];
for (const [f, ...names] of negative) {
    noInline(f);
    for (const name of names)
        doesNotApply(f, none + ":" + name);
}

for (let i = 0; i < 100; i++) {
    check(callsDeclarations(1), 2 + 5 + (0 + 5 + 10 - 3 + 3), "declarations that share a variable");
    check(recurses(4), "4,true,10", "functions that call themselves and each other");
    check(nestsDeeply(5), 513 + 514 + 523 + 524, "a function in a function in a function");
    check(callsArrow(1), 3 + 5, "an arrow function in a const");
    check(callsFromClosureThatEscapes(10)(), 11 + 12, "a function that is called from a closure that is given away");
    check(perActivation(3), 30 + 20 + 10 + 0, "each activation has its own");
    check(inBlockAndLoop(4), 12, "a function in a block in a loop");
    check(throwsFromNested(), "from nested|fails>throwsFromNested", "the stack of an error that is thrown in one");
    check(returnsIt()(), 1, "a function that is returned");
    check(storesIt().kept(), 2, "a function that is stored");
    check(passesIt(), 3, "a function that is passed");
    check(asksWhatItIs(), "function4", "typeof");
    check(readsItsName(), "named05", "its name and length");
    check(callsWithCall(), 6, "Function.prototype.call");
    check(constructsIt(), 7, "new");
    check(comparesIt(), "false,true89", "a comparison");
    check(mapsWithIt(), "2,3", "a callback of a builtin");
    check(readsItself(), "function", "a function that reads itself");
    check(usesArguments(), 3, "a function that uses arguments");
    check(assignsIt(false) + assignsIt(true), 21, "a variable that holds one of two");
}
