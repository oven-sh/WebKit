//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")

// What a match costs the non-backtracking matcher grows with the length of the subject and
// with nothing else. The matcher counts its steps exactly, so no clock is needed: the steps
// for subjects of n, 2n and 3n characters are on one line, and the working memory is the same
// for all three.

function statisticsOf(regExp, subject)
{
    let statistics = $vm.regExpMatchStatistics(regExp, subject, 0);
    if (statistics.engine !== "linear")
        throw new Error(regExp + " did not run on the non-backtracking matcher: " + statistics.refusal);
    return statistics;
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
        // twice at one position (once per value of "consumed"), so a character costs at most
        // the program twice over, plus once over for the states that wait on a character.
        let stepsPerCharacter = firstDifference / unit;
        if (stepsPerCharacter > 3 * one.programSize)
            throw new Error(regExp + " takes " + stepsPerCharacter + " steps per character with a program of " + one.programSize + " instructions");

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
