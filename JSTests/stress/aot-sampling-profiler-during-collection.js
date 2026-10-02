//@ runDefault("--compileMainScriptAheadOfTime=1", "--useSamplingProfiler=1", "--collectContinuously=1")
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
let names = new Set();
for (let trace of samplingProfilerStackTraces().traces) {
    for (let frame of trace.frames)
        names.add(frame.name);
}
for (let name of ["spin", "first", "second", "third"]) {
    if (!names.has(name))
        throw new Error("no sample has " + name + ": " + [...names].join());
}
