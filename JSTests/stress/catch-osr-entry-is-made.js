//@ runFTLNoCJIT; run("ftl-no-cjit-eager-catch-liveness", "--useFTLJIT=true", "--useConcurrentJIT=false", "--useLazyCatchLiveness=false")

// This handler has to get a catch OSR entrypoint: at some point it runs in optimized code.

function thrower(shouldThrow) {
    if (shouldThrow)
        throw new Error("thrown");
}
noInline(thrower);

function shouldEnter(f) {
    for (let i = 0; i < 10 * testLoopCount; ++i) {
        if (f(!(i % 10)))
            return;
    }
    if ($vm.useDFGJIT())
        throw new Error(f.name + ": the handler never ran in optimized code");
}

// The completion value of the finally is live at the catch and always the empty value there.
function catchInsideFinally(shouldThrow) {
    let optimized = false;
    try {
        try {
            thrower(shouldThrow);
        } catch {
            optimized = $vm.dfgTrue();
        }
    } finally {
        thrower(false);
    }
    return optimized;
}
noInline(catchInsideFinally);
shouldEnter(catchInsideFinally);
