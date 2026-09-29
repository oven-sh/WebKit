//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--maxRegExpStackSize=65536")

// Every state of the non-backtracking matcher has a copy of what the groups captured, so the
// memory a match can need is known from the program: (states) x (groups). The matcher refuses
// a pattern for which that is more than the interpreter may use (65536 bytes in this test).

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
    if (statistics.scratchBytes > 65536)
        throw new Error(groupCount + " groups: the matcher held " + statistics.scratchBytes + " bytes");

    let match = regExp.exec("0123z");
    if (match.index !== 4 || match[0] !== "z" || match[26] !== "z" || match[1] !== undefined)
        throw new Error(groupCount + " groups: matched " + JSON.stringify(match) + " at " + match.index);
}

test(26, "linear", "none");
test(52, "backtracking", "working memory too large");
test(400, "backtracking", "working memory too large");
