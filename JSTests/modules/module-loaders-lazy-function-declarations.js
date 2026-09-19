import { shouldBe } from "./resources/assert.js";

// Records for one module in different loaders share the module's code and its function declarations' executables,
// while each has its own environment. With useLazyModuleFunctionDeclarations a declaration's slot in that environment
// stays empty until the binding is first read, so code that one loader has warmed up (and optimized) meets empty slots
// when the next loader runs it: each loader fills its own slots, with functions made from the shared executables.
const options = jscOptions();
const lazy = options.useLazyModuleFunctionDeclarations && !options.useTypeProfiler && !options.useControlFlowProfiler;
const n = testLoopCount;
const load = () => $vm.moduleLoaderImport($vm.createModuleLoader(), "./module-loaders-lazy-function-declarations/main.js");
const own = 16, imported = 8;
const expected = (i, x) => i < own ? x * 10 + i : x * 100 + (i - own);
// (Not counting the two exports the test itself reads to get going.)
const uninstantiated = (ns) => (ns.driver, ns.callCounts, $vm.uninstantiatedFunctionDeclarations(ns));

// The first loader only ever reaches half of the functions.
const a = await load();
const initially = uninstantiated(a);
if (lazy)
    shouldBe(initially >= own, true);
else
    shouldBe(initially, 0);
const warm = (ns, rounds) => {
    let sum = 0;
    for (let r = 0; r < rounds; ++r) {
        for (let i = 0; i < own / 2; ++i)
            sum += ns.driver(i, r);
        for (let i = own; i < own + imported / 2; ++i)
            sum += ns.driver(i, r);
    }
    return sum;
};
const warmSum = warm(a, n);
shouldBe(JSON.stringify(a.callCounts()), `[${n * own / 2},${n * imported / 2}]`);
if (lazy)
    shouldBe(uninstantiated(a), initially - own / 2);

// The second one has that code from the start, and nothing instantiated.
const b = await load();
shouldBe(typeof $vm.codeBlockFor(b.driver), "string");
if (lazy)
    shouldBe(uninstantiated(b), initially);
for (let i = 0; i < own + imported; ++i)
    shouldBe(b.driver(i, 7), expected(i, 7));
shouldBe(JSON.stringify(b.callCounts()), `[${own},${imported}]`);
shouldBe(JSON.stringify(a.callCounts()), `[${n * own / 2},${n * imported / 2}]`);
if (lazy) {
    shouldBe(uninstantiated(b), initially - own);
    shouldBe(uninstantiated(a), initially - own / 2);
}

// One executable per declaration, one function per loader: b's f0 has the code a's f0 was given, b's f15, which a
// has not read to this day, is what a gets its code from.
shouldBe(a.f0 === b.f0, false);
shouldBe(typeof $vm.codeBlockFor(a.f0), "string");
shouldBe($vm.codeBlockFor(a.f0) === $vm.codeBlockFor(b.f0), true);
shouldBe(typeof $vm.codeBlockFor(b.f15), "string");
shouldBe($vm.codeBlockFor(a.f15) === $vm.codeBlockFor(b.f15), true);
shouldBe(a.f15(1), 25);
shouldBe(JSON.stringify(b.callCounts()), `[${own},${imported}]`);

// Now that two environments exist, optimized code stops assuming one. Once that has settled, a third loader runs it
// as is: where the code reads a declaration's slot it finds it empty and instantiates the function there and then.
shouldBe(warm(a, n), warmSum);
shouldBe(warm(b, n), warmSum);
const compiles = numberOfDFGCompiles(a.driver);
const c = await load();
if (lazy)
    shouldBe(uninstantiated(c), initially);
shouldBe(warm(c, 3), warm(a, 3));
if (!options.useConcurrentJIT)
    shouldBe(numberOfDFGCompiles(a.driver), compiles);
shouldBe(JSON.stringify(c.callCounts()), `[${3 * own / 2},${3 * imported / 2}]`);
if (lazy)
    shouldBe(uninstantiated(c), initially - own / 2);
// The other half, which no code has profiled a read of.
for (let i = 0; i < own + imported; ++i)
    shouldBe(c.driver(i, 3), expected(i, 3));
shouldBe(JSON.stringify(c.callCounts()), `[${3 * own / 2 + own},${3 * imported / 2 + imported}]`);
shouldBe(c.f0 === a.f0 || c.f0 === b.f0, false);
shouldBe($vm.codeBlockFor(c.f0) === $vm.codeBlockFor(a.f0), true);
fullGC();
shouldBe(warm(c, 2), warm(b, 2));
