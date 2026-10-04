//@ runDefault("-m", "--compileMainScriptAheadOfTime=1", "--useSoundTypes=1")

function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = aotRemarks(f.name);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + f.name + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = aotRemarks(f.name);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + f.name + ": " + remarks.join(" "));
    }
}
function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function outcomeOf(f) {
    try {
        f();
        return "returned";
    } catch (error) {
        return error.constructor.name;
    }
}

function readsProperty(o) { return o.property; }
readsProperty({ property: 1 });
const usesDataStubs = (aotRemarks("readsProperty") || []).includes("calls:GetById");

function product(x, y) {
    $$t(x, 8);
    $$t(y, 8);
    return $$t(x * y + 1, 8);
}
function sumOfProducts(u, k) {
    $$t(u, 10752);
    let t = 0;
    for (let i = 0; i < 100; i++)
        t += $$t(product(i, k), 8) * $$t(u[i], 8);
    return t;
}

function sumOfRoots(u) {
    $$t(u, 10752);
    let t = 0;
    for (let i = 0; i < 100; i++)
        t += Math.sqrt($$t(u[i], 8));
    return t;
}

function polynomial(x, y) {
    $$t(x, 8);
    $$t(y, 8);
    return x * y + x * x + y * y + x * 3 + y * 5 + x / 7 + y / 9 + x * y * 11;
}
function sumOfPolynomials(u, k) {
    $$t(u, 10752);
    let t = 0;
    for (let i = 0; i < 100; i++)
        t += polynomial(i, k) * $$t(u[i], 8);
    return t;
}

let replaced = function (x, y) {
    $$t(x, 8);
    $$t(y, 8);
    return x * y;
};
function sumOfReplaced(u, k) {
    $$t(u, 10752);
    let t = 0;
    for (let i = 0; i < 100; i++)
        t += replaced(i, k) * $$t(u[i], 8);
    return t;
}

let u = new Float64Array(100).fill(2);
for (let i = 0; i < 20; ++i) {
    check(sumOfProducts(u, 3), 29900, "products");
    check(sumOfProducts(u, 0.5), 5150, "products with a double");
    check(outcomeOf(() => sumOfProducts(u, "3")), "TypeError", "a string reaches the check of the callee");
    check(outcomeOf(() => sumOfProducts(u, undefined)), "TypeError", "undefined reaches the check of the callee");
    check(sumOfProducts(u, 3), 29900, "products after the failed checks");
    check(outcomeOf(() => sumOfProducts(new Float64Array(50).fill(2), 1)), "TypeError", "an element beyond the end");
    check(sumOfRoots(u), 141.42135623730945, "roots");
    check(sumOfPolynomials(u, 3), 1049080.9523809522, "polynomials");
    check(sumOfReplaced(u, 3), 29700, "the first function in the variable");
}
replaced = function (x, y) { return 1; };
check(sumOfReplaced(u, 3), 200, "the second function in the variable");

let isModule = this === undefined;
if (isModule) {
    applies(sumOfProducts, "inlined-call:product");
    doesNotApply(sumOfProducts, "guarded-intrinsic-call");
    if (usesDataStubs) {
        applies(sumOfProducts, "split-loop");
        applies(sumOfRoots, "split-loop", "guarded-intrinsic-call");
        applies(sumOfPolynomials, "split-loop");
    } else
        applies(sumOfRoots, "lowered-builtin:Math.sqrt");
    doesNotApply(sumOfPolynomials, "inlined-call");
    doesNotApply(sumOfReplaced, "inlined-call", "split-loop");
}
