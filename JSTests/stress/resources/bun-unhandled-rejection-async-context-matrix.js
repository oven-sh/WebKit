// Loaded by the bun-unhandled-rejection-async-context-matrix-*.js tests.
//
// Every program made of: a source promise (its kind, what it is settled with, and when), one or two derivations of it,
// and an async context of its own for each role (made in M, derived in D1 and D2, settled in S, jobs run in R; then each
// role in turn in no async context).
//
// There are no expectations. Each program runs in two worlds: one untouched, where the engine takes its fast paths (jobs
// with no handler), and one where Promise.prototype.then is replaced by a function that passes explicit handlers, where
// every job has a handler and enters the async context it was scheduled in. What the embedder is told
// (promiseRejectionTracker), and what script sees, has to be the same in both.
//
// A test runs a slice, to stay within the time a test may take. All of it, two derivations deep, takes a few seconds:
//     jsc --useDollarVM=1 -e 'load("resources/bun-unhandled-rejection-async-context-matrix.js"); runMatrix({ depth: 2 })'

$vm.setReportsUnhandledRejectionsInAsyncContext();

const worldSource = `(function (foreign, patched) {
    const reported = new Map();
    globalThis.asyncContextsWhenRejected = reported;
    foreign.asyncContextsWhenRejected = reported;
    if (patched) {
        for (const realm of [globalThis, foreign]) {
            const nativeThen = realm.Promise.prototype.then;
            realm.Promise.prototype.then = function (onFulfilled, onRejected) {
                return nativeThen.call(this,
                    typeof onFulfilled === "function" ? onFulfilled : value => value,
                    typeof onRejected === "function" ? onRejected : reason => { throw reason; });
            };
        }
    }
    const set = context => { $vm.setAsyncContext(context); foreign.$vm.setAsyncContext(context); };
    const get = () => $vm.asyncContext();
    function inContext(context, fn) {
        const previous = get();
        set(context);
        try {
            return fn();
        } finally {
            set(previous);
        }
    }
    let seen;
    const see = what => { seen.push(what + "@" + (get()?.name ?? "none")); };
    const noop = () => { };
    const error = () => new Error("rejected");
    class Subclass extends Promise {
        constructor(executor) {
            super((resolve, reject) => executor(
                value => { see("Subclass.resolve"); resolve(value); },
                reason => { see("Subclass.reject"); reject(reason); }));
        }
    }

    // A source: { promise, settle() }.
    const kinds = {
        native: executor => new Promise(executor),
        subclass: executor => new Subclass(executor),
        ownConstructor: executor => Object.defineProperty(new Promise(executor), "constructor", { value: Promise, configurable: true }),
        foreign: executor => new foreign.Promise(executor),
    };
    // What the source is settled with, and what is done to that afterwards.
    const outcomes = {
        rejects: () => ({ settle: (resolve, reject) => reject(error()) }),
        fulfilsWithPrimitive: () => ({ settle: resolve => resolve(1) }),
        fulfilsWithObject: () => ({ settle: resolve => resolve({}) }),
        fulfilsWithObjectThatGainsThen: () => {
            const value = {};
            return { settle: resolve => resolve(value), afterwards: () => { value.then = (_, reject) => { see("gained then"); reject(error()); }; } };
        },
        fulfilsWithObjectThatGainsThrowingThen: () => {
            const value = {};
            return { settle: resolve => resolve(value), afterwards: () => { Object.defineProperty(value, "then", { get() { see("gained then getter"); throw error(); } }); } };
        },
        resolvedWithPromiseThatRejects: () => ({ settle: resolve => resolve((async () => { throw error(); })()) }),
        resolvedWithThenableThatRejects: () => ({ settle: resolve => resolve({ then(_, reject) { see("thenable"); reject(error()); } }) }),
    };
    const timings = ["before", "after", "inAJob"];

    const derivations = {
        "then(f)": p => p.then(() => { see("f"); }),
        "then()": p => p.then(),
        "then(undefined, rethrow)": p => p.then(undefined, reason => { see("rethrow"); throw reason; }),
        "catch(undefined)": p => p.catch(undefined),
        "finally(f)": p => p.finally(() => { see("finally"); }),
        "finally(returns rejecting)": p => p.finally(() => (async () => { throw error(); })()),
        "finally(returns fulfilled)": p => p.finally(() => Promise.resolve(2)),
        "new Promise(resolve(p))": p => new Promise(resolve => resolve(p)),
        "new Subclass(resolve(p))": p => new Subclass(resolve => resolve(p)),
        "new foreign.Promise(resolve(p))": p => new foreign.Promise(resolve => resolve(p)),
        "async return p": p => (async () => p)(),
        "async await p": p => (async () => { await p; see("after await"); })(),
        "then(() => p)": p => Promise.resolve().then(() => p),
        "Promise.resolve(p).then(f)": p => Promise.resolve(p).then(noop),
        "Subclass.resolve(p)": p => Subclass.resolve(p),
        "withResolvers().resolve(p)": p => { const { promise, resolve } = Promise.withResolvers(); resolve(p); return promise; },
        "Promise.try(() => p)": p => Promise.try(() => p),
        "Promise.all([p])": p => Promise.all([p]),
        "Promise.all([1, p])": p => Promise.all([1, p]),
        "Promise.race([p])": p => Promise.race([p]),
        "Promise.any([p])": p => Promise.any([p]),
        "Promise.allSettled([p])": p => Promise.allSettled([p]),
        "Subclass.all([p])": p => Subclass.all([p]),
        "Subclass.race([p])": p => Subclass.race([p]),
        "foreign then(f)": p => foreign.Promise.prototype.then.call(p, noop),
        "foreign Promise.all([p])": p => foreign.Promise.all([p]),
    };

    const roles = ["M", "D1", "D2", "S", "R"];
    const named = Object.fromEntries(roles.map(name => [name, { name }]));

    function run(kind, outcome, timing, first, second, roleWithNone) {
        reported.clear();
        seen = [];
        const context = role => (role === roleWithNone ? undefined : named[role]);
        let resolve, reject;
        const { settle, afterwards } = outcomes[outcome]();
        const source = inContext(context("M"), () => kinds[kind]((a, b) => { resolve = a; reject = b; }));
        const settleNow = () => inContext(context("S"), () => settle(resolve, reject));
        if (timing === "before") {
            settleNow();
            // What settling it leaves is done with before anything is derived.
            inContext(context("R"), drainMicrotasks);
            reported.clear();
            seen = [];
        }
        const labels = new Map([[source, "source"]]);
        const one = inContext(context("D1"), () => derivations[first](source));
        labels.set(one, "first");
        if (second) {
            const two = inContext(context("D2"), () => derivations[second](one));
            labels.set(two, "second");
        }
        if (timing === "after")
            settleNow();
        else if (timing === "inAJob")
            inContext(context("S"), () => void (async () => { await null; settle(resolve, reject); })());
        afterwards?.();
        inContext(context("R"), drainMicrotasks);
        const told = [...reported].map(([promise, where]) => (labels.get(promise) ?? "another") + " in " + (where?.name ?? "none")).sort();
        return "told: " + (told.join(", ") || "nothing") + " | seen: " + (seen.join(", ") || "nothing");
    }
    return { run, kinds: Object.keys(kinds), outcomes: Object.keys(outcomes), timings, derivations: Object.keys(derivations), roles };
})`;

function makeWorld(patched) {
    const realm = createGlobalObject();
    const foreign = createGlobalObject();
    // As the realms of an embedder may: one async context for both.
    realm.$vm.shareAsyncContextWith(foreign);
    return realm.eval(worldSource)(foreign, patched);
}

// Differences that are known, and left out.
const known = {
    // A promise is fulfilled with an object, and the object gains a `then` afterwards: a job with no handler that passes
    // the object on calls that `then`, or schedules the job that does, in whatever async context is current.
    valueGainsThen: outcome => /Gains/.test(outcome),
    // The resolve and reject functions of a capability run in the async context its promise kept, which is none if the
    // promise is fulfilled or has a reaction. What the embedder is told is compared all the same.
    contextOfCapabilityFunctions: text => text.replace(/(Subclass\.(?:resolve|reject))@\w+/g, "$1"),
    // Not about async contexts: once the DFG has compiled it, Promise.try(() => p) returns p itself if p is what then() of
    // a promise of another realm returned.
    tryOfAnotherRealm: (kind, first, second) => second === "Promise.try(() => p)" && (kind === "foreign" || /foreign/.test(first)),
};

function runMatrix({ kinds, depth = 1, stride = 1, offset = 0 }) {
    const fast = makeWorld(false);
    const reference = makeWorld(true);
    let index = 0;
    for (const kind of kinds ?? fast.kinds) {
        for (const outcome of fast.outcomes) {
            if (known.valueGainsThen(outcome))
                continue;
            for (const timing of fast.timings) {
                for (const first of fast.derivations) {
                    for (const second of depth > 1 ? fast.derivations : [undefined]) {
                        if (known.tryOfAnotherRealm(kind, first, second))
                            continue;
                        for (const roleWithNone of [undefined, ...fast.roles]) {
                            if (index++ % stride !== offset)
                                continue;
                            const program = [kind, outcome, timing, first, second, roleWithNone];
                            const inFast = known.contextOfCapabilityFunctions(fast.run(...program));
                            const inReference = known.contextOfCapabilityFunctions(reference.run(...program));
                            if (inFast !== inReference)
                                throw new Error(`a ${kind} promise that ${outcome} (${timing} it is derived from), ${first}${second ? ", " + second : ""}, no async context for ${roleWithNone ?? "nothing"}:\n    ${inFast}\nbut with every job given a handler:\n    ${inReference}`);
                        }
                    }
                }
            }
        }
    }
}
