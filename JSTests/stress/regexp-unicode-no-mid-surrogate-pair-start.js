function shouldBe(actual, expected) {
    actual = JSON.stringify(actual);
    expected = JSON.stringify(expected);
    if (actual !== expected)
        throw new Error("expected " + expected + " but got " + actual);
}

shouldBe(/(?<=a[\S])/u.exec("a\u{1D49C}").index, 3);
shouldBe(/(?<=[\ud83d])/u.exec("\u{1F600}"), null);
shouldBe(/(?<=\uD83D)/u.exec("\u{1F600}"), null);
shouldBe(/(?<=\p{Script=Zzzz})/u.exec("\u{10428}"), null);
shouldBe([..."a\u{1D49C}".matchAll(/(?<![0-�])/gu)].map(m => m.index), [0, 3]);
shouldBe([..."\u{1F600}".matchAll(/(?<=[\S])/gu)].map(m => m.index), [2]);
shouldBe("\u{1F64F}\u{1F64F}".replace(/(?<=.)/gsu, "|"), "\u{1F64F}|\u{1F64F}|");
shouldBe("a\u{1F600}b".replace(/(?:)/gu, "-"), "-a-\u{1F600}-b-");
shouldBe("\u{1F600}\u{1F600}".split(/(?:)/u), ["\u{1F600}", "\u{1F600}"]);
shouldBe([..."x\u{1F600}y".matchAll(/\B|\u{1F600}/gu)].map(m => [m.index, m[0]]), [[1, "\u{1F600}"]]);
shouldBe(/(?<=(.?))(?<!\1)/su.exec("\u{10428}"), null);

{
    let re = /(?:)/uy;
    re.lastIndex = 1;
    shouldBe([re.exec("\u{1F600}").index, re.lastIndex], [0, 0]);
}
{
    let re = /./gsu;
    re.lastIndex = 1;
    shouldBe([re.exec("\u{1F600}x")[0], re.lastIndex], ["\u{1F600}", 2]);
}
{
    let re = /\udc00/u;
    shouldBe(re.exec("𐀀"), null);
    shouldBe(re.exec("x\udc00").index, 1);
}
{
    let re = /(?:)/g;
    shouldBe("\u{1F600}".replace(re, "-"), "-\ud83d-\ude00-");
}
