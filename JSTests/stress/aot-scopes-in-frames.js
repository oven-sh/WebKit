//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTCapturesByValue=1", "--useAOTScopesInFrames=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function remarksOf(f) {
    let remarks = aotRemarks(typeof f === "string" ? f : f.name);
    if (!remarks && typeof f !== "string" && isAOTCompiled(f))
        throw new Error("no remarks for " + f.name);
    return remarks;
}
function matches(remark, pattern) { return remark === pattern || remark.startsWith(pattern + ":"); }
function applies(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (!remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " does not apply to " + (f.name || f) + ": " + remarks.join(" "));
    }
}
function doesNotApply(f, ...patterns) {
    let remarks = remarksOf(f);
    for (let pattern of remarks ? patterns : []) {
        if (remarks.some(remark => matches(remark, pattern)))
            throw new Error(pattern + " applies to " + (f.name || f) + ": " + remarks.join(" "));
    }
}
const inFrame = "scope-in-frame", captures = "captures-by-value";
function identity(x) { return x; }
noInline(identity);
function resultOf(promise) {
    let result;
    promise.then(value => { result = value; }, error => { result = "rejected: " + error; });
    drainMicrotasks();
    return result;
}

async function closureAfterAwait(a) {
    const before = a + 1;
    const value = await a;
    const read = () => before * 100 + value;
    return identity(read)();
}
async function closureBeforeAwait(a) {
    const fixed = a + 1;
    const read = () => fixed;
    identity(read);
    await null;
    return read() + fixed;
}
async function usedAcrossSeveral(a, b) {
    const x = a * 2;
    const getX = () => x;
    identity(getX);
    await null;
    const y = b * 3;
    const getY = () => x + y;
    identity(getY);
    await null;
    return getX() * 1000 + getY() + x + y;
}
function* generatorScope(a) {
    const base = a * 10;
    const read = () => base;
    identity(read);
    yield read();
    yield base + 1;
    yield read() + 2;
}
async function keepsObjects(n) {
    const kept = { v: n }, list = [n, n + 1];
    const read = () => kept.v + list[1];
    identity(read);
    await null;
    gc();
    await null;
    return read() + kept.v;
}
for (const f of [closureAfterAwait, closureBeforeAwait, usedAcrossSeveral, generatorScope, keepsObjects]) {
    noInline(f);
    applies(f, inFrame, captures);
}

function blockScopeInLoop(n) {
    const w = 1, h = 2;
    const read = () => w + h;
    identity(read);
    let t = 0;
    while (n-- > 0) {
        const q = w + h + n;
        identity(() => q);
        t += q;
    }
    return t + read();
}
async function blockScopeInLoopOfBody(n) {
    const w = 1, h = 2;
    const read = () => w + h;
    identity(read);
    await null;
    let t = 0;
    while (n-- > 0) {
        const q = w + h + n;
        identity(() => q);
        t += q;
    }
    await null;
    return t + read();
}
noInline(blockScopeInLoop);
noInline(blockScopeInLoopOfBody);
applies(blockScopeInLoop, captures + ":2", captures + ":1");
applies(blockScopeInLoopOfBody, inFrame, captures + ":2", captures + ":1");

async function blockScopeWithAnObjectInside(flag, a) {
    const h = a + 1;
    const readH = () => h;
    identity(readH);
    await null;
    if (flag) {
        let count = 0;
        const bump = () => ++count;
        identity(bump);
        await null;
        bump();
        if (count > 100)
            return -1;
    }
    const again = () => h * 2;
    identity(again);
    await null;
    return [readH(), again(), h].join();
}
noInline(blockScopeWithAnObjectInside);
applies(blockScopeWithAnObjectInside, inFrame);

async function scopeWithAnObjectOutside(flag, a) {
    let total = a;
    const add = (n) => { total += n; };
    identity(add);
    await null;
    if (flag) {
        const step = a + 1;
        const readStep = () => step;
        identity(readStep);
        add(readStep());
    }
    add(1);
    await null;
    return total;
}
noInline(scopeWithAnObjectOutside);

async function assignedAfterAwait(a) {
    let changing = a;
    const read = () => changing;
    identity(read);
    await null;
    changing = a + 1;
    return read();
}
async function assignedByAwait(a) {
    let later;
    const read = () => later;
    identity(read);
    later = await a;
    return read();
}
async function assignedInClosure(a) {
    let count = a;
    const bump = () => ++count;
    identity(bump);
    await null;
    bump();
    await null;
    return count;
}
async function scopeInLoop(list) {
    const made = [];
    for (const item of list) {
        const doubled = item * 2;
        await null;
        made.push(() => doubled);
    }
    return made.map(f => f()).join();
}
async function usedByHandler(a) {
    const name = "n" + a;
    const read = () => name;
    identity(read);
    try {
        await Promise.reject("no");
    } catch (error) {
        return read() + name + error;
    }
}
for (const f of [assignedAfterAwait, assignedByAwait, assignedInClosure]) {
    noInline(f);
    doesNotApply(f, inFrame);
}
for (const f of [scopeInLoop, usedByHandler])
    noInline(f);

async function tdz(a) {
    const early = () => late;
    identity(early);
    let result;
    try { result = early(); } catch (error) { result = error.constructor.name; }
    await null;
    const late = a;
    return result + early();
}
async function nestedBlocks(a) {
    const outer = a + 1;
    const getOuter = () => outer;
    identity(getOuter);
    await null;
    {
        const inner = outer + 1;
        const getInner = () => inner + outer;
        identity(getInner);
        await null;
        return getOuter() * 100 + getInner();
    }
}
async function readsOuterFunction(a) {
    const local = a + 1;
    const read = () => local + shared;
    identity(read);
    await null;
    return read() + shared;
}
let shared = 1000;
function makesAsync(outerValue) {
    let mutable = outerValue;
    const set = (v) => { mutable = v; };
    const run = async (p) => {
        const local = p + 1;
        const read = () => [local, mutable, outerValue].join();
        identity(read);
        await null;
        return read() + "|" + [local, mutable, outerValue].join();
    };
    return [set, run];
}
async function loopWithItsOwnScopes(list) {
    const base = 10;
    const read = () => base;
    identity(read);
    const made = [];
    for (let i = 0; i < list.length; i++) {
        made.push(() => i + base + list[i]);
        await null;
    }
    return made.map(f => f()).join() + ":" + read();
}
async function awaitsInBlockOnOnePath(flag, a) {
    const outer = a + 1;
    const readOuter = () => outer;
    identity(readOuter);
    if (flag) {
        const inner = await a;
        const readInner = () => inner;
        identity(readInner);
        if (readInner() > 100)
            return -1;
    }
    const after = outer + 1;
    const readAfter = () => after + outer;
    identity(readAfter);
    await null;
    return [readOuter(), readAfter(), after, outer].join();
}
async function* asyncGeneratorScope(a) {
    const tag = "t" + a;
    const read = (i) => tag + i;
    identity(read);
    for (let i = 0; i < 2; i++) {
        await null;
        yield read(i);
    }
}
for (let round = 0; round < 60; round++) {
    check(resultOf(closureAfterAwait(1)), 201, "a closure that is made after an await");
    check(resultOf(closureBeforeAwait(1)), 4, "a closure that is made before an await");
    check(resultOf(usedAcrossSeveral(1, 2)), 2000 + 8 + 2 + 6, "variables that are initialized between awaits");
    check([...generatorScope(2)].join(), "20,21,22", "a generator");
    check(resultOf(keepsObjects(round)), round + round + 1 + round, "objects that only the frame refers to");
    check(resultOf(assignedAfterAwait(1)), 2, "a variable that is assigned after an await, after the closure is made");
    check(resultOf(assignedByAwait(7)), 7, "a variable that is assigned what is awaited");
    check(resultOf(assignedInClosure(1)), 2, "a variable that a closure assigns");
    check(resultOf(scopeInLoop([1, 2, 3])), "2,4,6", "a scope for each iteration");
    check(resultOf(usedByHandler(1)), "n1n1no", "a scope that a handler uses");
    check(resultOf(tdz(5)), "ReferenceError5", "a variable that is read too early");
    check(resultOf(nestedBlocks(1)), 205, "a block in the body");
    check(resultOf(readsOuterFunction(1)), 2002, "a global variable beside it");
    check(resultOf(loopWithItsOwnScopes([100, 200])), "110,211:10", "a loop with a scope for each iteration, inside the scope");
    check([resultOf(awaitsInBlockOnOnePath(true, 1)), resultOf(awaitsInBlockOnOnePath(false, 1)), resultOf(awaitsInBlockOnOnePath(true, 101))].join("|"), "2,5,3,2|2,5,3,2|-1", "an await in a block that only one path enters");
    check(blockScopeInLoop(3) + ":" + resultOf(blockScopeInLoopOfBody(3)), "15:15", "a loop whose body has a scope, inside the scope");
    check(resultOf(blockScopeWithAnObjectInside(true, 1)) + "|" + resultOf(blockScopeWithAnObjectInside(false, 1)), "2,4,2|2,4,2", "a scope with an object, across an await, inside the scope");
    check(resultOf(scopeWithAnObjectOutside(true, 1)) + "|" + resultOf(scopeWithAnObjectOutside(false, 1)), "4|2", "a scope without an object inside a restored scope with one");
    const [set, run] = makesAsync("o");
    set("changed");
    check(resultOf(run(1)), "2,changed,o|2,changed,o", "variables of the enclosing function beside it");
    check(resultOf((async () => { const all = []; for await (const v of asyncGeneratorScope(round)) all.push(v); return all.join(); })()), "t" + round + "0,t" + round + "1", "an async generator");
    const first = generatorScope(1), second = generatorScope(2);
    check([first.next().value, second.next().value, first.next().value, second.next().value, first.next().value, second.next().value].join(), "10,20,11,21,12,22", "two activations that take turns");
    const pending = [closureBeforeAwait(1), closureBeforeAwait(10), closureBeforeAwait(100)];
    check(resultOf(Promise.all(pending)).join(), "4,22,202", "three activations that are pending at once");
}
