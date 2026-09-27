//@ runBytecodeCache("--useUnlinkedCodeBlockJettisoning=1", "--forceCodeBlockToJettisonDueToOldAge=1", "--countParseTimes=1")
//@ runBytecodeCache("--useJIT=0", "--forceCodeBlockToJettisonDueToOldAge=1", "--countParseTimes=1")

// As bytecode-cache-leaf-lookup-after-jettison.js, with one group of functions. Their code is generated for the load
// that writes the cache, dropped, and generated again for the other load, in the opposite order: the executable of the
// function nested in one of them is where that of the function nested in another one was. The two functions differ in
// nothing but where they are in the source.

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
for (let i = 0; i < count; ++i)
    source += `function P${String(i).padStart(3, "0")}() { return function nestedInP() { return "nested in P${String(i).padStart(3, "0")}"; }; }\n`;

function load()
{
    let parsedBefore = $vm.parseCount();
    loadString(source);
    let functions = [];
    functions.parsed = $vm.parseCount() - parsedBefore;
    for (let i = 0; i < count; ++i)
        functions.push(globalThis["P" + String(i).padStart(3, "0")]);
    return functions;
}

let first = load();
for (let i = 0; i < count; ++i)
    first[i]();

dropUnlinkedCode();

let second = load();
// The second load has to be the unlinked code of the first one, which only the first one's cache knows.
if (!jscOptions().forceDiskCache && (!first.parsed || second.parsed))
    throw new Error("the loads parsed the source " + first.parsed + " and " + second.parsed + " times");

for (let i = count; i--;)
    second[i]();
for (let i = 0; i < count; ++i)
    check(first[i]()(), "nested in P" + String(i).padStart(3, "0"));
