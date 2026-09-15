//@ runBytecodeCache
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")

// An Error's stack is turned into a string by the collector (ErrorInstance::computeErrorInfo) once the code it points at
// is about to be collected. With the functions decoded from the bytecode cache their names may not have been read out of
// it yet; the collector must still be able to name them without atomizing anything.

function makeErrors() {
    function namedInnerFunction() { return new Error("boom"); }
    function middleFunction() { return namedInnerFunction(); }
    let errors = [];
    for (let i = 0; i < 50; ++i)
        errors.push(middleFunction());
    return errors;
}

let errors = makeErrors();
makeErrors = null;
fullGC();
fullGC();
edenGC();
fullGC();

for (let error of errors) {
    let stack = error.stack;
    if (!stack.includes("namedInnerFunction") || !stack.includes("middleFunction"))
        throw new Error("bad stack: " + stack);
}
