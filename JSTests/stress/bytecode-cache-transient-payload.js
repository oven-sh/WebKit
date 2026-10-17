//@ runDefault
//@ run("collect-continuously", "--collectContinuously=1")

// A payload JSC only borrows (a bare span with no destructor that nobody declared persistent, like node:vm's cachedData)
// may be freed by the embedder as soon as decoding returns, so nothing decoded from it may finish decoding later from
// the buffer. The block scope below becomes a SymbolTable constant whose entries are looked up when each new CodeBlock
// links and when eval resolves `captured`; the second evaluation happens after the buffer was scribbled over and freed.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + ", expected " + expected);
}

let results = evalTwiceFromTransientBytecodeCache(`
    var viaEval;
    { let captured = 40; const more = 2; viaEval = eval("captured + more"); }
    try { throw 1; } catch (caught) { viaEval += eval("caught - 1"); }
    viaEval
`);
shouldBe(results.length, 2);
shouldBe(results[0], 42);
shouldBe(results[1], 42);
