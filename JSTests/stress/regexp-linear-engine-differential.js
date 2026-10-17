//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")

// A match of the non-backtracking matcher is the match of the backtracking engines: the same
// start, the same end and the same groups. This runs one RegExp on both engines and compares.
// $vm.regExpMatchOnBothEngines(regExp, string, start) matches with the engine the RegExp is
// compiled for, and then the bytecode interpreter matches the same RegExp. It returns the index
// of the match when the two have the same output vector (the start and the end of each group,
// then the group that each duplicated name chose), and false when they do not.
//
// The backtracking interpreter answers "no match" when it reaches its limit. That answer is not
// one to compare with, so the function returns null for it. In this function the limit of the
// interpreter is low, and a pattern that would make it backtrack for a minute costs it
// milliseconds.

load("./resources/regexp-linear-engine-patterns.js", "caller relative");

let acceptedPatterns = 0;
let comparisons = 0;
let matches = 0;
let givenUp = 0;

function compare(regExp, subject, start)
{
    let index = $vm.regExpMatchOnBothEngines(regExp, subject, start);
    if (index === null) {
        ++givenUp;
        return;
    }
    ++comparisons;
    if (index === false) {
        let linear = $vm.regExpMatchStatistics(regExp, subject, start);
        let backtracking = $vm.regExpMatchStatistics(regExp, subject, start, "backtracking");
        throw new Error(regExp + " on " + JSON.stringify(subject) + " from " + start + ": the matcher has " + JSON.stringify(linear.captures) + ", the backtracking interpreter has " + JSON.stringify(backtracking.captures));
    }
    if (index >= 0)
        ++matches;
}

// Every start from 0 to startLimit, as a global or sticky RegExp has with lastIndex.
function compareFromEveryStart(source, flags, subjects, startLimit)
{
    let regExp = new RegExp(source, flags);
    if ($vm.regExpMatchStatistics(regExp, "", 0).engine !== "linear")
        return false;
    ++acceptedPatterns;
    for (let subject of subjects) {
        let lastStart = Math.min(subject.length, startLimit);
        for (let start = 0; start <= lastStart; ++start)
            compare(regExp, subject, start);
    }
    return true;
}

// What the two functions report, on one match that the engines agree on and on one that the
// interpreter gives up on.
{
    let regExp = /(?<y>\d{4})-(?<m>\d{2})|(?<m>\d{2})\/(?<y>\d{4})/;
    let linear = $vm.regExpMatchStatistics(regExp, "on 05/2024", 0);
    let backtracking = $vm.regExpMatchStatistics(regExp, "on 05/2024", 0, "backtracking");
    let expected = "3,10,-1,-1,-1,-1,3,5,6,10,3,4";
    if (linear.engine !== "linear" || linear.index !== 3 || String(linear.captures) !== expected)
        throw new Error("The matcher: " + JSON.stringify(linear));
    if (backtracking.engine !== "backtracking" || backtracking.index !== 3 || String(backtracking.captures) !== expected || backtracking.gaveUp)
        throw new Error("The backtracking interpreter: " + JSON.stringify(backtracking));
    if ($vm.regExpMatchOnBothEngines(regExp, "on 05/2024", 0) !== 3 || $vm.regExpMatchOnBothEngines(regExp, "on", 0) !== -1)
        throw new Error("The two engines on " + regExp);

    regExp = /(a*)*b/;
    let subject = "a".repeat(40);
    linear = $vm.regExpMatchStatistics(regExp, subject, 0);
    backtracking = $vm.regExpMatchStatistics(regExp, subject, 0, "backtracking");
    if (linear.engine !== "linear" || linear.index !== -1 || linear.gaveUp)
        throw new Error("The matcher: " + JSON.stringify(linear));
    if (backtracking.engine !== "backtracking" || backtracking.index !== -1 || !backtracking.gaveUp)
        throw new Error("The backtracking interpreter: " + JSON.stringify(backtracking));
    if ($vm.regExpMatchOnBothEngines(regExp, subject, 0) !== null)
        throw new Error("The two engines on " + regExp);
}

// 1. `.*X.*`, with and without ^ and $. The backtracking engines match X and then widen the match
// to its line (DotStarEnclosure). The matcher runs the pattern as it is written.
{
    let wrapped = ["a", "ab", "a|b", "(?:a|b)", "a+", "a*", "a?", "\\ba\\b", "(?=a)a", "(?!b)a", "(?<=x)a", "(?<!x)a", "[^\\n]a", "a$", "^a", "a{2}", "a{1,2}", "\\n", "\\r", "[ab]", "[^a]", ".", "a.", "", "x*", "(?:)", "\\u2028", "a\\b", "\\B", "(?:a(?:b)?)", "\\xe9", "\\d", "\\s", "a|", "|a", "$", "^", "a\\n", "\\na", "(?=\\n)", "(?<=\\n)a", "[\\s\\S]", "a.*", ".*a"];
    let wrappers = [
        x => ".*" + x + ".*",
        x => "^.*" + x + ".*",
        x => ".*" + x + ".*$",
        x => "^.*" + x + ".*$",
        x => ".*?" + x + ".*",
        x => "^.*?" + x + ".*$",
        x => "(?:.*" + x + ".*)",
        x => ".*(" + x + ").*",
        x => "^.*(?:" + x + ").*$",
        x => ".*" + x + ".*?",
        x => ".+" + x + ".*",
        x => ".*" + x + ".+",
    ];
    let flagSets = ["", "m", "s", "ms", "i", "y", "ym", "ys", "u", "v", "imsu"];
    let subjects = ["", "a", "ba", "ab", "xxaxx", "xax\nxbx", "xbx\nxax", "xbx\nxax\nxbx", "\na\n", "a\r\nb", "b\r\na\r\nb", "xx\u2028a\u2029xx", "xaaxax", "bxxaxx", "x\n\nax", "a\nb\na", "\n\n", "\xe9 a", "\u{1d4b3}a\u{1d4b3}", "a\u{1d4b3}\na", "xAx\nxax", "1a 2", "xab\nxa\nab"];

    let before = acceptedPatterns;
    for (let x of wrapped) {
        for (let wrapper of wrappers) {
            for (let flags of flagSets)
                compareFromEveryStart(wrapper(x), flags, subjects, 6);
        }
    }
    if (acceptedPatterns - before !== wrapped.length * wrappers.length * flagSets.length)
        throw new Error("The matcher accepted " + (acceptedPatterns - before) + " of the wrapped patterns");
}

// 2. Case folding with and without u and v, surrogate pairs, groups in loops, groups in
// lookarounds, duplicate names, lazy repeats, empty iterations, assertions.
{
    let sources = [
        ".", "[^a]", "\\u{1F600}", "[\\u{1F600}-\\u{1F64F}]", "\\p{Lu}", "\\p{L}+", "\\P{L}", "\\w", "\\W", "\\b", "\\B", "\\w+", "\\W+",
        "\\u017F", "\\u212A", "k", "s", "[a-z]", "[a-z]+", "[^a-z]", "\\uD83D", "\\uDE00", "[\\uD83D]", "[\\uDE00]", "(?<=\\uD83D)\\uDE00", "\\uD83D(?=\\uDE00)",
        "^.$", "..", ".{2}", "(?<=.)a", "(?<!.)a", "\\S", "[^]", "\\u{10400}", "\\u{10428}", "[\\u{10400}-\\u{10427}]", "\\u00DF", "ss", "\\u0130", "i", "\\u0131",
        "\\u03A3", "\\u03C3", "\\u03C2", "[\\u03C3]", "\\u1E9E", "a\\u0301", ".\\u0301",
        "(?<a>x)|(?<a>y)", "(?<y>\\d{4})-(?<m>\\d{2})|(?<m>\\d{2})\\/(?<y>\\d{4})", "(?:(?<n>a)|(?<n>b))+", "(?<n>a)?(?:(?<m>b)|(?<m>c))",
        // A duplicated name that a lookaround sets, that a lookaround sets and the match then
        // gives up, and that no group of the match has.
        "(?=(?<n>a)|(?<n>b))[ab]", "(?<=(?<n>a)|(?<n>b))c", "(?<n>a)(?=(?<m>b)|(?<m>c))|(?<n>x)", "(?<n>a)(?=(?<k>b))|(?<k>x)(?<n>y)",
        "(?:(?=(?<n>a)|(?<n>b))[ab])+", "(?!(?<n>a)|(?<n>b))c|(?<n>d)", "(?<n>a)(?!(?<m>b)|(?<m>c))|(?<n>b)(?<m>c)",
        "(?<n>a)|(?<n>b)|c", "(c)|(?<n>a)|(?<n>b)", "(?:(?<n>a)|(?<n>b)|c)+", "(?<n>a)?(?:(?<m>b)|(?<m>c)|d)", "(?=(?<n>a))a|(?<n>b)|c",
        "(?:(?<m>a)|(?<m>b))(?=(?<n>c)|(?<n>d))", "(?:(?<m>a)|(?<m>b))(?<=(?<n>a)|(?<n>b))(?=(?<k>c)|(?<k>d))", "(?:(?=(?<n>a)|(?<n>b))[ab]|c)+",
        "(?:(?<n>a)|(?=(?<n>b)).|c){1,3}", "(?:(?<n>a)|(?=(?<n>b)).|c){2}?", "(?:(?=(?<n>a))b|a)|(?<n>q)", "(?:(?!(?<n>a))x|a)|(?<n>q)", "(?!(?<n>a)|(?<n>b))",
        "(?:(a)|b)*", "(?:(a)|(b))+", "(a*)*", "(a|ab)(c|bcd)(d*)", "(?:a(b)?)+", "(z)((a+)?(b+)?(c))*", "(a*)+", "(a+)+", "((a)|(b))*?c", "(a)|b", "(?:(a)|b)\\b",
        "a*?", "(a+?)(b*)", "(?:a|ab)*?c", "a{2,3}?", "a{2,3}", "(a{1,2}){2}", "(?:a{0,2}){2,3}b", "(a){0}b", "(a){0,1}b", "(?:(a)|(b)|(c)){3}",
        "(?:a*)*", "(?:a?)*?b", "(a?)+", "(?:|a)+", "()*", "(?:)*", "(a*)+$", "(?:a|)*b", "(|a)+b", "(?:(?:a?)+)+b", "(a*?)*", "(a*?)+b",
        "(?<!(a))b", "(?!(a))b", "(?<=\\b)a", "(?<=a(?=b))b", "(?=(a)|(b))[ab]", "(?<=(a)|(b))[cd]", "(?=a(?<=a))a", "(?<=^|\\s)a", "a(?=$|\\s)",
        "(?<=(?<=a)b)c", "(?=(?=a)a)a", "(?!(?!a))a", "(?<!(?<!a)b)c", "(?<=a.{0,2})c", "(?<=a{1,3})b", "(?<![ab]{2})c", "(?=(a{1,2}))a", "(?<=(\\d{1,2})(\\d{1,2}))$",
        "^a", "a$", "^$", "^", "$", "^a|b$", "^\\b", "\\b$", "(?:^|b)a", "a(?:$|b)", "(?:^a)+", "(?:a$)+", "^^a", "a$$",
        "a|^b", "(?<=a)b", "a|b|", "|a", "(a)|(b)|", "a||b", "[ab]|a", "ab|a", "a|ab", "(?:a|ab)(?:c|bcd)",
        "[\\s\\S]*?a", "[^\\n]*a", "a[^]*", "[a-c]{2,}", "[^a-c]{1,2}", "\\d+\\.\\d+", "(\\d+)(?:\\.(\\d+))?", "\\s+$", "^\\s+", "^\\s+|\\s+$",
        "A", "[A-Z]", "[^A-Z]", "\\xE9", "[\\xE0-\\xFF]", "\\u00B5", "\\u039C", "\\u03BC",
    ];
    let sourcesOfV = ["[\\p{L}--[a-z]]", "\\p{RGI_Emoji}", "[\\q{ab|c}]", "[\\q{ab|a}]b", "[[a-z]&&[^aeiou]]", "[\\p{L}&&\\p{ASCII}]", "^\\p{RGI_Emoji}$", "[\\q{\\u{1F600}a}\\u{1F600}]"];
    let flagSets = ["", "i", "u", "iu", "v", "iv", "y", "yu", "yiu", "s", "m", "mu", "imsu"];
    let subjects = [
        "", "a", "b", "ab", "ba", "aab", "abab", "aaa", "abcd", "abbbcd", "zaacbbbcac", "xay", "ac", "abc", "c",
        "\u{1F600}", "a\u{1F600}b", "\u{1F600}\u{1F600}", "\uD83D", "\uDE00", "\uDE00\uD83D", "a\uD83Db\uDE00", "\uD83D\uD83D\uDE00", "\uD83D\uDE00\uDE00",
        "k", "K", "\u212A", "s", "S", "\u017F", "ks\u212A\u017F", "\u{10400}", "\u{10428}", "\u{10400}\u{10428}", "ss", "\u00DF", "\u1E9E", "i", "I", "\u0130", "\u0131",
        "\u03A3", "\u03C3", "\u03C2", "a\u0301", "e\u0301a", "2024-05", "05/2024", "12 2024-05 x", "1053", "3.14", "1.", " a ", "a b", "a\nb", "\na", "b\n", "a\n\nb", " \t a \n",
        "A", "Ab", "aB", "\xE9", "\xC9", "\xB5", "\u039C", "\u03BC", "cab", "bc", "abc abc", "dab", "a1 b2",
        "d", "ad", "bd", "xy", "ca", "q", "x",
    ];

    for (let source of sources) {
        for (let flags of flagSets) {
            // `i` and the property escapes need u or v in some of these, and the patterns of
            // surrogate pairs mean another thing without them: both are what this part is for.
            try {
                new RegExp(source, flags);
            } catch {
                continue;
            }
            if (!compareFromEveryStart(source, flags, subjects, 10))
                throw new Error("The matcher refused /" + source + "/" + flags);
        }
    }
    for (let source of sourcesOfV) {
        for (let flags of ["v", "iv", "yv", "mv", "sv"]) {
            if (!compareFromEveryStart(source, flags, subjects, 10))
                throw new Error("The matcher refused /" + source + "/" + flags);
        }
    }
}

// 3. Patterns made from a fixed sequence of numbers. The first ones nest as deep as the generator
// goes, and the rest are smaller.
{
    state = 20261007;
    let before = acceptedPatterns;
    let withLookaround = 0;
    for (let index = 0; index < 4000; ++index) {
        let source = disjunction(index < 1500 ? 0 : 2, false);
        let flags = pick(["", "", "i", "m", "s", "y", "u", "im", "v", "iu"]);
        let subjects = [];
        for (let count = 0; count < 4; ++count)
            subjects.push(subject(10));
        subjects.push(pick(["a", "b", "ab", "ba", "a "]).repeat(1 + random() % 4));
        if (compareFromEveryStart(source, flags, subjects, 3) && /\(\?<?[=!]/.test(source))
            ++withLookaround;
    }
    // Fewer means that this part no longer reaches what it is for.
    if (acceptedPatterns - before < 3000 || withLookaround < 1500)
        throw new Error("Only " + (acceptedPatterns - before) + " of the made patterns ran on the non-backtracking matcher, " + withLookaround + " of them with a lookaround");
}

// 4. Made patterns again, with group names this time: a name is in one group of an alternative and
// again in another alternative. The engines then also report which group of a name matched.
{
    state = 20261008;
    let names = ["n", "m", "k"];
    let duplicated = 0;
    let matchesBefore = matches;
    let withoutAGroupForAName = 0;
    for (let index = 0; index < 3000; ++index) {
        let source = disjunction(index < 500 ? 0 : 1, false, names.slice());
        let flags = pick(["", "", "i", "m", "s", "y", "u", "v"]);
        let subjects = [];
        for (let count = 0; count < 4; ++count)
            subjects.push(subject(10));
        subjects.push(pick(["a", "b", "ab", "ba", "a "]).repeat(1 + random() % 4));
        if (!hasDuplicatedName(source, names) || !compareFromEveryStart(source, flags, subjects, 3))
            continue;
        ++duplicated;
        // How often a name of the pattern has no group in the match: the case in which the
        // engines differ in what they leave in the vector.
        let regExp = new RegExp(source, flags.replace("y", ""));
        for (let subject of subjects) {
            let match = regExp.exec(subject);
            if (match && Object.values(match.groups).includes(undefined))
                ++withoutAGroupForAName;
        }
    }
    // Fewer means that this part no longer reaches what it is for.
    if (duplicated < 400 || matches - matchesBefore < 2500 || withoutAGroupForAName < 800)
        throw new Error("Only " + duplicated + " of the made patterns with a duplicated name ran on the non-backtracking matcher, with " + (matches - matchesBefore) + " matches, " + withoutAGroupForAName + " of them with a name that has no group");
}

// The interpreter gives up on few of these. More means that this test compares less than it says.
if (comparisons < 1300000 || matches < 420000 || givenUp > 100)
    throw new Error(comparisons + " comparisons, " + matches + " of them of a match, and the interpreter gave up " + givenUp + " times");
