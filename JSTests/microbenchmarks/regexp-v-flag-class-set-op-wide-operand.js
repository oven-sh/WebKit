// Builds `v` class sets whose operations have an operand that covers U+0100 to U+10FFFF: the complement of a
// nested class, \D, \S and \p{L}. Each source differs, so that no RegExp comes from the RegExp cache.
const sources = [];
for (let i = 0; i < 400; ++i) {
    const character = String.fromCharCode(0x4e00 + i);
    sources.push(`[${character}[^b]]`);
    sources.push(`[[^b]--${character}]`);
    sources.push(`[\\D--${character}]`);
    sources.push(`[\\D&&\\S&&[^${character}]]`);
    sources.push(`[${character}\u4dff\\D]`);
    sources.push(`[\\p{L}--${character}]`);
}

let count = 0;
for (const source of sources) {
    if (new RegExp(source, "v").test("a"))
        count++;
}
if (count !== sources.length)
    throw new Error("bad count: " + count);
