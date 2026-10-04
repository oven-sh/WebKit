//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=1", "--useAOTOperationCounters=1")

function check(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

function readAlpha(o) { return o.alpha; }
function readBeta(o) { return o.beta; }
function readGamma(o) { return o.gamma; }
function readDelta(o) { return o.delta; }
const readers = [["alpha", readAlpha], ["beta", readBeta], ["gamma", readGamma], ["delta", readDelta]];

function makeAlpha(v) { return { alpha: v }; }
function makeBeta(v) { return { beta: v }; }
function makeGamma(v) { return { gamma: v }; }
function makeDelta(v) { return { delta: v }; }
function makeAll(v) { return { alpha: v, beta: v + 1, gamma: v + 2, delta: v + 3 }; }
function makeAllBackwards(v) { return { delta: v + 3, gamma: v + 2, beta: v + 1, alpha: v }; }

function join(a, b) { return a + b; }

const madeElsewhere = [
    JSON.parse('{"alpha":1}'),
    JSON.parse('{"beta":1,"alpha":2}'),
    { [join("al", "pha")]: 3 },
    Object.fromEntries([[join("gam", "ma"), 4], [join("al", "pha"), 5]]),
    Object.assign({}, JSON.parse('{"delta":6,"gamma":7}')),
];
check(readAlpha(madeElsewhere[0]), 1, "parsed");
check(readBeta(madeElsewhere[0]), undefined, "parsed, absent");
check(readAlpha(madeElsewhere[1]), 2, "parsed, second");
check(readBeta(madeElsewhere[1]), 1, "parsed, first");
check(readAlpha(madeElsewhere[2]), 3, "computed");
check(readGamma(madeElsewhere[3]), 4, "entries, first");
check(readAlpha(madeElsewhere[3]), 5, "entries, second");
check(readDelta(madeElsewhere[4]), 6, "assigned, first");
check(readGamma(madeElsewhere[4]), 7, "assigned, second");
check(readAlpha(madeElsewhere[4]), undefined, "assigned, absent");

for (let i = 0; i < 100; ++i) {
    const single = [makeAlpha(i), makeBeta(i), makeGamma(i), makeDelta(i)];
    for (let made = 0; made < 4; ++made) {
        for (let read = 0; read < 4; ++read)
            check(readers[read][1](single[made]), made === read ? i : undefined, readers[read][0] + " of an object with " + readers[made][0]);
    }
    for (const all of [makeAll(i), makeAllBackwards(i)]) {
        for (let read = 0; read < 4; ++read)
            check(readers[read][1](all), i + read, readers[read][0] + " of an object with all");
    }
}

const numberOfOtherNames = 3000;
const others = [];
for (let i = 0; i < numberOfOtherNames; ++i) {
    const name = join("other", i);
    others.push([name, { [name]: i }, { [name]: i, alpha: -i }, { alpha: -i, [name]: i }]);
}
for (let pass = 0; pass < 3; ++pass) {
    for (let i = 0; i < numberOfOtherNames; ++i) {
        const [name, alone, first, second] = others[i];
        for (const [listed, read] of readers)
            check(read(alone), undefined, listed + " of an object with " + name);
        check(alone[name], i, name);
        check(readAlpha(first), -i, "alpha behind " + name);
        check(readAlpha(second), -i, "alpha before " + name);
        check(readBeta(first), undefined, "beta of an object with " + name + " and alpha");
        check(readBeta(second), undefined, "beta of an object with alpha and " + name);
        check(first[name], i, name + " before alpha");
        check(second[name], i, name + " behind alpha");
    }
}

const symbol = Symbol("alpha");
const withSymbols = { [symbol]: 1, [Symbol.iterator]: 2, alpha: 3 };
check(readAlpha(withSymbols), 3, "alpha behind symbols");
check(readBeta(withSymbols), undefined, "beta of an object with symbols");
check(readAlpha({ [symbol]: 1 }), undefined, "alpha of an object with a symbol described as alpha");
check(withSymbols[symbol], 1, "symbol");

class Private {
    #alpha = 1;
    beta = 2;
    read() { return this.#alpha; }
}
const withPrivate = new Private;
check(readAlpha(withPrivate), undefined, "alpha of an object with #alpha");
check(readBeta(withPrivate), 2, "beta behind #alpha");
check(withPrivate.read(), 1, "#alpha");

const dictionary = {};
for (let i = 0; i < 200; ++i)
    dictionary[join("entry", i)] = i;
dictionary.alpha = 1;
delete dictionary.entry0;
check(readAlpha(dictionary), 1, "alpha of a dictionary");
check(readBeta(dictionary), undefined, "beta of a dictionary");

const withAccessor = { get alpha() { return 1; }, beta: 2 };
check(readAlpha(withAccessor), 1, "getter");
check(readBeta(withAccessor), 2, "beta behind a getter");
const frozen = Object.freeze({ alpha: 1 });
check(readAlpha(frozen), 1, "frozen");
check(readAlpha(Object.create(makeAlpha(1))), 1, "inherited");
check(readAlpha(1), undefined, "number");
check(readAlpha("alpha"), undefined, "string");

const counts = typeof aotOperationCount === "function" && aotOperationCount("propertyNameIDIfKnown:known") !== null;
function known() { return aotOperationCount("propertyNameIDIfKnown:known"); }
function unknown() { return aotOperationCount("propertyNameIDIfKnown:unknown"); }
if (counts && known() + unknown()) {
    let knownBefore = known();
    let unknownBefore = unknown();
    const parsed = JSON.parse('{"delta":1,"beta":2,"alpha":3,"gamma":4}');
    if (known() - knownBefore < 3)
        throw new Error("names of the program that come from elsewhere are known: " + (known() - knownBefore));
    check(unknown() - unknownBefore, 0, "names of the program that are not known");
    check(readGamma(parsed) + readAlpha(parsed) + readDelta(parsed) + readBeta(parsed), 10, "parsed in another order");

    const fresh = [];
    for (let i = 0; i < 100; ++i)
        fresh.push(join("fresh", i));
    knownBefore = known();
    unknownBefore = unknown();
    const withFresh = [];
    for (let i = 0; i < 100; ++i)
        withFresh.push({ [fresh[i]]: i });
    if (unknown() - unknownBefore < 100)
        throw new Error("names made at run time are not known: " + (unknown() - unknownBefore));
    check(known() - knownBefore, 0, "names made at run time that are known");
    for (let i = 0; i < 100; ++i) {
        check(withFresh[i][fresh[i]], i, fresh[i]);
        check(readAlpha(withFresh[i]), undefined, "alpha of an object with " + fresh[i]);
    }
}
