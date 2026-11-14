//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ requireOptions("--forceUnlinkedDFG=0")
//@ defaultNoEagerRun

// A truncated multiply of an int32 by a numeric string is an Int32 ArithMul in the DFG. Where the product leaves int32 and
// neither operand is known to be below 2^22, the product is not exact, so the multiply keeps its overflow check. Once that
// check has failed, the next compile has to give up on int32. It used to make the same choice again, every time.

function shouldBe(actual, expected, name)
{
    if (actual !== expected)
        throw new Error(`${name}: expected ${expected} but got ${actual}`);
}

const shapes = [
    ["(a * s) | 0", "(a * n) | 0"],
    ["(s * a) | 0", "(n * a) | 0"],
    ["(a * s) >>> 0", "(a * n) >>> 0"],
    ["~~(a * s)", "~~(a * n)"],
    ["(a * s) & 0xffff", "(a * n) & 0xffff"],
    ["a * s", "a * n"],
    ["((a & 1023) * s) | 0", "((a & 1023) * n) | 0"],
    ["(s * 3) | 0", "(n * 3) | 0"],
];

// [a, s]
const overflowing = [[2147483632, "70001"], [2147483647, "2147483647"], [-2147483648, "3"], [5, "2147483647"], [-70001, "-2147483632"]];
const small = [[1000, "7"], [-3, "12"], [0, "5"], [46340, "46340"]];

let id = 0;
function make(body, parameters)
{
    // The comment gives every function a source, and so a profile, of its own.
    const f = new Function(...parameters, `/* ${id++} */ return ${body};`);
    noInline(f);
    return f;
}

function run(name, f, reference, inputs, iterations)
{
    const expected = inputs.map(([a, s]) => reference(a, Number(s)));
    for (let i = 0; i < iterations; ++i) {
        const index = i % inputs.length;
        const input = inputs[index];
        const actual = f(input[0], input[1]);
        if (actual !== expected[index])
            shouldBe(actual, expected[index], `${name} with ${input[0]} and "${input[1]}"`);
    }
}

function shouldHaveSettled(name, f, limit)
{
    const compiles = numberOfDFGCompiles(f);
    if (compiles > limit)
        throw new Error(`${name}: ${compiles} optimizing compiles, at most ${limit} expected`);
}

for (const [body, referenceBody] of shapes) {
    // The same multiply of two numbers, called too few times to leave the interpreter.
    const reference = make(referenceBody, ["a", "n"]);

    // Overflows from the start: the profile of the baseline code has seen it.
    let f = make(body, ["a", "s"]);
    run(body, f, reference, overflowing, testLoopCount * 3);
    shouldHaveSettled(`${body}, overflowing from the start`, f, 2);

    // Never overflows.
    f = make(body, ["a", "s"]);
    run(body, f, reference, small, testLoopCount * 3);
    shouldHaveSettled(`${body}, never overflowing`, f, 2);

    // Overflows once it is optimized: only the exit tells.
    f = make(body, ["a", "s"]);
    run(body, f, reference, small, testLoopCount);
    run(body, f, reference, overflowing, testLoopCount * 3);
    shouldHaveSettled(`${body}, overflowing once it is optimized`, f, 3);
}

// What the interpreter answers. The second product is rounded: a multiply that wraps would answer 1.
const f = make("(a * s) | 0", ["a", "s"]);
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(f(2147483632, "70001"), 2146363632, "(a * s) | 0");
    shouldBe(f(2147483647, "2147483647"), 0, "(a * s) | 0");
    shouldBe(f(-2147483648, "3"), -2147483648, "(a * s) | 0");
}
