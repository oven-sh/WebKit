//@ runNoCJIT; runFTLNoCJIT

// A delete that the inline cache has is a Structure check and a store in optimized code. An object of a Structure that
// the site never saw then leaves that code, and $vm.dfgTrue() is false after the delete. A delete that the DFG could
// not inline is a call, and stays. No delete of a property with attributes was inlined: the cache never had it.
// (Only the modes that compile on the main thread, and not the eager ones: there the DFG compiles before the cache has a case.)

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": bad value: " + actual + " expected: " + expected);
}

function define(object, descriptor) {
    return Object.defineProperty(object, "a", { configurable: true, ...descriptor });
}

function readError(extra) {
    let error = new Error("message");
    if (extra)
        error.extra = 1;
    error.stack;
    return error;
}

function deleteNonEnumerable(o) { let result = delete o.a; return result && $vm.dfgTrue(); }
function deleteReadOnly(o) { let result = delete o.a; return result && $vm.dfgTrue(); }
function deleteAccessor(o) { let result = delete o.a; return result && $vm.dfgTrue(); }
function deleteStack(o) { let result = delete o.stack; return result && $vm.dfgTrue(); }

function test(del, name, make) {
    noInline(del);
    let optimized = false;
    for (let i = 0; i < 10 * testLoopCount && !optimized; ++i) {
        let o = make(false);
        optimized = del(o);
        shouldBe(Object.getOwnPropertyDescriptor(o, name), undefined, del.name);
        o[name] = i;
        shouldBe(o[name], i, del.name);
    }
    if (!$vm.useDFGJIT())
        return;
    shouldBe(optimized, true, del.name + " ran in optimized code");
    let unseen = make(true);
    shouldBe(del(unseen), false, del.name + " stays in optimized code for a Structure it has not seen");
    shouldBe(name in unseen, false, del.name);
}

test(deleteNonEnumerable, "a", extra => define(extra ? { x: 1, y: 2 } : { x: 1 }, { value: 1, writable: true }));
test(deleteReadOnly, "a", extra => define(extra ? { x: 1, y: 2 } : { x: 1 }, { value: 1, enumerable: true }));
test(deleteAccessor, "a", extra => define(extra ? { x: 1, y: 2 } : { x: 1 }, { get() { return 1; }, set(value) { } }));
test(deleteStack, "stack", readError);
