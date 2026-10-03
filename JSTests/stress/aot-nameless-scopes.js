//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTNamelessScopes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(typeof f === "string" ? f : f.name);
    if (!remarks && typeof f !== "string" && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + (f.name || f) + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + (f.name || f) + ": " + remarks.join(" "));
    }
}
const nameless = "nameless-scope";
function identity(x) { return x; }
noInline(identity);

function counter(start) {
    let count = start;
    return [() => ++count, () => count];
}
function nested(a) {
    let outer = a;
    return function (b) {
        let inner = b;
        return [() => { outer += 1; inner += 10; }, () => outer * 1000 + inner];
    };
}
function perIteration(limit) {
    const made = [];
    for (let i = 0; i < limit; i++) {
        let seen = i;
        made.push(() => seen += i);
    }
    return made;
}
function keepsObjects(n) {
    let a = { v: n }, b = [n, n + 1], c = "text" + n;
    return [() => { a = { v: a.v + 1 }; b = [b[1], b[0]]; c += "!"; }, () => a.v + ":" + b.join() + ":" + c];
}
async function acrossAwait(n) {
    let total = n;
    const add = (x) => { total += x; };
    await null;
    add(1);
    await null;
    add(2);
    return total;
}
function* acrossYield(n) {
    let total = n;
    const add = (x) => { total += x; };
    yield total;
    add(5);
    yield total;
}
for (const f of [counter, nested, perIteration, keepsObjects]) {
    noInline(f);
    applies(f, nameless);
}

function seenByEval(x, text) { let hidden = x; const bump = () => { hidden++; }; bump(); return eval(text); }
function enclosesEval(x) { let enclosed = x; const bump = () => { enclosed++; }; bump(); return function (text) { return eval(text); }; }
function seenByWith(object) { let shadowed = 1; const bump = () => { shadowed++; }; bump(); with (object) { return (() => shadowed)(); } }
function mappedArguments(a) { const read = () => a; arguments[0] = 7; return read() + arguments[0]; }
for (const f of [seenByEval, enclosesEval, seenByWith, mappedArguments]) {
    noInline(f);
    doesNotApply(f, nameless);
}

const bySize = [];
for (let size = 1; size <= 36; size++) {
    const names = Array.from({ length: size }, (_, i) => "v" + i);
    bySize.push([size, names]);
}
function ofSize3() { let v0 = { n: 0 }, v1 = { n: 1 }, v2 = { n: 2 }; return [() => { v0 = { n: v0.n + 1 }; v1 = { n: v1.n + 1 }; v2 = { n: v2.n + 1 }; }, () => v0.n + v1.n + v2.n]; }
function ofSize34() {
    let a0 = [0], a1 = [1], a2 = [2], a3 = [3], a4 = [4], a5 = [5], a6 = [6], a7 = [7], a8 = [8], a9 = [9], a10 = [10], a11 = [11], a12 = [12], a13 = [13], a14 = [14], a15 = [15], a16 = [16],
        a17 = [17], a18 = [18], a19 = [19], a20 = [20], a21 = [21], a22 = [22], a23 = [23], a24 = [24], a25 = [25], a26 = [26], a27 = [27], a28 = [28], a29 = [29], a30 = [30], a31 = [31], a32 = [32], a33 = [33];
    return [() => { a0 = [a0[0] + 1]; a33 = [a33[0] + 1]; }, () => a0[0] + a1[0] + a2[0] + a3[0] + a4[0] + a5[0] + a6[0] + a7[0] + a8[0] + a9[0] + a10[0] + a11[0] + a12[0] + a13[0] + a14[0] + a15[0] + a16[0]
        + a17[0] + a18[0] + a19[0] + a20[0] + a21[0] + a22[0] + a23[0] + a24[0] + a25[0] + a26[0] + a27[0] + a28[0] + a29[0] + a30[0] + a31[0] + a32[0] + a33[0]];
}
function ofSize32() {
    let a0 = [0], a1 = [1], a2 = [2], a3 = [3], a4 = [4], a5 = [5], a6 = [6], a7 = [7], a8 = [8], a9 = [9], a10 = [10], a11 = [11], a12 = [12], a13 = [13], a14 = [14], a15 = [15], a16 = [16],
        a17 = [17], a18 = [18], a19 = [19], a20 = [20], a21 = [21], a22 = [22], a23 = [23], a24 = [24], a25 = [25], a26 = [26], a27 = [27], a28 = [28], a29 = [29], a30 = [30], a31 = [31];
    return [() => { a0 = [a0[0] + 1]; a31 = [a31[0] + 1]; }, () => a0[0] + a1[0] + a2[0] + a3[0] + a4[0] + a5[0] + a6[0] + a7[0] + a8[0] + a9[0] + a10[0] + a11[0] + a12[0] + a13[0] + a14[0] + a15[0] + a16[0]
        + a17[0] + a18[0] + a19[0] + a20[0] + a21[0] + a22[0] + a23[0] + a24[0] + a25[0] + a26[0] + a27[0] + a28[0] + a29[0] + a30[0] + a31[0]];
}
for (const f of [ofSize3, ofSize32, ofSize34])
    noInline(f);
applies(ofSize3, nameless);
applies(ofSize32, nameless);
doesNotApply(ofSize34, nameless);

function resultOf(promise) {
    let result;
    promise.then(value => { result = value; }, error => { result = "rejected: " + error; });
    drainMicrotasks();
    return result;
}
for (let i = 0; i < 200; i++) {
    const [bump, read] = counter(i);
    check(bump() + bump() + read(), 3 * i + 5, "a variable that two closures share");
    const [change, sum] = nested(1)(2);
    change();
    check(sum(), 2012, "variables of two scopes");
    check(perIteration(4).map(f => f()).join(), "0,2,4,6", "a scope for each iteration");
    const [replace, show] = keepsObjects(i);
    const kept3 = ofSize3(), kept32 = ofSize32(), kept34 = ofSize34();
    if (i % 16 === 0)
        gc();
    replace();
    check(show(), (i + 1) + ":" + (i + 1) + "," + i + ":text" + i + "!", "objects that only a scope refers to");
    kept3[0](); kept32[0](); kept34[0]();
    check(kept3[1]() + ":" + kept32[1]() + ":" + kept34[1](), "6:498:563", "each variable of a small, a large and a too large scope");
    check(resultOf(acrossAwait(i)), i + 3, "a scope that is kept across an await");
    check([...acrossYield(i)].join(), i + "," + (i + 5), "a scope that is kept across a yield");

    check(seenByEval(i, "hidden"), i + 1, "a variable that evaluated code reads by name");
    check(seenByEval(i, "hidden = 5; hidden + 1"), 6, "and writes");
    check(enclosesEval(i)("enclosed"), i + 1, "a variable of a function that encloses the one that evaluates");
    check(seenByWith({}), 2, "a variable that is looked for behind an object");
    check(seenByWith({ shadowed: "object's" }), "object's", "and is hidden by it");
    check(mappedArguments(1), 14, "a parameter that arguments is mapped to");
}
