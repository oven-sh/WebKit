//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSamplingProfiler=1", "--collectContinuously=1", "--sampleInterval=100")
function spin(n) {
    let total = 0;
    let list = [];
    for (let i = 0; i < n; ++i) {
        total += i % 7;
        if (!(i & 15))
            list.push({ i });
    }
    return total + list.length;
}
function makeRelays() {
    return [
        function first(f, n) { return f(n) + 1; },
        function second(f, n) { return f(n) + 2; },
        function third(f, n) { return f(n) + 3; },
    ];
}
let relays = makeRelays();
let total = 0;
let start = Date.now();
while (Date.now() - start < 1500) {
    for (let relay of relays)
        total += relay(spin, 20000);
}

function makeCallersOfNativeFunctions() {
    return [
        function callsNow(n) { let total = 0; for (let i = 0; i < n; ++i) total += Date.now() & 1; return total; },
        function callsPush(n) { let list = []; for (let i = 0; i < n; ++i) list.push(i); return list.length; },
        function callsIsFrozen(n) { let total = 0; let list = [n]; for (let i = 0; i < n; ++i) total += Object.isFrozen(list) ? 1 : 2; return total; },
    ];
}
function relaysTo(f, n) { return f(n) + 1; }
function allocates(n) { let list = []; for (let i = 0; i < n; ++i) list.push({ i }); return list.length; }
let callers = makeCallersOfNativeFunctions();
start = Date.now();
while (Date.now() - start < 1500) {
    for (let caller of callers)
        total += relaysTo(caller, 5000);
    total += allocates(500);
}

let names = new Set();
for (let trace of samplingProfilerStackTraces().traces) {
    for (let frame of trace.frames)
        names.add(frame.name);
}
for (let name of ["spin", "first", "second", "third", "callsNow", "callsPush", "callsIsFrozen", "relaysTo"]) {
    if (!names.has(name))
        throw new Error("no sample has " + name + ": " + [...names].join());
}
