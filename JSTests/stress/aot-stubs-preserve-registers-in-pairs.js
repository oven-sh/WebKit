//@ skip if $architecture != "arm64"
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSoundTypes=1", "--aotMapFilePath=aot-stubs-preserve-registers-in-pairs.map")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function storesWhileDoublesAreLive(object, value, a, b, c, d, e, f, g, h) {
    const p = a * 1.5, q = b * 2.5, r = c * 3.5, s = d * 4.5, t = e * 5.5, u = f * 6.5, v = g * 7.5, w = h * 8.5;
    object.field = value;
    return p + q * 2 + r * 4 + s * 8 + t * 16 + u * 32 + v * 64 + w * 128;
}
function checksWhileDoublesAreLive(callable, a, b, c, d, e, f, g, h) {
    const p = a * 1.5, q = b * 2.5, r = c * 3.5, s = d * 4.5, t = e * 5.5, u = f * 6.5, v = g * 7.5, w = h * 8.5;
    $$t(callable, 128);
    return p + q * 2 + r * 4 + s * 8 + t * 16 + u * 32 + v * 64 + w * 128;
}
const expected = 1.5 + 5 * 2 + 10.5 * 4 + 18 * 8 + 27.5 * 16 + 39 * 32 + 52.5 * 64 + 68 * 128;
const old = { field: null };
const callables = [check, class { }, new Proxy(check, { }), check.bind(null), Math.max];
for (let i = 0; i < 40; ++i) {
    fullGC();
    check(storesWhileDoublesAreLive(old, { i }, 1, 2, 3, 4, 5, 6, 7, 8), expected, "doubles across the slow path of a write barrier");
    check(old.field.i, i, "the store");
    check(checksWhileDoublesAreLive(callables[i % callables.length], 1, 2, 3, 4, 5, 6, 7, 8), expected, "doubles across a type check");
}

if (isAOTCompiled(storesWhileDoublesAreLive)) {
    const stubs = [];
    for (const line of readFile("aot-stubs-preserve-registers-in-pairs.map").split("\n")) {
        const fields = line.split("\t");
        if (fields[0] === "T")
            stubs.push({ offset: Number(fields[1]), name: fields[2] });
    }
    function numberOfInstructionsIn(name) {
        const stub = stubs.find(stub => stub.name === name);
        check(!!stub, true, "the map has " + name);
        let end = Infinity;
        for (const other of stubs) {
            if (other.offset > stub.offset && other.offset < end)
                end = other.offset;
        }
        return (end - stub.offset) / 4;
    }
    const numberOfGPRs = 18, numberOfFPRs = 24;
    check(numberOfInstructionsIn("WriteBarrier") < 2 * (numberOfGPRs + numberOfFPRs), true, "fewer instructions than one store and one load for each register");
    check(numberOfInstructionsIn("ColdOperationVoid") < 16 + 2 * numberOfFPRs, true, "fewer instructions than pairs of integer registers and single floating-point registers");
    check(numberOfInstructionsIn("ColdOperationValue") < 16 + 2 * numberOfFPRs, true, "fewer instructions than pairs of integer registers and single floating-point registers");
}
