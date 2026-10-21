//@ runDefault
//@ runDefault("--useSourceProviderCache=false")
//@ runDefault("--useBytecodeOptimizer=1", "--validateBytecodeOptimizerStaticScopes=1")
// A parameter of an async arrow function is not a use of a variable of the same name further out:
// async-arrow-function-parameters-should-not-capture-outer-variables.js. While the parser recorded it as one, that
// record hid every use of the variable that the parser missed. So each statement below uses the variable `n` of the
// function around `inner` next to such an arrow function, and has to work with no help from it.
//
// A failure here is a use that the parser does not record. It is not in the code for `async (`. The uses that it
// missed were a default value of a function whose body declares the name again (parseFunctionInfo), and a shorthand
// target of an object assignment pattern that `=` does not follow (parseDestructuringPattern).
//
// Each text runs once, so the configurations that compile early or collect all the time add nothing.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected} but got ${actual}`);
}

// Each statement reads `n` into the global variable `q`.
const reads = [
    // Expressions and statements.
    'q = n;',
    'q = typeof n === "string" ? n : "undef";',
    'q = (0, n);',
    'q = `${n}`;',
    'q = ({ n }).n;',
    'q = Object.keys({ [n]: 1 })[0];',
    'q = [...n].join("");',
    'q = n?.toString();',
    'q = n ?? "nullish";',
    'switch (n) { default: q = n; }',
    'l: { q = n; break l; }',
    'for (var i = n; ; ) { q = i; break; }',
    'for (var c of [n]) q = c;',
    'with ({}) { q = n; }',
    'q = eval("n");',
    // Functions inside `inner`.
    'q = (() => n)();',
    'q = (function () { return n; })();',
    'q = (() => (() => n)())();',
    'q = (function* () { yield n; })().next().value;',
    '(async () => { q = n; })();',
    'q = (function () { return eval("n"); })();',
    // Default values.
    'q = ((a = n) => a)();',
    'q = ((a = () => n) => a())();',
    'q = ((p = n) => p)();',
    'try { throw {}; } catch ({ m = n }) { q = m; }',
    // Default values of a function whose body declares `n` again.
    'q = (function (p = n) { var n; return p; })();',
    'q = (function (p = n) { let n; return p; })();',
    'q = (function (p = n) { return p; function n() {} })();',
    'q = (function(p=n){var n;return p})();',
    'q = (function (p = () => n) { var n; return p(); })();',
    'q = (function ({ p = n } = {}) { var n; return p; })();',
    'q = (function ({ [n]: p } = { init: "init", new: "new" }) { var n; return p; })();',
    'q = (function (p = function (r = n) { var n; return r; }) { var n; return p(); })();',
    'q = (function (p = [n][0]) { var n = 1; return p; })();',
    'q = (function (p = n) { var n = arguments.length; return p; })();',
    'q = (function (...[p = n]) { var n; return p; })();',
    'q = ((p = n) => { var n; return p; })();',
    'q = ((p = n) => { function n() {} return p; })();',
    'q = ((p = n) => { let n; return p; })();',
    'q = "pending"; (async function (p = n) { var n; q = p; })();',
    'q = (function* (p = n) { var n; yield p; })().next().value;',
    'q = ({ m(p = n) { var n; return p; } }).m();',
    '({ set s(p) { var n; q = p; } , get g() { return 1; } }); q = ({ m(p = n) { let n; return p; } }).m();',
    'q = new (class { m(p = n) { var n; return p; } })().m();',
    'q = new (class { constructor(p = n) { var n; this.v = p; } })().v;',
    // Classes.
    'q = new (class { f = n; })().f;',
    'q = (class { static f = n; }).f;',
    'q = Object.getOwnPropertyNames((class { [n]() {} }).prototype)[1];',
    'class C { static { q = n; } }',
    'var C0 = class extends (q = n, Object) {};',
    // Assignment patterns that read `n` in a default value, a key or a target.
    '[q = n] = [];',
    '({ a: q = n } = {});',
    'var z0; ({ z0 = n } = {}); q = z0;',
    '({ [n]: q } = { init: "init", new: "new" });',
    'var o0 = {}; [o0[n]] = [1]; q = Object.keys(o0)[0];',
    'for ({ a: q = n } of [{}]) ;',
    'for ([q = n] of [[]]) ;',
    'var z1; for ({ z1 = n } of [{}]) ; q = z1;',
    'for ({ [n]: q } of [{ init: "init", new: "new" }]) ;',
    'var o1 = {}; for (o1[n] of [1]) ; q = Object.keys(o1)[0];',
    'var o2 = {}; for ({ a: o2[n] } of [{ a: 1 }]) ; q = Object.keys(o2)[0];',
];

// Each statement writes "new" to `n`.
const writes = [
    // Assignments.
    'n = "new";',
    'n = ""; n += "new";',
    'n = 0; n++; n = n === 1 ? "new" : n;',
    'n = null; n ??= "new";',
    'n &&= "new";',
    'for (n = "new"; ; ) break;',
    'try { throw "new"; } catch (e) { n = e; }',
    'for (var [i0 = (n = "new")] of [[]]) ;',
    '(function () { n = "new"; })();',
    '((a = (n = "new")) => a)();',
    // Array assignment patterns.
    '[n] = ["new"];',
    '[n = "new"] = [];',
    '[...n] = ["new"]; n = n[0];',
    'var z6; [n, z6] = ["new", 1];',
    '[[n]] = [["new"]];',
    '[{ a: n }] = [{ a: "new" }];',
    '[{ n }] = [{ n: "new" }];',
    '[{ a = 1, n }] = [{ n: "new" }];',
    // Object assignment patterns.
    '({ n } = { n: "new" });',
    '({ n = "new" } = {});',
    '({ a = 1, n } = { n: "new" });',
    '({ n, a = 1 } = { n: "new" });',
    'var z2; ({ n = 1, z2 } = { n: "new" });',
    'var z3; ({ z3, n } = { n: "new" });',
    'var z4, z5; ({ z4 = 1, n, z5 = 2 } = { n: "new" });',
    '({ a: n } = { a: "new" });',
    '({ a: n = "new" } = {});',
    '({ n: n } = { n: "new" });',
    '({ "n": n } = { n: "new" });',
    '({ a: { n } } = { a: { n: "new" } });',
    '({ a: [n] } = { a: ["new"] });',
    '({ a: [{ n }] } = { a: [{ n: "new" }] });',
    '({ ...n } = { v: "new" }); n = n.v;',
    'var r0; ({ n, ...r0 } = { n: "new" });',
    '(({ n } = { n: "new" }));',
    '0, { n } = { n: "new" };',
    // Heads of for-of and for-in statements.
    'for (n of ["new"]) ;',
    'for (n in { new: 1 }) ;',
    'for ({ n } of [{ n: "new" }]) ;',
    'for ({ n = "new" } of [{}]) ;',
    'for ({ a = 1, n } of [{ n: "new" }]) ;',
    'for ({ n, b = 1 } of [{ n: "new" }]) ;',
    'for ({ a: n } of [{ a: "new" }]) ;',
    'for ({ a: n = "new" } of [{}]) ;',
    'for ({ a: { n } } of [{ a: { n: "new" } }]) ;',
    'for ({ a: { b = 1, n } } of [{ a: { n: "new" } }]) ;',
    'for ({ a: [n] } of [{ a: ["new"] }]) ;',
    'for ({ ...n } of [{ v: "new" }]) ; n = n.v;',
    'for ([n] of [["new"]]) ;',
    'for ([n = "new"] of [[]]) ;',
    'for ([{ n }] of [[{ n: "new" }]]) ;',
    'for ([{ a: n }] of [[{ a: "new" }]]) ;',
    'for ([[n]] of [[["new"]]]) ;',
    'for ([...n] of [["new"]]) ; n = n[0];',
    'for ({ length: n } in { new: 1 }) ; n = n === 3 ? "new" : n;',
    'for ({ n } in { key: 1 }) ; n = n === undefined ? "new" : n;',
    'for ([n] in { new: 1 }) ; n = n === "n" ? "new" : n;',
    // Assignment patterns in a function, a default value, a catch block or a class.
    '(() => { ({ a = 1, n } = { n: "new" }); })();',
    '(() => { for ({ n } of [{ n: "new" }]) ; })();',
    '(function () { for ({ n } of [{ n: "new" }]) ; })();',
    '((a = ({ n } = { n: "new" })) => a)();',
    '((a = ({ z = 1, n } = { n: "new" })) => a)();',
    '((a = 0, b = ({ c = 1, n } = { n: "new" })) => a)();',
    '(async (a = ({ c = 1, n } = { n: "new" })) => a)();',
    'var async = (x) => x; async(({ c = 1, n } = { n: "new" }));',
    '(async (a = function () { for ({ n } of [{ n: "new" }]) ; }) => a())();',
    'try { throw 1; } catch (e) { ({ z = 1, n } = { n: "new" }); }',
    'new (class { f = ({ z = 1, n } = { n: "new" }); })();',
    'class C { static { for ({ n } of [{ n: "new" }]) ; } }',
];

// These write to `n` after the function around `inner` has returned.
const writesLater = [
    '(async () => { for await (n of ["new"]) ; })();',
    '(async () => { for await ({ n } of [{ n: "new" }]) ; })();',
];

// The async arrow function is after the statement, has a pattern, or is before the statement.
const siblings = [
    statement => `${statement} var sibling = async (n) => n;`,
    statement => `${statement} var sibling = async ({ n }) => n;`,
    statement => `var sibling = async (n) => n; ${statement}`,
];

// What the function around `inner` returns for `result`. A write that misses `n` makes a global variable.
var q;
function run(body, result) {
    q = "unset";
    let value;
    try {
        value = new Function(`var n = "init"; function inner() { ${body} } inner(); return ${result};`)();
    } catch (error) {
        value = String(error);
    }
    drainMicrotasks();
    if ("n" in globalThis) {
        value += ", and a global n";
        delete globalThis.n;
    }
    return value;
}

for (const sibling of siblings) {
    for (const statement of reads)
        shouldBe(run(sibling(statement), "q"), "init", sibling(statement));
    for (const statement of writes)
        shouldBe(run(sibling(statement), "n"), "new", sibling(statement));
    for (const statement of writesLater)
        shouldBe(run(sibling(statement), "n"), "init", sibling(statement));
}
