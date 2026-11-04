//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault

// A pattern that begins with a text, over subjects of 64 units and more: it matches where indexOf finds the text.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

const made = (source, flags) => new RegExp(source + String(Date.now()).slice(99), flags);

let checked = 0;
for (const text of ["needle", "/private", "ne", "n7x", "@[x", "`{x", "ßeta"]) {
    const source = text.replace(/[\/\[{]/g, "\\$&");
    for (const flags of ["", "i", "g", "u"]) {
        const ignoresCase = flags === "i";
        for (const pattern of [made(source, flags), made(source + "(?:y|)", flags), made("(?:" + source + ")|$", flags)]) {
            const findsEnd = pattern.source.endsWith("|$");
            for (const filler of ["x", "n", "ne", "/p", "xy/", "@{", "ﷺ"]) {
                const run = filler.repeat(300);
                for (const written of [text, text.toUpperCase(), text.slice(0, -1) + "#", text[0] + "#" + text.slice(2)]) {
                    for (const total of [63, 64, 65, 67, 68, 79, 80, 81, 83, 84, 100, 300]) {
                        for (const offset of [0, 1, 15, 16, 17, 31, 44, 47, 48, 60, total - written.length - 1, total - written.length]) {
                            if (offset < 0 || offset + written.length > total)
                                continue;
                            const subject = run.slice(0, offset) + written + run.slice(0, total - offset - written.length);
                            for (const from of flags === "g" ? [0, 1, offset, offset + 1] : [0]) {
                                let expected = ignoresCase ? subject.toLowerCase().indexOf(text.toLowerCase(), from) : subject.indexOf(text, from);
                                if (expected < 0 && findsEnd)
                                    expected = subject.length;
                                pattern.lastIndex = from;
                                const match = pattern.exec(subject);
                                shouldBe(match ? match.index : -1, expected, JSON.stringify([pattern.source, flags, filler, written, total, offset, from]));
                                checked++;
                            }
                        }
                    }
                }
            }
        }
    }
}
shouldBe(checked > 300000, true, "checked");
