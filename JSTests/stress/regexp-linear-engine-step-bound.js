//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")

// The compiler of the non-backtracking matcher takes, from the program of a pattern, the most
// steps a position of the subject can cost a match (maximumStepsPerPosition) and the most
// working memory a match can hold (maximumScratchBytes). No match takes more. This checks that
// on patterns made from a fixed sequence of numbers: groups, alternatives, repeats, and
// lookarounds inside one another.

let state = 1;

function random()
{
    state ^= state << 13;
    state >>>= 0;
    state ^= state >>> 17;
    state ^= state << 5;
    state >>>= 0;
    return state;
}

function pick(list)
{
    return list[random() % list.length];
}

function oneIn(count)
{
    return !(random() % count);
}

function atom(depth, inLookaround)
{
    let kind = random() % 14;
    if (kind < 4)
        return pick(["a", "b", "c", "a", "b", ".", "\\w", "\\d", "[ab]", "[^a]", "\\s", "x"]);
    if (kind < 6 && depth < 4)
        return "(" + disjunction(depth + 1, inLookaround) + ")";
    if (kind < 8 && depth < 4)
        return "(?:" + disjunction(depth + 1, inLookaround) + ")";
    if (kind < 11 && depth < 5)
        return pick(["(?=", "(?!", "(?<=", "(?<!"]) + disjunction(depth + 1, true) + ")";
    if (kind < 12)
        return pick(["^", "$", "\\b", "\\B"]);
    return pick(["a", "b"]);
}

// Inside a lookaround almost every repeat has a limit: the matcher refuses a lookaround of
// unbounded length.
function quantifier(inLookaround)
{
    let kind = random() % 12;
    if (kind < 5)
        return "";
    let lazy = oneIn(4) ? "?" : "";
    if (kind < 6)
        return "?" + lazy;
    if (kind < 7)
        return (inLookaround && !oneIn(40) ? "{0,3}" : "*") + lazy;
    if (kind < 8)
        return (inLookaround && !oneIn(40) ? "{1,2}" : "+") + lazy;
    let low = random() % 3;
    let high = low + random() % 4;
    if (kind < 10)
        return "{" + low + "," + high + "}" + lazy;
    if (kind < 11 || inLookaround)
        return "{" + (1 + random() % 3) + "}";
    return "{" + low + ",}" + lazy;
}

function alternative(depth, inLookaround)
{
    let source = "";
    for (let count = 1 + random() % 4; count; --count) {
        let one = atom(depth, inLookaround);
        let isAssertion = /^(?:\^|\$|\\b|\\B|\(\?<?[=!])/.test(one);
        source += one + (isAssertion ? "" : quantifier(inLookaround));
    }
    return source;
}

function disjunction(depth, inLookaround)
{
    let alternatives = [];
    for (let count = oneIn(3) ? 2 + random() % 2 : 1; count; --count)
        alternatives.push(alternative(depth, inLookaround));
    return alternatives.join("|");
}

function subject()
{
    let alphabet = oneIn(5) ? ["a"] : ["a", "a", "b", "b", "c", "x", " ", "1", "\n"];
    let characters = "";
    for (let length = random() % 48; length; --length)
        characters += pick(alphabet);
    return characters;
}

let accepted = 0;
let acceptedWithLookaround = 0;
let tooCostly = 0;
for (let index = 0; index < 2000; ++index) {
    let source = disjunction(0, false);
    let regExp = new RegExp(source, pick(["", "", "i", "m", "s", "g", "y", "u", "im"]));

    // The empty subject says which engine the pattern has. The backtracking interpreter gets
    // no other subject: this test is not about what it costs.
    let statistics = $vm.regExpMatchStatistics(regExp, "", 0);
    if (statistics.engine !== "linear") {
        if (statistics.refusal === "lookaround too costly")
            ++tooCostly;
        continue;
    }
    ++accepted;
    if (/\(\?<?[=!]/.test(source))
        ++acceptedWithLookaround;
    if (statistics.maximumStepsPerPosition > 4 * 65536)
        throw new Error(regExp + " was accepted with a bound of " + statistics.maximumStepsPerPosition + " steps per position");

    for (let count = 0; count < 5; ++count) {
        let characters = subject();
        let start = oneIn(3) ? random() % (characters.length + 1) : 0;
        statistics = $vm.regExpMatchStatistics(regExp, characters, start);
        if (statistics.steps > statistics.maximumStepsPerPosition * (characters.length - start + 1))
            throw new Error(regExp + " on " + JSON.stringify(characters) + " from " + start + ": " + statistics.steps + " steps, with a bound of " + statistics.maximumStepsPerPosition + " per position");
        if (statistics.scratchBytes > statistics.maximumScratchBytes)
            throw new Error(regExp + " on " + JSON.stringify(characters) + " from " + start + ": " + statistics.scratchBytes + " bytes, with a bound of " + statistics.maximumScratchBytes);
    }
}

// The numbers above make these patterns. Fewer means that this test no longer reaches what it
// is for.
if (accepted < 1400 || acceptedWithLookaround < 1000 || tooCostly < 10)
    throw new Error("Only " + accepted + " patterns ran on the non-backtracking matcher, " + acceptedWithLookaround + " of them with a lookaround, and " + tooCostly + " were too costly");
