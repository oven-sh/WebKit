//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ requireOptions("--forceUnlinkedDFG=0")
//@ defaultNoEagerRun

load("./resources/overloaded-operators.js", "caller relative");

// Code that has only ever had objects that overload operators for operands is compiled for that, and not for what
// the operators give otherwise, to find out that it does not hold and be compiled again.
const functions = [
    (a, b) => a < b ? 1 : 2,
    (a, b) => a * b + a - b,
    (a, b) => a / b % b,
    (a, b) => a ** b * a,
    (a, b) => (a & b) | a,
    (a, b) => (a << b) >> b,
    (a, b) => (a - b).v,
    a => -a - a,
    a => ~a + a,
    a => { a++; return a + a; },
    a => { --a; return a - a; },
    a => { const old = a++; return old + a; },
];

const a = make(6);
const b = make(3);
for (const f of functions) {
    noInline(f);
    for (let i = 0; i < testLoopCount; i++)
        f(a, b);
    if (numberOfDFGCompiles(f) > 1)
        throw new Error(`${f} was compiled ${numberOfDFGCompiles(f)} times`);
}

// Two objects are equal if they are the same one, for all but these, and that is what == is compiled for the first time, with a check. It is
// these that pay for finding out, once, and not every other kind of object for ever.
for (const f of [(a, b) => a == b, (a, b) => a != b, (a, b) => a == b ? 1 : 2]) {
    noInline(f);
    for (let i = 0; i < testLoopCount; i++)
        f(a, b);
    if (numberOfDFGCompiles(f) > 2)
        throw new Error(`${f} was compiled ${numberOfDFGCompiles(f)} times`);
}
