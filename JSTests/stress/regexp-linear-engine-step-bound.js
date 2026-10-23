//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")

// The compiler of the non-backtracking matcher takes, from the program of a pattern, the most
// steps a position of the subject can cost a match (maximumStepsPerPosition) and the most
// working memory a match can hold (maximumScratchBytes). No match takes more. This checks that
// on the patterns of resources/regexp-linear-engine-patterns.js.

load("./resources/regexp-linear-engine-patterns.js", "caller relative");

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
