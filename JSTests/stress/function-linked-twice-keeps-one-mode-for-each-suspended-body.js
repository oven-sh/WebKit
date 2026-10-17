//@ runDefault("--useDollarVM=1")
//@ runDefault("--useDollarVM=1", "--forceCodeBlockToJettisonDueToOldAge=1")
//@ runDefault("--useDollarVM=1", "--useJIT=0")

// One realm, one function evaluated twice: the two linked executables share the unlinked one (the CodeCache gives the same
// code for the same source), and with it the slot its code is kept in. A mode change (a debugger, the type profiler, the
// control flow profiler) deletes all code; whichever of the two runs next generates the function for the mode it asks
// for, and the other one must not be handed that code when it asks for another mode: the body of a generator or an
// async function that is suspended keeps the mode it started in (ScriptExecutable::codeGenerationModeForResumableBody),
// because its registers are in a generator frame that only code generated the same way reads back. The type profiler
// changes what is alive across a yield, the debugger where every variable lives.
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
        setTimeout(() => Promise.resolve(step(state, out)).then(next), 0);
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
