//@ runBytecodeCache("--useUnlinkedCodeBlockJettisoning=1", "--forceCodeBlockToJettisonDueToOldAge=1", "--countParseTimes=1")
//@ runBytecodeCache("--useJIT=0", "--forceCodeBlockToJettisonDueToOldAge=1", "--countParseTimes=1")

// As bytecode-cache-leaf-lookup-after-jettison.js, but the functions of the first group run twice before the cache is
// asked about an executable it never registered. The second time their code is generated again, so what the cache has
// under the addresses of the functions nested in them are records under that new code, the ones the second run reads.

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
