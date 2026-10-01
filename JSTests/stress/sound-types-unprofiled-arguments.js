//@ requireOptions("--useSoundTypes=1", "--ignoreArgumentProfilesForTesting=1")

// With no argument profiles at all, the only thing the DFG knows about an argument is what its sound type checks let through.

function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error(`bad value: ${String(actual)}, expected ${String(expected)}`);
}

function shouldFail(func, ...args) {
    let threw = false;
    try {
        func(...args);
    } catch (e) {
        threw = e instanceof TypeError && e.message.startsWith("Type check failed: ");
    }
    shouldBe(threw, true);
}

function add(a, b) { $$t(a, 8); $$t(b, 8); return a + b; }
function concat(a, b) { $$t(a, 16); $$t(b, 16); return a + b; }
function addOptional(a, b) { $$t(a, 8); $$t(b, 9); return b === undefined ? a : a + b; }
function numberOrString(a) { $$t(a, 24); return a + a; }
function length(a) { $$t(a, 256); return a.length; }
function field(o) { $$t(o, 512); return o.x; }
function call(f, x) { $$t(f, 128); $$t(x, 8); return f(x); }
function not(b) { $$t(b, 4); return !b; }
function bigint(a) { $$t(a, 64); return a * 2n; }
function unchecked(a, b) { return a + b; }
// The check is not in the root block, so it only seeds the prediction after propagation has found nothing.
function late(a, flag) {
    if (flag)
        return 0;
    $$t(a, 8);
    return a * 2;
}
function inLoop(a, n) {
    let sum = 0;
    for (let i = 0; i < n; ++i) {
        $$t(a, 8);
        sum += a;
    }
    return sum;
}
function inlinee(a) { $$t(a, 8); return a + 1; }
function inliner(a) { return inlinee(a) + inlinee(a); }

for (let f of [add, concat, addOptional, numberOrString, length, field, call, not, bigint, unchecked, late, inLoop, inliner])
    noInline(f);

let double = x => x * 2;
for (let round = 0; round < 2; ++round) {
    for (let i = 0; i < testLoopCount; ++i) {
        shouldBe(add(i, 1), i + 1);
        shouldBe(add(i, 0.5), i + 0.5);
        shouldBe(concat("a", "b"), "ab");
        shouldBe(addOptional(i), i);
        shouldBe(addOptional(i, 2), i + 2);
        shouldBe(numberOrString(i), i * 2);
        shouldBe(numberOrString("a"), "aa");
        shouldBe(length([i]), 1);
        shouldBe(field({ x: i }), i);
        shouldBe(call(double, i), i * 2);
        shouldBe(not(true), false);
        shouldBe(bigint(2n), 4n);
        shouldBe(unchecked(i, 1), i + 1);
        shouldBe(late(i, false), i * 2);
        shouldBe(late("s", true), 0);
        shouldBe(inLoop(2, 3), 6);
        shouldBe(inliner(i), i * 2 + 2);
    }
    shouldFail(add, "s", 1);
    shouldFail(add, 1, undefined);
    shouldFail(concat, "a", 1);
    shouldFail(addOptional, 1, null);
    shouldFail(numberOrString, true);
    shouldFail(length, "s");
    shouldFail(field, [1]);
    shouldFail(call, { }, 1);
    shouldFail(not, 0);
    shouldFail(bigint, 1);
    shouldFail(late, "s", false);
    shouldFail(inLoop, "s", 1);
    shouldBe(inLoop("s", 0), 0);
    shouldFail(inliner, "s");
}
