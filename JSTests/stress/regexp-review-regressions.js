function shouldBe(actual, expected, message) {
    actual = JSON.stringify(actual);
    expected = JSON.stringify(expected);
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + expected + " but got " + actual);
}

function execSummary(re, subject) {
    let m = re.exec(subject);
    return m ? [m.index, ...m] : null;
}

shouldBe(execSummary(/(?!(?=^a))a/, "a"), null);
shouldBe(execSummary(/(?!(?=^))a|q/, "a"), null);
shouldBe(execSummary(/(?!(?=^)|y)x/, "x"), null);
shouldBe(execSummary(/(?!(?=^)|y)x/, "ax"), [1, "x"]);
shouldBe(execSummary(/(?=(?=^a))a/, "a"), [0, "a"]);
shouldBe(execSummary(/(?:^)?a/, "ba"), [1, "a"]);
shouldBe(execSummary(/(^a|^b)?c/, "bc"), [0, "bc", "b"]);
shouldBe(execSummary(/x(?:^a)?y/, "xy"), [0, "xy"]);

shouldBe("x\u{1F600} y\u{1F600} z-".match(/\p{Extended_Pictographic}|[a-z]-/gu), ["\u{1F600}", "\u{1F600}", "z-"]);
shouldBe(execSummary(/\u{1F600}|k_/iu, "a\u{1F600}"), [1, "\u{1F600}"]);
shouldBe(execSummary(/\u{1F600}|[ab]x/u, "c\u{1F600}"), [1, "\u{1F600}"]);
shouldBe(execSummary(/\u{1F600}|-?a/u, "-\u{1F600}"), [1, "\u{1F600}"]);
shouldBe(execSummary(/y|a[a-z]b/u, "ay\u{1F600}"), [1, "y"]);
shouldBe(execSummary(/y|a.b/su, "ay\u{1F600}"), [1, "y"]);
shouldBe(execSummary(/y|a\Sb/u, "ay\u{1F600}"), [1, "y"]);
shouldBe(execSummary(/y|[a-z][a-z]b/u, "ay\u{1F600}"), [1, "y"]);
shouldBe(execSummary(/^(?:a\u{1F600}|qa)/u, "azk\u0101"), null);
shouldBe(execSummary(/^(?:a\u{1F600}c|q)/u, "azkc\u0101"), null);
shouldBe(execSummary(/^(?:a\u{1F600}c|q)/u, "a\u{1F600}c\u0101"), [0, "a\u{1F600}c"]);
shouldBe(execSummary(/^(?:ab\u{1F600}cd|abzz)$/u, "ab\u{1F600}cd"), [0, "ab\u{1F600}cd"]);
shouldBe(execSummary(/(?<=^(?:a\u{1F600}|qa))k/u, "a\u{1F600}k"), [3, "k"]);
shouldBe(execSummary(/[az\D]/v, "\u{10428}"), [0, "\u{10428}"]);
shouldBe(execSummary(/[az\S]/v, "\u{10428}"), [0, "\u{10428}"]);
shouldBe(/^[az\D]$/v.test("\u{10428}"), true);
shouldBe(execSummary(/[\D--[a-z]]/v, "\u{10428}"), [0, "\u{10428}"]);

shouldBe(execSummary(/(?<=X[^"]*)!/u, "X ab\u{1F600}de!"), [8, "!"]);
shouldBe(execSummary(/(?<=a[^x]*b[^x]*)!/u, "a\u{1F600}b\u{1F600}c!"), [7, "!"]);
shouldBe(execSummary(/(?<=a[^x]*\u{1F600}[^x]*)!/u, "a\u{1F600}\u{1F600}\u{1F600}c!"), [8, "!"]);
shouldBe(execSummary(/(?<=^\D*b\D*)c/u, "\u{1F600}b\u{1F600}\u{1F600}c"), [7, "c"]);
shouldBe(execSummary(/(?<=(\S*)b(\S*))c/u, "\u{1D49C}\u{1D49C}b\u{1D49C}c"), [7, "c", "\u{1D49C}\u{1D49C}", "\u{1D49C}"]);
shouldBe(execSummary(/(?<![^b]*bb[^b]*)c/u, "\u{1F600}bb\u{1F600}c"), null);
shouldBe(execSummary(/(?<=a.*?b.*)$/su, "a\u{1F600}b\u{1F600}"), [6, ""]);
shouldBe(execSummary(/(?<=[^a]+?a[^a]+)z/u, "\u{1F600}a\u{1F600}\u{1F600}z"), [7, "z"]);

{
    let subject = "“" + "word ".repeat(4000) + "!";
    shouldBe(execSummary(/(?<=X[^"]*)!/u, subject), null);
    shouldBe(execSummary(/(?<!X[^"]*)!/u, subject), [subject.length - 1, "!"]);
    shouldBe(execSummary(/(?<=X\D*)!/u, subject), null);
    shouldBe(execSummary(/(?<=X[^\u{1F600}]*)!/u, subject), null);
}

{
    let alternatives = [];
    for (let i = 1; i <= 600; ++i)
        alternatives.push("a".repeat(i) + "z");
    let re = new RegExp(alternatives.join("|"));
    shouldBe(execSummary(re, "x" + "a".repeat(600) + "zy"), [1, "a".repeat(600) + "z"]);
    shouldBe(execSummary(re, "a".repeat(50) + "q"), null);
    shouldBe(execSummary(re, "aaz"), [0, "aaz"]);
}

{
    let alternatives = [];
    for (let i = 0; i < 20000; ++i)
        alternatives.push("k" + i);
    let re = new RegExp("(?<=" + alternatives.join("|") + ")z");
    shouldBe(execSummary(re, "k19999z"), [6, "z"]);
    shouldBe(execSummary(re, "q1z"), null);
}

{
    let words = ["about", "above", "abuse", "actor", "acute", "admit", "adopt", "adult", "after", "again", "agent", "agree", "ahead", "alarm", "album", "alert", "alike", "alive", "allow", "alone"];
    let re = new RegExp("\\b(?:" + words.join("|") + ")\\b", "g");
    shouldBe("go ahead and admit the album is above average".match(re), ["ahead", "admit", "album", "above"]);
    let capturing = new RegExp("(" + words.join(")|(") + ")");
    let m = capturing.exec("stay alert");
    shouldBe(m.index, 5);
    shouldBe(m[0], "alert");
    shouldBe(m[16], "alert");
    shouldBe(m.filter((x, i) => i > 0 && x !== undefined).length, 1);
}

{
    let re = /(?<=(.?))(?<!\1)/su;
    let subjects = ["\u{10428}", "a\u{10428}", "\u{10428}\u{10428}", "ab", "\uD801", "\uDC28", "xxxx\u{10428}", "x\uDC28x"];
    for (let n = 0; n < 600; ++n) {
        let s = subjects[n % subjects.length];
        for (let i = 0; i < n % 7; ++i)
            s = "x" + s;
        for (let k = 0; k <= s.length; ++k) {
            re.lastIndex = k;
            re.exec(s);
        }
    }
    let sticky = /(?<=(\P{L}?)\1?)(\1{0,2}\W\1{2}){0,2}(?<!\1{1,2}|.)/uy;
    for (let n = 0; n < 600; ++n) {
        let s = subjects[n % subjects.length];
        for (let k = 0; k <= s.length; ++k) {
            sticky.lastIndex = k;
            sticky.exec(s);
        }
    }
}

shouldBe(execSummary(/(?<=.\1+(q))x/u, "aqx"), null);
shouldBe(execSummary(/(?<=\1+(q))x/, "aqx"), null);
shouldBe(execSummary(/(?<=\1{1,2}(q))x/, "aqx"), null);
shouldBe(execSummary(/(?<=b\1+(q))x/, "bqqx"), [3, "x", "q"]);
shouldBe(execSummary(/(?<=b\1+(q))x/, "baqx"), null);
shouldBe(execSummary(/(?<=\1{2,}(q))x/, "qqqx"), [3, "x", "q"]);
shouldBe(execSummary(/(?<=\1{2,}(q))x/, "aqqx"), null);
shouldBe(execSummary(/(?<=\k<n>+(?<n>q))x/, "aqx"), null);
shouldBe(execSummary(/(?<=\k<n>+(?<n>q))x/, "qqx"), [2, "x", "q"]);
shouldBe(execSummary(/(?<=\1*(q))x/, "aqx"), [2, "x", "q"]);

shouldBe(execSummary(/(?<n>b)a(?<=\k<n>)/, "ba"), null);
shouldBe(execSummary(/(?<n>b)a(?<=a\k<n>|\k<n>a)/, "ba"), [0, "ba", "b"]);
shouldBe(execSummary(/(?<n>b)(?<=\k<n>)/, "b"), [0, "b", "b"]);
shouldBe(execSummary(/(?<n>b)a(?<=(?<=\k<n>)a)/, "ba"), [0, "ba", "b"]);
shouldBe(execSummary(/(?<=\k<n>(?<n>q))x/, "aqx"), null);
shouldBe(execSummary(/(?<=(?<n>q)\k<n>)x/, "aqx"), [2, "x", "q"]);
shouldBe(execSummary(/(?:(?<n>a)|(?<n>b))(?<=\k<n>)c/, "bc"), [0, "bc", null, "b"]);

shouldBe(/[\P{Number}&&\P{Alphabetic}]/v.test("A"), false);
shouldBe(/[\P{Number}&&\P{Alphabetic}]/v.test("1"), false);
shouldBe(/[\P{Number}&&\P{Alphabetic}]/v.test("!"), true);
shouldBe(/[\P{Number}&&\P{Alphabetic}]/v.test("\u{1D7D8}"), false);
shouldBe(/[\P{Number}&&\P{Alphabetic}]/v.test("\u{1F600}"), true);
shouldBe(/[\p{L}&&\P{Lu}]/v.test("a"), true);
shouldBe(/[\p{L}&&\P{Lu}]/v.test("A"), false);
shouldBe(/[\P{Lu}--\P{L}]/v.test("a"), true);
shouldBe(/[\P{Lu}--\P{L}]/v.test("A"), false);
shouldBe(/[\P{Lu}--\P{L}]/v.test("1"), false);
shouldBe(/[^\P{L}&&\P{N}]/v.test("a"), true);
shouldBe(/[^\P{L}&&\P{N}]/v.test("!"), false);
