//@ runDefault("--compileMainScriptAheadOfTime=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuessedPlaces=0")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTGuardsOverWholeFunctions=1", "--validateGraphAtEachPhase=1", "--validateAOTInferredTypes=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}

function makesHook(i) { return { sharedState: i, hookQueue: i + 1, hookNext: null }; }
function makesNode(i) { return { nodeTag: 0, nodeKey: i + 2, sharedState: i }; }
function readsStateOfParameter(o) { return o.sharedState; }

function makesReaders() {
    let currentHook = null;
    let currentNode = null;
    let either = null;
    function setsHook(o) { currentHook = o; }
    function setsNode(o) { currentNode = o; }
    function setsEither(o) { either = o; }
    function readsStateOfHook() { return currentHook.sharedState; }
    function readsQueueOfHook() { return currentHook.hookQueue; }
    function readsNextOfHook() { return currentHook.hookNext; }
    function writesStateOfHook(v) { currentHook.sharedState = v; }
    function readsStateOfNode() { return currentNode.sharedState; }
    function readsKeyOfNode() { return currentNode.nodeKey; }
    function readsThreeOfHook() { return currentHook.sharedState + currentHook.hookQueue + (currentHook.hookNext === null ? 100 : 200); }
    function readsQueueOfEither() { return either.hookQueue; }
    function readsKeyOfEither() { return either.nodeKey; }
    function readsStateOfEither() { return either.sharedState; }
    return [setsHook, setsNode, setsEither, readsStateOfHook, readsQueueOfHook, readsNextOfHook, writesStateOfHook, readsStateOfNode, readsKeyOfNode, readsThreeOfHook, readsQueueOfEither, readsKeyOfEither, readsStateOfEither];
}
let [setsHook, setsNode, setsEither, readsStateOfHook, readsQueueOfHook, readsNextOfHook, writesStateOfHook, readsStateOfNode, readsKeyOfNode, readsThreeOfHook, readsQueueOfEither, readsKeyOfEither, readsStateOfEither] = makesReaders();

let countOf = (operation, detail) => typeof aotOperationCount === "function" && aotOperationCount(operation + ":" + detail) || 0;
let exits = () => countOf("operationAOTCountGuessedPlace", "exit-into-generic-copy");

for (let i = 0; i < 100; i++) {
    setsHook(makesHook(i));
    setsNode(makesNode(i));
    check(readsStateOfHook(), i, "a name that two shapes hold, read from a variable that holds the first");
    check(readsStateOfNode(), i, "the same name, read from a variable that holds the second");
    check(readsQueueOfHook(), i + 1, "a name of the first shape alone");
    check(readsNextOfHook(), null, "another name of the first shape alone");
    check(readsKeyOfNode(), i + 2, "a name of the second shape alone");
    writesStateOfHook(i + 7);
    check(readsStateOfHook(), i + 7, "a store to the name that two shapes hold");
    check(readsThreeOfHook(), 2 * i + 108, "three fresh reads of one variable");
    check(readsStateOfParameter(makesHook(i)) + readsStateOfParameter(makesNode(i)), 2 * i, "the same name on a parameter");
    setsEither(i & 1 ? makesHook(i) : makesNode(i));
    check(readsStateOfEither(), i, "a variable that holds both shapes in turn");
    check(readsQueueOfEither(), i & 1 ? i + 1 : undefined, "a name of the first shape on that variable");
    check(readsKeyOfEither(), i & 1 ? undefined : i + 2, "a name of the second shape on that variable");
}
{
    let before = [countOf("operationAOTCountGuessedPlace", "hit"), countOf("operationAOTCountGuessedPlace", "another-slot"), exits()];
    for (let i = 0; i < 100; i++) {
        setsHook(makesHook(i));
        setsNode(makesNode(i));
        readsStateOfHook();
        readsStateOfNode();
        readsQueueOfHook();
        readsKeyOfNode();
        readsThreeOfHook();
    }
    if (countOf("operationAOTCountGuessedPlace", "hit") > before[0]) {
        check(countOf("operationAOTCountGuessedPlace", "hit") - before[0] >= 700, true, "guessed reads of what the variables usually hold hit");
        check(countOf("operationAOTCountGuessedPlace", "another-slot") - before[1], 0, "guessed reads of what the variables usually hold that find the name in another slot");
        check(exits() - before[2], 0, "no guard fails on what the variables usually hold");
    }
}
for (let i = 0; i < 100; i++) {
    setsHook(makesNode(i));
    setsNode(makesHook(i));
    check(readsStateOfHook(), i, "the first variable holds the second shape");
    check(readsStateOfNode(), i, "the second variable holds the first shape");
    check(readsQueueOfHook(), undefined, "a name that the object in the variable lacks");
    check(readsThreeOfHook(), NaN, "three fresh reads of a variable that holds the other shape");
    writesStateOfHook(i + 7);
    check(readsStateOfHook(), i + 7, "a store to the other shape");
    setsHook(JSON.parse('{"hookNext":1,"hookQueue":' + i + ',"sharedState":' + i + '}'));
    check(readsThreeOfHook(), 2 * i + 200, "the same names in another order");
    setsHook(Object.defineProperty(makesHook(i), "sharedState", { get() { return i + 1; } }));
    check(readsThreeOfHook(), 2 * i + 102, "a getter in the place of the name");
    check(readsStateOfHook(), i + 1, "a getter read alone");
}

if (aotRemarks("readsQueueOfHook") && aotRemarks("readsQueueOfHook").some(remark => remark.startsWith("guessed-place") || remark.startsWith("no-guess"))) {
    let has = (name, remark) => aotRemarks(name).includes(remark);
    let applies = (name, remark) => {
        if (!has(name, remark))
            throw new Error(remark + " does not apply to " + name + ": " + aotRemarks(name).join(" | "));
    };
    let doesNotApply = (name, remark) => {
        if (has(name, remark))
            throw new Error(remark + " applies to " + name + ": " + aotRemarks(name).join(" | "));
    };
    for (let name of ["readsStateOfHook", "writesStateOfHook", "readsStateOfNode", "readsThreeOfHook"]) {
        applies(name, "guessed-place:sharedState");
        applies(name, "guessed-place-of-one-shape:sharedState");
        applies(name, "guessed-place-by-names-on-variable:sharedState");
        doesNotApply(name, "no-guess:disagree");
    }
    applies("readsQueueOfHook", "guessed-place:hookQueue");
    applies("readsNextOfHook", "guessed-place:hookNext");
    applies("readsKeyOfNode", "guessed-place:nodeKey");
    applies("readsStateOfParameter", "no-guess:disagree");
    doesNotApply("readsStateOfParameter", "guessed-place:sharedState");
    applies("readsQueueOfEither", "guessed-place:hookQueue");
    applies("readsKeyOfEither", "guessed-place:nodeKey");
    doesNotApply("readsQueueOfEither", "guessed-place-by-names-on-variable:hookQueue");
    doesNotApply("readsKeyOfEither", "guessed-place-by-names-on-variable:nodeKey");
    doesNotApply("readsQueueOfEither", "no-guess:no-shape");
    applies("readsStateOfEither", "no-guess:disagree");
    doesNotApply("readsStateOfEither", "guessed-place:sharedState");
    if (typeof jscOptions === "function" && jscOptions().useAOTGuardsOverWholeFunctions) {
        applies("readsThreeOfHook", "guards-over-whole-function");
        applies("readsThreeOfHook", "name-check-guards-read:sharedState");
        doesNotApply("readsThreeOfHook", "no-guards-over-whole-function:too-few-places");
        if (jscOptions().useAOTOperationCounters)
            check(exits() >= 300, true, "a guard fails whenever the variable holds something else");
    }
}
