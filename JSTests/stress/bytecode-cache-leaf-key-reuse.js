//@ runBytecodeCache("--useUnlinkedCodeBlockJettisoning=1", "--forceCodeBlockToJettisonDueToOldAge=1")
//@ runBytecodeCache("--useJIT=0", "--forceCodeBlockToJettisonDueToOldAge=1")

// The run that fills the bytecode cache appends a function's code to the cache when the function is first generated,
// and points the function's record at it. It finds the record by the address of the function's
// UnlinkedFunctionExecutable. With these options the collector drops unlinked code that has not run lately, and the
// executables of the functions nested in it die with it. The executables allocated next take their addresses. What was
// registered under such an address is the record of another function: the second run, which takes its code from the
// cache, ran the inner function of a `second` where the inner function of a `first` was called.

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
    source += `function first${i}() { return function () { return "first ${i}"; }; }\n`;
    source += `function second${i}() { return function () { return "second ${i}"; }; }\n`;
}
loadString(source);

for (let i = 0; i < count; ++i)
    check(globalThis["first" + i]()(), "first " + i);

dropUnlinkedCode();

for (let i = 0; i < count; ++i)
    check(globalThis["second" + i]()(), "second " + i);
