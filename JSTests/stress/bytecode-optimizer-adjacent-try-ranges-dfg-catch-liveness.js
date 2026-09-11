//@ runDefault("--useBytecodeOptimizer=1")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--useFTLJIT=0")
// The optimizer deletes the jmp that closes the try body (it targets the next live instruction once the catch
// block has been threaded straight to the loop header), so the catch's try range ends by falling through into
// the range of the for-of's synthesized iterator-close handler. DFG's LiveCatchVariablePreservationPhase has to
// flush the first handler's live locals (the for-of iterator state) when it crosses that boundary, not the
// second handler's.

function firstParsable(kind) {
    for (let name of ["a", "b"]) {
        try {
            return JSON.parse(kind === "k" && name === "b" ? "1" : "{bad"), true;
        } catch { }
    }
    return false;
}
noInline(firstParsable);

function firstParsableInlined(kind) {
    return firstParsable2(kind);
}
function firstParsable2(kind) {
    for (let name of ["a", "b"]) {
        try {
            return JSON.parse(kind === "k" && name === "b" ? "1" : "{bad"), true;
        } catch { }
    }
    return false;
}
noInline(firstParsableInlined);

for (let i = 0; i < 1e5; ++i) {
    let expected = i % 3 === 0;
    let kind = expected ? "k" : "x";
    if (firstParsable(kind) !== expected)
        throw new Error("firstParsable: bad result at " + i);
    if (firstParsableInlined(kind) !== expected)
        throw new Error("firstParsableInlined: bad result at " + i);
}
