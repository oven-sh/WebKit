//@ requireOptions("--useZombieMode=1")
// The result of each replacer call is written into the result buffer as a JSString cell.
// A rope result is not resolved into a StringImpl of its own first. This drives every
// replacer loop: the global regexp loop with a JS function, the generic call loop (bound
// and host callables, and the non-global regexp), and the string search loops.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${JSON.stringify(actual)}, expected ${JSON.stringify(expected)}`);
}

function shouldThrow(func, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!error)
        throw new Error("not thrown");
    if (String(error) !== message)
        throw new Error(`bad error: ${String(error)}`);
}

function rope(a, b) {
    return a + b;
}
noInline(rope);

// A replacer whose result is a rope.
function ropeReplacer(match, offset) {
    return rope("<" + offset, match + ">");
}

// A replacer whose result is a 16-bit string.
function wideReplacer(match) {
    return rope("\u2603", match);
}

// A replacer whose result is long enough for the result to become a rope itself.
function longReplacer(match) {
    return rope("x".repeat(200), match + "y".repeat(200));
}

const bound = ropeReplacer.bind(null);

function checkAllLoops(subject, regexp, search, replacer, expected) {
    shouldBe(subject.replace(regexp, replacer), expected);
    shouldBe(subject.replaceAll(regexp, replacer), expected);
    shouldBe(subject.replace(regexp, replacer.bind(null)), expected);
    shouldBe(subject.replaceAll(regexp, replacer.bind(null)), expected);
    shouldBe(subject.replaceAll(search, replacer), expected);
}

for (let i = 0; i < testLoopCount; ++i) {
    checkAllLoops("a-b-c", /-/g, "-", ropeReplacer, "a<1->b<3->c");
    checkAllLoops("a-b-c", /-/g, "-", wideReplacer, "a\u2603-b\u2603-c");
    checkAllLoops("\u3042-\u3044", /-/g, "-", ropeReplacer, "\u3042<1->\u3044");
    checkAllLoops("\u3042-\u3044", /-/g, "-", wideReplacer, "\u3042\u2603-\u3044");
    checkAllLoops("---", /-/g, "-", ropeReplacer, "<0-><1-><2->");
    checkAllLoops("a-b-c", /-/g, "-", () => "", "abc");

    // One match in a short string, and one match in a long string: the latter builds a rope of
    // left part, replacement and right part.
    shouldBe("my-file(1)-copy.jpg".replace(/[()]/g, ropeReplacer), "my-file<7(>1<9)>-copy.jpg");
    const long = "x".repeat(300) + "-" + "y".repeat(300);
    shouldBe(long.replace(/-/g, ropeReplacer), "x".repeat(300) + "<300->" + "y".repeat(300));
    shouldBe(long.replace(/-/g, wideReplacer), "x".repeat(300) + "\u2603-" + "y".repeat(300));
    shouldBe(long.replace("-", ropeReplacer), "x".repeat(300) + "<300->" + "y".repeat(300));
    shouldBe(long.replace(/-/, longReplacer).length, 300 + 401 + 300);

    // Non-global regexp and single string search.
    shouldBe("a-b-c".replace(/-/, ropeReplacer), "a<1->b-c");
    shouldBe("a-b-c".replace(/-/, wideReplacer), "a\u2603-b-c");
    shouldBe("a-b-c".replace("-", ropeReplacer), "a<1->b-c");
    shouldBe("a-b-c".replace("-", wideReplacer), "a\u2603-b-c");

    // Captures, with the generic call loop as well.
    shouldBe("a1b22".replace(/(\d)(\d)?/g, (m, a, b, offset) => rope(String(b), a + offset)), "aundefined11b223");
    shouldBe("a1b22".replace(/(\d)(\d)?/g, ((m, a, b, offset) => rope(String(b), a + offset)).bind(null)), "aundefined11b223");
    shouldBe("a1b22".replace(/(?<d>\d)/g, (m, d, offset, string, groups) => rope(groups.d, "!")), "a1!b2!2!");
}

// More matches than the inline storage of the result list, with a GC inside the replacer. Zombie
// mode (see the directive above) scribbles over a dead cell, so a result the list does not keep
// alive is caught at the splice.
{
    const subject = "a".repeat(40);
    let expected = "";
    for (let i = 0; i < 40; ++i)
        expected += "<" + i + "a>";
    const replacer = (match, offset) => {
        if (offset % 7 === 0)
            gc();
        return rope("<" + offset, match + ">");
    };
    shouldBe(subject.replace(/a/g, replacer), expected);
    shouldBe(subject.replace(/a/g, replacer.bind(null)), expected);
    shouldBe(subject.replaceAll("a", replacer), expected);
    // With a capture, the regexp loops take their capture path, and the string replace cache path
    // takes its general loop.
    shouldBe(subject.replace(/(a)/g, (match, capture, offset) => {
        if (offset % 7 === 0)
            gc();
        return rope("<" + offset, capture + ">");
    }), expected);
}

// The same cell returned for every match: a rope of flat fibers is copied from its fibers each
// time, a deeper rope is resolved once at the first match. Both survive a GC between the matches.
{
    const shallow = rope("<", ">");
    let deep = "";
    for (let i = 0; i < 100; ++i)
        deep = rope(deep, String.fromCharCode(97 + (i % 26)));
    const subject = "a".repeat(20);
    for (const result of [shallow, deep]) {
        const expected = result.repeat(20);
        const replacer = (match, offset) => {
            if (offset % 5 === 0)
                gc();
            return result;
        };
        shouldBe(subject.replace(/a/g, replacer), expected);
        shouldBe(subject.replace(/(a)/g, replacer), expected);
        shouldBe(subject.replace(/a/g, replacer.bind(null)), expected);
        shouldBe(subject.replaceAll("a", replacer), expected);
        shouldBe(subject.replace(/a/, replacer), result + "a".repeat(19));
        shouldBe(subject.replace("a", replacer), result + "a".repeat(19));
    }
}

// A result that is an object: its toString runs once per match, in match order, and a throw
// from it discards the partial result.
{
    const log = [];
    const replacer = (match, offset) => ({
        toString() {
            log.push(offset);
            return rope("[", offset + "]");
        },
    });
    shouldBe("a-b-c-d".replace(/-/g, replacer), "a[1]b[3]c[5]d");
    shouldBe("a-b-c-d".replace(/-/g, replacer.bind(null)), "a[1]b[3]c[5]d");
    shouldBe("a-b-c-d".replaceAll("-", replacer), "a[1]b[3]c[5]d");
    shouldBe(log.join(","), "1,3,5,1,3,5,1,3,5");

    const throwing = (match, offset) => ({
        toString() {
            if (offset === 3)
                throw new Error("toString at " + offset);
            return "x";
        },
    });
    shouldThrow(() => "a-b-c-d".replace(/-/g, throwing), "Error: toString at 3");
    shouldThrow(() => "a-b-c-d".replace(/-/g, throwing.bind(null)), "Error: toString at 3");
    shouldThrow(() => "a-b-c-d".replaceAll("-", throwing), "Error: toString at 3");
    shouldThrow(() => "a-b-c-d".replace(/-/g, () => Symbol()), "TypeError: Cannot convert a symbol to a string");
}

// A replacer that returns the match itself, and one that returns the subject, keep the cells alive
// through the splice.
shouldBe("a-b".replace(/-/g, (match) => match), "a-b");
shouldBe("a-b".replace(/-/g, (match, offset, string) => string), "aa-bb");
