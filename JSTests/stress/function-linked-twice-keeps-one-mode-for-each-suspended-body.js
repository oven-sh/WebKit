//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--forceCodeBlockToJettisonDueToOldAge=1")
//@ runDefault("--useDollarVM=1", "--useJIT=0")

// One realm, one function evaluated twice: the two linked executables share the unlinked one (the CodeCache gives the same
// code for the same source), and with it the slot its code is kept in. A mode change (a debugger, the type profiler, the
// control flow profiler) deletes all code, and from then on the two can ask for different code:
//
// - The body of a generator or an async function that has started keeps its registers in a generator frame, and only the
//   code that saved them reads them back. The debugger changes where every variable lives, the type profiler what is
//   alive across a yield, and the control flow profiler's instruction, where it sits between two stores to one
//   register, which registers are saved. So a body with a suspended activation is always linked from the unlinked code
//   it first ran in (FunctionExecutable::codeOfStartedBody()), whatever the other executable made of the function since.
// - A body that starts after the change gets code for the new mode: with the debug hooks, if a debugger is attached.
//   That includes a body that ran before, once none of its activations is suspended.
//
// Every scenario below has the same steps as a run with no mode change at all, so the values are those of the language.

const failures = [];
const scenarios = [];

// A mode change as the inspector makes it: once the VM is idle, and with all code deleted to be generated again.
const modeChanges = {
    debuggerOn: () => $vm.enableDebuggerModeWhenIdle(),
    debuggerOff: () => $vm.disableDebuggerModeWhenIdle(),
    typeProfilerOn: () => $vm.setTypeProfilerEnabledWhenIdle(true),
    typeProfilerOff: () => $vm.setTypeProfilerEnabledWhenIdle(false),
    controlFlowProfilerOn: () => $vm.setControlFlowProfilerEnabledWhenIdle(true),
    controlFlowProfilerOff: () => $vm.setControlFlowProfilerEnabledWhenIdle(false),
    // What an embedder may do: only code generated from here on is instrumented.
    controlFlowProfilerOnCodeKept: () => $vm.setControlFlowProfilerEnabledWhenIdle(true, false),
    deleteAllCode: () => $vm.deleteAllCodeWhenIdle(),
    // What an idle embedder may do: unlinked code goes, the CodeBlocks stay.
    deleteUnlinkedCode: () => $vm.returnCodeToBytecodeCacheWhenIdle(),
    collect: () => { fullGC(); fullGC(); },
    none: () => { },
};

let uniqueSources = 0;
// Two linked executables over one unlinked function. The comment keeps one scenario's code apart from the next one's.
function twice(source) {
    source = source.replace("{", "{ /* " + uniqueSources++ + " */");
    return [(0, eval)(source), (0, eval)(source)];
}

function scenario(name, setup, steps, expected) {
    scenarios.push({ name, setup, steps, expected });
}

function run(index) {
    if (index === scenarios.length) {
        if (failures.length)
            throw new Error(failures.join("\n"));
        return;
    }
    const { name, setup, steps, expected } = scenarios[index];
    const out = [];
    const state = setup(out);
    let at = 0;
    const next = () => {
        if (at === steps.length) {
            const got = out.join(" ");
            if (got !== expected)
                failures.push(name + ": " + got + ", expected " + expected);
            // Everything off again for the next scenario.
            modeChanges.debuggerOff();
            modeChanges.typeProfilerOff();
            modeChanges.controlFlowProfilerOff();
            setTimeout(() => run(index + 1), 0);
            return;
        }
        const [change, step] = steps[at++];
        modeChanges[change]();
        // (A rejection is thrown from a task of its own: the shell does not report one that nothing handles.)
        setTimeout(() => Promise.resolve().then(() => step(state, out)).then(next).catch(error => setTimeout(() => { throw error; }, 0)), 0);
    };
    next();
}

const counter = "(function* gen(tag) { let n = 0; while (true) { const got = yield tag + n; n += got; } })";
const asyncCounter = "(async function* gen(tag) { let n = 0; while (true) { const got = yield tag + n; n += got; } })";
// The type profiler's hooks use x, y and z where the plain code does not, so more of them is alive across each yield.
const shaped = "(function* gen(a) { let y = a; let x; x = ++y + (yield 1); let z = x * 2; const w = yield z; return x + y + z + w; })";
// `var later;` is a use of later's register only with the type profiler's hooks.
const declared = "(function* gen(tag) { const first = yield tag + 1; var later; const second = yield first + 1; later = second * 2; const third = yield later; return [first, second, later, third].join(); })";
const incremented = "(function* gen(tag) { let x = 0; let a = (++x) + (yield tag + 1); let b = yield a; return [x, a, b].join(); })";
// Bodies whose generator frame the control flow profiler's instruction changes. op_profile_control_flow has no register
// operand, but between two stores to one register it keeps the first one, which BytecodeGenerator::emitMove drops
// otherwise, and with it a use of that store's source: one more register is saved at the yield before it.
function sink(f) { return f; }
// The finally block starts with `break`: the scope register is restored on entry to the block and again by the break.
const handlerStartsWithBreak = "(function* gen(tag) { let top = 0; sink(() => top); let keep = tag + 'K'; let tail; for (;;) { let outer = 1; sink(() => outer); try { { let inner = 2; sink(() => inner); } const got = yield keep; keep = keep + got; } finally { break; } } tail = tag + 'T'; const last = yield keep; return keep + '/' + last + '/' + tail; })";
// A block ends with a copy, and the next statement stores to the same register.
const blockEndsWithCopy = "(function* gen(tag) { let a = tag + 'A'; let x; const got = yield 1; { x = a; } x = got; const last = yield x; return x + '/' + last; })";
const framesTheControlFlowProfilerChanges = [
    ["a handler that starts with break", handlerStartsWithBreak, "aK aK7 bK aK7/9/aT bK5 bK5/6/bT"],
    ["a block that ends with a copy", blockEndsWithCopy, "1 7 1 7/9 5 5/6"],
];
// Says from inside the body whether the code it runs in has the debug hooks.
const hooked = "(function* gen(tag) { let n = 0; while (true) { const got = yield tag + n + ':' + $vm.wasCompiledWithDebuggerHooks($vm.codeBlockForFrame(0)); n += got; } })";

// --- A debugger.

scenario("debugger on, off, on: generator", () => {
    const [first, second] = twice(counter);
    return { a: first("a"), b: second("b") };
}, [
    ["none", ({ a }, out) => { out.push(a.next().value); }], // suspended in code without debug hooks
    ["debuggerOn", ({ a, b }, out) => { out.push(b.next().value, a.next(5).value); }], // b generates the body first
    ["debuggerOff", ({ a, b }, out) => { out.push(b.next(3).value, a.next(1).value); }],
    ["debuggerOn", ({ a, b }, out) => { out.push(a.next(2).value, b.next(4).value); }],
    ["deleteAllCode", ({ a, b }, out) => { out.push(b.next(1).value, a.next(1).value); }],
], "a0 b0 a5 b3 a6 a8 b7 b8 a9");

scenario("debugger on, off: async generator", () => {
    const [first, second] = twice(asyncCounter);
    return { a: first("a"), b: second("b") };
}, [
    ["none", async ({ a }, out) => { out.push((await a.next()).value); }],
    ["debuggerOn", async ({ a, b }, out) => { out.push((await b.next()).value, (await a.next(5)).value); }],
    ["debuggerOff", async ({ a, b }, out) => { out.push((await b.next(3)).value, (await a.next(1)).value); }],
], "a0 b0 a5 b3 a6");

scenario("debugger on, off: async function", () => {
    const [first, second] = twice("(async function sum(tag, gates) { let n = 0; for (const gate of gates) n += await gate.promise; return tag + n; })");
    const gates = () => [Promise.withResolvers(), Promise.withResolvers()];
    const state = { first, second, aGates: gates(), bGates: gates() };
    state.a = first("a", state.aGates); // suspended at its first await, in code without debug hooks
    return state;
}, [
    ["debuggerOn", async (state, out) => {
        state.b = state.second("b", state.bGates); // generates the body first
        state.aGates[0].resolve(5);
        state.bGates[0].resolve(3);
        await 0;
    }],
    ["debuggerOff", async (state, out) => {
        state.bGates[1].resolve(4);
        state.aGates[1].resolve(1);
        out.push(await state.a, await state.b);
    }],
], "a6 b7");

// The wrapper's CodeBlock dies of old age and is linked again: a second linked body over the one unlinked body, with
// no second evaluation. (Only with --forceCodeBlockToJettisonDueToOldAge=1 does the collection take it.)
scenario("debugger on: body linked again after its wrapper was jettisoned", () => {
    const make = (0, eval)("(function make(tag) { /* relinked */ return (function* gen() { let n = 0; while (true) { const got = yield tag + n; n += got; } })(); })");
    return { make, a: make("a") };
}, [
    ["collect", (state, out) => { out.push(state.a.next().value); }],
    ["collect", (state, out) => { state.b = state.make("b"); }],
    ["debuggerOn", ({ a, b }, out) => { out.push(b.next().value, a.next(5).value, b.next(3).value); }],
], "a0 b0 a5 b3");

// A body that starts with the debugger on has the debug hooks, and one that started before has none, whichever of the two
// runs first after the change.
scenario("debugger on: the suspended body resumes first, the other one starts with the debug hooks", () => {
    const [first, second] = twice(hooked);
    return { a: first("a"), b: second("b") };
}, [
    ["none", ({ a }, out) => { out.push(a.next().value); }],
    ["debuggerOn", ({ a, b }, out) => { out.push(a.next(5).value, b.next().value, b.next(3).value, a.next(1).value); }],
    ["debuggerOff", ({ a, b }, out) => { out.push(a.next(1).value, b.next(1).value); }],
], "a0:false a5:false b0:true b3:true a6:false a7:false b4:true");

scenario("debugger on: the other one starts first, the suspended body resumes without the debug hooks", () => {
    const [first, second] = twice(hooked);
    return { a: first("a"), b: second("b") };
}, [
    ["none", ({ a }, out) => { out.push(a.next().value); }],
    ["debuggerOn", ({ a, b }, out) => { out.push(b.next().value, a.next(5).value, b.next(3).value); }],
], "a0:false b0:true a5:false b3:true");

// --- A body keeps the code it started in for as long as one of its activations is suspended, and no longer: when
// unlinked code is deleted and no activation has a generator frame, the code goes like that of any other function.

// One wrapper, so one linked executable of the body for all the generators it makes.
function once(source) {
    return (0, eval)(source.replace("{", "{ /* " + uniqueSources++ + " */"));
}

scenario("debugger on: a body whose only activation has finished starts with the debug hooks", () => {
    const make = once(hooked);
    return { finished: make("f"), waiting: make("w") };
}, [
    ["none", ({ finished }, out) => { out.push(finished.next().value, finished.return("end").value); }],
    ["debuggerOn", ({ waiting }, out) => { out.push(waiting.next().value, waiting.next(2).value); }], // made before, starts now
], "f0:false end w0:true w2:true");

scenario("debugger on: a body keeps its code while an activation is suspended, and starts with the debug hooks once none is", () => {
    const make = once(hooked);
    return { a: make("a"), b: make("b"), c: make("c") };
}, [
    ["none", ({ a }, out) => { out.push(a.next().value); }],
    // b starts in the code that a is suspended in: one executable has one CodeBlock.
    ["debuggerOn", ({ a, b }, out) => { out.push(b.next().value, a.next(5).value, a.return("end").value, b.return("end").value); }],
    ["deleteAllCode", ({ c }, out) => { out.push(c.next().value); }],
], "a0:false b0:false a5:false end end c0:true");

// The CodeBlock of the body stays, and an activation starts in it afterwards: it is suspended in the code that the
// executable had before, and resumes in it.
scenario("unlinked code deleted, CodeBlocks kept; a body starts; debugger on", () => {
    const make = once(hooked);
    return { finished: make("f"), waiting: make("w") };
}, [
    ["none", ({ finished }, out) => { out.push(finished.next().value, finished.return("end").value); }],
    ["deleteUnlinkedCode", ({ waiting }, out) => { out.push(waiting.next().value); }],
    ["debuggerOn", ({ waiting }, out) => { out.push(waiting.next(5).value); }],
], "f0:false end w0:false w5:false");

// Iterator.zip is a generator of the engine's own, and the type profiler changes its frame. It is suspended between two
// of its results.
scenario("type profiler on: a builtin generator that is suspended", () => {
    return { zipped: Iterator.zip([[1, 2, 3], [4, 5, 6]]) };
}, [
    ["none", ({ zipped }, out) => { out.push(zipped.next().value.join("+")); }],
    ["typeProfilerOn", ({ zipped }, out) => { for (const pair of zipped) out.push(pair.join("+")); }],
], "1+4 2+5 3+6");

// --- The profilers. `on` and `off` are a profiler's two mode changes.

function profilerScenarios(profiler, on, off) {
    scenario(profiler + " on: the other executable runs the body first", () => {
        const [first, second] = twice(shaped);
        return { a: first(10), b: second(10) };
    }, [
        ["none", ({ a }, out) => { out.push(a.next().value, a.next(5).value); }],
        [on, ({ a, b }, out) => { out.push(b.next().value, a.next(7).value, b.next(5).value, b.next(7).value); }],
    ], "1 32 1 66 32 66");

    scenario(profiler + " on: the suspended body resumes first, the other one starts; off", () => {
        const [first, second] = twice(shaped);
        return { a: first(10), b: second(10) };
    }, [
        ["none", ({ a }, out) => { out.push(a.next().value); }],
        [on, ({ a, b }, out) => { out.push(a.next(5).value, b.next().value); }],
        [off, ({ a, b }, out) => { out.push(b.next(5).value, a.next(7).value, b.next(7).value); }],
    ], "1 32 1 32 66 66");

    scenario(profiler + " on, a body starts; off, the other executable runs first", () => {
        const [first, second] = twice(shaped);
        return { first, second };
    }, [
        [on, (state, out) => { state.a = state.first(10); state.b = state.second(10); out.push(state.a.next().value, state.a.next(5).value); }],
        [off, ({ a, b }, out) => { out.push(b.next().value, a.next(7).value, b.next(5).value, b.next(7).value); }],
    ], "1 32 1 66 32 66");

    for (const order of ["BA", "AB"]) {
        scenario(profiler + " on with a body suspended at its third yield, then " + order, () => {
            const [first, second] = twice(declared);
            return { a: first("a"), b: second("b") };
        }, [
            ["none", ({ a }, out) => { out.push(a.next().value, a.next(10).value, a.next(20).value); }],
            [on, ({ a, b }, out) => {
                if (order === "BA")
                    out.push(b.next().value, a.next(30).value);
                else
                    out.push(a.next(30).value, b.next().value);
                out.push(b.next(10).value, b.next(20).value, b.next(30).value);
            }],
        ], order === "BA" ? "a1 11 40 b1 10,20,40,30 11 40 10,20,40,30" : "a1 11 40 10,20,40,30 b1 11 40 10,20,40,30");

        scenario(profiler + " on, a body starts; off, then " + order, () => {
            const [first, second] = twice(incremented);
            return { a: first("a"), b: second("b") };
        }, [
            [on, ({ a }, out) => { out.push(a.next().value, a.next(10).value); }],
            [off, ({ a, b }, out) => {
                if (order === "BA")
                    out.push(b.next().value, a.next(100).value);
                else
                    out.push(a.next(100).value, b.next().value);
                out.push(b.next(10).value, b.next(100).value);
            }],
        ], order === "BA" ? "a1 11 b1 1,11,100 11 1,11,100" : "a1 11 1,11,100 b1 11 1,11,100");

        scenario(profiler + " on, a body starts at its first yield; off, then " + order, () => {
            const [first, second] = twice(counter);
            return { a: first("a"), b: second("b") };
        }, [
            [on, ({ a }, out) => { out.push(a.next().value); }],
            [off, ({ a, b }, out) => {
                if (order === "BA")
                    out.push(b.next().value, a.next(5).value);
                else
                    out.push(a.next(5).value, b.next().value);
                out.push(a.next(1).value, b.next(2).value);
            }],
        ], order === "BA" ? "a0 b0 a5 a6 b2" : "a0 a5 b0 a6 b2");
    }

    scenario(profiler + " on: the suspended body's code takes the slot, the other one starts, all code is deleted", () => {
        const [first, second] = twice(incremented);
        return { a: first("a"), b: second("b") };
    }, [
        ["none", ({ a }, out) => { out.push(a.next().value, a.next(10).value); }],
        [on, ({ a, b }, out) => { out.push(a.next(100).value, b.next().value, b.next(10).value); }],
        ["deleteAllCode", ({ b }, out) => { out.push(b.next(100).value); }],
    ], "a1 11 1,11,100 b1 11 1,11,100");

    // One linked executable: what it started in has to be there to resume in after the profiler is turned off.
    scenario(profiler + " on, a body starts; off; it resumes", () => {
        const [only] = twice(shaped);
        return { only };
    }, [
        [on, (state, out) => { state.a = state.only(10); out.push(state.a.next().value); }],
        [off, ({ a }, out) => { out.push(a.next(5).value, a.next(7).value); }],
        [on, (state, out) => { state.b = state.only(10); out.push(state.b.next().value, state.b.next(5).value, state.b.next(7).value); }],
    ], "1 32 66 1 32 66");

    for (const [shape, source, expected] of framesTheControlFlowProfilerChanges) {
        scenario(profiler + " on: " + shape + ", the other executable runs the body first", () => {
            const [first, second] = twice(source);
            return { a: first("a"), b: second("b") };
        }, [
            ["none", ({ a }, out) => { out.push(a.next().value, a.next(7).value); }],
            [on, ({ a, b }, out) => { out.push(b.next().value, a.next(9).value, b.next(5).value, b.next(6).value); }],
        ], expected);

        scenario(profiler + " on, " + shape + " starts; off, the other executable runs the body first", () => {
            const [first, second] = twice(source);
            return { first, second };
        }, [
            [on, (state, out) => { state.a = state.first("a"); state.b = state.second("b"); out.push(state.a.next().value, state.a.next(7).value); }],
            [off, ({ a, b }, out) => { out.push(b.next().value, a.next(9).value, b.next(5).value, b.next(6).value); }],
        ], expected);
    }
}

profilerScenarios("type profiler", "typeProfilerOn", "typeProfilerOff");
profilerScenarios("control flow profiler", "controlFlowProfilerOn", "controlFlowProfilerOff");

// The second executable has not run when the profiler is turned on, so it asks for the profiler's mode while the slot
// still holds the code the first one is suspended in.
scenario("control flow profiler on with the code that exists kept: the other executable starts", () => {
    const [first, second] = twice(shaped);
    return { a: first(10), b: second(10) };
}, [
    ["none", ({ a }, out) => { out.push(a.next().value, a.next(5).value); }],
    ["controlFlowProfilerOnCodeKept", ({ a, b }, out) => { out.push(b.next().value, a.next(7).value, b.next(5).value, b.next(7).value); }],
], "1 32 1 66 32 66");

// What an embedder's late start of coverage followed by a deletion of all code does: the executable that has not run
// generates the body first, for the profiler.
for (const [shape, source, expected] of framesTheControlFlowProfilerChanges) {
    scenario("control flow profiler on with the code that exists kept, then all code is deleted: " + shape, () => {
        const [first, second] = twice(source);
        return { a: first("a"), b: second("b") };
    }, [
        ["none", ({ a }, out) => { out.push(a.next().value, a.next(7).value); }],
        ["controlFlowProfilerOnCodeKept", () => { }],
        ["deleteAllCode", ({ a, b }, out) => { out.push(b.next().value, a.next(9).value, b.next(5).value, b.next(6).value); }],
    ], expected);
}

// With the code kept, a function that has code keeps running it, in every CodeBlock made for it: only a function that
// is generated after the profiler was turned on has the profiler's instructions.
scenario("control flow profiler on with the code that exists kept: a function that has code keeps it", () => {
    const [first, second] = twice("(function plain(a) { return a + 1; })");
    return { first, second };
}, [
    ["none", ({ first }, out) => { out.push(first(1)); }],
    ["controlFlowProfilerOnCodeKept", ({ second }, out) => {
        const later = (0, eval)("(function later(a) { return a + 2; })");
        out.push(second(2), $vm.wasCompiledWithControlFlowProfilerHooks(second), later(3), $vm.wasCompiledWithControlFlowProfilerHooks(later));
    }],
], "2 3 false 5 true");

// Two realms and the type profiler, which is the VM's: the other realm's body is suspended, this realm runs it first.
scenario("type profiler on: two realms", () => {
    const source = "return " + shaped.slice(1, -1).replace("{", "{ /* two realms */");
    const other = createGlobalObject();
    return { mine: new Function(source)()(10), theirs: new other.Function(source)()(10) };
}, [
    ["none", ({ theirs }, out) => { out.push(theirs.next().value, theirs.next(5).value); }],
    ["typeProfilerOn", ({ mine, theirs }, out) => { out.push(mine.next().value, theirs.next(7).value, mine.next(5).value, mine.next(7).value); }],
], "1 32 1 66 32 66");

run(0);
