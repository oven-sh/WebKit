function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: ${String(actual).slice(0, 80)}, expected ${String(expected).slice(0, 80)}`);
}

function join(array, separator) { return array.join(separator); }
noInline(join);

const wide = { toString() { return "\u3042"; } };
for (let i = 0; i < testLoopCount; ++i) {
    let rope = "a" + i, object = { };
    shouldBe(join([object, object, { }, rope, rope, object, null, object], "-"), `[object Object]-[object Object]-[object Object]-a${i}-a${i}-[object Object]--[object Object]`);
    shouldBe(join([wide, wide, 1.5, wide, rope, rope], ""), `\u3042\u30421.5\u3042a${i}a${i}`);
    shouldBe(String([[object, object], [rope, rope]]), `[object Object],[object Object],a${i},a${i}`);
}

for (let count of [0xffff, 0x10000, 0x10001, 0x20001]) {
    shouldBe(join(Array(count).fill({ }), ""), "[object Object]".repeat(count));
    shouldBe(join([wide, ...Array(count).fill("ab")], ","), "\u3042" + ",ab".repeat(count));
}
