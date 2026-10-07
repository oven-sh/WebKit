// Patterns and subjects for the tests of the non-backtracking matcher, made from a fixed sequence
// of numbers: groups, alternatives, repeats, and lookarounds inside one another. A test sets
// `state` to start the sequence somewhere else.

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

function subject(lengthLimit = 48)
{
    let alphabet = oneIn(5) ? ["a"] : ["a", "a", "b", "b", "c", "x", " ", "1", "\n"];
    let characters = "";
    for (let length = random() % lengthLimit; length; --length)
        characters += pick(alphabet);
    return characters;
}
