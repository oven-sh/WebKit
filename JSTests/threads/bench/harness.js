// Shared harness for the serial-performance bench gate (Tools/threads/bench-gate.sh).
//
// Each benchmark calls reportBench(name, fn). The harness warms fn up so all
// JIT tiers come online, then times a fixed number of measured iterations and
// prints a single machine-parseable line:
//
//     BENCH <name> <milliseconds>
//
// The gate script medians these across runs and compares against
// Tools/threads/baseline.json. Only the measured loop is timed, so jsc
// startup/teardown noise does not pollute the comparison.
//
// Benchmarks must be deterministic and self-checking: fn returns a checksum
// which is validated on every iteration, per JSTests/microbenchmarks
// convention (throw on bad result).

function reportBench(name, fn, expected, warmupIterations, measuredIterations)
{
    if (warmupIterations === undefined)
        warmupIterations = 20;
    if (measuredIterations === undefined)
        measuredIterations = 50;

    // Warm up: let the LLInt -> Baseline -> DFG -> FTL pipeline settle.
    for (var i = 0; i < warmupIterations; ++i) {
        var result = fn();
        if (result != expected)
            throw "Error: bad result during warmup of " + name + ": " + result;
    }

    // Sub-millisecond timing when available (the jsc shell's preciseTime()
    // returns seconds as a double). Date.now()'s 1ms quantization is ~2% of
    // a 50ms benchmark — bigger than the gate's 1% threshold, so the gate
    // could fail against its own baseline on quantization noise alone.
    var nowMs = typeof preciseTime === "function"
        ? function() { return preciseTime() * 1000; }
        : Date.now;

    var before = nowMs();
    for (var i = 0; i < measuredIterations; ++i) {
        var result = fn();
        if (result != expected)
            throw "Error: bad result during measurement of " + name + ": " + result;
    }
    var after = nowMs();

    print("BENCH " + name + " " + (after - before).toFixed(3));
}
