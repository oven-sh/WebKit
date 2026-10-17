// A fixed-width counted class followed by a single variable-width class under /u:
// when the single class consumed a surrogate pair and a later term fails, the
// single class must restore the index itself, because the counted class's
// backtrack does not (it is fixed width and falls through to the previous op).
function shouldBe(actual, expected, message) {
    actual = JSON.stringify(actual);
    expected = JSON.stringify(expected);
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + expected + " but got " + actual);
}
function execSummary(re, s) { const m = re.exec(s); return m ? [m.index, ...m] : null; }

// Every subject holds an astral character, so these all compile the 16-bit code.
shouldBe(execSummary(/\w{2}[^o]q|b/u, "fb\u{1F600}K"), [1, "b"]);
shouldBe(execSummary(/\d{2}.q|x*/su, "12\u{1F600}K"), [0, ""]);
shouldBe(execSummary(/\d{2}.%|(\d)/su, "12\u{1F600}3"), [0, "1", "1"]);
shouldBe("fb\u{1F600}Kb".replace(/\w{2}[^o]q|b/gu, "#"), "f#\u{1F600}K#");
shouldBe(execSummary(/[a-z]{3}\Sq|(k)/u, "abc\u{10428}k"), [5, "k", "k"]);
shouldBe(execSummary(/(?:\w{2}.q|b)c/su, "fb\u{1F600}Kbc"), [5, "bc"]);
shouldBe(execSummary(/(?<=\w{2}[^o]q|b)c/u, "fb\u{1F600}Kbc"), [6, "c"]);
shouldBe(execSummary(/(?<=q[^o]\w{2}|b)c/u, "q\u{1F600}fbc"), [5, "c"]);
shouldBe(execSummary(/[\u{1F600}-\u{1F64F}]{2}\Sq|b/u, "\u{1F600}\u{1F601}\u{10428}xb"), [7, "b"]); // astral-only (fixed width 2) counted class
shouldBe(execSummary(/[a\u{1F600}]{2}.q|b/su, "a\u{1F600}\u{10428}xb"), [6, "b"]); // variable-width counted class
// Chains of single classes, each restoring its own start.
shouldBe(execSummary(/[^x][^o]q|b/u, "fb\u{1F600}K"), [1, "b"]);
shouldBe(execSummary(/[^y][^x][^o]q|K/u, "fb\u{1F600}K"), [4, "K"]);
shouldBe(execSummary(/\w{2}[^y][^o]q|K/u, "zzfb\u{1F600}\u{1F600}K"), [8, "K"]);
