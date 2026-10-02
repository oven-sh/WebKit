//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")

// What a match costs the non-backtracking matcher grows with the length of the subject and
// with nothing else. The matcher counts its steps exactly, so no clock is needed: the steps
// for subjects of n, 2n and 3n characters are on one line, and the working memory is the same
// for all three.
//
// How steep the line is has a limit too. The compiler takes from the program the most steps a
// position of the subject can cost (maximumStepsPerPosition), and it refuses a program that
// can cost more than four steps for each of the maximumRegExpLinearProgramSize instructions.

const maximumProgramSize = 65536;

function statisticsOf(regExp, subject)
{
    let statistics = $vm.regExpMatchStatistics(regExp, subject, 0);
    if (statistics.engine !== "linear")
        throw new Error(regExp + " did not run on the non-backtracking matcher: " + statistics.refusal);
    if (statistics.steps > statistics.maximumStepsPerPosition * (subject.length + 1))
        throw new Error(regExp + " took " + statistics.steps + " steps for " + subject.length + " characters, with a bound of " + statistics.maximumStepsPerPosition + " per position");
    if (statistics.maximumStepsPerPosition > 4 * maximumProgramSize)
        throw new Error(regExp + " was accepted with a bound of " + statistics.maximumStepsPerPosition + " steps per position");
    return statistics;
}

function hasLookaround(regExp)
{
    return /\(\?<?[=!]/.test(regExp.source);
}

function testLinear(regExp, makeSubject, expectedIndex)
{
    for (let unit of [16, 64, 4096]) {
        let one = statisticsOf(regExp, makeSubject(unit));
        let two = statisticsOf(regExp, makeSubject(2 * unit));
        let three = statisticsOf(regExp, makeSubject(3 * unit));

        for (let statistics of [one, two, three]) {
            if (statistics.index !== expectedIndex)
                throw new Error(regExp + " matched at " + statistics.index + ", expected " + expectedIndex);
        }

        let firstDifference = two.steps - one.steps;
        let secondDifference = three.steps - two.steps;
        if (firstDifference <= 0 || firstDifference !== secondDifference)
            throw new Error(regExp + " is not linear: " + one.steps + ", " + two.steps + ", " + three.steps + " steps for " + unit + ", " + 2 * unit + ", " + 3 * unit + " characters");

        // A step is one instruction of the program, run for one state. No state is entered
        // twice at one position (once per value of "consumed"), and only an instruction that
        // waits on a character has states to run. So without a lookaround a character costs
        // at most four steps per instruction. A lookaround adds what its evaluations cost.
        let stepsPerCharacter = firstDifference / unit;
        if (stepsPerCharacter > one.maximumStepsPerPosition)
            throw new Error(regExp + " takes " + stepsPerCharacter + " steps per character, with a bound of " + one.maximumStepsPerPosition + " per position");
        if (!hasLookaround(regExp) && one.maximumStepsPerPosition > 4 * one.programSize)
            throw new Error(regExp + " has a bound of " + one.maximumStepsPerPosition + " steps per position for a program of " + one.programSize + " instructions");

        if (one.scratchBytes !== two.scratchBytes || two.scratchBytes !== three.scratchBytes)
            throw new Error(regExp + " holds " + one.scratchBytes + ", " + two.scratchBytes + ", " + three.scratchBytes + " bytes for " + unit + ", " + 2 * unit + ", " + 3 * unit + " characters");
    }
}

// The same subject as a Latin-1 string and, with a character past U+00FF after it, as a UTF-16
// string.
let latin1 = (repeated, end) => count => repeated.repeat(count) + end;
let utf16 = (repeated, end) => count => repeated.repeat(count) + end + "\u2603";

for (let subject of [latin1, utf16]) {
    // Exponential for backtracking: the YARR engines stop at their match limit after 2^27 tries.
    testLinear(/(a*)*b/, subject("a", "!"), -1);
    testLinear(/(a+)+b/, subject("a", "!"), -1);
    testLinear(/(a|a)*b/, subject("a", "!"), -1);
    testLinear(/(a|aa)+b/, subject("a", "!"), -1);
    testLinear(/(x+x+)+y/, subject("x", "!"), -1);
    testLinear(/^(\w+\s?)*$/, subject("a ", "!"), -1);
    testLinear(/(?:a*)*?b/, subject("a", "!"), -1);
    testLinear(/((a*)*)*b/, subject("a", "!"), -1);
    testLinear(/(?:(?=a)a|a)*b/, subject("a", "!"), -1);
    testLinear(/(?:a(?<=a)|a)*b/, subject("a", "!"), -1);

    // Lookarounds inside one another: every position evaluates the outer one, and every
    // position the outer one reads evaluates the inner one.
    testLinear(/(?:(?=a{0,4}(?=a{0,4}(?!a{0,4}b)))a)*b/, subject("a", "!"), -1);
    testLinear(/(?:a(?<=a{1,3}(?<!b{1,2}a{0,2})))*b/, subject("a", "!"), -1);
    testLinear(/(?:(?=(?:a|aa){0,8}(a?))a)*b/, subject("a", "!"), -1);

    // Polynomial for backtracking, and no limit stops it.
    testLinear(/a*a*a*a*a*a*a*a*b/, subject("a", "!"), -1);
    testLinear(/a*b/, subject("a", "!"), -1);
    testLinear(/(?:a{1,8}){1,8}b/, subject("a", "!"), -1);

    // Matches cost the same.
    testLinear(/(a*)*b/, subject("a", "b"), 0);
    testLinear(/a*a*a*a*a*a*a*a*b/, subject("a", "b"), 0);
    testLinear(/^(\w+\s?)*\W?$/, subject("a ", ""), 0);
}

// A subject of code points, for a pattern that reads them.
testLinear(/(\p{L}*)*!/u, count => "\u{10400}".repeat(count) + "?", -1);
testLinear(/(.*)*!/u, count => "\u{1F600}".repeat(count) + "?", -1);

// A counted repeat is a copy of its atom for each count. While the subject is shorter than the
// count, a longer subject also has more states alive at a position, so the steps grow faster
// than the subject does. The bound for a position holds all the same, and past the count the
// steps are on one line.
for (let length of [50, 100, 200, 300])
    statisticsOf(/x{0,300}y/, "x".repeat(length) + "!");
testLinear(/x{0,300}y/, count => "x".repeat(300 + count) + "!", -1);
testLinear(/(?:\w+\s){1,50}!/, count => "ab ".repeat(50 + count) + "?", -1);

// What lookarounds inside one another cost a position is the product of what each one can
// read. Six of these, each with four characters to read, are under the limit. Seven are not,
// and the matcher refuses them.
function nested(depth)
{
    let source = "b";
    for (let i = 0; i < depth; ++i)
        source = "(?=a{0,4}" + source + ")";
    return new RegExp(source);
}

// Every subject is longer than what the three lookarounds read together.
testLinear(nested(3), count => "a".repeat(16 + count), -1);

if (statisticsOf(nested(6), "a".repeat(40)).index !== -1)
    throw new Error("Six lookarounds inside one another matched");

let refused = $vm.regExpMatchStatistics(nested(7), "a", 0);
if (refused.engine !== "backtracking" || refused.refusal !== "lookaround too costly")
    throw new Error("Seven lookarounds inside one another: " + refused.engine + " (" + refused.refusal + ")");
