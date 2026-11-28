//@ requireOptions("--useDollarVM=1", "--useExecutableAllocationFuzz=false")
// The string arena's scopes nest: an FFI call made inside another one (from a callback, or from a getter that an argument
// conversion runs) gives back what its strings took when it returns, and leaves the outer call's strings alone.
if (!$vm.useJIT()) quit();

const fixture = name => $vm.ffiFixture(name);
const echoAddress = $vm.ffiFunction({ args: ["cstring"], returns: "ptr" }, fixture("ffi_echo_cstring"), "echo_address");
const echo = $vm.ffiFunction({ args: ["cstring"], returns: "cstring" }, fixture("ffi_echo_cstring"), "echo");
const echoFirst = $vm.ffiFunction({ args: ["cstring", "ptr"], returns: "cstring" }, fixture("ffi_echo_cstring"), "echo_first");
const callThrough = $vm.ffiFunction({ args: ["ptr"], returns: "cstring" }, fixture("ffi_call_cb_ret_cstring"), "call_cb_ret_cstring");

const contents = "x".repeat(1000);
function addressOf(string) { return echoAddress(string); }
noInline(addressOf);

let depthInCallback = 0;
const callback = $vm.ffiCallback({ args: [], returns: "cstring" }, () => {
    depthInCallback = $vm.ffiArenaDepth();
    const addresses = new Set;
    for (let i = 0; i < testLoopCount; ++i)
        addresses.add(addressOf(contents));
    if (addresses.size !== 1)
        throw new Error("the strings of nested calls were kept: " + addresses.size + " addresses");
    return "done";
});
// An object in place of `.ptr` takes the path that opens a scope for every call.
for (let i = 0; i < 5; ++i) {
    if (callThrough(callback) !== "done")
        throw new Error("wrong result of the outer call");
    if (depthInCallback !== 1)
        throw new Error("expected an open scope in the callback, depth " + depthInCallback);
}
callback.close();

// Sizes around the arena's chunks: the inner calls fit beside the outer string, need a chunk of their own, or a large one.
const outerLengths = [1, 100, 3000, 4095, 4096, 10000, 70000];
const innerLengths = [1, 1000, 4095, 5000, 70000];
function outer(string, argument) { return echoFirst(string, argument); }
noInline(outer);
for (let round = 0; round < Math.min(300, testLoopCount / 10); ++round) {
    const outerString = "o".repeat(outerLengths[round % outerLengths.length]);
    let depth = 0;
    const result = outer(outerString, {
        get ptr() {
            depth = $vm.ffiArenaDepth();
            for (const length of innerLengths) {
                const innerString = "i".repeat(length);
                if (echo(innerString) !== innerString)
                    throw new Error("wrong result of an inner call of length " + length);
            }
            // Another level, which throws out of its own conversion.
            try {
                outer(innerLengths.join(), { get ptr() { echo("y".repeat(6000)); throw new Error("thrown"); } });
                throw new Error("unreachable");
            } catch (error) {
                if (error.message !== "thrown")
                    throw error;
            }
            if ($vm.ffiArenaDepth() !== depth)
                throw new Error("unbalanced scopes");
            return 0x1000;
        }
    });
    if (result !== outerString)
        throw new Error("an inner call overwrote the outer string of length " + outerString.length);
    if (depth !== 1 || $vm.ffiArenaDepth() !== 0)
        throw new Error("wrong depth " + depth);
}
