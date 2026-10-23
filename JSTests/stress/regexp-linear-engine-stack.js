//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")
//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--useRegExpJIT=0")
//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--maxPerThreadStackUsage=1073741824")
//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--maxPerThreadStackUsage=1073741824", "--softReservedZoneSize=70000") unless $asan

// Whether a RegExp runs on the non-backtracking matcher depends on its pattern, and not on the
// stack that is left when the RegExp is first used.
// - The compiler of the matcher recurses once per level of groups. It refuses a pattern with
//   more than 32 levels, whatever the stack is.
// - When the stack has too little room for the compiler, the compile fails as the other stages
//   of a compile do: the match throws a RangeError, nothing is remembered, and the next match
//   asks the matcher again.
// The third run lets JavaScript use the stack to its end, as a thread with a small stack does.
// The fourth also leaves native code less stack than it has by default at the end of a
// recursion, so that the compiler does run out of it there.

function describe(value)
{
    return JSON.stringify(value, (key, entry) => entry === undefined ? "<undefined>" : entry);
}

function expectSame(name, actual, expected)
{
    if (describe(actual) !== describe(expected))
        throw new Error(name + ": expected " + describe(expected) + " but got " + describe(actual));
}

// [a-c]x: the JIT moves the class behind the x when it compiles a pattern. The answers below
// are the same whichever engine gives them.
function nested(depth, open = "(?:")
{
    return "[a-c]x" + open.repeat(depth) + "a" + ")".repeat(depth);
}

function answers(regExp)
{
    let statistics = $vm.regExpMatchStatistics(regExp, "axa", 0);
    let first = regExp.exec("axa");
    let second = regExp.exec("xaa");
    return { engine: statistics.engine, refusal: statistics.refusal, axa: first && first.index, xaa: second && second.index };
}

const onTheMatcher = { engine: "linear", refusal: "none", axa: 0, xaa: null };
const tooDeep = { engine: "backtracking", refusal: "nesting too deep", axa: 0, xaa: null };

// The limit is on the levels, of groups of every kind. The last kind has a lookahead in its
// innermost group, which is one level more. Lookarounds inside one another have a lower limit
// of their own.
for (let [open, most] of [["(?:", 32], ["(", 32], ["(?:b|", 32], ["(?i:", 32], ["(?:(?=a)", 31], ["(?=", 16]]) {
    expectSame(open + " x " + most, answers(new RegExp(nested(most, open))), onTheMatcher);
    expectSame(open + " x " + (most + 1), answers(new RegExp(nested(most + 1, open))), tooDeep);
}
expectSame("200 levels", answers(new RegExp(nested(200))), tooDeep);

// The compile that finds no stack. The mode "outOfStack" makes a RegExp that asks the matcher
// in that call find the compiler out of stack, as it is at the end of a deep recursion.
function asksSoFar(regExp)
{
    return $vm.regExpMatchStatistics(regExp, "", 0).linearCompiles;
}

function firstUseWithoutStack(regExp)
{
    try {
        $vm.regExpMatchStatistics(regExp, "axa", 0, "outOfStack");
    } catch (error) {
        return error.constructor.name + ": " + error.message;
    }
    return "no error";
}

const outOfStack = "RangeError: Out of memory: Invalid regular expression: too many nested disjunctions";
for (let source of [nested(3) + "|y", nested(3, "(") + "|y", "[a-c]x(?=a)a|y", "[a-c]xa|y"]) {
    let regExp = new RegExp(source);
    let before = asksSoFar(/a RegExp that has its program/);
    expectSame("/" + source + "/, first use", firstUseWithoutStack(regExp), outOfStack);
    // Nothing is remembered: the RegExp asks again, and gets its program.
    expectSame("/" + source + "/, second use", answers(regExp), onTheMatcher);
    expectSame("/" + source + "/, times it asked", asksSoFar(regExp) - before, 2);
    // A RegExp that has its program does not ask, and the mode is over with the call.
    expectSame("/" + source + "/, with its program", firstUseWithoutStack(regExp), "no error");
    expectSame("/" + source + "/, the next RegExp", answers(new RegExp(source + "|z")), onTheMatcher);
}

// The stack as it is at the end of a recursion. A RegExp that is first used there runs on the
// matcher, or its match throws the RangeError of the compile. It does not run on a backtracking
// engine because the stack was short.
const stackOverflow = "Maximum call stack size exceeded.";
function fromTheDeepestFrame(callback)
{
    function recurse()
    {
        try {
            return recurse();
        } catch (error) {
            if (!(error instanceof RangeError) || error.message !== stackOverflow)
                throw error;
            // From the deepest frame that has the stack for the callback itself.
            return callback();
        }
    }
    return recurse();
}

const hasDefaultReservedZone = jscOptions().softReservedZoneSize >= 128 * 1024;
const depths = [0, 1, 2, 4, 8, 16, 24, 32];
let regExps = depths.map(depth => new RegExp(nested(depth) + "|w"));
let firstUses = fromTheDeepestFrame(() => regExps.map(regExp => {
    try {
        let statistics = $vm.regExpMatchStatistics(regExp, "axa", 0);
        return statistics.engine + ", " + statistics.refusal + ", " + statistics.index;
    } catch (error) {
        if (error.message === stackOverflow)
            throw error;
        return error.constructor.name + ": " + error.message;
    }
}));
depths.forEach((depth, index) => {
    if (firstUses[index] !== "linear, none, 0" && firstUses[index] !== outOfStack)
        throw new Error(depth + " levels, first used from the deepest frame: " + firstUses[index]);
    // With the stack that native code has by default at the end of a recursion, the compiler
    // has the room for the patterns that programs have. A sanitizer makes its frames several
    // times larger.
    if (depth <= 8 && hasDefaultReservedZone && !$vm.asanEnabled() && firstUses[index] !== "linear, none, 0")
        throw new Error(depth + " levels, first used from the deepest frame: " + firstUses[index]);
    expectSame(depth + " levels, next use", answers(regExps[index]), onTheMatcher);
});
