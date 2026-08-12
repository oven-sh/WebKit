//@ requireOptions("--useExplicitResourceManagement=1", "--useConcurrentJIT=0", "--validateGraph=1")

// Found by fuzzing. The FTL compile of this eval code used to fail OSR
// availability validation ("Live bytecode local not available") on the local
// that the synthesized catch handler around the dispose call reads: the try
// range of that handler ends at a block boundary inside the enclosing for-of
// handler, and LiveCatchVariablePreservationPhase flushed the outer handler's
// locals instead of the inner handler's when it crossed from one to the other.

(0, eval)(`
    const resource = { [Symbol.dispose]() { } };
    for (using r of [resource]) {
        try { r(); } catch (e) { }
    }
    for (let i = 0; i < 2000000; ++i) { }
`);
