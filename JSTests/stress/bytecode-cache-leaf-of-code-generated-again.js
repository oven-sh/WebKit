//@ runBytecodeCache("--useUnlinkedCodeBlockJettisoning=1", "--forceCodeBlockToJettisonDueToOldAge=1", "--countParseTimes=1")
//@ runBytecodeCache("--useJIT=0", "--forceCodeBlockToJettisonDueToOldAge=1", "--countParseTimes=1")

// With these options the collector drops unlinked code that has not run lately. A function that runs again after that
// has its code generated again, and the run that fills the bytecode cache appends that code to the cache as it did the
// first time. The functions nested in it are new executables, at the addresses of the ones that died with the old code.
// When one of them runs, its code belongs in the record that the new code of its parent has for it: the record under
// the old code is no longer reached, and the second run would have to generate the nested function from its source.

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
    source += `function outer${i}() { return function () { return "inner ${i}"; }; }\n`;
loadString(source);

function callAll()
{
    for (let i = 0; i < count; ++i)
        check(globalThis["outer" + i]()(), "inner " + i);
}

let parsedBefore = $vm.parseCount();
callAll();
dropUnlinkedCode();
callAll();
let parsed = $vm.parseCount() - parsedBefore;

// The second run has the code of every function that ran in the first one from the cache.
if (jscOptions().forceDiskCache && parsed)
    throw new Error("the second run generated " + parsed + " functions from their source");
