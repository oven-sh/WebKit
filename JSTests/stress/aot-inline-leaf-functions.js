//@ requireOptions("--useSoundTypes=1")
// In the fast copy of a loop, a call of a function that only works something out from its arguments is replaced by that.
function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error("bad value: " + actual + ", expected " + expected);
}

function A(i, j) { $$t(i, 8); $$t(j, 8); return $$t(1 / ((i + j) * (i + j + 1) / 2 + i + 1), 8); }
function mix(a, b) { $$t(a, 8); $$t(b, 8); return ((a | 0) ^ (b << 3)) + (a < b ? 0 : 0) - -b; }
function less(a, b) { $$t(a, 8); $$t(b, 8); return a < b; }
function first(a, b, c) { $$t(a, 8); return a; }
// So that they have code for whoever compiles their callers to look at.
A(1, 2); mix(1, 2); less(1, 2); first(1, 2, 3);

function sum(n, extra) {
    let s = 0;
    for (let i = 0; i < n; i++)
        s += A(i, extra) + mix(i, extra) + (less(i, 3) ? 1 : 2) + first(i, s, "x");
    return s;
}
function reference(n, extra) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        s += 1 / ((i + extra) * (i + extra + 1) / 2 + i + 1) + (((i | 0) ^ (extra << 3)) + 0 - -extra) + (i < 3 ? 1 : 2) + i;
    }
    return s;
}
for (let k = 0; k < 50; k++)
    shouldBe(sum(20, k), reference(20, k));
shouldBe(sum(5, 0.5), reference(5, 0.5));
shouldBe(sum(5, NaN), NaN);

// The check that the function makes of its arguments fails as it would have in the function.
let error;
try { sum(3, "1"); } catch (e) { error = e; }
shouldBe(String(error), "TypeError: Type check failed: expected number, got string");
shouldBe(/\bA@/.test(error.stack), true);

// Too few arguments, and more than enough.
function tooFew(n) { let s = 0; for (let i = 0; i < n; i++) { try { s += A(i); } catch { s += 100; } } return s; }
shouldBe(tooFew(3), 300);

// Another function comes to be there.
const original = A;
A = function (i, j) { return 1000; };
shouldBe(sum(2, 1), reference(2, 1) - original(0, 1) - original(1, 1) + 2000);
A = original;
shouldBe(sum(20, 1), reference(20, 1));

// Halfway through.
function swaps(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        if (i === 2)
            first = function () { return -1; };
        s += first(i, 0, 0);
    }
    return s;
}
shouldBe(swaps(5), 0 + 1 - 3);
