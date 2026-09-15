//@ runDefault
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=200")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--useFTLJIT=0")
//@ runBytecodeCache

// A generator, an async function or an async generator is suspended in the body of a loop while all code is thrown away
// (the same happens to it alone when its CodeBlock gets old and its unlinked code is dropped). It resumes in code that is
// generated again. Closures made in later iterations of the loop have to read those iterations' bindings: the DFG folds
// a closure's scope to the one environment its SymbolTable has seen, and the environments the new code makes have to
// reach that inference.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const iterations = 3;
const calls = testLoopCount;

function staleReads(read, expected) {
    let stale = 0;
    for (let k = 0; k < calls; k++) {
        if (read() !== expected)
            stale++;
    }
    return stale;
}

function* generatorBody(out) {
    for (let iter = 0; iter < iterations; iter++) {
        const box = { iter };
        if (!iter)
            yield;
        out.push(staleReads(() => [0].reduce(acc => acc + box.iter, 0), iter));
    }
}

async function asyncBody(out, gate) {
    for (let iter = 0; iter < iterations; iter++) {
        const box = { iter };
        if (!iter)
            await gate;
        out.push(staleReads(() => [0].reduce(acc => acc + box.iter, 0), iter));
    }
}

const asyncArrowBody = async (out, gate) => {
    for (let iter = 0; iter < iterations; iter++) {
        const box = { iter };
        if (!iter)
            await gate;
        out.push(staleReads(() => [0].reduce(acc => acc + box.iter, 0), iter));
    }
};

async function* asyncGeneratorBody(out) {
    for (let iter = 0; iter < iterations; iter++) {
        const box = { iter };
        if (!iter)
            yield;
        out.push(staleReads(() => [0].reduce(acc => acc + box.iter, 0), iter));
    }
}

// The loop's own binding: a fresh environment per iteration from one SymbolTable.
function* perIterationBinding(out) {
    for (let iter = 0; iter < iterations; iter++) {
        if (!iter)
            yield;
        out.push(staleReads(() => [0].reduce(acc => acc + iter, 0), iter));
    }
}

// Two levels down, and a binding that is assigned again after the closure over it was compiled (the per-variable
// watchpoints: the assignment has to reach the one that the compiled closure folded the old value under).
function* nestedAndReassigned(out) {
    for (let iter = 0; iter < iterations; iter++) {
        let value = -1;
        if (!iter)
            yield;
        const read = () => (() => value)();
        staleReads(read, -1);
        value = iter;
        out.push(staleReads(read, iter));
    }
}

// The function's own variable environment, made again by later activations after the first one resumed. (Right without
// the change as well; here so that it stays so.)
function* functionScope(out, id) {
    var captured = id;
    const read = () => captured;
    if (!id)
        yield;
    out.push(staleReads(read, id));
}

// Suspended, and its code generated again, in two iterations.
function* twice(out) {
    for (let iter = 0; iter < iterations; iter++) {
        const box = { iter };
        if (iter < 2)
            yield;
        out.push(staleReads(() => [0].reduce(acc => acc + box.iter, 0), iter));
    }
}

const shapes = [];
function shape(name, start) { shapes.push({ name, start }); }

shape("generator", (done) => {
    const out = [];
    const generator = generatorBody(out);
    generator.next();
    return () => { generator.next(); done(out); };
});
shape("async function", (done) => {
    const out = [];
    let open;
    const gate = new Promise(resolve => { open = resolve; });
    asyncBody(out, gate).then(() => done(out));
    return open;
});
shape("async arrow function", (done) => {
    const out = [];
    let open;
    const gate = new Promise(resolve => { open = resolve; });
    asyncArrowBody(out, gate).then(() => done(out));
    return open;
});
shape("async generator", (done) => {
    const out = [];
    const generator = asyncGeneratorBody(out);
    generator.next();
    return () => { generator.next().then(() => done(out)); };
});
shape("per-iteration binding", (done) => {
    const out = [];
    const generator = perIterationBinding(out);
    generator.next();
    return () => { generator.next(); done(out); };
});
shape("nested closure and a reassigned binding", (done) => {
    const out = [];
    const generator = nestedAndReassigned(out);
    generator.next();
    return () => { generator.next(); done(out); };
});
shape("function scope of a second activation", (done) => {
    const out = [];
    const first = functionScope(out, 0);
    first.next();
    return () => {
        first.next();
        for (let id = 1; id < iterations; id++)
            functionScope(out, id).next();
        done(out);
    };
});

shape("generated again twice", (done) => {
    const out = [];
    const generator = twice(out);
    generator.next();
    return () => {
        generator.next();
        $vm.deleteAllCodeWhenIdle();
        setTimeout(() => {
            fullGC();
            setTimeout(() => {
                generator.next();
                done(out);
            }, 0);
        }, 0);
    };
});

let index = 0;
function next() {
    if (index === shapes.length)
        return;
    const { name, start } = shapes[index++];
    const resume = start((out) => {
        assert(out.length === iterations, name + ": ran " + out.length + " iterations");
        for (let i = 0; i < out.length; i++)
            assert(!out[i], name + ": " + out[i] + " of " + calls + " closures made in iteration " + i + " read another iteration's binding");
        setTimeout(next, 0);
    });
    // Nothing is thrown away while JS is running: the request is served once this turn is over.
    $vm.deleteAllCodeWhenIdle();
    setTimeout(() => {
        fullGC();
        setTimeout(resume, 0);
    }, 0);
}
next();
