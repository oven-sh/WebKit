//@ runBytecodeCache("--useUnlinkedCodeBlockJettisoning=1", "--forceCodeBlockToJettisonDueToOldAge=1", "--countParseTimes=1")
//@ runBytecodeCache("--useJIT=0", "--forceCodeBlockToJettisonDueToOldAge=1", "--countParseTimes=1")

// Two loads of one source share its unlinked code, and only the first load writes a bytecode cache. A function whose
// code is generated through the second load is not registered with that cache, and neither are the functions nested in
// it. One of those can still be called through a closure of the first load, so the cache is asked about an executable
// it never registered. The executable is at an address where an executable of the first load died: the cache had the
// record of that one under the address, and the second run ran a `nestedInP` where a `nestedInQ` was called.

function check(actual, expected)
{
    if (actual !== expected)
        throw new Error("expected " + expected + " but got " + actual);
}

function dropUnlinkedCode()
{
    // The collector drops the unlinked code of a function that has not run for a number of collections
    // (UnlinkedCodeBlock::maxAge). In the second run the code is from the cache, and stays.
    let unlinkedFunctions = () => $vm.codeBlockCensus().unlinkedFunction;
    let before = unlinkedFunctions();
    for (let i = 0; i < 20 && unlinkedFunctions() * 2 > before; ++i)
        fullGC();
    if (!jscOptions().forceDiskCache && unlinkedFunctions() * 2 > before)
        throw new Error("the collector kept the unlinked code: " + unlinkedFunctions() + " of " + before + " code blocks");
}

// More functions than one block of executables holds, so that addresses are used again whatever order the allocator
// hands them out in.
const count = 200;
let source = "";
for (let i = 0; i < count; ++i) {
    source += `function Q${i}() { return function nestedInQ() { return "nested in Q${i}"; }; }\n`;
    source += `function P${i}() { return function nestedInP() { return "nested in P${i}"; }; }\n`;
}

function load()
{
    let parsedBefore = $vm.parseCount();
    loadString(source);
    let functions = { Q: [], P: [], parsed: $vm.parseCount() - parsedBefore };
    for (let i = 0; i < count; ++i) {
        functions.Q.push(globalThis["Q" + i]);
        functions.P.push(globalThis["P" + i]);
    }
    return functions;
}

let first = load();
for (let i = 0; i < count; ++i)
    check(first.Q[i]()(), "nested in Q" + i);

dropUnlinkedCode();

let second = load();
// The second load has to be the unlinked code of the first one, which only the first one's cache knows.
if (!jscOptions().forceDiskCache && (!first.parsed || second.parsed))
    throw new Error("the loads parsed the source " + first.parsed + " and " + second.parsed + " times");

for (let i = 0; i < count; ++i)
    second.P[i]();
for (let i = 0; i < count; ++i)
    check(first.P[i]()(), "nested in P" + i);
