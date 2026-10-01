//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1", "--maximumRegExpLinearProgramSize=200")

// A pattern the non-backtracking matcher cannot run goes to the bytecode interpreter, and
// matches what it always matched.

function describe(value)
{
    return JSON.stringify(value, (key, entry) => entry === undefined ? "<undefined>" : entry);
}

function check(regExp, subject, engine, refusal, expected)
{
    let statistics = $vm.regExpMatchStatistics(regExp, subject, 0);
    if (statistics.engine !== engine || statistics.refusal !== refusal)
        throw new Error(regExp + ": expected " + engine + " (" + refusal + ") but got " + statistics.engine + " (" + statistics.refusal + ")");

    let match = regExp.exec(subject);
    let actual = match ? { index: match.index, match: Array.from(match) } : null;
    if (describe(actual) !== describe(expected))
        throw new Error(regExp + " on " + describe(subject) + ": expected " + describe(expected) + " but got " + describe(actual));
}

function refused(regExp, subject, refusal, expected)
{
    check(regExp, subject, "backtracking", refusal, expected);
}

function accepted(regExp, subject, expected)
{
    check(regExp, subject, "linear", "none", expected);
}

// What a backreference matches depends on what a group captured, which is not a state of an
// automaton.
refused(/(a)\1/, "aa", "backreference", { index: 0, match: ["aa", "a"] });
refused(/(?<n>a)\k<n>/, "aa", "backreference", { index: 0, match: ["aa", "a"] });
refused(/(?:(a)|b)\1?/, "b", "backreference", { index: 0, match: ["b", undefined] });
refused(/(a+)b\1/, "aabaa", "backreference", { index: 0, match: ["aabaa", "aa"] });
refused(/(?<=\1(a))b/, "aab", "backreference", { index: 2, match: ["b", "a"] });
refused(/(?=(a+))a*b\1/, "baaabac", "backreference", { index: 3, match: ["aba", "a"] });

// A reference to a group that cannot have matched yet is not one.
accepted(/\1(a)/, "a", { index: 0, match: ["a", "a"] });
accepted(/(a\1)/, "aa", { index: 0, match: ["a", "a"] });

// The matcher runs a lookaround to its end at every position that asserts it. One that can
// match a string of any length would make that quadratic.
refused(/(?=.*\d)\w+/, "ab1", "lookaround of unbounded length", { index: 0, match: ["ab1"] });
refused(/(?!.*\d)\w+/, "ab1c", "lookaround of unbounded length", { index: 3, match: ["c"] });
refused(/(?<=a+)b/, "aab", "lookaround of unbounded length", { index: 2, match: ["b"] });
refused(/(?<!a*)b/, "aab", "lookaround of unbounded length", null);
refused(/(?<=(\d+)(\d+))$/, "1053", "lookaround of unbounded length", { index: 4, match: ["", "1", "053"] });
refused(/(?=(?:ab){2,})a/, "abab", "lookaround of unbounded length", { index: 0, match: ["a"] });

accepted(/(?=.{0,3}\d)\w+/, "ab1", { index: 0, match: ["ab1"] });
accepted(/(?<=a{1,2})b/, "aab", { index: 2, match: ["b"] });
accepted(/(?<=(\d{1,3})(\d{1,3}))$/, "1053", { index: 4, match: ["", "1", "053"] });
accepted(/(?=(?:){3,}a)a/, "a", { index: 0, match: ["a"] });

// A counted repeat is unrolled, and this test allows a program of 200 instructions.
refused(/a{300}/, "a".repeat(300), "program too large", { index: 0, match: ["a".repeat(300)] });
refused(/(?:a{20}){20}/, "a".repeat(400), "program too large", { index: 0, match: ["a".repeat(400)] });
refused(/a{0,300}b/, "aab", "program too large", { index: 0, match: ["aab"] });
refused(/(?=a{300})a/, "a".repeat(300), "program too large", { index: 0, match: ["a"] });

accepted(/a{100}/, "a".repeat(100), { index: 0, match: ["a".repeat(100)] });
accepted(/(?:a{5}){20}/, "a".repeat(100), { index: 0, match: ["a".repeat(100)] });
accepted(/(?:(?:(?:){1000}){1000}){1000}a/, "a", { index: 0, match: ["a"] });
accepted(/(?:(?:(?:\b){1000}){1000}){1000}a/, "a", { index: 0, match: ["a"] });
accepted(/(?:(?:(?:){0,1000}){0,1000}){0,1000}a/, "a", { index: 0, match: ["a"] });

// A lookaround is run to its end at every position that asserts it, and one inside another
// at every position the outer one reads. What a position of the subject costs is therefore a
// product, which has a limit: four steps per instruction of the largest program, 800 here.
refused(/(?=a{0,4}(?=a{0,4}(?=a{0,4}b)))a/, "aab", "lookaround too costly", { index: 0, match: ["a"] });
refused(/(?=\w{0,6}(?=\w{0,6}(?=\w{0,6}(\d))))a/, "ab1", "lookaround too costly", { index: 0, match: ["a", "1"] });
refused(/(?=a{0,9}(?!a{0,9}c))a/, "aab", "lookaround too costly", { index: 0, match: ["a"] });
// Each of the ten iterations can begin after as many characters as the ones before it read.
refused(/(?=(?:a|aa){0,10}b)a/, "aab", "lookaround too costly", { index: 0, match: ["a"] });

accepted(/(?=a{0,4}(?=a{0,4}b))a/, "aab", { index: 0, match: ["a"] });
accepted(/(?=\w{0,4}(?=\w{0,4}(\d)))a/, "ab1", { index: 0, match: ["a", "1"] });
accepted(/(?=(?:a|aa){0,5}b)a/, "aab", { index: 0, match: ["a"] });
// A long lookaround with one way through it costs its length once.
accepted(/(?=a{40})a/, "a".repeat(40), { index: 0, match: ["a"] });
accepted(/(?=a{0,40}b)a/, "aab", { index: 0, match: ["a"] });
// The copies of a repeated group assert one lookaround, and a position evaluates it once.
accepted(/(?:(?=a{1,3})a){1,40}/, "aaa", { index: 0, match: ["aaa"] });

// Lookarounds can be inside one another 16 deep.
function nest(depth)
{
    let source = "a";
    for (let i = 0; i < depth; ++i)
        source = "(?=" + source + ")";
    return new RegExp(source + "a");
}
accepted(nest(16), "a", { index: 0, match: ["a"] });
refused(nest(17), "a", "nesting too deep", { index: 0, match: ["a"] });
