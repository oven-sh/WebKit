//@ runDefault("--useBytecodeOptimizer=1")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--useFTLJIT=0")
//@ runDefault("--useBytecodeOptimizer=1", "--useConcurrentJIT=0", "--useFTLJIT=0", "--maximumFunctionForCallInlineCandidateBytecodeCostForDFG=1000")
//@ runDefault("--useConcurrentJIT=0", "--useFTLJIT=0", "--maximumFunctionForCallInlineCandidateBytecodeCostForDFG=1000")
// Once an empty catch block has been threaded straight to the loop header, the jmp that closes the try body targets
// the next live instruction and the optimizer deletes it. The catch's try range then ends by falling through into
// whatever range starts there (the for-of's synthesized iterator-close handler, or a try in the code a `return` /
// labelled `break` lands on). DFG's LiveCatchVariablePreservationPhase has to flush the live locals of the handler
// it is leaving at that boundary, not those of the one it is entering: otherwise locals that only the loop uses
// come back as undefined after the first exception the catch swallows.

let sink = 0;
function touch() { sink++; }

function forOfResultDiscarded(kind) {
    for (let name of ["a", "b"]) {
        try {
            return JSON.parse(kind === "k" && name === "b" ? "1" : "{bad"), true;
        } catch { }
    }
    return false;
}

function forOfResultUsed(kind) {
    for (let name of ["a", "b"]) {
        try {
            return JSON.parse(kind === "k" && name === "b" ? "1" : "{bad");
        } catch { }
    }
    return false;
}

function whileInsideFinally(kind) {
    let n = 0, limit = { value: 3 }, log = [];
    try {
        while (true) {
            n++;
            log.push(n);
            if (n > limit.value) {
                sink += log.length;
                break;
            }
            try {
                return JSON.parse(kind === "k" && n >= 2 ? "1" : "{bad"), true;
            } catch { }
        }
    } finally {
        try {
            touch();
        } catch { }
    }
    return false;
}

function labelledBreakIntoTry(kind) {
    let result = false, n = 0, limit = { value: 3 }, log = [];
    done: {
        for (;;) {
            n++;
            log.push(n);
            if (n > limit.value) {
                sink += log.length;
                break;
            }
            try {
                JSON.parse(kind === "k" && n >= 2 ? "1" : "{bad");
                result = true;
                break done;
            } catch { }
        }
    }
    try {
        touch();
    } catch { }
    return result;
}

// Nothing throws when the local is lost here: it silently becomes NaN.
let observed = 0;
function loopOnlyNumber(kind) {
    let result = false, n = 0, doubled = 1;
    done: {
        for (;;) {
            n++;
            doubled = doubled * 2;
            observed = doubled;
            if (n > 3)
                break;
            try {
                JSON.parse(kind === "k" && n >= 2 ? "1" : "{bad");
                result = true;
                break done;
            } catch { }
        }
    }
    try {
        touch();
    } catch { }
    return result + ":" + n + ":" + observed;
}

function throwIf(flag) {
    if (flag)
        throw new Error("thrown");
}
noInline(throwIf);

// The inlined recursive call reaches the same HandlerInfo through a second inline call frame, within one block.
function recursive(depth, flag) {
    let saved = 0;
    try {
        saved = depth * 3 + 1;
        if (depth > 0)
            sink += recursive(depth - 1, flag);
        throwIf(flag);
        return -1;
    } catch {
        return saved;
    }
}

function callThrough(f, kind) {
    return f(kind);
}

const cases = [
    [forOfResultDiscarded, "true", "false"],
    [forOfResultUsed, "1", "false"],
    [whileInsideFinally, "true", "false"],
    [labelledBreakIntoTry, "true", "false"],
    [loopOnlyNumber, "true:2:4", "false:4:16"],
];
for (let [f] of cases)
    noInline(f);
noInline(recursive);
noInline(callThrough);

for (let i = 0; i < 5e4; ++i) {
    let hit = i % 3 === 0;
    let kind = hit ? "k" : "x";
    for (let [f, whenHit, whenMiss] of cases) {
        let result = String(f(kind));
        if (result !== (hit ? whenHit : whenMiss))
            throw new Error(f.name + ": got " + result + " at " + i);
    }
    if (recursive(2, true) !== 7)
        throw new Error("recursive: bad result at " + i);
}

// The same loop, inlined into a caller (needs the raised inlining budget above).
function forOfInlinee(kind) {
    for (let name of ["a", "b"]) {
        try {
            return JSON.parse(kind === "k" && name === "b" ? "1" : "{bad"), true;
        } catch { }
    }
    return false;
}
for (let i = 0; i < 5e4; ++i) {
    let hit = i % 3 === 0;
    if (callThrough(forOfInlinee, hit ? "k" : "x") !== hit)
        throw new Error("forOfInlinee: bad result at " + i);
}
