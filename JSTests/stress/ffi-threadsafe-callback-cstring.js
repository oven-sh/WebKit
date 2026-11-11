//@ requireOptions("--useDollarVM=1", "--useExecutableAllocationFuzz=false")
// A `cstring` argument of a threadsafe callback is copied inside the C caller's call. Here the caller's strings are in the
// FFI string arena, which the next call overwrites.
if (!$vm.useJIT()) quit();

let seen = [];
const signature = { args: ["cstring", "i32", "cstring"], returns: "void" };
const callback = $vm.ffiCallback(signature, (first, number, second) => { seen.push([first, number, second]); }, { threadsafe: true });
const call = $vm.ffiFunction({ args: ["cstring", "i32", "cstring"], returns: "void" }, callback.ptr, "call_threadsafe");

const strings = ["", "ascii", "naïve", "あいう", "x".repeat(5000)];
for (let round = 0; round < Math.min(200, testLoopCount / 10); ++round) {
    seen = [];
    for (let i = 0; i < strings.length; ++i)
        call(strings[i] + round, i, null);
    for (let i = 0; i < strings.length; ++i)
        call(null, -i, strings[i]);
    if (seen.length)
        throw new Error("the callback ran inline");
    if ($vm.drainThreadsafeCallbacks() !== strings.length * 2)
        throw new Error("wrong number of invocations");
    for (let i = 0; i < strings.length; ++i) {
        const [first, number, second] = seen[i];
        if (first !== strings[i] + round || number !== i || second !== null)
            throw new Error("wrong arguments of call " + i + ": " + JSON.stringify(seen[i]).slice(0, 100));
        const [nothing, negative, string] = seen[strings.length + i];
        if (nothing !== null || negative !== -i || string !== strings[i])
            throw new Error("wrong arguments of second call " + i + ": " + JSON.stringify(seen[strings.length + i]).slice(0, 100));
    }
}
callback.close();
