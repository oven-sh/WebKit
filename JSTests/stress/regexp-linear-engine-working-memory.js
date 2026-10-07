//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--maximumRegExpLinearWorkingMemory=65536")

// Every state of the non-backtracking matcher has a copy of what the groups captured, so the
// memory a match can need is known from the program: (states) x (groups). The matcher refuses
// a pattern for which that is more than maximumRegExpLinearWorkingMemory (65536 bytes in this
// test). A refused pattern is compiled as it is without the matcher.

function alternation(groupCount)
{
    let alternatives = [];
    for (let i = 0; i < groupCount; ++i)
        alternatives.push("(" + String.fromCharCode(97 + i % 26) + ")");
    return new RegExp(alternatives.join("|"));
}

function test(groupCount, engine, refusal)
{
    let regExp = alternation(groupCount);
    let statistics = $vm.regExpMatchStatistics(regExp, "0123z", 0);
    if (statistics.engine !== engine || statistics.refusal !== refusal)
        throw new Error(groupCount + " groups: expected " + engine + " (" + refusal + ") but got " + statistics.engine + " (" + statistics.refusal + ")");
    if (statistics.scratchBytes > statistics.maximumScratchBytes || statistics.maximumScratchBytes > 65536)
        throw new Error(groupCount + " groups: the matcher held " + statistics.scratchBytes + " bytes, with a bound of " + statistics.maximumScratchBytes);
    if (engine === "backtracking" && statistics.jit !== $vm.useJIT())
        throw new Error(groupCount + " groups: a refused pattern " + (statistics.jit ? "has" : "does not have") + " the code of the JIT");

    let match = regExp.exec("0123z");
    if (match.index !== 4 || match[0] !== "z" || match[26] !== "z" || match[1] !== undefined)
        throw new Error(groupCount + " groups: matched " + JSON.stringify(match) + " at " + match.index);
}

test(26, "linear", "none");
test(52, "backtracking", "working memory too large");
test(400, "backtracking", "working memory too large");

// The matcher keeps a frame for every slot an instruction wrote, until it is done with what
// follows the instruction. This pattern writes many slots at a time: each copy of the group
// asserts a lookaround that hands over the slots of its 40 groups.
let handsOver = new RegExp("(?:(?=" + "(a)".repeat(40) + ")a?){40}");
let statistics = $vm.regExpMatchStatistics(handsOver, "a".repeat(100), 0);
if (statistics.engine !== "backtracking" || statistics.refusal !== "working memory too large")
    throw new Error(handsOver + ": " + statistics.engine + " (" + statistics.refusal + ")");
