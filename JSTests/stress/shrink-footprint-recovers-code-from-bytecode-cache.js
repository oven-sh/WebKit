//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLeanBytecodeCacheDecoder=0")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useThinChildExecutables=0", "--useLazyCachedExpressionInfo=0")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useCodeRecoveryFromBytecodeCache=0")
//@ runBytecodeCache

// VM::shrinkFootprintWhenIdle(KeepCodeThatNeedsParsing) drops linked code and the unlinked code blocks that were decoded
// from a persistent bytecode cache payload; a dropped function is decoded again, from the same record, when next called.
// Functions whose code was generated from source (the run that fills the cache, a non-persistent cache, or
// useCodeRecoveryFromBytecodeCache=0) keep their unlinked code. Either way everything must keep working: closures made
// before the drop, suspended generators and async functions, classes, names, positions in stack traces.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const functionCount = 300;
const functions = [];
for (let i = 0; i < functionCount; ++i) {
    // Distinct source text per function, each with a nested closure and a class or generator now and then.
    let body = `let acc = a + ${i}; const inner = (x) => x * 2 + ${i % 7}; acc = inner(acc);`;
    if (!(i % 5))
        body += ` class K${i} { constructor(v) { this.v = v; } get twice() { return this.v * 2; } } acc += new K${i}(a).twice;`;
    if (!(i % 9))
        body += ` for (let k = 0; k < 3; ++k) acc += k;`;
    body += ` return acc;`;
    functions.push(eval(`(function f${i}(a) { ${body} })`));
}

// These come from the file itself (and so, on the second run, from the cache).
function outer(seed) {
    let state = seed;
    function next() { return ++state; }
    function* walk() { for (let i = 0; i < 4; ++i) yield next() * 10 + i; }
    async function slow(gate) { const before = next(); await gate; return before * 100 + next(); }
    class Box { constructor(v) { this.v = v; } static wrap(v) { return new Box(v); } describe() { return `Box(${this.v}:${state})`; } }
    return { next, walk, slow, Box, thrower() { return new Error("from thrower"); } };
}
// The function in the middle has no closure left by the time of the drop, the innermost one does.
function grandparent() { function parent() { return function child(a) { return "child" + a; }; } return parent(); }
function fileFunction0(a) { return a + 1; }
function fileFunction1(a) { return [a, a].map((x) => x * 3).join(); }
function fileFunction2(a) { try { null.x; } catch (e) { return e instanceof TypeError ? a : -1; } }
function fileFunction3(a) { switch (a) { case 1: return "one"; default: return `n${a}`; } }
const fileFunctions = [fileFunction0, fileFunction1, fileFunction2, fileFunction3];
const expectedFileResults = ["8", "21,21", "7", "n7"];

function exercise() {
    let sum = 0;
    for (let i = 0; i < functionCount; ++i)
        sum += functions[i](i);
    return sum + "|" + fileFunctions.map((f) => String(f(7))).join("|");
}

const kit = outer(5);
const expected = exercise();
// A function that has run a lot by the time of the drop.
for (let i = 0; i < Math.min(testLoopCount, 300); ++i)
    fileFunction3(i);
assert(expected.endsWith("|" + expectedFileResults.join("|")), "file functions before the drop: " + expected);
const walker = kit.walk();
assert(walker.next().value === 60, "generator before the drop");
let openGate;
const slowResult = kit.slow(new Promise((resolve) => { openGate = resolve; }));
const errorBefore = kit.thrower();
const childBefore = grandparent();
assert(childBefore(1) === "child1", "child before the drop");

const before = $vm.codeBlockCensus();
const recovers = jscOptions().useCodeRecoveryFromBytecodeCache && jscOptions().useLeanBytecodeCacheDecoder && jscOptions().useBorrowedBytecodeFromCache && jscOptions().diskCachePayloadIsPersistentForTesting && jscOptions().forceDiskCache;

$vm.shrinkFootprintWhenIdle();
setTimeout(() => {
    try {
        fullGC();
        const after = $vm.codeBlockCensus();
        // (What runs right now, this callback, was linked again already.)
        assert(after.function <= 1 && after.llint + after.baseline + after.dfg + after.ftl <= 1, "linked code is gone: " + JSON.stringify(after));
        if (recovers) {
            assert(after.cachedExecutables > before.cachedExecutables, "executables went back to naming their cache records: " + before.cachedExecutables + " -> " + after.cachedExecutables);
            assert(after.unlinkedFunction < before.unlinkedFunction, "unlinked code blocks of cached functions were dropped: " + before.unlinkedFunction + " -> " + after.unlinkedFunction);
        }
        // Generated code (the eval'd functions here, never cached) keeps its unlinked code in this mode.
        assert(after.unlinkedFunction >= functionCount, "generated functions keep their unlinked code: " + after.unlinkedFunction);

        for (let round = 0; round < 3; ++round)
            assert(exercise() === expected, "results after the drop, round " + round);
        assert(walker.next().value === 81 && walker.next().value === 92, "generator resumed after the drop");
        assert(kit.Box.wrap(3).describe() === "Box(3:9)", "class after the drop: " + kit.Box.wrap(3).describe());
        assert(fileFunction2.name === "fileFunction2" && kit.next.name === "next" && kit.Box.name === "Box", "names");
        assert(fileFunction1.toString().startsWith("function fileFunction1(a)"), "toString");
        const lineOf = (error) => /thrower@[^\n]*:(\d+):/.exec(error.stack)[1];
        assert(lineOf(kit.thrower()) === lineOf(errorBefore), "same position before and after: " + lineOf(errorBefore) + " " + lineOf(kit.thrower()));
        if (recovers) {
            // outer's code was decoded again; the closures it makes now come from the executables the old ones came from.
            const kit2 = outer(50);
            kit.thrower();
            const unlinkedBefore = $vm.codeBlockCensus().unlinkedFunction;
            kit2.thrower();
            assert($vm.codeBlockCensus().unlinkedFunction === unlinkedBefore, "closures from before and after the drop share their unlinked code: " + unlinkedBefore + " -> " + $vm.codeBlockCensus().unlinkedFunction);
            // Also when the executable in between died with its code and comes back as a new one.
            const childAfter = grandparent();
            assert(childBefore(2) === "child2", "child from before the drop");
            const unlinkedWithChild = $vm.codeBlockCensus().unlinkedFunction;
            assert(childAfter(3) === "child3", "child from after the drop");
            assert($vm.codeBlockCensus().unlinkedFunction === unlinkedWithChild, "the two children share their unlinked code: " + unlinkedWithChild + " -> " + $vm.codeBlockCensus().unlinkedFunction);
            // A function that was hot before the drop keeps the code it is given afterwards.
            assert(fileFunction3(7) === "n7", "hot function");
            edenGC();
            fullGC();
            assert(typeof $vm.codeBlockFor(fileFunction3) === "string", "the hot function kept the CodeBlock it got after the drop");
        }
        openGate();
        slowResult.then((value) => {
            assert(value === 710, "async function resumed after the drop: " + value);
            // And once more, now that everything was decoded a second time.
            $vm.shrinkFootprintWhenIdle();
            setTimeout(() => {
                try {
                    fullGC();
                    assert(exercise() === expected, "results after the second drop");
                    assert(walker.next().value === 113, "generator after the second drop");
                } catch (e) {
                    print("FAIL: " + e + "\n" + e.stack);
                    $vm.abort();
                }
            }, 0);
        }).catch((e) => { print("FAIL: " + e + "\n" + e.stack); $vm.abort(); });
    } catch (e) {
        print("FAIL: " + e + "\n" + e.stack);
        $vm.abort();
    }
}, 0);
