//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// A DataView getter or setter with a byte offset that is a double made in the same function: `view.getUint8(x / 4)`.
// The DFG node takes the offset as an Int32, and a double that was never a JSValue cannot pass a check for one: the
// compiled code left on every call, in every compile. FixupPhase now truncates the offset in place, as ToIndex does.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`return function ${name}(view, x) { ${body} };`)();
}

function test(body) {
    let optimized = make("optimized", body);
    let reference = make("reference", body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    let view = new DataView(new ArrayBuffer(32));
    for (let i = 0; i < 32; ++i)
        view.setUint8(i, i * 17 + 3);

    for (let i = 0; i < testLoopCount * 2; ++i) {
        let actual = optimized(view, i & 31);
        let expected = reference(view, i & 31);
        if (!Object.is(actual, expected))
            throw new Error(`${body} with ${i & 31} gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > 1)
        throw new Error(`${body} was compiled by the DFG ${compiles} times, expected 1`);
}

[
    "return view.getUint8(x / 4);",
    "return view.getInt16(x * 0.3);",
    "return view.getUint32(x / 8, true);",
    "return view.getFloat64(x / 4);",
    "view.setUint8(x / 4, x); return view.getUint8(x >> 2);",
    "view.setFloat32(x * 0.3, 1.5); return view.getFloat32(x * 0.3);",
    "view.setInt16(x / 4, x * 0.3); return view.getInt16(x / 4);",
].forEach(test);
