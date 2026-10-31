//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault

// `\/[^\/]*\.log` stops at every slash of a path and walks a folder's name from there, so its attempts together walk the subject.
// A flat program whose start scan is selective looks once for the text that every match holds behind such a run: when its attempts have come 128 units without a match and as many are left.
// Where the text is missing it gives up. The answers are those of a search written out by hand, for subjects around twice that length, with the text missing, in another case,
// in front of where the search begins, or there without a match.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

function folders(count, filler) {
    const parts = [];
    for (let depth = 0; depth < count; depth++)
        parts.push((depth % 10) + filler.repeat(6));
    return parts.join("/");
}

// Where `(?:^|\/)[^\/]*<text>(?=$|\/$)` matches from `from` on: at the slash in front of the last name, or at the start of the subject.
function lastNameEndingWith(subject, text, ignoresCase, from) {
    let end = subject.length;
    if (subject.endsWith("/"))
        end--;
    const name = subject.slice(0, end);
    const tail = name.slice(name.length - text.length);
    if (name.length < text.length || (ignoresCase ? tail.toLowerCase() !== text.toLowerCase() : tail !== text))
        return -1;
    const slash = name.lastIndexOf("/", name.length - text.length);
    // The text itself holds no slash.
    const start = slash < 0 ? 0 : slash;
    return start >= from ? start : -1;
}

const made = (source, flags) => new RegExp(source + String(Date.now()).slice(99), flags);

let checked = 0;
for (const filler of ["x", "Ж", "ﷺ", "\u{1f600}"]) {
    for (const count of [1, 2, 16, 17, 19, 20, 31, 32, 33, 34, 40, 300]) {
        const path = folders(count, filler);
        for (const flags of ["", "i", "g", "gi", "s", "m", "u", "iu", "v"]) {
            const ignoresCase = flags.includes("i");
            const pattern = made("(?:^|\\/)[^\\/]*\\.log(?=$|\\/$)", flags);
            for (const tail of ["", "/", "/a.log", "/a.log/", "/a.LOG", "/a.lOg/", "/.log", ".log", ".log/", "/a.logs", "/a.log/b", "/a.log//", "/a.lo", "/a.log\n", "/ſ.log", "/a.loɡ"]) {
                const subject = path + tail;
                for (const from of flags.includes("g") ? [0, 1, subject.length - 6, subject.length - 1, subject.length] : [0]) {
                    if (from < 0)
                        continue;
                    pattern.lastIndex = from;
                    const match = pattern.exec(subject);
                    // With m, $ holds in front of a line end too.
                    if (flags === "m" && tail.includes("\n"))
                        continue;
                    shouldBe(match ? match.index : -1, lastNameEndingWith(subject, ".log", ignoresCase, from), JSON.stringify([filler, count, flags, tail, from]));
                    checked++;
                }
            }
        }
    }
}

// One attempt, with the run first: no text is looked for.
for (const filler of ["x", "ﷺ"]) {
    for (const count of [1, 17, 18, 19, 300]) {
        const path = folders(count, filler);
        for (const flags of ["", "i"]) {
            const pattern = made("^(?:.*\\/)?\\.cache(?=$|\\/$)", flags);
            shouldBe(pattern.test(path), false, "no text");
            shouldBe(pattern.test(path + "/.cache"), true, "at the end");
            shouldBe(pattern.test(path + "/.cache/"), true, "in front of the last slash");
            shouldBe(pattern.test(path + "/.CACHE"), flags === "i", "in capitals");
            shouldBe(pattern.test(path + "/.cache/" + path), false, "in the middle");
            shouldBe(pattern.test(".cache/" + path), false, "at the start, with more behind it");
            shouldBe(pattern.test(path + "/x.cache"), false, "behind a letter");
            shouldBe(pattern.test(path + "\n/.cache"), false, "behind a line end, which the dot does not take");
            shouldBe(made("^(?:.*\\/)?\\.cache(?=$|\\/$)", flags + "s").test(path + "\n/.cache"), true, "behind a line end, which the dot takes");
            shouldBe(made("^(?:.*\\/)?\\.cache(?=$|\\/$)", flags + "m").test(path + "\n.cache"), true, "at the start of the second line");
            checked += 10;
        }
    }
}

// An alternative without the text, a text in a lookahead that is refused, a text that may be left out, a reference: nothing may be ruled out.
const long = folders(300, "x");
shouldBe(made("\\/[^\\/]*\\.log|9x+$", "").test(long), true, "the other alternative");
shouldBe(made("\\/[^\\/]*(?!\\.log)x$", "").test(long), true, "a refused lookahead");
shouldBe(made("\\/[^\\/]*(?:\\.log)?$", "").test(long), true, "a group that may be left out");
shouldBe(made("\\/[^\\/]*(?:\\.log){0}x$", "").test(long), true, "a group taken no times");
shouldBe(made("^(?:.*\\/)?(?:\\.cache|9x+)$", "").test(long), true, "one attempt, two endings");
shouldBe(made("(\\.log)?\\/[^\\/]*\\1x$", "").test(long), true, "a reference to a group that took nothing");
shouldBe(made("\\/[^\\/]*\\.log", "y").test(long), false, "sticky");
shouldBe(long.replace(made("\\/[^\\/]*\\.log", "g"), "").length, long.length, "replace");
shouldBe((long + "/a.log").split(made("\\/[^\\/]*\\.log", "")).length, 2, "split");
shouldBe((long + "/a.log" + long + "/b.log").match(made("\\/[^\\/]*\\.log", "g")).length, 2, "every match");
shouldBe((long + "/a.log").search(made("\\/[^\\/]*\\.LOG", "i")), long.length, "search");
if (checked < 12000)
    throw new Error("only " + checked + " cases");
