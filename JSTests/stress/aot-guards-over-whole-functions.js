//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--minimumAOTGuardsOverWholeFunction=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTOperationCounters=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTInlining=0", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTFamilies=1", "--useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting=1", "--useAOTOperationCounters=1", "--useDollarVM=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useDollarVM=1")
//@ runDefault("--useDollarVM=1")

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const nameOf = f => typeof f === "string" ? f : f.name;
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(nameOf(f)) || null;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isCompiled = !!remarksOf(check);
if (isCompiled)
    check("useAOTGuardsOverWholeFunctions" in options, true, "the option exists");
const isOn = isCompiled && !!options.useAOTGuardsOverWholeFunctions && !!options.useAOTDataStubs && !!options.useAOTFamilies;
const isCounting = isOn && !!options.useAOTOperationCounters;
const keepsLoopsWhole = isOn && !!options.useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting;
const minimum = options.minimumAOTGuardsOverWholeFunction;
const hasGuards = f => remarksOf(f).includes("guards-over-whole-function");
function applies(...functions) {
    for (let f of functions) {
        if (isOn && !hasGuards(f))
            throw new Error("no guards over the whole of " + nameOf(f) + ": " + remarksOf(f).join(" "));
        if (isCompiled && !isOn && hasGuards(f))
            throw new Error("guards over the whole of " + nameOf(f) + " though the option is off");
    }
}
function doesNotApply(f, ...reasons) {
    if (!isCompiled)
        return;
    if (hasGuards(f))
        throw new Error("guards over the whole of " + nameOf(f) + ": " + remarksOf(f).join(" "));
    if (isOn && !reasons.some(reason => remarksOf(f).includes("no-guards-over-whole-function:" + reason)))
        throw new Error(nameOf(f) + " is refused for none of " + reasons.join(", ") + ": " + remarksOf(f).filter(remark => remark.startsWith("no-guards")).join(" "));
}
function guards(f, ...remarks) {
    for (let remark of remarks) {
        if (isOn && !remarksOf(f).includes(remark))
            throw new Error(nameOf(f) + " lacks " + remark + ": " + remarksOf(f).join(" "));
    }
}
function lacksGuards(f, ...remarks) {
    for (let remark of remarks) {
        if (isCompiled && remarksOf(f).includes(remark))
            throw new Error(nameOf(f) + " has " + remark);
    }
}
const exits = () => isCounting ? ["exits-not-a-cell", "exits-without-number", "exits-with-another-number", "exits-departed"].reduce((sum, outcome) => sum + (aotOperationCount("exit-into-generic-copy:" + outcome) || 0), 0) : 0;

const kept = [];
function keep(o) { kept.push(o); return o; }
function build(...pairs) {
    let o = {};
    for (let [name, value] of pairs)
        o[name] = value;
    return keep(o);
}
function Item(tag, key, child, sibling, flags) {
    this.tag = tag;
    this.key = key;
    this.child = child;
    this.sibling = sibling;
    this.flags = flags;
}
function makeItem(which) { return keep(new Item("tag" + which, "key" + which, "child" + which, "sibling" + which, "flags" + which)); }

let log = [];
let position = 0;
function note(what) { log.push(what); return what; }
noInline(note);

function sixAccesses(a, b, c, d, e, f) {
    note("before 1"); position = 1;
    let first = a.tag;
    note("after 1"); position = 2;
    b.key = "stored 2";
    note("after 2"); position = 3;
    let third = c.child;
    note("after 3"); position = 4;
    d.flags = "stored 4";
    note("after 4"); position = 5;
    let fifth = e.sibling;
    note("after 5"); position = 6;
    f.tag = "stored 6";
    note("after 6"); position = 7;
    return first + "|" + third + "|" + fifth;
}
function sixAccessesStrictly(a, b, c, d, e, f) {
    "use strict";
    note("before 1"); position = 1;
    let first = a.tag;
    note("after 1"); position = 2;
    b.key = "stored 2";
    note("after 2"); position = 3;
    let third = c.child;
    note("after 3"); position = 4;
    d.flags = "stored 4";
    note("after 4"); position = 5;
    let fifth = e.sibling;
    note("after 5"); position = 6;
    f.tag = "stored 6";
    note("after 6"); position = 7;
    return first + "|" + third + "|" + fifth;
}
noInline(sixAccesses);
noInline(sixAccessesStrictly);
const accesses = [["read", "tag"], ["store", "key"], ["read", "child"], ["store", "flags"], ["read", "sibling"], ["store", "tag"]];

const many = name => { let pairs = []; for (let i = 0; i < 30; ++i) pairs.push(["filler" + i, i]); pairs.push([name, "far"]); return pairs; };
const own = (o, name) => { let descriptor = Object.getOwnPropertyDescriptor(o, name); return descriptor ? descriptor.value : "nothing of its own"; };
const slotInItem = { tag: 0, key: 1, child: 2, sibling: 3, flags: 4 };
const fillers = count => { let pairs = []; for (let i = 0; i < count; ++i) pairs.push(["before" + i, i]); return pairs; };
const plain = (receiver, value) => ({ receiver, read: { value, log: [] }, store: () => ({ log: [], after: (name, stored) => check(own(receiver, name), stored, "the value stored") }) });
const refusing = (receiver, value, before) => ({ receiver, read: { value, log: [] }, store: isStrict => ({ log: [], throws: isStrict, after: name => check(typeof receiver === "object" ? own(receiver, name) : before, before, "nothing is stored") }) });
const kinds = [
    ["the names in another order", name => plain(build(...fillers(5), [name, "moved"]), "moved")],
    ["the name one slot further", name => plain(build(...fillers(slotInItem[name] + 1), [name, "further"]), "further")],
    ["another shape with the name in the same slot", name => plain(build(...fillers(slotInItem[name]), [name, "same slot"], ["behind", 0]), "same slot")],
    ["the name alone", name => plain(build([name, "alone"]), "alone")],
    ["the name inherited", name => plain(keep(Object.create(build([name, "inherited"]))), "inherited")],
    ["the name nowhere", name => plain(build(["other", 0]), undefined)],
    ["the name out of line", name => plain(build(...many(name)), "far")],
    ["a dictionary", name => { let o = build([name, "in a dictionary"]); $vm.toCacheableDictionary(o); return plain(o, "in a dictionary"); }],
    ["an accessor that logs", name => {
        let receiver = Object.defineProperty(build(), name, { get() { note("get " + name); return "got"; }, set(v) { note("set " + name + " to " + v); }, configurable: true });
        return { receiver, read: { value: "got", log: ["get " + name] }, store: () => ({ log: stored => ["set " + name + " to " + stored], after: () => { } }) };
    }],
    ["an accessor on the prototype", name => {
        let receiver = keep(Object.create(Object.defineProperty(build(), name, { get() { note("inherited get " + name); return "got above"; }, set(v) { note("inherited set " + name + " to " + v); } })));
        return { receiver, read: { value: "got above", log: ["inherited get " + name] }, store: () => ({ log: stored => ["inherited set " + name + " to " + stored], after: () => check(Object.hasOwn(receiver, name), false, "nothing of its own is made") }) };
    }],
    ["a proxy that logs", name => {
        let target = build([name, "behind a proxy"]);
        let receiver = keep(new Proxy(target, { get(t, p) { note("trap get " + String(p)); return t[p]; }, set(t, p, v) { note("trap set " + String(p) + " to " + v); t[p] = v; return true; } }));
        return { receiver, read: { value: "behind a proxy", log: ["trap get " + name] }, store: () => ({ log: stored => ["trap set " + name + " to " + stored], after: (n, stored) => check(own(target, n), stored, "the value stored behind the proxy") }) };
    }],
    ["a frozen object", name => refusing(Object.freeze(build([name, "frozen"])), "frozen", "frozen")],
    ["a frozen item", name => { let item = makeItem(9); return refusing(Object.freeze(item), own(item, name), own(item, name)); }],
    ["a read-only property", name => refusing(Object.defineProperty(build(), name, { value: "fixed", writable: false, enumerable: true, configurable: true }), "fixed", "fixed")],
    ["a read-only property on the prototype", name => refusing(keep(Object.create(Object.defineProperty(build(), name, { value: "fixed above", writable: false }))), "fixed above", "nothing of its own")],
    ["an object that cannot be extended", name => refusing(Object.preventExtensions(build(["other", 0])), undefined, "nothing of its own")],
    ["a number", () => refusing(5, undefined, undefined)],
    ["a string", () => refusing("text", undefined, undefined)],
    ["a boolean", () => refusing(true, undefined, undefined)],
    ["null", () => ({ receiver: null, read: { throws: true, log: [] }, store: () => ({ log: [], throws: true, after: () => { } }) })],
    ["undefined", () => ({ receiver: undefined, read: { throws: true, log: [] }, store: () => ({ log: [], throws: true, after: () => { } }) })],
];

function runSix(subject, isStrict, failingAt, kind) {
    let what = subject.name + ", " + (kind ? kind[0] + " at access " + failingAt : "all good");
    let items = accesses.map((access, i) => makeItem(i + 1));
    let receivers = items.slice();
    let odd = null;
    if (kind) {
        odd = kind[1](accesses[failingAt - 1][1]);
        receivers[failingAt - 1] = odd.receiver;
    }
    let expectedLog = ["before 1"], values = [], expectedPosition = 7, throws = false, checks = [];
    for (let i = 1; i <= accesses.length && !throws; ++i) {
        let [how, name] = accesses[i - 1], stored = "stored " + i;
        if (i !== failingAt) {
            if (how === "read")
                values.push(name + i);
            else
                checks.push(() => check(own(items[i - 1], name), stored, what + ": the store at access " + i));
        } else if (how === "read") {
            expectedLog.push(...odd.read.log);
            throws = !!odd.read.throws;
            values.push(odd.read.value);
        } else {
            let model = odd.store(isStrict);
            expectedLog.push(...(typeof model.log === "function" ? model.log(stored) : model.log));
            throws = !!model.throws;
            checks.push(() => model.after(name, stored));
        }
        if (throws)
            expectedPosition = i;
        else
            expectedLog.push("after " + i);
    }
    for (let i = failingAt + 1; throws && i <= accesses.length; ++i) {
        let [how, name] = accesses[i - 1];
        if (how === "store")
            checks.push(() => check(own(items[i - 1], name), name + i, what + ": no store behind the exception, at access " + i));
    }
    log = [];
    position = 0;
    let before = exits(), result, thrown = "nothing";
    try {
        result = subject(...receivers);
    } catch (error) {
        thrown = error.constructor.name;
    }
    check(thrown, throws ? "TypeError" : "nothing", what + ": thrown");
    check(log.join(), expectedLog.join(), what + ": the log");
    check(position, expectedPosition, what + ": the module's variable");
    if (!throws)
        check(result, values.map(String).join("|"), what + ": the result");
    for (let verify of checks)
        verify();
    if (isCounting)
        check(exits() - before, kind ? 1 : 0, what + ": exits into the generic copy");
}

for (let [subject, isStrict] of [[sixAccesses, false], [sixAccessesStrictly, true]]) {
    for (let round = 0; round < 30; ++round)
        runSix(subject, isStrict, 0, null);
    for (let kind of kinds) {
        for (let failingAt = 1; failingAt <= accesses.length; ++failingAt) {
            runSix(subject, isStrict, failingAt, kind);
            runSix(subject, isStrict, 0, null);
        }
    }
}
applies(sixAccesses, sixAccessesStrictly);
guards(sixAccesses, "family-guards-read:tag", "family-guards-store:key", "family-guards-read:child", "family-guards-store:flags", "family-guards-read:sibling", "family-guards-store:tag");

let hook = null;
function callHook(x) { note("hook"); if (hook) hook(x); }
noInline(callHook);
function changesUnderIt(x) {
    let first = x.tag;
    let second = x.key;
    callHook(x);
    let third = x.tag;
    let fourth = x.key;
    x.flags = "stored";
    note("end");
    return first + "|" + second + "|" + third + "|" + fourth;
}
noInline(changesUnderIt);
function removeAndAddAgain(x) { delete x.tag; x.tag = "again"; }
function makeAccessor(x) { Object.defineProperty(x, "tag", { get() { note("late get"); return "late"; }, configurable: true }); }
function freezeIt(x) { Object.freeze(x); }
for (let round = 0; round < 30; ++round) {
    hook = null;
    log = [];
    check(changesUnderIt(makeItem(1)), "tag1|key1|tag1|key1", "nothing changes under it");
}
for (let [change, expected, expectedLog, flags] of [[removeAndAddAgain, "tag1|key1|again|key1", "hook,end", "stored"], [makeAccessor, "tag1|key1|late|key1", "hook,late get,end", "stored"], [freezeIt, "tag1|key1|tag1|key1", "hook,end", "flags1"]]) {
    for (let round = 0; round < 3; ++round) {
        let item = makeItem(1), before = exits();
        hook = change;
        log = [];
        check(changesUnderIt(item), expected, change.name + " between two accesses");
        check(log.join(), expectedLog, change.name + ": the log");
        check(own(item, "flags"), flags, change.name + ": the store behind it");
        if (isCounting)
            check(exits() - before, 1, change.name + ": the guards before it pass, one behind it exits");
    }
}
hook = null;
applies(changesUnderIt);

function overAll(items) {
    let total = "";
    for (let i = 0; i < items.length; ++i) {
        let item = items[i];
        note("item " + i);
        total += item.tag;
        item.key = "seen " + i;
        total += item.flags;
    }
    note("done");
    return total;
}
noInline(overAll);
for (let round = 0; round < 30; ++round) {
    log = [];
    overAll([makeItem(1), makeItem(2), makeItem(3)]);
}
for (let oddAt = 0; oddAt < 6; ++oddAt) {
    for (let numberOfOdd of [1, 2]) {
        let items = [], expected = "", expectedLog = [];
        for (let i = 0; i < 6; ++i) {
            let isOdd = i >= oddAt && i < oddAt + numberOfOdd;
            items.push(isOdd ? build(["flags", "F" + i], ["key", "K"], ["tag", "T" + i]) : makeItem(i));
            expected += isOdd ? "T" + i + "F" + i : "tag" + i + "flags" + i;
            expectedLog.push("item " + i);
        }
        expectedLog.push("done");
        let before = exits();
        log = [];
        check(overAll(items), expected, "a loop that exits in iteration " + oddAt);
        check(log.join(), expectedLog.join(), "a loop that exits in iteration " + oddAt + ": the log");
        for (let i = 0; i < 6; ++i)
            check(own(items[i], "key"), "seen " + i, "a loop that exits in iteration " + oddAt + ": the store in iteration " + i);
        if (isCounting)
            check(exits() - before, 1, "a loop that exits in iteration " + oddAt + ": it does not come back");
    }
}
applies(overAll);

function twoPlaces(x, y) { note("two"); return x.tag + y.key; }
function tooLarge(x, y, z) { let t = 0; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; t += 1; t += 2; t += 3; t += 4; t += 5; t += 6; t += 0; note("large"); return x.tag + y.key + z.child + t; }
function withHandler(x, y, z) { try { note("try"); return x.tag + y.key + z.child; } catch (error) { return "caught"; } }
function* generator(x, y, z) { note("generator"); yield x.tag + y.key + z.child; }
async function asynchronous(x, y, z) { note("async"); return x.tag + y.key + z.child; }
class Constructed { constructor(x, y, z) { note("constructed"); this.sum = x.tag + y.key + z.child; } }
function loopWithoutCalls(x, y, z, n) { let t = 0; for (let i = 0; i < n; ++i) t += i; note("loop"); return x.tag + y.key + z.child + t; }
function noPlaces(x, y, z) { note("none"); return x.unknownOne + y.unknownTwo + z.unknownThree; }
for (let f of [twoPlaces, tooLarge, withHandler, generator, asynchronous, Constructed, loopWithoutCalls, noPlaces])
    noInline(f);
for (let round = 0; round < 30; ++round) {
    let [x, y, z] = [makeItem(1), makeItem(2), makeItem(3)];
    check(twoPlaces(x, y), "tag1key2", "two places");
    check(tooLarge(x, y, z), "tag1key2child37497", "a large function");
    check(withHandler(x, y, z), "tag1key2child3", "a handler");
    check(generator(x, y, z).next().value, "tag1key2child3", "a generator");
    asynchronous(x, y, z);
    check(new Constructed(x, y, z).sum, "tag1key2child3", "a constructor");
    check(loopWithoutCalls(x, y, z, 4), "tag1key2child36", "a loop without calls");
    check(noPlaces(x, y, z), NaN, "no places");
}
if (minimum > 2)
    doesNotApply(twoPlaces, "too-few-places");
else
    applies(twoPlaces);
doesNotApply(tooLarge, "too-large");
doesNotApply(withHandler, "has-handler");
doesNotApply(generator, "is-generator-or-async");
doesNotApply(asynchronous, "is-generator-or-async");
doesNotApply(Constructed, "is-constructor");
doesNotApply(loopWithoutCalls, keepsLoopsWhole ? "has-loop-without-places" : "has-loop-without-calls");
doesNotApply(noPlaces, "too-few-places");

(function () {
    function returnsPair(x, y, z) {
        let first = x.tag + y.key;
        let result = { first, second: 1 };
        note(z.child);
        return result;
    }
    function readsPair(x, y, z) {
        let pair = returnsPair(x, y, z);
        return pair.first + pair.second;
    }
    for (let round = 0; round < 30; ++round) {
        log = [];
        check(readsPair(makeItem(1), makeItem(2), makeItem(3)), "tag1key21", "an object returned in registers");
        check(log.join(), "child3", "an object returned in registers: the log");
    }
    for (let oddAt = 0; oddAt < 3; ++oddAt) {
        let receivers = [makeItem(1), makeItem(2), makeItem(3)];
        receivers[oddAt] = build(["other", 0], ["child", "C"], ["key", "K"], ["tag", "T"]);
        log = [];
        check(readsPair(...receivers), (oddAt === 0 ? "T" : "tag1") + (oddAt === 1 ? "K" : "key2") + "1", "an object returned in registers, an odd receiver at " + oddAt);
        check(log.join(), oddAt === 2 ? "C" : "child3", "an object returned in registers, an odd receiver at " + oddAt + ": the log");
    }
})();
if (isCompiled && remarksOf("readsPair").includes("reads-returned-object-from-registers:returnsPair"))
    doesNotApply("returnsPair", "returns-values-in-registers");

function readsOwnLiteral(x, y, z) {
    let a = x.tag, b = y.key, c = z.child;
    let local = { one: a, two: b };
    note("own");
    return local.one + local.two + c;
}
function loopWithCallAndTwoPlaces(x, y, n) {
    let t = "";
    for (let i = 0; i < n; ++i)
        t += note(i);
    return t + x.tag + y.key;
}
noInline(readsOwnLiteral);
noInline(loopWithCallAndTwoPlaces);
for (let round = 0; round < 30; ++round) {
    check(readsOwnLiteral(makeItem(1), makeItem(2), makeItem(3)), "tag1key2child3", "a literal that is only read");
    check(loopWithCallAndTwoPlaces(makeItem(1), makeItem(2), 3), "012tag1key2", "a loop with a call and two places");
}
for (let oddAt = 0; oddAt < 3; ++oddAt) {
    let receivers = [makeItem(1), makeItem(2), makeItem(3)];
    receivers[oddAt] = build(["other", 0], ["child", "C"], ["key", "K"], ["tag", "T"]);
    check(readsOwnLiteral(...receivers), (oddAt === 0 ? "T" : "tag1") + (oddAt === 1 ? "K" : "key2") + (oddAt === 2 ? "C" : "child3"), "a literal that is only read, an odd receiver at " + oddAt);
}
if (isCompiled && remarksOf(readsOwnLiteral).includes("scalar-replaced-object"))
    doesNotApply(readsOwnLiteral, "does-not-allocate-object");
if (minimum > 2)
    doesNotApply(loopWithCallAndTwoPlaces, "too-few-places");
else
    applies(loopWithCallAndTwoPlaces);

function makesEnvironment(w, x, y, z) {
    let first = w.flags;
    {
        let captured = first;
        let read = () => captured;
        captured += "!";
        let a = x.tag;
        captured += a;
        note(read());
        let b = y.key;
        captured += b;
        note(read());
        let c = z.child;
        captured += c;
        return read();
    }
}
function makesEnvironmentLate(v, w, x, y) {
    let first = v.flags + w.sibling;
    {
        let captured = first;
        let read = () => captured;
        captured += "!";
        let a = x.tag;
        captured += a;
        note(read());
        let b = y.key;
        captured += b;
        return read();
    }
}
function environmentInEachIteration(items, x, y, z) {
    let total = "";
    for (let i = 0; i < items.length; ++i) {
        let item = items[i];
        let read = () => item;
        note("item " + i);
        total += read().tag + item.sibling;
    }
    return total + x.key + y.child + z.flags;
}
for (let f of [makesEnvironment, makesEnvironmentLate, environmentInEachIteration])
    noInline(f);
const oddItem = () => build(["other", 0], ["sibling", "S"], ["flags", "F"], ["child", "C"], ["key", "K"], ["tag", "T"]);
for (let round = 0; round < 30; ++round) {
    log = [];
    check(makesEnvironment(makeItem(0), makeItem(1), makeItem(2), makeItem(3)), "flags0!tag1key2child3", "a variable captured behind a read");
    check(log.join(), "flags0!tag1,flags0!tag1key2", "a variable captured behind a read: the log");
    check(makesEnvironmentLate(makeItem(0), makeItem(1), makeItem(2), makeItem(3)), "flags0sibling1!tag2key3", "a variable captured behind two reads");
    check(environmentInEachIteration([makeItem(1), makeItem(2)], makeItem(3), makeItem(4), makeItem(5)), "tag1sibling1tag2sibling2key3child4flags5", "a variable captured in each iteration");
}
for (let oddAt = 0; oddAt < 4; ++oddAt) {
    let receivers = [makeItem(0), makeItem(1), makeItem(2), makeItem(3)];
    receivers[oddAt] = oddItem();
    let parts = [oddAt === 0 ? "F" : "flags0", "!", oddAt === 1 ? "T" : "tag1", oddAt === 2 ? "K" : "key2", oddAt === 3 ? "C" : "child3"];
    let before = exits();
    log = [];
    check(makesEnvironment(...receivers), parts.join(""), "a variable captured behind a read, an odd receiver at " + oddAt);
    check(log.join(), parts.slice(0, 3).join("") + "," + parts.slice(0, 4).join(""), "a variable captured behind a read, an odd receiver at " + oddAt + ": what the closure saw");
    if (isCounting)
        check(exits() - before, oddAt ? 1 : 0, "a variable captured behind a read, an odd receiver at " + oddAt + ": exits");
    receivers = [makeItem(0), makeItem(1), makeItem(2), makeItem(3)];
    receivers[oddAt] = oddItem();
    check(makesEnvironmentLate(...receivers), (oddAt === 0 ? "F" : "flags0") + (oddAt === 1 ? "S" : "sibling1") + "!" + (oddAt === 2 ? "T" : "tag2") + (oddAt === 3 ? "K" : "key3"), "a variable captured behind two reads, an odd receiver at " + oddAt);
}
for (let oddAt = 0; oddAt < 5; ++oddAt) {
    let all = [makeItem(1), makeItem(2), makeItem(3), makeItem(4), makeItem(5)];
    all[oddAt] = oddItem();
    let expected = [oddAt === 0 ? "TS" : "tag1sibling1", oddAt === 1 ? "TS" : "tag2sibling2", oddAt === 2 ? "K" : "key3", oddAt === 3 ? "C" : "child4", oddAt === 4 ? "F" : "flags5"].join("");
    check(environmentInEachIteration(all.slice(0, 2), all[2], all[3], all[4]), expected, "a variable captured in each iteration, an odd receiver at " + oddAt);
}
applies(makesEnvironment, environmentInEachIteration);
guards(makesEnvironment, "family-guards-read:tag", "family-guards-read:key", "family-guards-read:child");
lacksGuards(makesEnvironment, "family-guards-read:flags");
guards(environmentInEachIteration, "family-guards-read:key", "family-guards-read:child", "family-guards-read:flags");
lacksGuards(environmentInEachIteration, "family-guards-read:tag", "family-guards-read:sibling");
if (minimum > 2)
    doesNotApply(makesEnvironmentLate, "makes-environment-behind-guard");
else
    applies(makesEnvironmentLate);

function link(item, next) { item.sibling = next; return item; }
function overList(head) {
    let total = "";
    for (let x = head; x; x = x.sibling)
        total += x.flags + x.tag;
    return total;
}
function overListAndArray(head, array) {
    let total = "";
    let i = 0;
    for (let x = head; x; x = x.sibling) {
        total += x.flags + x.tag + array[i];
        ++i;
    }
    return total;
}
noInline(overList);
noInline(overListAndArray);
const lengthOfList = 6;
const nodesThatFail = [
    ["the names in another order", (i, next) => ({ node: build(["tag", "T" + i], ["sibling", next], ["flags", "F" + i]), log: [] })],
    ["an accessor that logs", (i, next, change) => ({ node: Object.defineProperty(build(["tag", "T" + i], ["sibling", next]), "flags", { get() { note("get flags " + i); change(); return "F" + i; } }), log: ["get flags " + i] })],
    ["a proxy that logs", (i, next, change) => ({ node: keep(new Proxy(build(["flags", "F" + i], ["tag", "T" + i], ["sibling", next]), { get(t, p) { note("trap " + String(p)); if (p === "flags") change(); return t[p]; } })), log: ["trap flags", "trap tag", "trap sibling"] })],
];
function makeList(failsAt, kind, change) {
    let head = null, expectedLog = [];
    for (let i = lengthOfList; i--;) {
        if (i === failsAt) {
            let made = kind[1](i, head, change);
            head = made.node;
            expectedLog = made.log;
        } else
            head = link(makeItem(i), head);
    }
    return { head, expectedLog };
}
for (let round = 0; round < 30; ++round) {
    overList(makeList(-1).head);
    overListAndArray(makeList(-1).head, [0, 1, 2, 3, 4, 5]);
}
for (let kind of nodesThatFail) {
    for (let failsAt = 0; failsAt < lengthOfList; ++failsAt) {
        let what = kind[0] + " in iteration " + failsAt;
        let text = i => i === failsAt ? "F" + i + "T" + i : "flags" + i + "tag" + i;
        let expected = "";
        for (let i = 0; i < lengthOfList; ++i)
            expected += text(i);
        let list = makeList(failsAt, kind, () => { });
        let before = exits();
        log = [];
        check(overList(list.head), expected, "a list, " + what);
        check(log.join(), list.expectedLog.join(), "a list, " + what + ": the log");
        if (isCounting && keepsLoopsWhole)
            check(exits() - before, 1, "a list, " + what + ": it does not come back");

        let array = ["a0", "a1", "a2", "a3", "a4", "a5"];
        let changes = kind !== nodesThatFail[0];
        list = makeList(failsAt, kind, () => {
            for (let i = 0; i < 100; ++i)
                array.push("more");
            for (let i = failsAt + 1; i < lengthOfList; ++i)
                array[i] = "changed" + i;
        });
        expected = "";
        for (let i = 0; i < lengthOfList; ++i)
            expected += text(i) + (changes && i > failsAt ? "changed" + i : "a" + i);
        before = exits();
        log = [];
        check(overListAndArray(list.head, array), expected, "a list and an array, " + what);
        check(log.join(), list.expectedLog.join(), "a list and an array, " + what + ": the log");
        if (isCounting && keepsLoopsWhole)
            check(exits() - before, 1, "a list and an array, " + what + ": it does not come back");
    }
}
function withFlags(item, flags) { item.flags = flags; return item; }
function untilUnflagged(items, array, y, z) {
    let total = y.tag + z.key;
    let i = 0;
    for (;;) {
        if (!items[i].flags)
            break;
        total += array[i];
        ++i;
    }
    return total;
}
noInline(untilUnflagged);
const flagged = () => { let items = []; for (let i = 0; i < lengthOfList; ++i) items.push(withFlags(makeItem(i), 1)); items.push(withFlags(makeItem(lengthOfList), 0)); return items; };
for (let round = 0; round < 30; ++round)
    check(untilUnflagged(flagged(), ["a0", "a1", "a2", "a3", "a4", "a5"], makeItem(1), makeItem(2)), "tag1key2a0a1a2a3a4a5", "a loop whose only guard is in its header");
const elementsThatFail = [
    ["the names in another order", () => ({ element: build(["other", 0], ["flags", 1]), log: [] })],
    ["an accessor that logs", (i, change) => ({ element: Object.defineProperty(build(), "flags", { get() { note("get flags " + i); change(); return 1; } }), log: ["get flags " + i] })],
    ["a proxy that logs", (i, change) => ({ element: keep(new Proxy(build(["flags", 1]), { get(t, p) { note("trap " + String(p)); change(); return t[p]; } })), log: ["trap flags"] })],
];
for (let kind of elementsThatFail) {
    for (let failsAt = 0; failsAt < lengthOfList; ++failsAt) {
        let what = "a loop whose only guard is in its header, " + kind[0] + " in iteration " + failsAt;
        let items = flagged(), array = ["a0", "a1", "a2", "a3", "a4", "a5"];
        let changes = kind !== elementsThatFail[0];
        let made = kind[1](failsAt, () => {
            for (let i = failsAt; i < lengthOfList; ++i)
                array[i] = "changed" + i;
            for (let i = 0; i < 100; ++i) {
                array.push("more");
                items.push(null);
            }
            items[lengthOfList - 1] = withFlags(makeItem(9), 0);
        });
        items[failsAt] = made.element;
        let expected = "tag1key2";
        for (let i = 0; i < (changes && failsAt < lengthOfList - 1 ? lengthOfList - 1 : lengthOfList); ++i)
            expected += changes && i >= failsAt ? "changed" + i : "a" + i;
        let before = exits();
        log = [];
        check(untilUnflagged(items, array, makeItem(1), makeItem(2)), expected, what);
        check(log.join(), made.log.join(), what + ": the log");
        if (isCounting && keepsLoopsWhole)
            check(exits() - before, 1, what + ": it does not come back");
    }
}
if (keepsLoopsWhole) {
    applies(untilUnflagged);
    guards(untilUnflagged, "family-guards-read:tag", "family-guards-read:key", "family-guards-read:flags");
} else
    doesNotApply(untilUnflagged, "has-loop-without-calls");
(function () {
    function countsUntilUnflagged(items, array, wanted, y, z) {
        let prefix = y.tag + z.key;
        let i = 0, found = 0, last = null;
        for (;;) {
            if (!items[i].flags)
                break;
            if (array[i] === wanted)
                ++found;
            last = array[i];
            ++i;
        }
        return prefix + found + last;
    }
    const on = which => withFlags(makeItem(which), 1);
    for (let round = 0; round < 30; ++round)
        check(countsUntilUnflagged([on(0), on(1), on(2), on(3), on(4), on(5), withFlags(makeItem(6), 0)], ["a0", "a1", "a2", "a3", "a4", "a5"], "a4", makeItem(1), makeItem(2)), "tag1key21a5", "a loop from which the arrays' storage is hoisted");
    for (let kind of elementsThatFail) {
        for (let failsAt = 0; failsAt < lengthOfList; ++failsAt) {
            let what = "a loop from which the arrays' storage is hoisted, " + kind[0] + " in iteration " + failsAt;
            let items = [on(0), on(1), on(2), on(3), on(4), on(5), withFlags(makeItem(6), 0)], array = ["a0", "a1", "a2", "a3", "a4", "a5"];
            let sameItems = items, sameArray = array;
            let changes = kind !== elementsThatFail[0];
            let made = kind[1](failsAt, () => {
                for (let i = failsAt; i < lengthOfList; ++i)
                    sameArray[i] = "changed" + i;
                for (let i = 0; i < 100; ++i) {
                    sameArray.push("more");
                    sameItems.push(null);
                }
                sameItems[lengthOfList - 1] = withFlags(makeItem(9), 0);
            });
            items[failsAt] = made.element;
            let iterations = changes && failsAt < lengthOfList - 1 ? lengthOfList - 1 : lengthOfList, found = 0, last = null;
            for (let i = 0; i < iterations; ++i) {
                last = changes && i >= failsAt ? "changed" + i : "a" + i;
                if (last === "a4")
                    ++found;
            }
            let before = exits();
            log = [];
            check(countsUntilUnflagged(items, array, "a4", makeItem(1), makeItem(2)), "tag1key2" + found + last, what);
            check(log.join(), made.log.join(), what + ": the log");
            if (isCounting && keepsLoopsWhole)
                check(exits() - before, 1, what + ": it does not come back");
        }
    }
})();
if (keepsLoopsWhole) {
    applies("countsUntilUnflagged");
    guards("countsUntilUnflagged", "family-guards-read:tag", "family-guards-read:key", "family-guards-read:flags", "array-view");
} else
    doesNotApply("countsUntilUnflagged", "has-loop-without-calls");
if (keepsLoopsWhole) {
    applies(overList, overListAndArray);
    guards(overList, "family-guards-read:flags", "family-guards-read:tag", "family-guards-read:sibling");
} else {
    doesNotApply(overList, "has-loop-without-calls");
    doesNotApply(overListAndArray, "has-loop-without-calls");
}
