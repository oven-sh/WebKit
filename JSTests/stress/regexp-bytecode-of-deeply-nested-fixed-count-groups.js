//@ runDefault
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

// Groups that are repeated a fixed number of times, one inside the other, thousands deep. The bytecode compiler takes a time in proportion to the depth for them, as it does for groups that are repeated
// lazily: it walks the groups below a group once, not once for each group above them. The count that no JIT takes keeps the pattern out of the JIT.
function nest(depth, quantifier, number) {
    return new RegExp("(?:".repeat(depth) + "\\b" + (")" + quantifier).repeat(depth) + "|q{2147483648}|never" + number);
}

// The time to compile one such pattern and to match it once: the shortest of several, each with a text of its own.
let number = 0;
function shortestTime(depth, quantifier) {
    let shortest = Infinity;
    for (let i = 0; i < 5; ++i) {
        const pattern = nest(depth, quantifier, ++number);
        const began = preciseTime();
        if (pattern.exec("") !== null)
            throw new Error("a match in the empty string at depth " + depth);
        shortest = Math.min(shortest, preciseTime() - began);
    }
    return shortest;
}

for (const depth of [3000, 6000]) {
    const lazy = shortestTime(depth, "{2,3}?");
    const fixed = shortestTime(depth, "{2}");
    if (fixed > lazy * 3)
        throw new Error("at depth " + depth + " groups with a fixed count take " + (fixed / lazy).toFixed(1) + " times as long as lazy ones");
}
