//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTInlining=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTInlining=0", "--useAOTGuardsOverWholeFunctions=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1", "--useAOTInlining=0", "--useAOTGuessedPlaces=0")
//@ runDefault("--compileMainScriptAheadOfTime=1")

function shouldBe(actual, expected, what)
{
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const count = name => typeof aotOperationCount === "function" && aotOperationCount(name) || 0;
const options = typeof jscOptions === "function" ? jscOptions() : { };
const isCounting = typeof aotOperationCount === "function" && aotOperationCount("operationAOTGetById") !== null && !!options.useAOTGuessedPlaces && !!options.useAOTDataStubs;

function makesTriple(i) { return { firstOfTriple: i, secondOfTriple: i + 1, thirdOfTriple: i + 2 }; }
function build(...pairs)
{
    let object = { };
    for (let i = 0; i < pairs.length; i += 2)
        object[pairs[i]] = pairs[i + 1];
    return object;
}

const lineOfRead = new Error().line; function readsSecond(o) { return o.secondOfTriple; }
const lineOfStore = new Error().line; function storesSecond(o, value) { o.secondOfTriple = value; }
const lineOfThree = new Error().line; function readsThree(o) { return o.firstOfTriple + o.secondOfTriple + o.thirdOfTriple; }
noInline(readsSecond);
noInline(storesSecond);
noInline(readsThree);

function countAtLine(outcome, line)
{
    let sum = 0;
    for (let column = 0; column < 160; ++column) {
        for (let offset = 0; offset < 96; ++offset)
            sum += count(outcome + ":at:" + line + ":" + column + ":bc" + offset);
    }
    return sum;
}

const inAnotherOrder = i => build("secondOfTriple", i + 1, "firstOfTriple", i, "thirdOfTriple", i + 2);
const withoutSecond = i => build("firstOfTriple", i, "unrelated", 0, "thirdOfTriple", i + 2);
const withGetter = i => Object.defineProperty(build("firstOfTriple", i), "secondOfTriple", { get() { return i + 1; }, set(value) { }, configurable: true });

const cases = [
    ["operationAOTCountGuessedPlace", lineOfRead, "another-slot", i => shouldBe(readsSecond(inAnotherOrder(i)), i + 1, "a read in another slot")],
    ["operationAOTCountGuessedPlace", lineOfRead, "has-attributes", i => shouldBe(readsSecond(withGetter(i)), i + 1, "a read of a getter")],
    ["operationAOTCountGuessedPlace", lineOfRead, "not-a-cell", i => shouldBe(readsSecond(i), undefined, "a read from a number")],
    ["operationAOTCountGuessedStore", lineOfStore, "another-slot", i => { let o = inAnotherOrder(i); storesSecond(o, i); shouldBe(o.secondOfTriple, i, "a store to another slot"); }],
    ["operationAOTCountGuessedStore", lineOfStore, "not-an-own-property", i => { let o = withoutSecond(i); storesSecond(o, i); shouldBe(o.secondOfTriple, i, "a store that adds"); }],
    ["operationAOTCountGuessedPlace", lineOfThree, "another-slot", i => shouldBe(readsThree(inAnotherOrder(i)), 3 * i + 3, "three reads in another order")],
];

for (let i = 0; i < 100; ++i) {
    let triple = makesTriple(i);
    shouldBe(readsSecond(triple), i + 1, "a read");
    storesSecond(triple, i + 5);
    shouldBe(readsThree(triple), 3 * i + 7, "three reads");
}
if (isCounting) {
    shouldBe(count("operationAOTCountGuessedPlace:hit") >= 400, true, "the reads are guessed and hit");
    shouldBe(count("operationAOTCountGuessedStore:hit") >= 100, true, "the stores are guessed and hit");
    for (const line of [lineOfRead, lineOfStore, lineOfThree])
        shouldBe(countAtLine("hit", line), 0, "a hit is not counted at its site");
}
for (const [operation, line, outcome, run] of cases) {
    const before = count(operation + ":" + outcome);
    const beforeAtLine = countAtLine(outcome, line);
    for (let i = 0; i < 50; ++i)
        run(i);
    if (!isCounting)
        continue;
    const misses = count(operation + ":" + outcome) - before;
    shouldBe(misses >= 50, true, operation + " counts " + outcome);
    shouldBe(countAtLine(outcome, line) - beforeAtLine, misses, "each " + outcome + " of " + operation + " is counted at its site");
}
