function test(strings)
{
    let result = 0;
    for (let i = 0; i < strings.length; ++i)
        result += parseInt(strings[i], 16);
    return result;
}
noInline(test);

// 16 hex digits, different in every string and too many for the branch predictor to learn. Every value is above
// 2^53 and takes parseIntOverflow.
let state = 12345;
function random32()
{
    state = (state + 0x6D2B79F5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return (t ^ (t >>> 14)) >>> 0;
}

let strings = [];
for (let i = 0; i < 16384; ++i)
    strings.push(((random32() | 0x80000000) >>> 0).toString(16) + random32().toString(16).padStart(8, "0"));
let expected = test(strings);

for (let i = 0; i < 100; ++i) {
    if (test(strings) !== expected)
        throw new Error("bad");
}
