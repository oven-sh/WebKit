//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// The value of an integer DataView setter that is a number but not an Int32: `view.setInt16(0, sample * 0x7fff)`.
// The DFG used to check that the value is an Int32 (an Int52 for setUint32). With a double made in the same function
// that check can never pass, and the exit it left (Uncountable, or Int52Overflow for setUint32) was one the parser
// does not read, so every compile made the same code. The value is now converted the way SetViewValue converts it.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, type, value) {
    return new Function(`return function ${name}(view, x) { view.set${type}(0, ${value}); return view.get${type}(0); };`)();
}

// What ToInt8 .. ToUint32 wrap, truncate or turn into 0: first the numbers that are not whole, then all of them.
let fractions = [300.7, 40000.9, -1.5, NaN, Infinity, -Infinity, 0.5, 4294967297.5, -2147483649.5];
let edges = fractions.concat([4000000000, -0, 2147483648, 1e21, 7]);

function test(type, value, x, calls, maxCompiles) {
    let optimized = make("optimized", type, value);
    let reference = make("reference", type, value);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    let view = new DataView(new ArrayBuffer(8));
    for (let i = 0; i < calls; ++i) {
        let actual = optimized(view, x(i));
        let expected = reference(view, x(i));
        if (actual !== expected)
            throw new Error(`set${type}(0, ${value}) stored ${actual} in call ${i}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > maxCompiles)
        throw new Error(`set${type}(0, ${value}) was compiled by the DFG ${compiles} times, expected at most ${maxCompiles}`);
}

for (let type of ["Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32"]) {
    // A double made in the function, which the compiler knows is not an Int32.
    test(type, "x * 0.3", i => i & 31, testLoopCount, 1);
    // A number that arrives in a variable and is never a whole number, whichever calls the value profile samples.
    test(type, "x", i => fractions[i % fractions.length], testLoopCount, 1);
    // With whole numbers among them the first compile can take the value for an integer, and leave once.
    test(type, "x", i => edges[i % edges.length], testLoopCount / 2, 3);
}

// A value that the function loads: an Int32 while the function warms up, another number after that. The check of the
// value exits with BadType, and the next compile has a call.
let holder = { v: 0 };
function loaded(late) {
    return i => {
        holder.v = i < testLoopCount ? i & 31 : late;
        return holder;
    };
}
test("Int16", "x.v", loaded(300.7), testLoopCount * 2, 2);
test("Uint32", "x.v", loaded(4000000000), testLoopCount * 2, 2);

// A Uint32 copied from another view: below 2^31 while the function warms up, above after that. getUint32() has an
// Int52 result whatever it is predicted to be, and setUint32() stores that without a check.
let source = new DataView(new ArrayBuffer(8));
test("Uint32", "x.getUint32(4)", i => {
    source.setUint32(4, (i < testLoopCount ? 0x01020304 : 0xf1020304) + (i & 31));
    return source;
}, testLoopCount * 2, 1);
