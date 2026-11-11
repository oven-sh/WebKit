// A string value of more than 16 characters has the width of the JSON text it is parsed from. The values are kept in one
// table for both widths, and text of one width does not take the string that was made for the other.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

function width(string) {
    return /8Bit:\(1\)/.test(describe(string)) ? 8 : 16;
}

function textOf(length, seed) {
    let text = "";
    for (let i = 0; i < length; i++)
        text += String.fromCharCode(97 + ((i * 7 + seed) % 26));
    return text;
}

let seed = 0;
for (const length of [17, 44, 63, 64, 70, 84, 256]) {
    // 16-bit text first. The 8-bit text then reads the value as an element, and twice as the value of a property.
    const a = textOf(length, ++seed);
    const fromWide = JSON.parse('["' + a + '","\u4e00"]');
    shouldBe(fromWide[0], a, "16-bit text, " + length);
    shouldBe(width(fromWide[0]), 16, "width from 16-bit text, " + length);
    const fromNarrow = JSON.parse('["' + a + '",{"a":"' + a + '"},{"a":"' + a + '"}]');
    shouldBe(fromNarrow[0], a, "8-bit text after 16-bit text, " + length);
    shouldBe(width(fromNarrow[0]), 8, "width of an element from 8-bit text after 16-bit text, " + length);
    shouldBe(width(fromNarrow[1].a), 8, "width of a property from 8-bit text after 16-bit text, " + length);
    shouldBe(width(fromNarrow[2].a), 8, "width of a second property from 8-bit text after 16-bit text, " + length);
    // The 16-bit text again still gets a 16-bit string.
    shouldBe(width(JSON.parse('["' + a + '","\u4e00"]')[0]), 16, "width from 16-bit text again, " + length);

    // 8-bit text first.
    const b = textOf(length, ++seed);
    const narrowFirst = JSON.parse('[{"a":"' + b + '"},"' + b + '"]');
    shouldBe(width(narrowFirst[0].a), 8, "width of a property from 8-bit text, " + length);
    shouldBe(width(narrowFirst[1]), 8, "width of an element from 8-bit text, " + length);
    const wideSecond = JSON.parse('["' + b + '",{"a":"' + b + '"},"\u4e00"]');
    shouldBe(wideSecond[0], b, "16-bit text after 8-bit text, " + length);
    shouldBe(width(wideSecond[0]), 16, "width of an element from 16-bit text after 8-bit text, " + length);
    shouldBe(width(wideSecond[1].a), 16, "width of a property from 16-bit text after 8-bit text, " + length);
    shouldBe(width(JSON.parse('[{"a":"' + b + '"}]')[0].a), 8, "width from 8-bit text again, " + length);
}
