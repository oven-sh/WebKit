// Patterns and subjects for the tests of the non-backtracking matcher, made from a fixed sequence
// of numbers: groups, alternatives, repeats, and lookarounds inside one another. A test sets
// `state` to start the sequence somewhere else.
//
// With `names`, a list of group names, some groups take a name. A name is used once on any way
// through the pattern and again in another alternative, which is what the syntax allows, so
// the patterns have duplicated names. Without `names` the sequence of patterns is as it was
// before names existed here.

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

function atom(depth, inLookaround, names)
{
    let kind = random() % 14;
    if (kind < 4)
        return pick(["a", "b", "c", "a", "b", ".", "\\w", "\\d", "[ab]", "[^a]", "\\s", "x"]);
    if (kind < 6 && depth < 4) {
        if (names && names.length && !oneIn(3)) {
            // The name is gone for the rest of this alternative, and for what is inside the group.
            let name = names.splice(random() % names.length, 1)[0];
            return "(?<" + name + ">" + disjunction(depth + 1, inLookaround, names) + ")";
        }
        return "(" + disjunction(depth + 1, inLookaround, names) + ")";
    }
    if (kind < 8 && depth < 4)
        return "(?:" + disjunction(depth + 1, inLookaround, names) + ")";
    if (kind < 11 && depth < 5)
        return pick(["(?=", "(?!", "(?<=", "(?<!"]) + disjunction(depth + 1, true, names) + ")";
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

function alternative(depth, inLookaround, names)
{
    let source = "";
    for (let count = 1 + random() % 4; count; --count) {
        let one = atom(depth, inLookaround, names);
        let isAssertion = /^(?:\^|\$|\\b|\\B|\(\?<?[=!])/.test(one);
        source += one + (isAssertion ? "" : quantifier(inLookaround));
    }
    return source;
}

function disjunction(depth, inLookaround, names)
{
    let alternatives = [];
    // Every alternative starts from the same names. What one of them took is gone for the terms
    // after the disjunction.
    let namesBefore = names ? names.slice() : null;
    let taken = new Set;
    for (let count = oneIn(3) ? 2 + random() % 2 : 1; count; --count) {
        let namesOfAlternative = namesBefore ? namesBefore.slice() : undefined;
        alternatives.push(alternative(depth, inLookaround, namesOfAlternative));
        if (namesBefore) {
            for (let name of namesBefore) {
                if (!namesOfAlternative.includes(name))
                    taken.add(name);
            }
        }
    }
    if (names) {
        for (let index = names.length - 1; index >= 0; --index) {
            if (taken.has(names[index]))
                names.splice(index, 1);
        }
    }
    return alternatives.join("|");
}

function hasDuplicatedName(source, names)
{
    return names.some(name => source.split("(?<" + name + ">").length > 2);
}

function subject(lengthLimit = 48)
{
    let alphabet = oneIn(5) ? ["a"] : ["a", "a", "b", "b", "c", "x", " ", "1", "\n"];
    let characters = "";
    for (let length = random() % lengthLimit; length; --length)
        characters += pick(alphabet);
    return characters;
}
