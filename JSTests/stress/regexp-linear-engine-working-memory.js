//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--maximumRegExpLinearWorkingMemory=65536")
//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")
//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--maximumRegExpLinearWorkingMemory=65536", "--useRegExpJIT=0")

// Every state of the non-backtracking matcher has a copy of what the groups captured, so the
// memory a match can need is known from the program: (states) x (groups). The compiler takes
// that bound from the program (maximumScratchBytes), the matcher lets none of its vectors grow
// past what the compiler counted, and the matcher refuses a pattern whose bound is more than
// maximumRegExpLinearWorkingMemory. A refused pattern is compiled as it is without the matcher.

const limit = jscOptions().maximumRegExpLinearWorkingMemory;
const hasRegExpJIT = $vm.useJIT() && !!jscOptions().useRegExpJIT;

// One state per group.
function alternation(groupCount)
{
    let alternatives = [];
    for (let i = 0; i < groupCount; ++i)
        alternatives.push("(" + String.fromCharCode(97 + i % 26) + ")");
    return new RegExp(alternatives.join("|"));
}

// The same in a loop: on "aaa" every group has a state in both lists of the matcher, and each
// state has a copy of the slots of every group.
function alternationInALoop(groupCount)
{
    return new RegExp("(?:" + Array(groupCount).fill("(a)").join("|") + ")*b");
}

function isAccepted(regExp)
{
    return $vm.regExpMatchStatistics(regExp, "", 0).engine === "linear";
}

// The largest member of a family that the matcher accepts. The next one is refused.
function largestAccepted(family)
{
    let accepted = 1;
    let refused = 2;
    while (isAccepted(family(refused))) {
        accepted = refused;
        refused *= 2;
    }
    while (refused - accepted > 1) {
        let middle = (accepted + refused) >> 1;
        if (isAccepted(family(middle)))
            accepted = middle;
        else
            refused = middle;
    }
    return accepted;
}

function checkAccepted(name, regExp, subjects)
{
    for (let subject of subjects) {
        let statistics = $vm.regExpMatchStatistics(regExp, subject, 0);
        if (statistics.engine !== "linear" || statistics.refusal !== "none")
            throw new Error(name + ": expected the matcher but got " + statistics.engine + " (" + statistics.refusal + ")");
        if (statistics.scratchBytes > statistics.maximumScratchBytes || statistics.maximumScratchBytes > limit)
            throw new Error(name + " on " + subject.length + " characters: the matcher held " + statistics.scratchBytes + " bytes, with a bound of " + statistics.maximumScratchBytes + " and a limit of " + limit);
    }
}

function checkRefused(name, regExp, subject)
{
    let statistics = $vm.regExpMatchStatistics(regExp, subject, 0);
    if (statistics.engine !== "backtracking" || statistics.refusal !== "working memory too large")
        throw new Error(name + ": expected a refusal for its working memory but got " + statistics.engine + " (" + statistics.refusal + ")");
    if (statistics.jit !== hasRegExpJIT)
        throw new Error(name + ": a refused pattern " + (statistics.jit ? "has" : "does not have") + " the code of the JIT");
}

function checkAlternationMatch(groupCount)
{
    let match = alternation(groupCount).exec("0123z");
    if (match.index !== 4 || match[0] !== "z" || match[26] !== "z" || match[1] !== undefined)
        throw new Error(groupCount + " groups: matched " + JSON.stringify(match) + " at " + match.index);
}

// What the groups of the loop captured is the same on every engine: the last iteration's.
function checkLoopMatch(groupCount)
{
    let match = alternationInALoop(groupCount).exec("aaab");
    if (match.index !== 0 || match[0] !== "aaab" || match[1] !== "a" || match.length !== groupCount + 1 || match[groupCount] !== undefined)
        throw new Error(groupCount + " groups in a loop: matched " + JSON.stringify(match) + " at " + match.index);
}

let subjectsOfAlternation = ["0123z", "abc", ""];
let most = largestAccepted(alternation);
if (most < 26)
    throw new Error("The matcher accepts an alternation of " + most + " groups at most");
for (let groupCount of [26, most - 1, most]) {
    checkAccepted(groupCount + " groups", alternation(groupCount), subjectsOfAlternation);
    checkAlternationMatch(groupCount);
}
for (let groupCount of [most + 1, most * 2, most * 8]) {
    checkRefused(groupCount + " groups", alternation(groupCount), "0123z");
    checkAlternationMatch(groupCount);
}

// These subjects fill the lists of the matcher. With 57 groups and more, a list once held more
// than the compiler had counted for it, because a Vector grows by half.
let subjectsOfLoop = ["aaa", "aaaaaaaaaaaaab", "a".repeat(40), "b", ""];
let mostInALoop = largestAccepted(alternationInALoop);
if (mostInALoop < 30)
    throw new Error("The matcher accepts a loop over an alternation of " + mostInALoop + " groups at most");
let counts = [2, 17, mostInALoop >> 1, mostInALoop - 1, mostInALoop];
for (let groupCount = 55; groupCount <= 62 && groupCount <= mostInALoop; ++groupCount)
    counts.push(groupCount);
for (let groupCount of counts) {
    checkAccepted(groupCount + " groups in a loop", alternationInALoop(groupCount), subjectsOfLoop);
    checkLoopMatch(groupCount);
}
// A subject that the first way through the pattern matches: on "aaa" a backtracking engine
// tries every choice of a group for every "a".
for (let groupCount of [mostInALoop + 1, mostInALoop * 2]) {
    checkRefused(groupCount + " groups in a loop", alternationInALoop(groupCount), "aaab");
    checkLoopMatch(groupCount);
}

// The limits of this test and the default ones. A change of a number here is a change of what
// the compiler counts. A sanitizer makes the matcher itself larger (the inline storage of its
// vectors), so these are the numbers of a build without one.
let expected = $vm.asanEnabled() ? undefined : { 65536: [40, 38], 8388608: [507, 505] }[limit];
if (expected && (most !== expected[0] || mostInALoop !== expected[1]))
    throw new Error("With " + limit + " bytes the matcher accepts " + most + " groups and " + mostInALoop + " in a loop, expected " + expected);

// The matcher keeps a frame for every slot an instruction wrote, until it is done with what
// follows the instruction. This pattern writes many slots at a time: each copy of the group
// asserts a lookaround that hands over the slots of its 40 groups.
if (limit === 65536)
    checkRefused("the lookaround that hands over 40 groups", new RegExp("(?:(?=" + "(a)".repeat(40) + ")a?){40}"), "a".repeat(100));
