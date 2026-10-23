//@ runDefault("--regExpMatchLimit=10000")
//@ runDefault("--regExpMatchLimit=10000", "--useRegExpJIT=0")

// A search that uses up Options::regExpMatchLimit is abandoned. It has no answer, so every RegExp
// entry point throws a RangeError for it instead of reporting no match. The limit is lowered so
// that the interpreter and JIT code both reach it at once; each engine counts its own steps.

const limitMessage = "Regular expression backtracking limit exceeded";

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

function shouldAbandon(run, what) {
    let result;
    try {
        result = run();
    } catch (error) {
        if (!(error instanceof RangeError) || error.message !== limitMessage)
            throw new Error(what + ": expected the RangeError, got " + error);
        return;
    }
    throw new Error(what + ": expected the RangeError, got the result " + JSON.stringify(result));
}

// 35 a's then "c" matches, at the "c". Finding that takes about 1e8 steps: the first alternative
// splits the a's into runs of one and two in every possible way at each of 35 start positions.
const pattern = "(?:a|aa)+b|c";
const subject = "a".repeat(35) + "c";
const small = "aaac";

shouldAbandon(() => new RegExp(pattern).test(subject), "test");
shouldAbandon(() => new RegExp(pattern).exec(subject), "exec");
shouldAbandon(() => subject.match(new RegExp(pattern)), "match");
shouldAbandon(() => subject.match(new RegExp(pattern, "g")), "match /g");
shouldAbandon(() => [...subject.matchAll(new RegExp(pattern, "g"))], "matchAll");
shouldAbandon(() => subject.search(new RegExp(pattern)), "search");
shouldAbandon(() => subject.replace(new RegExp(pattern), "X"), "replace");
shouldAbandon(() => subject.replace(new RegExp(pattern, "g"), "X"), "replace /g");
shouldAbandon(() => subject.replaceAll(new RegExp(pattern, "g"), "X"), "replaceAll");
shouldAbandon(() => subject.split(new RegExp(pattern)), "split");

// The same pattern under the limit.
shouldBe(new RegExp(pattern).test(small), true, "small test");
shouldBe(new RegExp(pattern).exec(small)[0], "c", "small exec");
shouldBe(small.search(new RegExp(pattern)), 3, "small search");
shouldBe(small.replace(new RegExp(pattern), "X"), "aaaX", "small replace");
shouldBe(small.split(new RegExp(pattern)).length, 2, "small split");

// A throw is not a failed match: lastIndex stays where it was.
for (const flags of ["g", "y", "gy"]) {
    const regExp = new RegExp(pattern, flags);
    regExp.lastIndex = 3;
    shouldAbandon(() => regExp.test(subject), "test /" + flags);
    shouldBe(regExp.lastIndex, 3, "lastIndex after test /" + flags);
    shouldAbandon(() => regExp.exec(subject), "exec /" + flags);
    shouldBe(regExp.lastIndex, 3, "lastIndex after exec /" + flags);
}

// The RegExp is not left in an error state.
{
    const regExp = new RegExp(pattern);
    shouldAbandon(() => regExp.test(subject), "first use");
    shouldBe(regExp.test(small), true, "reuse after the throw");
    shouldAbandon(() => regExp.test(subject), "second use");
}

// The legacy static properties keep the last match that succeeded.
{
    shouldBe(/b(c)d/.exec("abcde")[0], "bcd", "legacy statics setup");
    shouldAbandon(() => new RegExp(pattern).exec(subject), "exec before reading the statics");
    shouldBe(RegExp.lastMatch, "bcd", "RegExp.lastMatch");
    shouldBe(RegExp.$1, "c", "RegExp.$1");
    shouldBe(RegExp.leftContext, "a", "RegExp.leftContext");
    shouldBe(RegExp.rightContext, "e", "RegExp.rightContext");
}

// The iterator matchAll returns throws from next(), and again when it is asked again.
{
    const iterator = subject.matchAll(new RegExp(pattern, "g"));
    shouldAbandon(() => iterator.next(), "matchAll next");
    shouldAbandon(() => iterator.next(), "matchAll next again");
}

// A /g operation throws when one of its searches is abandoned: there is no partial result.
{
    const late = "c" + subject;
    shouldAbandon(() => late.replace(new RegExp(pattern, "g"), "X"), "replace /g, second search");
    shouldAbandon(() => late.match(new RegExp(pattern, "g")), "match /g, second search");
    shouldAbandon(() => late.split(new RegExp(pattern)), "split, second search");
}

// The limit is for one search. A /g replace that runs 20000 cheap searches takes more steps than
// the limit in total, and is not stopped.
{
    const many = "ab".repeat(20000);
    shouldBe(many.replace(/(?:a)+b/g, "").length, 0, "replace /g over many matches");
    shouldBe(many.match(/(?:a)+b/g).length, 20000, "match /g over many matches");
}

// An error in the pattern is still a SyntaxError.
{
    let error;
    try {
        new RegExp("(");
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof SyntaxError, true, "invalid pattern");
}
