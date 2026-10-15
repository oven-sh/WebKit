// The pass that lets a tail loop match a run of characters compared each alternative with the first characters of all the earlier
// ones, so a loop of N alternatives took N * N steps to compile: 36 seconds for 95,000. It stops when that set has grown large.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

const character = index => String.fromCharCode(0x400 + 2 * index);

// A two-character alternative between the classes keeps them from being merged into one class.
function tailLoop(count, descending)
{
    const alternatives = [];
    for (let i = 0; i < count; ++i)
        alternatives.push(`[${character(descending ? count - 1 - i : i)}]`, "xy");
    return new RegExp(`(?:${alternatives.join("|")})*`);
}

// What matches is the same below the limit, at it and far above it.
for (const count of [3, 500, 1023, 1024, 1025, 1026, 2000, 25000]) {
    for (const descending of [false, true]) {
        const regExp = tailLoop(count, descending);
        const first = character(0), last = character(count - 1), middle = character(count >> 1), outside = character(count);
        shouldBe(regExp.exec(first + first + "xy" + last + last + last + middle + "xy" + outside)[0].length, 10);
        shouldBe(regExp.exec(last.repeat(50) + "x" + last)[0].length, 50);
        shouldBe(regExp.exec(middle + "xyxy" + first.repeat(20) + "yx")[0].length, 25);
        shouldBe(regExp.exec(outside + first)[0].length, 0);
        shouldBe(regExp.exec("")[0].length, 0);
    }
}
