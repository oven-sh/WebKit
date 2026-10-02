//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")

// The RegExp nodes of DFG and FTL code with the non-backtracking matcher on. An accepted
// pattern has no code of the JIT, so no tier has anything to inline, and every match goes to
// the matcher: from the operations the nodes call, and from the compiler thread, which matches
// a constant subject when it folds a node. A refused pattern keeps the code of the JIT.

function describe(value)
{
    return JSON.stringify(value, (key, entry) => entry === undefined ? "<undefined>" : entry);
}

function check(name, actual, expected)
{
    if (describe(actual) !== describe(expected))
        throw new Error(name + ": expected " + describe(expected) + " but got " + describe(actual));
}

// Subjects the compiler cannot know.
function accepted(subject)
{
    let date = /(\d{4})-(\d{2})-(\d{2})/.exec(subject);
    return [
        /^(?:a|b)+!?$/.test(subject),
        date && [date.index, date[1], date[2], date[3]],
        subject.replace(/\s+/g, "_"),
        subject.split(/\s*,\s*/).length,
        subject.search(/\d/),
        subject.match(/[a-z]+/g),
        // The backtracking engines give up on this one and answer "no match", so these four
        // say which engine ran.
        /(a*)*b|a*!/.test(subject),
        /(a*)*b|a*!/.exec(subject) !== null,
        subject.replace(/(a*)*b|a*!/, "").length,
        subject.split(/(?:a*)*b|a*!/).length,
    ];
}
noInline(accepted);

// Subjects the compiler knows: it folds the node, and matches on its own thread to do so.
function folded()
{
    return [
        /^(?:a|b)+$/.test("abba"),
        /(\d+)-(\d+)/.exec("10-20")[2],
        "a, b,c".split(/\s*,\s*/).length,
        "on 2026-10-01".replace(/(\d+)-(\d+)-(\d+)/, "$3.$2.$1"),
        /(a*)*b|a*!/.test("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!"),
    ];
}
noInline(folded);

function refused(subject)
{
    let tag = /<(\w+)>[^<]*<\/\1>/.exec(subject);
    return [
        /(a)\1/.test(subject),
        tag && tag[1],
        subject.replace(/(\w)\1/g, "$1"),
        /(?=.*\d)\w+/.test(subject),
    ];
}
noInline(refused);

let manyA = "a".repeat(64) + "!";
for (let i = 0; i < 20000; ++i) {
    check("accepted", accepted("ab 2026-10-01, x"), [false, [3, "2026", "10", "01"], "ab_2026-10-01,_x", 2, 3, ["ab", "x"], true, true, 14, 2]);
    check("accepted", accepted(manyA), [true, null, manyA, 1, -1, ["a".repeat(64)], true, true, 0, 2]);
    check("folded", folded(), [true, "20", 3, "on 01.10.2026", true]);
    check("refused", refused("aa <b>x</b> 1"), [true, "b", "a <b>x</b> 1", true]);
    check("refused", refused("ab"), [false, null, "ab", false]);
}

if ($vm.useJIT() && $vm.useDFGJIT()) {
    for (let f of [accepted, folded, refused]) {
        if (!numberOfDFGCompiles(f))
            throw new Error(f.name + " never ran as DFG code");
    }
}

for (let [regExp, subject] of [[/^(?:a|b)+!?$/, "ab"], [/(\d{4})-(\d{2})-(\d{2})/, "2026-10-01"], [/(a*)*b|a*!/, "a!"]]) {
    let statistics = $vm.regExpMatchStatistics(regExp, subject, 0);
    if (statistics.engine !== "linear" || statistics.jit)
        throw new Error(regExp + " ran on " + statistics.engine + (statistics.jit ? ", with the code of the JIT" : ""));
}
for (let [regExp, subject] of [[/(a)\1/, "aa"], [/(?=.*\d)\w+/, "a1"]]) {
    let statistics = $vm.regExpMatchStatistics(regExp, subject, 0);
    if (statistics.engine !== "backtracking" || statistics.jit !== $vm.useJIT())
        throw new Error(regExp + " ran on " + statistics.engine + (statistics.jit ? ", with" : ", without") + " the code of the JIT");
}
