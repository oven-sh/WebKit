//@ runDefault
//@ runDefault("--useRegExpJIT=false")

// Each pattern has a \k<name> inside a second group of that name. Such a
// reference matches the empty string, and the pattern constructor makes it a
// forward reference (regexp-named-backreference-inside-duplicate-named-group.js).
// When this test was written it was a greedy backreference whose group was
// written again between the two times the term ran, and the interpreter gave
// back the wrong amount. The cases stay as a check of both engines on the shape.
//
// Each case gives one pattern in two forms that describe the same language. The
// second form adds a prefix that matches the empty string only, or an
// alternative that no string can match. Both keep the pattern out of the JIT,
// so the two forms run in the two engines. They must give the same result, and
// every index and every length in a result must lie inside the subject.

var failures = [];

function describe(value, subjectLength) {
    if (value === null)
        return "null";
    if (value === undefined)
        return "undefined";
    if (typeof value === "string") {
        if (value.length > subjectLength)
            return "<out of range, length " + value.length + ">";
        return JSON.stringify(value);
    }

    var result = "[";
    for (var i = 0; i < value.length; ++i)
        result += (i ? "," : "") + describe(value[i], subjectLength);
    result += "]";

    if (value.index !== undefined) {
        result += "@";
        result += value.index > subjectLength ? "<out of range, " + value.index + ">" : value.index;
    }

    if (value.groups) {
        var names = Object.keys(value.groups).sort();
        result += "{";
        for (var i = 0; i < names.length; ++i)
            result += (i ? "," : "") + names[i] + ":" + describe(value.groups[names[i]], subjectLength);
        result += "}";
    }

    return result;
}

function run(operation, source, flags, subject) {
    var regExp = new RegExp(source, flags);
    if (operation === "match")
        return subject.match(regExp);
    if (operation === "split")
        return subject.split(regExp, 100);
    return regExp.exec(subject);
}

function test(name, operation, flags, subject, plainSource, otherSource, expected) {
    var sources = [plainSource, otherSource];
    for (var i = 0; i < sources.length; ++i) {
        var actual;
        try {
            actual = describe(run(operation, sources[i], flags, subject), subject.length);
        } catch (error) {
            actual = "threw " + error;
        }
        if (actual !== expected)
            failures.push(name + ", /" + sources[i] + "/" + flags + " on " + JSON.stringify(subject) + ": expected " + expected + " but got " + actual);
    }
}

test("split on a greedy backreference in the group that writes it",
    "split", "", "cqcIIxq",
    "(?:(?<x>qcIIx)|(?<x>c*(?:\\k<x>*II)?))q",
    "(?:Z?)+(?:(?<x>qcIIx)|(?<x>c*(?:\\k<x>*II)?))q",
    '["",undefined,"c","cIIx",undefined,"",""]');

test("global match of an alternation behind a lookbehind",
    "match", "g", "q?cIIx",
    "^q\\?|(?<=\\?)(?:(?<x>a)|(?<x>c*(?:\\k<x>*II)?))q|\\?cII",
    "^q\\?|(?<=\\?)(?:Z?)+(?:(?<x>a)|(?<x>c*(?:\\k<x>*II)?))q|\\?cII",
    '["q?"]');

test("empty match of a repeated group that holds the backreference",
    "exec", "", "ccIx",
    "(?:(?<x>a)|(?<x>c*\\k<x>*I?)+)(?!\\w)",
    "(?:Z?)+(?:(?<x>a)|(?<x>c*\\k<x>*I?)+)(?!\\w)",
    '["",undefined,""]@4{x:""}');

test("a non-greedy group around the backreference",
    "exec", "", "aaBaacAc",
    "(?:(?<d>z)|(?<d>\\w{1,3}?\\k<d>*){2,}x)",
    "(?:q*)+(?:(?<d>z)|(?<d>\\w{1,3}?\\k<d>*){2,}x)",
    "null");

test("a term after the group that no subject satisfies",
    "exec", "", "cIx",
    "(?:(?<x>a)|(?<x>c*(?:\\k<x>*I)?))>",
    "(?:q*)+(?:(?<x>a)|(?<x>c*(?:\\k<x>*I)?))>",
    "null");

test("an alternative whose length no string can hold",
    "exec", "", "zcIx",
    "(?:(?<x>a)|(?<x>c*(?:\\k<x>*I)?))>",
    "(?:(?<x>a)|(?<x>c*(?:\\k<x>*I)?))>|z{2147483648}",
    "null");

test("a backreference repeated by two quantifiers, unicode",
    "match", "ug", "ccIIxa",
    "(?:(?<x>\\w)|(?<x>\\k<x>+(?:\\k<x>?\\k<x>?)*?c))x",
    "(?:Z?)+(?:(?<x>\\w)|(?<x>\\k<x>+(?:\\k<x>?\\k<x>?)*?c))x",
    '["Ix"]');

test("the search continues after a match that ends where it starts",
    "match", "ug", "q?cIIx",
    "(?:(?<x>\\w)|(?<x>I+?\\k<x>?(?:\\k<x>*)*?))(?!\\w)",
    "(?:){2}(?:(?<x>\\w)|(?<x>I+?\\k<x>?(?:\\k<x>*)*?))(?!\\w)",
    '["q","x"]');

if (failures.length)
    throw new Error(failures.length + " case(s) failed:\n" + failures.join("\n"));
