function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

const operands = [
    [[0x100, 0x100], [0x102, 0x1ff], [0x200, 0x2ff], [0xffff, 0x10001]],
    [[0x41, 0x120], [0x180, 0x180], [0x181, 0x181], [0x1ff, 0x200], [0x10000, 0x10ffff]],
    [[0x150, 0x250], [0x140, 0x160], [0x2ff, 0x300], [0x10ffff, 0x10ffff]],
    [[0x100, 0x10ffff]],
    [[0x101, 0x101], [0x103, 0x103], [0x105, 0x105]],
    [],
];
const escape = point => `\\u{${point.toString(16)}}`;
const textOf = ranges => `[${ranges.map(([first, last]) => first === last ? escape(first) : `${escape(first)}-${escape(last)}`).join("")}]`;
const has = (ranges, point) => ranges.some(([first, last]) => first <= point && point <= last);
const points = new Set(operands.flat(2).flatMap(point => [point - 1, point, point + 1]).filter(point => point <= 0x10ffff));

for (const left of operands) {
    for (const right of operands) {
        for (const [operator, expected] of [["--", (a, b) => a && !b], ["&&", (a, b) => a && b], ["", (a, b) => a || b]]) {
            const source = `${textOf(left)}${operator}${textOf(right)}`;
            const plain = new RegExp(`^[${source}]$`, "v");
            const inverted = new RegExp(`^[^${source}]$`, "v");
            for (const point of points) {
                const isIn = expected(has(left, point), has(right, point));
                shouldBe(plain.test(String.fromCodePoint(point)), isIn, `${plain} on ${point}`);
                shouldBe(inverted.test(String.fromCodePoint(point)), !isIn, `${inverted} on ${point}`);
            }
        }
    }
}

shouldBe(/^[\p{L}--\p{Script=Han}]$/v.test("一"), false, "Han is taken out");
shouldBe(/^[\p{L}--\p{Script=Han}]$/v.test("Ж"), true, "Cyrillic stays");
shouldBe(/^[\p{L}&&\p{Script=Han}]$/v.test("\u{20000}"), true, "Han above the BMP");
