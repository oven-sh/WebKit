//@ runBytecodeCache
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1")
//@ runBytecodeCache("--diskCachePayloadIsPersistentForTesting=1", "--useLazyCachedExpressionInfo=0")

// UnlinkedCodeBlock::expressionInfoIfDecoded() lets a caller that cannot take a lock or allocate (a sampling hook that
// runs inside malloc) know whether asking a code block for a source position would decode it from the cache payload.
// It must not decode anything itself, and it is non-null from the first position anyone asks for on.

function quiet(a, b) { return a + b; }
function asked(i) { return new Error("e" + i); }

quiet(1, 2);
asked(0);

// Only code decoded from a persistent cache payload, with the lazy decode on, starts out undecoded: that is the second
// run of the second configuration above.
let options = jscOptions();
let lazy = !!(options.forceDiskCache && options.diskCachePayloadIsPersistentForTesting && options.useLazyCachedExpressionInfo && options.useBorrowedBytecodeFromCache);

function expectDecoded(fn, expected, when) {
    let actual = $vm.hasDecodedExpressionInfo(fn);
    if (actual !== expected)
        throw new Error(fn.name + " " + when + ": expected " + expected + ", got " + actual + " (lazy: " + lazy + ")");
}

expectDecoded(quiet, !lazy, "after it ran");
expectDecoded(quiet, !lazy, "asked twice"); // asking does not decode
expectDecoded(asked, !lazy, "before its position was resolved");

// A position of `asked` is resolved when the stack is read. From then on its expression info is decoded.
let line = /asked@.*:(\d+):\d+/.exec(asked(1).stack);
if (!line || Number(line[1]) !== 10)
    throw new Error("unexpected stack: " + asked(1).stack);
expectDecoded(asked, true, "after its position was resolved");

// Resolving a position in one function does not decode another.
expectDecoded(quiet, !lazy, "after another function's position was resolved");
