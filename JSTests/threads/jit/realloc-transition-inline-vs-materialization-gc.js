//@ requireOptions("--useJSThreads=1", "--useConcurrentJIT=false", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=20", "--verifyHeap=true")
// SPEC-jit §5.5 Transition, (re)allocating form (r14): the DFG/FTL inline an
// owner's storage-growing transition as allocate, store the value into the
// fresh storage, InvalidationPoint, install (nuke + tagged word), PutStructure.
// The install must come AFTER everything that can allocate - here the
// materialization of a sunk object stored as the new property's value - or a
// collection at that allocation meets an object with a nuked structure (sixth
// round: the first cut installed before the value store; the heap verifier
// aborted "structureID is nuked", GIL on, eager FTL). This is the body of
// stress/materialize-past-butterfly-allocation.js under the flag and the heap
// verifier: one function per number of preceding adds, so whichever add crosses
// the inline capacity is the one storing the sunk object, results retained to
// keep collections coming.
function bar() {
    return {f:42};
}

noInline(bar);

function foo0(b) {
    var o = {f:42};
    if (b) {
        var p = bar();
        p.g = o;
        return p;
    }
}

function foo1(b) {
    var o = {f:42};
    if (b) {
        var p = bar();
        p.f1 = 1;
        p.g = o;
        return p;
    }
}

function foo2(b) {
    var o = {f:42};
    if (b) {
        var p = bar();
        p.f1 = 1;
        p.f2 = 2;
        p.g = o;
        return p;
    }
}

function foo3(b) {
    var o = {f:42};
    if (b) {
        var p = bar();
        p.f1 = 1;
        p.f2 = 2;
        p.f3 = 3;
        p.g = o;
        return p;
    }
}

function foo4(b) {
    var o = {f:42};
    if (b) {
        var p = bar();
        p.f1 = 1;
        p.f2 = 2;
        p.f3 = 3;
        p.f4 = 4;
        p.g = o;
        return p;
    }
}

noInline(foo0);
noInline(foo1);
noInline(foo2);
noInline(foo3);
noInline(foo4);

var array = new Array(1000);
for (var i = 0; i < 400000; ++i) {
    var o = foo0(true);
    array[i % array.length] = o;
}
for (var i = 0; i < 400000; ++i) {
    var o = foo1(true);
    array[i % array.length] = o;
}
for (var i = 0; i < 400000; ++i) {
    var o = foo2(true);
    array[i % array.length] = o;
}
for (var i = 0; i < 400000; ++i) {
    var o = foo3(true);
    array[i % array.length] = o;
}
for (var i = 0; i < 400000; ++i) {
    var o = foo4(true);
    array[i % array.length] = o;
}


print("PASS");
