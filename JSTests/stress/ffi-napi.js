//@ requireOptions("--useDollarVM=1")

// napi_env is a synthetic argument supplied live from FFIContext (never
// consumed from the JS argument list, never baked at code-generation time);
// napi_value is a raw EncodedJSValue pass-through (SPEC sections 5, 6, 15.7).

function describe(value) {
    if (typeof value === "bigint")
        return String(value) + "n";
    if (typeof value === "symbol")
        return value.toString();
    if (Object.is(value, -0))
        return "-0";
    if (value !== null && (typeof value === "object" || typeof value === "function")) {
        // Object.create(null), Proxy, etc. may have no toString/valueOf, so
        // String(value) would throw "No default value" while merely
        // formatting a message; describe by structure instead.
        const tag = Object.prototype.toString.call(value);
        return typeof value === "function" ? "[function " + (value.name || "anonymous") + "]" : tag;
    }
    return String(value);
}

function check(actual, expected, message) {
    if (!Object.is(actual, expected))
        throw new Error(message + ": expected " + describe(expected) + " but got " + describe(actual));
}

function main() {
    const fixture = name => $vm.ffiFixture(name);
    // The C fixture is `void* ffi_recv_napi_env(napi_env)`: it just returns the env.
    const recvEnv = $vm.ffiFunction({ args: ["napi_env"], returns: "ptr" }, fixture("ffi_recv_napi_env"), "ffi_recv_napi_env");
    // napi_env followed by a real JS argument: the JS argument is the FIRST JS argument.
    const recvEnvThenInt = $vm.ffiFunction({ args: ["napi_env", "i32"], returns: "ptr" }, fixture("ffi_recv_napi_env"), "env then i32");
    const echoNapiValue = $vm.ffiFunction({ args: ["napi_value"], returns: "napi_value" }, fixture("ffi_echo_napi_value"), "ffi_echo_napi_value");

    check(recvEnv.length, 0, "napi_env does not count as a JS parameter");
    check(recvEnvThenInt.length, 1, "napi_env excluded from length");

    // ---- Initially unset: the synthesized env is a null pointer -> JS null.
    check(recvEnv(), null, "napi env before ffiSetNapiEnv");
    check(recvEnv(12345), null, "JS arguments never fill the napi_env slot");
    check(recvEnvThenInt(7), null, "napi env before set, with a JS argument");

    // ---- Live values: every call reads the current env, including functions
    // created before the env was (re)set, in every tier.
    const envA = 0x1000;
    const envB = 0x00007ffd0000abc0;
    $vm.ffiSetNapiEnv(envA);
    check(recvEnv(), envA, "env A cold");
    check(recvEnvThenInt(999), envA, "env A with a JS argument");
    for (let i = 0; i < 3e4; ++i) {
        if (recvEnv() !== envA)
            throw new Error("hot recvEnv (env A) iteration " + i + ": " + recvEnv());
        if (recvEnvThenInt(i) !== envA)
            throw new Error("hot recvEnvThenInt (env A) iteration " + i);
    }
    // Switch AFTER the callers are (probably) compiled: no baked immediates allowed.
    $vm.ffiSetNapiEnv(envB);
    check(recvEnv(), envB, "env B right after switching");
    for (let i = 0; i < 3e4; ++i) {
        if (recvEnv() !== envB)
            throw new Error("hot recvEnv did not pick up env B at iteration " + i + ": " + recvEnv());
        if (recvEnvThenInt(i) !== envB)
            throw new Error("hot recvEnvThenInt did not pick up env B at iteration " + i);
    }
    // Flip-flop inside the hot loop.
    for (let i = 0; i < 2000; ++i) {
        const env = (i & 1) ? envA : envB;
        $vm.ffiSetNapiEnv(env);
        if (recvEnv() !== env)
            throw new Error("flip-flop iteration " + i + ": expected " + env + " got " + recvEnv());
    }
    // Back to null.
    $vm.ffiSetNapiEnv(0);
    check(recvEnv(), null, "env reset to null");
    for (let i = 0; i < 1e4; ++i) {
        if (recvEnv() !== null)
            throw new Error("hot recvEnv after reset iteration " + i);
    }
    $vm.ffiSetNapiEnv(envA);
    check(recvEnv(), envA, "env A restored");

    // ---- napi_value: identity of arbitrary JSValues in both directions.
    const object = { deep: { array: [1, 2, 3] } };
    const array = [1, "two", 3n];
    const fn = function named() { return 1; };
    const symbol = Symbol("napi");
    const registrySymbol = Symbol.for("napi.registry");
    const bigint = 123456789012345678901234567890n;
    const values = [
        object, array, fn, symbol, registrySymbol, bigint, 0, -0, 1, -1, 0.5, NaN, Infinity, -Infinity,
        2147483647, -2147483648, 2147483648, 4294967295, Number.MAX_SAFE_INTEGER, Number.MIN_VALUE,
        true, false, null, undefined, "", "string", "\u{1F600}", 0n, -1n,
        new Uint8Array(3), new ArrayBuffer(2), Object.freeze({}), Object.create(null),
        recvEnv, echoNapiValue, // JSFFIFunctions themselves
        $vm.ffiCallback({ args: [], returns: "void" }, () => { }), // a JSFFICallback
        new Proxy({}, {}), new Error("as a value"), Promise.resolve(1), new Map(), new WeakRef(object),
    ];
    for (const value of values) {
        const result = echoNapiValue(value);
        check(result, value, "napi_value identity for " + describe(value));
        if ((typeof value === "object" && value !== null) || typeof value === "function" || typeof value === "symbol") {
            if (result !== value)
                throw new Error("napi_value must preserve object identity (===), got a different object for " + describe(value));
        }
    }
    // Missing napi_value argument: undefined bits pass through.
    check(echoNapiValue(), undefined, "missing napi_value argument is undefined");
    // Hot identity through the tiers with a few classes of values.
    for (let i = 0; i < 3e4; ++i) {
        const value = values[i % values.length];
        const result = echoNapiValue(value);
        if (!Object.is(result, value))
            throw new Error("hot napi_value identity iteration " + i + " for " + describe(value) + " got " + describe(result));
    }
    // Values created inside the loop (young objects): identity, and no GC crash.
    for (let i = 0; i < 5000; ++i) {
        const young = { i, payload: new Array(8).fill(i) };
        if (echoNapiValue(young) !== young)
            throw new Error("young object identity iteration " + i);
        if ((i & 1023) === 0)
            gc();
    }
    // napi_value inside a callback: JS -> native -> JS receives the very same values.
    const seen = [];
    const cb = $vm.ffiCallback({ args: ["napi_value"], returns: "napi_value" }, v => { seen.push(v); return v; });
    const throughCallback = $vm.ffiFunction({ args: ["napi_value"], returns: "napi_value" }, cb, "napi_value round trip");
    for (const value of values) {
        seen.length = 0;
        const result = throughCallback(value);
        check(result, value, "callback napi_value round trip for " + describe(value));
        check(seen.length, 1, "callback invoked once");
        check(seen[0], value, "callback saw the identical value");
    }
}

if ($vm.useJIT())
    main();
