//@ skip if not $jitTests
//@ requireOptions("--useConcurrentJIT=false")
//@ runDefault
//@ runDefault("--weakReferenceJettisonReoptimizationLimit=0")
//@ runDefault("--weakReferenceJettisonReoptimizationLimit=100")

// Optimized code dies when a cell it references weakly dies. The function is alive and tiers up again. Up to
// Options::weakReferenceJettisonReoptimizationLimit() of these deaths count toward the function's reoptimization
// back-off, like code jettisoned for exiting too often: each doubles the number of calls the next compile waits for.

if (!$vm.useDFGJIT())
    quit();

const limit = jscOptions().weakReferenceJettisonReoptimizationLimit;
const lives = 9;
const maximumCallsPerLife = 1 << 20;

var ranOptimized = false;

// Each life hands the function an object whose prototype nothing else refers to. The optimized code checks the
// object's structure, or loads from the prototype it has proved the object to have, and refers to both weakly;
// a structure whose prototype is dead is not kept alive by the code that checks it.
const scenarios = [
    {
        name: "structure",
        body: "if ($vm.dfgTrue()) ranOptimized = true; return o.value;",
        make(life) {
            let o = Object.create({ life });
            o.value = 42;
            return o;
        },
    },
    {
        name: "prototype",
        body: "if ($vm.dfgTrue()) ranOptimized = true; return o.inherited;",
        make(life) {
            return Object.create({ life, inherited: 42 });
        },
    },
];
for (let scenario of scenarios)
    noInline(scenario.make);

// Overwrites what the frames of a life left on the stack, so that the collection's own frames do not find the life's
// object in stack memory they have not written to yet.
function clobberStack(depth) {
    let a = depth + 1, b = depth + 2, c = depth + 3, d = depth + 4;
    if (!depth)
        return 0;
    return clobberStack(depth - 1) + a + b + c + d;
}
noInline(clobberStack);

// Calls the function until it runs optimized; returns how many calls that took.
function callsUntilOptimized(scenario, subject, life) {
    let object = scenario.make(life);
    ranOptimized = false;
    for (let calls = 1; calls <= maximumCallsPerLife; ++calls) {
        if (subject(object) !== 42)
            throw new Error("bad result in " + scenario.name + " life " + life);
        if (ranOptimized)
            return calls;
    }
    throw new Error(scenario.name + ": not optimized after " + maximumCallsPerLife + " calls in life " + life);
}
noInline(callsUntilOptimized);

// Returns the calls each life took, or null if a collection did not kill the optimized code: then something else
// kept the object alive, the code exits on the next object and is compiled without the weak reference from then on.
function measure(scenario) {
    let subject = new Function("o", scenario.body);
    noInline(subject);
    let calls = [];
    for (let life = 0; life < lives; ++life) {
        calls.push(callsUntilOptimized(scenario, subject, life));
        clobberStack(200);
        fullGC();
        // numberOfDFGCompiles() is the retry count plus one if optimized code is installed.
        if (numberOfDFGCompiles(subject) !== reoptimizationRetryCount(subject))
            return null;
    }
    let expectedRetries = Math.min(limit, lives);
    if (reoptimizationRetryCount(subject) !== expectedRetries)
        throw new Error(scenario.name + ": retry count " + reoptimizationRetryCount(subject) + ", expected " + expectedRetries + "; calls per life " + calls);
    return calls;
}

function within(actual, expected) {
    return actual >= expected / 1.5 && actual <= expected * 1.5;
}

for (let scenario of scenarios) {
    let calls = null;
    for (let attempt = 0; attempt < 3 && !calls; ++attempt)
        calls = measure(scenario);
    if (!calls)
        throw new Error(scenario.name + ": optimized code kept surviving the death of the prototype");

    // Life 0 also pays for getting out of the interpreter; life 1 is the first that starts in baseline code.
    for (let life = 2; life < lives; ++life) {
        let doublings = Math.min(life, limit) - Math.min(1, limit);
        if (!within(calls[life], calls[1] * (1 << doublings)))
            throw new Error(scenario.name + ": life " + life + " took " + calls[life] + " calls, expected about " + calls[1] * (1 << doublings) + " with limit " + limit + "; calls per life " + calls);
    }
}
