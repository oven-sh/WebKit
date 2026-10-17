//@ skip if not $jitTests
//@ requireOptions("--useConcurrentJIT=false")
//@ runDefault
//@ runDefault("--keepMutuallyExclusiveStructureChecksInLoop=false")

// Each arm of a branch in a loop checks the structure of one loop-invariant value, and no structure passes both
// checks. Neither arm runs every time, so LICM can hoist each check only as a blind speculation. With both in the
// pre-header, the FTL code exits every time the loop is entered. That exit is recorded as HoistingFailed for the
// whole function. LICM leaves such checks in their arms, so the loop runs in the first FTL code.

const keepsThemInLoop = jscOptions().keepMutuallyExclusiveStructureChecksInLoop;
const iterations = 1000;

const scenarios = [
    {
        // Each arm has its own load of the captured variable. Neither load dominates the other, so they are two
        // nodes until the CSE after LICM.
        name: "captured variable",
        make() {
            function makeLoop(o) {
                return function loop() {
                    let sum = 0;
                    for (let i = 0; i < iterations; i++) {
                        if (o.p)
                            sum += o.f;
                        else
                            sum += o.g;
                    }
                    return sum;
                };
            }
            let loops = [makeLoop({ p: true, f: 1 }), makeLoop({ p: false, g: 2 })];
            return { subject: loops[0], calls: [() => loops[0](), () => loops[1]()], results: [iterations, 2 * iterations] };
        },
    },
    {
        // The shape of microbenchmarks/licm-dragons-two-structures.js: both arms check one node.
        name: "argument",
        make() {
            function loop(o) {
                let sum = 0;
                for (let i = 0; i < iterations; i++) {
                    if (o.p)
                        sum += o.f;
                    else
                        sum += o.g;
                }
                return sum;
            }
            noInline(loop);
            let objects = [{ p: true, f: 1 }, { p: false, g: 2 }];
            return { subject: loop, calls: [() => loop(objects[0]), () => loop(objects[1])], results: [iterations, 2 * iterations] };
        },
    },
    {
        // Each arm loads the checked value from a property of the argument.
        name: "property of the argument",
        make() {
            function loop(holder) {
                let sum = 0;
                for (let i = 0; i < iterations; i++) {
                    if (holder.first)
                        sum += holder.o.f;
                    else
                        sum += holder.o.g;
                }
                return sum;
            }
            noInline(loop);
            let holders = [{ first: true, o: { f: 1 } }, { first: false, o: { g: 2 } }];
            return { subject: loop, calls: [() => loop(holders[0]), () => loop(holders[1])], results: [iterations, 2 * iterations] };
        },
    },
    {
        // One arm calls a method of a Map. The other arm calls a method that the classes of the two other readers
        // have. The reader is a captured variable.
        name: "captured Map or instance",
        make() {
            class Lookup extends Map {
                read(key) { return this.get(key); }
            }
            class Holder {
                #data;
                constructor(data) { this.#data = data; }
                read(key) { return this.#data.get(key); }
            }
            let data = new Map([[0, 1]]);
            function makeLoop(direct, reader) {
                return function loop() {
                    let sum = 0;
                    for (let i = 0; i < iterations; i++)
                        sum += (direct ? reader.get(0) : reader.read(0));
                    return sum;
                };
            }
            let loops = [makeLoop(true, data), makeLoop(false, new Lookup(data)), makeLoop(false, new Holder(data))];
            return { subject: loops[0], calls: loops.map(loop => () => loop()), results: [iterations, iterations, iterations] };
        },
    },
    {
        // The arms are the two callees of one call site, which the FTL inlines behind a switch. Each callee checks
        // the receiver. Both receivers take turns, so the call IC of the DFG code counts both callees. The loop is
        // short, so that the DFG compiles it after it has seen both receivers.
        name: "receiver of inlined callees",
        takeTurns: true,
        make() {
            const shortIterations = 100;
            class First {
                constructor() { this.a = 1; }
                value() { return this.a; }
            }
            class Second {
                constructor() { this.b = 2; }
                value() { return this.b; }
            }
            function loop(o) {
                let sum = 0;
                for (let i = 0; i < shortIterations; i++)
                    sum += o.value();
                return sum;
            }
            noInline(loop);
            let objects = [new First, new Second];
            return { subject: loop, calls: [() => loop(objects[0]), () => loop(objects[1])], results: [shortIterations, 2 * shortIterations] };
        },
    },
];

for (let scenario of scenarios) {
    let { subject, calls, results } = scenario.make();

    function check(index) {
        let result = calls[index]();
        if (result !== results[index])
            throw new Error(scenario.name + ": bad result " + result + " of call " + index);
    }

    // Every arm runs before the loop is compiled. Then one arm only, or each arm in turn.
    for (let index = 0; index < calls.length; index++)
        check(index);
    for (let i = 0; i < 3000; i++)
        check(scenario.takeTurns ? i % calls.length : 0);

    let retries = reoptimizationRetryCount(subject);
    if (keepsThemInLoop) {
        if (retries)
            throw new Error(scenario.name + ": the loop was compiled again " + retries + " times");
    } else if (jscOptions().useFTLJIT && !retries)
        throw new Error(scenario.name + ": expected the FTL code with both checks hoisted to be thrown away");
}
