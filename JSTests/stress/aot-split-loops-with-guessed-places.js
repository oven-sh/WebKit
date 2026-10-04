//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTInlineGuessedPlacesEverywhere=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTGuessedPlaces=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTLoopSplitting=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTGuardsOverWholeFunctions=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTGuardsOverWholeFunctionsInsteadOfLoopSplitting=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--validateAOTInferredTypes=1", "--validateGraphAtEachPhase=1")
//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault

function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function thrown(f) {
    try {
        f();
    } catch (error) {
        return error.constructor.name;
    }
    return "nothing";
}
const remarksOf = f => typeof aotRemarks === "function" && aotRemarks(f.name) || null;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isCompiled = !!remarksOf(check);
const guesses = isCompiled && !!options.useAOTGuessedPlaces && !!options.useAOTDataStubs;
const isCounting = isCompiled && !!options.useAOTOperationCounters && !!options.useAOTDataStubs;
const counters = ["operationAOTCountGuessedPlace", "operationAOTCountReadByName", "operationAOTCountGuessedStore", "operationAOTCountStoreByName"];
const accessesOutsideFastCopies = () => isCounting ? counters.reduce((sum, counter) => sum + (aotOperationCount(counter) || 0), 0) : 0;

function Fiber(tag) {
    this.tag = tag;
    this.key = null;
    this.alternate = null;
    this.memoizedProps = null;
    this.memoizedState = null;
    this.pendingProps = null;
    this.child = null;
    this.sibling = null;
    this.return = null;
    this.flags = 0;
}
const inAnotherOrder = ["flags", "return", "sibling", "child", "pendingProps", "memoizedState", "memoizedProps", "alternate", "tag"];
function makeOther(tag) {
    const other = { };
    for (const name of inAnotherOrder)
        other[name] = name === "flags" ? 0 : name === "tag" ? tag : null;
    return other;
}
function set(o, name, value) {
    o[name] = value;
    return o;
}
function build(depth, width, parent, make) {
    const fiber = make(depth);
    set(set(fiber, "return", parent), "pendingProps", depth);
    let previous = null;
    for (let i = 0; depth && i < width; ++i) {
        const child = build(depth - 1, width, fiber, make);
        if (previous)
            set(previous, "sibling", child);
        else
            set(fiber, "child", child);
        previous = child;
    }
    return fiber;
}

function walks(root) {
    let node = root, seen = 0;
    for (;;) {
        ++seen;
        let other = node.alternate;
        if (other !== null)
            node.memoizedProps = other.memoizedProps;
        node.memoizedState = node.pendingProps;
        node.flags = node.flags | 1;
        if (node.child !== null) {
            node.child.return = node;
            node = node.child;
            continue;
        }
        if (node === root)
            return seen;
        while (node.sibling === null) {
            if (node.return === null || node.return === root)
                return seen;
            node = node.return;
        }
        node.sibling.return = node.return;
        node = node.sibling;
    }
}
function sumsSiblings(first) {
    let sum = 0;
    for (let node = first; node !== null; node = node.sibling)
        sum += node.pendingProps + node.flags;
    return sum;
}
const visitors = [node => 1, node => 2];
function walksAndCalls(first, which) {
    let sum = 0;
    for (let node = first; node !== null; node = node.sibling) {
        sum += visitors[which & 1](node);
        sum += node.pendingProps + node.flags;
        node.memoizedState = sum;
    }
    return sum;
}
function readsWithoutLoop(node) {
    node.memoizedProps = node.tag;
    return node.pendingProps + node.flags + node.memoizedProps;
}
for (const f of [walks, sumsSiblings, walksAndCalls, readsWithoutLoop, makeOther, set, build])
    noInline(f);

const root = build(4, 4, null, tag => new Fiber(tag));
const numberOfNodes = 341;
const row = root.child;
const warmUp = () => {
    for (let i = 0; i < 30; ++i) {
        check(walks(root), numberOfNodes, "nodes seen");
        check(sumsSiblings(row), 16, "sum over siblings");
        check(walksAndCalls(row, i), i & 1 ? 24 : 20, "sum with calls");
        check(readsWithoutLoop(row), 3 + 1 + 3, "reads outside a loop");
    }
};
warmUp();

const during = f => {
    const before = accessesOutsideFastCopies();
    f();
    return accessesOutsideFastCopies() - before;
};
const isSplit = f => isCompiled && remarksOf(f).includes("split-loop");
const hasTwin = f => isCompiled && remarksOf(f).includes("guards-over-whole-function");
const rounds = 50;
for (const [f, operand, result, accessesARound] of [[walks, root, numberOfNodes, 8 * numberOfNodes], [sumsSiblings, row, 16, 12]]) {
    const outside = during(() => {
        for (let i = 0; i < rounds; ++i)
            check(f(operand), result, f.name);
    });
    if (!isCounting)
        continue;
    if (isSplit(f)) {
        if (outside > 4 * rounds)
            throw new Error(f.name + " does not run in the fast copy of its loop: " + outside + " reads and stores of " + rounds + " rounds were made outside it");
    } else if (!hasTwin(f) && outside < rounds * accessesARound)
        throw new Error(f.name + " is not split, yet only " + outside + " reads and stores of " + rounds + " rounds were counted");
}
if (isCounting && !hasTwin(walksAndCalls) && !hasTwin(readsWithoutLoop)) {
    const outside = during(() => {
        for (let i = 0; i < rounds; ++i) {
            walksAndCalls(row, i);
            readsWithoutLoop(row);
        }
    });
    if (outside < rounds * (4 * 4 + 5))
        throw new Error("reads and stores outside split loops are not counted: " + outside);
}

{
    const mixed = build(4, 4, null, tag => tag & 1 ? new Fiber(tag) : makeOther(tag));
    const other = build(4, 4, null, makeOther);
    const withAlternates = build(3, 3, null, tag => set(new Fiber(tag), "alternate", set(new Fiber(-tag), "memoizedProps", "props " + tag)));
    const grown = build(3, 3, null, tag => set(new Fiber(tag), "extra", tag));
    const logged = [];
    const watched = build(2, 2, null, tag => {
        let flags = 0;
        return Object.defineProperty(new Fiber(tag), inAnotherOrder[0], { get() { logged.push("get " + tag); return flags; }, set(v) { logged.push("set " + tag); flags = v; } });
    });
    for (let i = 0; i < 40; ++i) {
        check(walks(mixed), numberOfNodes, "fibers of two kinds");
        check(walks(other), numberOfNodes, "fibers of the other kind");
        check(walks(root), numberOfNodes, "fibers of the first kind again");
        check(walks(withAlternates), 40, "fibers with alternates");
        check(withAlternates[inAnotherOrder[3]][inAnotherOrder[6]], "props 2", "what was copied from an alternate");
        check(walks(grown), 40, "fibers with one more property");
        logged.length = 0;
        check(walks(watched), 7, "fibers with an accessor");
        check(logged.join(), "get 2,set 2,get 1,set 1,get 0,set 0,get 0,set 0,get 1,set 1,get 0,set 0,get 0,set 0", "the calls of the accessor");
        check(sumsSiblings(mixed[inAnotherOrder[3]]) + sumsSiblings(other[inAnotherOrder[3]]) + sumsSiblings(grown[inAnotherOrder[3]]), 16 + 16 + 9, "sums over fibers of several kinds");
        check(mixed[inAnotherOrder[3]][inAnotherOrder[3]][inAnotherOrder[5]], 2, "what was stored in a fiber of the other kind");
    }
    check(thrown(() => walks(null)), "TypeError", "no fiber at all");
    check(sumsSiblings(null), 0, "an empty row");
    warmUp();
}

if (isCompiled) {
    const about = f => remarksOf(f).filter(remark => remark.includes("guess") || remark.includes("loop") || remark.includes("guards-over")).join(" ");
    const takesGuess = f => remarksOf(f).some(remark => remark.startsWith("guessed-place-read") || remark.startsWith("guessed-place-store") || remark.startsWith("name-check-guards"));
    for (const f of [walks, sumsSiblings]) {
        if (!isSplit(f)) {
            if (remarksOf(f).some(remark => remark.includes("fills-cache-of-split-loop")))
                throw new Error(f.name + " is not split: " + about(f));
            if (guesses && !takesGuess(f))
                throw new Error(f.name + " is not split and must keep its guessed places: " + about(f));
            continue;
        }
        if (!remarksOf(f).includes("read-fills-cache-of-split-loop:sibling"))
            throw new Error("the generic copy of the loop of " + f.name + " does not fill the caches that the fast copy compares with: " + about(f));
        if (f === walks && !remarksOf(f).includes("store-fills-cache-of-split-loop:return"))
            throw new Error("the generic copy of the loop of " + f.name + " does not fill the caches of its stores: " + about(f));
        if (remarksOf(f).some(remark => remark === "guessed-place-read-through-stub:sibling" || remark === "guessed-place-read:sibling"))
            throw new Error(f.name + " takes a guessed place in the generic copy of its loop: " + about(f));
    }
    for (const f of [walksAndCalls, readsWithoutLoop]) {
        if (isSplit(f) || remarksOf(f).some(remark => remark.includes("fills-cache-of-split-loop")))
            throw new Error(f.name + " has no loop to split: " + about(f));
        if (guesses && !takesGuess(f))
            throw new Error(f.name + " must keep its guessed places: " + about(f));
    }
    if (guesses && hasTwin(readsWithoutLoop) && !remarksOf(readsWithoutLoop).some(remark => remark.startsWith("guessed-place-read-through-stub") || remark.startsWith("guessed-place-read:")))
        throw new Error("the generic copy of a whole function must keep its guessed places: " + about(readsWithoutLoop));
}
