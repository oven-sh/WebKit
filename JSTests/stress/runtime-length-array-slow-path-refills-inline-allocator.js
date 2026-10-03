//@ skip if not $jitTests
//@ runDefault("--useConcurrentJIT=false")
//@ runDefault("--useConcurrentJIT=false", "--useFTLJIT=false")
//@ runDefault("--useConcurrentJIT=false", "--forceGCSlowPaths=true")
//@ runDefault("--useConcurrentJIT=false", "--useFTLJIT=false", "--forceGCSlowPaths=true")

// DFG and FTL allocate the storage of an array inline. For a length that is known only at run time, the inline path
// asks for room for exactly that many elements. When it finds no free cell of that size class, it calls a slow path.
// The slow path must take the storage from the same size class: that is what refills the free list that the inline path
// reads. If it takes a larger cell (the capacity that the runtime gives a short array: 5 elements for length 0, 3 for
// length 1), that free list stays empty and each later allocation calls the slow path again.
//
// Arrays that one function makes one after the other have their storage in adjacent cells. So the most frequent
// distance between two consecutive storages is the cell size of the size class that they come from. The test reads
// that distance for the arrays that DFG code made and for the arrays that FTL code made. --forceGCSlowPaths=true sends
// each allocation to the slow path, so those modes read the size class of the slow path alone.

const interpreterOrBaseline = 0;
const dfg = 1;
const ftl = 2;
const tierNames = ["the interpreter or the Baseline JIT", "DFG", "FTL"];
const topTier = $vm.useFTLJIT() ? ftl : dfg;

// The function under test sets this to the tier that ran it.
let tier = interpreterOrBaseline;

// The loop of a test ends when the top tier made this many arrays.
const wantedSamples = 1000;
// A tier below the top one is checked when it made at least this many.
const minimumSamples = 200;
const maximumCalls = 100 * testLoopCount;

// A default constructor passes its arguments on with a spread, and DFG calls the Array constructor for that.
class DerivedArray extends Array {
    constructor(length)
    {
        super(length);
    }
}
function identity(value) { return value; }

function mostFrequent(counts) {
    let result;
    let best = 0;
    for (const [value, count] of counts) {
        if (count > best) {
            best = count;
            result = value;
        }
    }
    return result;
}

// Each test has a function of its own, so that each one goes through DFG and then FTL. The name in its source keeps
// two tests of one expression from sharing code.
function test(name, parameters, expression, args, expectedDistance) {
    const make = new Function(parameters, "/* " + name + " */ tier = $vm.ftlTrue() ? ftl : ($vm.dfgTrue() ? dfg : interpreterOrBaseline); return " + expression + ";");
    noInline(make);

    const distances = [null, new Map, new Map];
    const samples = [0, 0, 0];
    // Every array stays alive, so that no cell is used again while the test measures.
    const arrays = [make(...args)];
    for (let i = 0; i < maximumCalls && samples[topTier] < wantedSamples; ++i) {
        const array = make(...args);
        if (tier !== interpreterOrBaseline) {
            const distance = $vm.deltaBetweenButterflies(array, arrays[arrays.length - 1]);
            distances[tier].set(distance, (distances[tier].get(distance) || 0) + 1);
            samples[tier]++;
        }
        arrays.push(array);
    }

    if (samples[topTier] < wantedSamples)
        throw new Error(name + ": only " + samples[topTier] + " arrays came from " + tierNames[topTier]);
    for (let measured = dfg; measured <= topTier; ++measured) {
        if (samples[measured] < minimumSamples)
            continue;
        const distance = mostFrequent(distances[measured]);
        if (distance !== expectedDistance)
            throw new Error(name + " in " + tierNames[measured] + ": expected " + expectedDistance + " but got " + distance);
    }
}

if ($vm.useDFGJIT()) {
    // Not literals: a literal shares its storage until a write, and an empty literal has no element type yet.
    const noElements = [1, 2];
    noElements.length = 0;
    const oneElement = [1, 2];
    oneElement.length = 1;
    const twoElements = [1, 2, 3];
    twoElements.length = 2;

    // A length of 0 or 1 that is known only at run time: 8 bytes of header and at most one element of 8 bytes.
    test("new Array(0)", "length", "new Array(length)", [0], 16);
    test("new Array(1)", "length", "new Array(length)", [1], 16);
    test("Array(1)", "length", "Array(length)", [1], 16);
    test("new DerivedArray(0)", "length", "new DerivedArray(length)", [0], 16);
    test("rest parameter, no arguments", "...values", "values", [], 16);
    test("rest parameter, one argument", "...values", "values", [1], 16);
    test("rest parameter after a named one, one argument", "first, ...values", "values", [1, 2], 16);
    test("slice of no elements", "array", "array.slice()", [noElements], 16);
    test("slice of one element", "array", "array.slice()", [oneElement], 16);
    test("map of one element", "array", "array.map(identity)", [oneElement], 16);
    test("two spreads, one element", "first, second", "[...first, ...second]", [oneElement, noElements], 16);

    // A run-time length of 2: the inline path and the runtime agree on 32 bytes.
    test("new Array(2)", "length", "new Array(length)", [2], 32);
    test("rest parameter, two arguments", "...values", "values", [1, 2], 32);
    test("slice of two elements", "array", "array.slice()", [twoElements], 32);

    // A length that the compiler knows: the inline path gives the capacity of the runtime, and so does its slow path.
    test("[value]", "value", "[value]", [1], 32);
    test("[]", "", "[]", [], 48);
}
