//@ requireOptions("--useDollarVM=1", "--useConcurrentJIT=0", "--jitPolicyScale=0", "--useExecutableAllocationFuzz=false")

// $vm.ffiFunctionClose(fn) is JSFFIFunction::close(): what an embedder does before it unloads the
// library a function points into. After it, every call must throw a TypeError and none may reach the
// target, on every path that has the target baked in: the host call, the IC stub, and DFG / FTL
// CallFFI code compiled before the close. The fixtures stay mapped, so a path that was missed shows up
// here as a call that returns a value instead of throwing.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

function shouldThrowClosed(fn, what) {
    let error = null;
    let result;
    try {
        result = fn();
    } catch (e) {
        error = e;
    }
    if (!error)
        throw new Error(what + ": the call reached the target and returned " + String(result));
    if (!(error instanceof TypeError) || !/was closed/.test(error.message))
        throw new Error(what + ": wrong error: " + error);
}

function makeAdd(name) {
    return $vm.ffiFunction({ args: ["i32", "i32"], returns: "i32" }, $vm.ffiFixture("ffi_add_i32"), name);
}

function testColdCall() {
    const add = makeAdd("coldAdd");
    const other = makeAdd("otherAdd");
    shouldBe(add(1, 2), 3, "open cold call");
    const ptr = add.ptr;

    $vm.ffiFunctionClose(add);
    shouldThrowClosed(() => add(1, 2), "cold call after close");
    shouldThrowClosed(() => add(), "cold call after close, too few arguments");
    shouldThrowClosed(() => add("x", {}), "cold call after close, arguments that do not convert inline");
    shouldThrowClosed(() => add.call(null, 1, 2), "Function.prototype.call after close");
    shouldThrowClosed(() => add.apply(null, [1, 2]), "Function.prototype.apply after close");
    shouldThrowClosed(() => Reflect.apply(add, null, [1, 2]), "Reflect.apply after close");
    shouldThrowClosed(() => add.bind(null, 1)(2), "bound function after close");
    shouldThrowClosed(() => [1].map(add.native), "call from C++ after close");

    // Closing is per function and idempotent, and it leaves the properties alone.
    $vm.ffiFunctionClose(add);
    shouldThrowClosed(() => add(1, 2), "cold call after a second close");
    shouldBe(other(40, 2), 42, "another function bound to the same target");
    shouldBe(add.ptr, ptr, ".ptr after close");
    shouldBe(add.native, add, ".native after close");

    let threw = false;
    try {
        $vm.ffiFunctionClose(function () { });
    } catch (e) {
        threw = e instanceof TypeError;
    }
    shouldBe(threw, true, "$vm.ffiFunctionClose rejects a non-FFI function");
}

function testMessageNamesTheFunction() {
    const add = makeAdd("sqlite3_open");
    $vm.ffiFunctionClose(add);
    try {
        add(1, 2);
    } catch (e) {
        if (!e.message.includes("sqlite3_open"))
            throw new Error("the error does not name the function: " + e.message);
        return;
    }
    throw new Error("no error");
}

function testCloseAfterTierUp() {
    const add = makeAdd("hotAdd");
    const before = $vm.ffiCompileCounts();

    function hot(a, b) {
        return add(a, b);
    }
    noInline(hot);

    let sum = 0;
    for (let i = 0; i < 1e5; ++i)
        sum += hot(i, 1);
    shouldBe(sum, 5000050000, "hot sum");

    // The close must be tested against compiled CallFFI code, not only against the stub.
    const after = $vm.ffiCompileCounts();
    if ($vm.useDFGJIT() && numberOfDFGCompiles(hot) > 0 && after.dfgCallFFI + after.ftlCallFFI === before.dfgCallFFI + before.ftlCallFFI)
        throw new Error("the hot caller was compiled without a CallFFI node");

    $vm.ffiFunctionClose(add);
    shouldThrowClosed(() => hot(1, 2), "optimized caller, first call after close");

    // The caller is compiled again while the function is closed. It must not get a CallFFI back.
    for (let i = 0; i < 1e4; ++i)
        shouldThrowClosed(() => hot(i, 2), "optimized caller, call " + i + " after close");
}

function testCloseInsideHotLoop() {
    const add = makeAdd("loopAdd");
    const closeAt = 60000;
    let reached = 0;
    let thrown = 0;

    function loop() {
        for (let i = 0; i < 1e5; ++i) {
            if (i === closeAt)
                $vm.ffiFunctionClose(add);
            try {
                add(i, 1);
                ++reached;
            } catch (e) {
                if (!(e instanceof TypeError))
                    throw e;
                ++thrown;
            }
        }
    }
    noInline(loop);
    loop();

    shouldBe(reached, closeAt, "calls that reached the target");
    shouldBe(thrown, 1e5 - closeAt, "calls that threw");
}

// Converting an argument can run JS, and that JS can close the function after the call has started.
function testCloseDuringArgumentConversion() {
    // Host path.
    {
        const add = makeAdd("convertAddCold");
        const closer = { valueOf() { $vm.ffiFunctionClose(add); return 1; } };
        shouldThrowClosed(() => add(closer, 2), "close inside valueOf, cold");
        shouldThrowClosed(() => add(1, 2), "call after close inside valueOf, cold");
    }

    // DFG / FTL: untyped arguments convert through operationFFIWriteSlot, then the compiled code calls
    // the target it baked in.
    {
        const add = makeAdd("convertAddHot");
        let armed = false;
        const sometimesCloser = { valueOf() { if (armed) $vm.ffiFunctionClose(add); return 1; } };

        function hot(a, b) {
            return add(a, b);
        }
        noInline(hot);

        let sum = 0;
        for (let i = 0; i < 1e5; ++i)
            sum += hot((i & 1) ? sometimesCloser : 1, 1);
        shouldBe(sum, 2e5, "hot sum with untyped arguments");

        armed = true;
        shouldThrowClosed(() => hot(sometimesCloser, 2), "close inside valueOf, optimized caller");
        shouldThrowClosed(() => hot(1, 2), "call after close inside valueOf, optimized caller");
    }

    // A pointer argument reads `.ptr` off an object, which can be a getter.
    {
        const echo = $vm.ffiFunction({ args: ["ptr"], returns: "ptr" }, $vm.ffiFixture("ffi_echo_ptr"), "convertEchoPtr");
        let armed = false;
        const sometimesCloser = { get ptr() { if (armed) $vm.ffiFunctionClose(echo); return 1234; } };

        function hot(p) {
            return echo(p);
        }
        noInline(hot);

        for (let i = 0; i < 1e5; ++i)
            shouldBe(hot((i & 1) ? sometimesCloser : 1234), 1234, "hot pointer echo");

        armed = true;
        shouldThrowClosed(() => hot(sometimesCloser), "close inside a ptr getter, optimized caller");
        shouldThrowClosed(() => hot(1234), "call after close inside a ptr getter, optimized caller");
    }
}

function testHookedFunction() {
    const owner = { hookLog: [] };
    const add = $vm.ffiFunction({ args: ["i32", "i32"], returns: "i32" }, $vm.ffiFixture("ffi_add_i32"), "hookedAdd", { owner, hooks: "test" });
    shouldBe(add(1, 2), 3, "open hooked call");
    $vm.ffiFunctionClose(add);
    shouldThrowClosed(() => add(1, 2), "hooked call after close");
}

if ($vm.useJIT()) {
    testColdCall();
    testMessageNamesTheFunction();
    testCloseAfterTierUp();
    testCloseInsideHotLoop();
    testCloseDuringArgumentConversion();
    testHookedFunction();
}
