//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTGuardsOverWholeFunctionsWithHandlers=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTGuardsOverWholeFunctionsWithHandlers=1", "--minimumAOTGuardsOverWholeFunction=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTGuardsOverWholeFunctionsWithHandlers=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTGuardsOverWholeFunctionsWithHandlers=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTGuardsOverWholeFunctionsWithHandlers=1", "--useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTGuardsOverWholeFunctionsWithHandlers=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTGuardsOverWholeFunctionsWithHandlers=1", "--useMiniVMModeWithoutJIT=0", "--slowPathAllocsBetweenGCs=31")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(f.name) || null;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isCompiled = !!remarksOf(check);
const hasTwins = isCompiled && !!options.useAOTGuardsOverWholeFunctions && !!options.useAOTDataStubs && !!options.useAOTGuessedPlaces;
const isOn = hasTwins && !!options.useAOTGuardsOverWholeFunctionsWithHandlers;
const isCounting = isOn && !!options.useAOTOperationCounters;
const exits = () => isCounting ? (aotOperationCount("operationAOTCountGuessedPlace:exit-into-generic-copy") || 0) + (aotOperationCount("operationAOTCountGuessedStore:exit-into-generic-copy") || 0) : 0;

const kept = [];
let numberKept = 0;
function keep(o) { kept[numberKept++ % 256] = o; return o; }
function Item(which) {
    this.tag = "tag" + which;
    this.key = "key" + which;
    this.child = "child" + which;
    this.sibling = "sibling" + which;
    this.flags = "flags" + which;
}
const names = ["tag", "key", "child", "sibling", "flags"];
const name = index => names[index % names.length];

let log = [];
function note(what) { log.push(what); return what; }
function throwIf(when, position) {
    log.push(position);
    if (when === position)
        throw "thrown at " + position;
}
function describe(error) { return typeof error === "string" ? error : error.constructor.name; }
noInline(note);
noInline(throwIf);
noInline(describe);

function readsInTry(a, b, c, when) {
    let first = "no first", second = "no second", third = "no third", stage = "start", count = 0;
    try {
        stage = "before a"; ++count;
        throwIf(when, 1);
        first = a.tag;
        stage = "after a"; ++count;
        throwIf(when, 2);
        second = b.key;
        stage = "after b"; ++count;
        throwIf(when, 3);
        third = c.child;
        stage = "after c"; ++count;
        throwIf(when, 4);
    } catch (error) {
        return "caught " + describe(error) + " at " + stage + " " + count + ": " + first + "," + second + "," + third;
    }
    return "done at " + stage + " " + count + ": " + first + "," + second + "," + third;
}
function readsInTryByKey(a, b, c, when) {
    let first = "no first", second = "no second", third = "no third", stage = "start", count = 0;
    try {
        stage = "before a"; ++count;
        throwIf(when, 1);
        first = a[name(0)];
        stage = "after a"; ++count;
        throwIf(when, 2);
        second = b[name(1)];
        stage = "after b"; ++count;
        throwIf(when, 3);
        third = c[name(2)];
        stage = "after c"; ++count;
        throwIf(when, 4);
    } catch (error) {
        return "caught " + describe(error) + " at " + stage + " " + count + ": " + first + "," + second + "," + third;
    }
    return "done at " + stage + " " + count + ": " + first + "," + second + "," + third;
}

function storesInTry(a, b, c, when) {
    let stage = 0;
    try {
        a.tag = "stored 1"; stage = 1;
        throwIf(when, 1);
        b.key = "stored 2"; stage = 2;
        throwIf(when, 2);
        c.child = "stored 3"; stage = 3;
        throwIf(when, 3);
    } catch (error) {
        return "caught " + describe(error) + " at " + stage;
    }
    return "done at " + stage;
}
function storesInTryByKey(a, b, c, when) {
    let stage = 0;
    try {
        a[name(0)] = "stored 1"; stage = 1;
        throwIf(when, 1);
        b[name(1)] = "stored 2"; stage = 2;
        throwIf(when, 2);
        c[name(2)] = "stored 3"; stage = 3;
        throwIf(when, 3);
    } catch (error) {
        return "caught " + describe(error) + " at " + stage;
    }
    return "done at " + stage;
}

function goesOnBehindCatch(a, b, c, when) {
    let before = a.tag, inside = "no inside", caught = "nothing";
    try {
        throwIf(when, 1);
        inside = b.key;
        throwIf(when, 2);
    } catch (error) {
        caught = describe(error);
    }
    let after = c.child;
    throwIf(when, 3);
    let again = a.sibling;
    let last = b.flags;
    return [before, inside, caught, after, again, last].join();
}
function goesOnBehindCatchByKey(a, b, c, when) {
    let before = a[name(0)], inside = "no inside", caught = "nothing";
    try {
        throwIf(when, 1);
        inside = b[name(1)];
        throwIf(when, 2);
    } catch (error) {
        caught = describe(error);
    }
    let after = c[name(2)];
    throwIf(when, 3);
    let again = a[name(3)];
    let last = b[name(4)];
    return [before, inside, caught, after, again, last].join();
}

function hasFinally(a, b, c, when) {
    let seen = "";
    try {
        seen += a.tag;
        throwIf(when, 1);
        seen += b.key;
        if (when === 5)
            return note("returned " + seen);
        throwIf(when, 2);
    } finally {
        seen += c.child;
        note("finally " + seen);
        throwIf(when, 3);
        seen += a.flags;
        note("end of finally " + seen);
    }
    return seen + b.sibling;
}
function hasFinallyByKey(a, b, c, when) {
    let seen = "";
    try {
        seen += a[name(0)];
        throwIf(when, 1);
        seen += b[name(1)];
        if (when === 5)
            return note("returned " + seen);
        throwIf(when, 2);
    } finally {
        seen += c[name(2)];
        note("finally " + seen);
        throwIf(when, 3);
        seen += a[name(4)];
        note("end of finally " + seen);
    }
    return seen + b[name(3)];
}

function isNested(a, b, c, when) {
    let outer = "no outer", inner = "no inner", path = "";
    try {
        outer = a.tag; path += "1";
        try {
            throwIf(when, 1);
            inner = b.key; path += "2";
            throwIf(when, 2);
        } catch (error) {
            path += "(" + describe(error) + ")";
            inner += c.child;
            if (when === 2)
                throw "again " + inner;
        }
        path += "3";
        outer += c.sibling;
        throwIf(when, 3);
    } catch (error) {
        return "outer caught " + describe(error) + " " + path + " " + outer + " " + inner;
    }
    return path + " " + outer + " " + inner + " " + a.flags;
}
function isNestedByKey(a, b, c, when) {
    let outer = "no outer", inner = "no inner", path = "";
    try {
        outer = a[name(0)]; path += "1";
        try {
            throwIf(when, 1);
            inner = b[name(1)]; path += "2";
            throwIf(when, 2);
        } catch (error) {
            path += "(" + describe(error) + ")";
            inner += c[name(2)];
            if (when === 2)
                throw "again " + inner;
        }
        path += "3";
        outer += c[name(3)];
        throwIf(when, 3);
    } catch (error) {
        return "outer caught " + describe(error) + " " + path + " " + outer + " " + inner;
    }
    return path + " " + outer + " " + inner + " " + a[name(4)];
}

function catchesInLoop(items, when) {
    let result = "", caught = 0, previous = "none";
    for (let i = 0; i < items.length; ++i) {
        let item = items[i];
        try {
            throwIf(when, i);
            result += item.tag;
            previous = item.key;
            item.flags = "seen " + i;
            throwIf(when, i + 100);
        } catch (error) {
            ++caught;
            result += "<" + describe(error) + " after " + previous + ">";
        }
    }
    return result + " " + caught + " " + previous;
}
function catchesInLoopByKey(items, when) {
    let result = "", caught = 0, previous = "none";
    for (let i = 0; i < items.length; ++i) {
        let item = items[i];
        try {
            throwIf(when, i);
            result += item[name(0)];
            previous = item[name(1)];
            item[name(4)] = "seen " + i;
            throwIf(when, i + 100);
        } catch (error) {
            ++caught;
            result += "<" + describe(error) + " after " + previous + ">";
        }
    }
    return result + " " + caught + " " + previous;
}

function keepsManyRegisters(a, b, c, when) {
    let r0 = 0, r1 = "one", r2 = 2.5, r3 = null, r4 = undefined, r5 = true, r6 = a, r7 = [when], r8 = { when }, r9 = -0;
    try {
        r0 = 10; r1 = a.tag;
        throwIf(when, 1);
        r2 = 3.5; r3 = b.key; r4 = b;
        throwIf(when, 2);
        r5 = false; r6 = c.child; r7 = [r0, r2];
        throwIf(when, 3);
        r8 = c.sibling; r9 = r0 / 4;
        throwIf(when, 4);
    } catch (error) {
        return [describe(error), r0, r1, r2, String(r3), r4 === b ? "b" : String(r4), r5, r6 === a ? "a" : r6, r7.join("+"), typeof r8 === "object" ? "when " + r8.when : r8, Object.is(r9, -0) ? "-0" : r9].join();
    }
    return ["done", r0, r1, r2, r3, r5, r6, r7.join("+"), r8, r9].join();
}
function keepsManyRegistersByKey(a, b, c, when) {
    let r0 = 0, r1 = "one", r2 = 2.5, r3 = null, r4 = undefined, r5 = true, r6 = a, r7 = [when], r8 = { when }, r9 = -0;
    try {
        r0 = 10; r1 = a[name(0)];
        throwIf(when, 1);
        r2 = 3.5; r3 = b[name(1)]; r4 = b;
        throwIf(when, 2);
        r5 = false; r6 = c[name(2)]; r7 = [r0, r2];
        throwIf(when, 3);
        r8 = c[name(3)]; r9 = r0 / 4;
        throwIf(when, 4);
    } catch (error) {
        return [describe(error), r0, r1, r2, String(r3), r4 === b ? "b" : String(r4), r5, r6 === a ? "a" : r6, r7.join("+"), typeof r8 === "object" ? "when " + r8.when : r8, Object.is(r9, -0) ? "-0" : r9].join();
    }
    return ["done", r0, r1, r2, r3, r5, r6, r7.join("+"), r8, r9].join();
}

function capturesWhatItCatches(a, b, c, when) {
    let tellers = [];
    let first = a.tag;
    try {
        throwIf(when, 1);
        first += b.key;
        throwIf(when, 2);
    } catch (error) {
        tellers.push(() => describe(error) + " " + first);
    }
    first += c.child + a.sibling;
    return tellers.map(teller => teller()).join() + "|" + first;
}
function capturesWhatItCatchesByKey(a, b, c, when) {
    let tellers = [];
    let first = a[name(0)];
    try {
        throwIf(when, 1);
        first += b[name(1)];
        throwIf(when, 2);
    } catch (error) {
        tellers.push(() => describe(error) + " " + first);
    }
    first += c[name(2)] + a[name(3)];
    return tellers.map(teller => teller()).join() + "|" + first;
}

const pairs = [
    [readsInTry, readsInTryByKey], [storesInTry, storesInTryByKey], [goesOnBehindCatch, goesOnBehindCatchByKey], [hasFinally, hasFinallyByKey],
    [isNested, isNestedByKey], [keepsManyRegisters, keepsManyRegistersByKey], [capturesWhatItCatches, capturesWhatItCatchesByKey],
];
for (const [guarded, byKey] of [...pairs, [catchesInLoop, catchesInLoopByKey]]) {
    noInline(guarded);
    noInline(byKey);
}

const receivers = {
    item: which => keep(new Item(which)),
    moved: which => {
        const o = { other: 0 };
        for (let i = names.length; i--;)
            o[names[i]] = "moved " + names[i] + which;
        return keep(o);
    },
    empty: which => keep({ }),
    heir: which => keep(Object.create(new Item("inherited" + which))),
    throwing: which => {
        const o = { };
        for (const property of names)
            Object.defineProperty(o, property, { get() { note("get " + property); throw "getter of " + property; }, set(v) { note("set " + property); throw "setter of " + property; }, configurable: true });
        return keep(o);
    },
    logging: which => {
        const o = { };
        for (const property of names)
            Object.defineProperty(o, property, { get() { return note("got " + property + which); }, set(v) { note("set " + property + " to " + v); }, configurable: true });
        return keep(o);
    },
    frozen: which => keep(Object.freeze(new Item("frozen" + which))),
    nothing: which => null,
    number: which => 7,
};
const kinds = Object.keys(receivers);
const stateOf = o => o === null || typeof o !== "object" ? String(o) : names.map(property => { const descriptor = Object.getOwnPropertyDescriptor(o, property); return descriptor && "value" in descriptor ? descriptor.value : "-"; }).join("/");

function run(f, make, when) {
    log = [];
    const operands = make();
    let outcome;
    try {
        outcome = "returns " + f(...operands, when);
    } catch (error) {
        outcome = "throws " + describe(error);
    }
    return outcome + " | " + log.join(";") + " | " + operands.flat().map(stateOf).join(" ");
}

for (let round = 0; round < 3; ++round) {
    for (const [guarded, byKey] of pairs) {
        for (let when = 0; when <= 5; ++when)
            check(run(guarded, () => [receivers.item(1), receivers.item(2), receivers.item(3)], when), run(byKey, () => [receivers.item(1), receivers.item(2), receivers.item(3)], when), guarded.name + " with its own kind, " + when);
    }
}
{
    const before = exits();
    for (const [guarded, byKey] of pairs) {
        for (let when = 0; when <= 5; ++when)
            run(guarded, () => [receivers.item(1), receivers.item(2), receivers.item(3)], when);
    }
    check(exits() - before, 0, "exits with receivers of the guessed kind");
}
for (const [guarded, byKey] of pairs) {
    for (const first of kinds) {
        for (const second of kinds) {
            for (const third of first === "item" || second === "item" ? kinds : ["item", "moved", "throwing"]) {
                for (let when = 0; when <= 5; ++when) {
                    const make = () => [receivers[first](1), receivers[second](2), receivers[third](3)];
                    check(run(guarded, make, when), run(byKey, make, when), [guarded.name, first, second, third, when].join(" "));
                }
            }
        }
    }
}
if (isCounting) {
    for (const [guarded, byKey] of pairs) {
        const before = exits();
        run(guarded, () => [receivers.item(1), receivers.moved(2), receivers.item(3)], 0);
        if (remarksOf(guarded).includes("guards-over-whole-function") && exits() === before)
            throw new Error(guarded.name + " did not leave its fast copy for a receiver of another kind");
    }
}

for (const length of [0, 1, 2, 5, 9]) {
    for (const odd of kinds) {
        for (let position = 0; position <= length; ++position) {
            for (const when of [-1, 0, 1, position, position + 1, 100, 100 + position, 101 + position]) {
                const make = () => {
                    const items = [];
                    for (let i = 0; i < length; ++i)
                        items.push(i === position ? receivers[odd](i) : receivers.item(i));
                    return [items];
                };
                check(run(catchesInLoop, make, when), run(catchesInLoopByKey, make, when), ["catchesInLoop", length, odd, position, when].join(" "));
            }
        }
    }
}

if (isCompiled) {
    const about = f => remarksOf(f).filter(remark => remark.includes("guards-over-whole-function")).join(" ");
    for (const [guarded, byKey] of [...pairs, [catchesInLoop, catchesInLoopByKey]]) {
        const hasGuards = remarksOf(guarded).includes("guards-over-whole-function");
        if (remarksOf(byKey).includes("guards-over-whole-function"))
            throw new Error("guards over the whole of " + byKey.name);
        if (!hasTwins) {
            if (hasGuards)
                throw new Error("guards over the whole of " + guarded.name + " though the option is off");
            continue;
        }
        if (!isOn) {
            if (hasGuards || !remarksOf(guarded).includes("no-guards-over-whole-function:has-handler"))
                throw new Error(guarded.name + " has a handler and must be refused for it: " + about(guarded));
            continue;
        }
        if (remarksOf(guarded).includes("no-guards-over-whole-function:has-handler"))
            throw new Error(guarded.name + " is refused for its handler: " + about(guarded));
        const mayBeRefusedFor = guarded === capturesWhatItCatches ? ["makes-environment-behind-guard", "too-few-places"] : guarded === catchesInLoop ? ["has-loop-without-calls"] : [];
        if (!hasGuards && !mayBeRefusedFor.some(reason => remarksOf(guarded).includes("no-guards-over-whole-function:" + reason)))
            throw new Error("no guards over the whole of " + guarded.name + ": " + about(guarded));
    }
}
