// Reads from many small frozen arrays (oven-sh/bun#44305).
const N = 20000, LEN = 16, PASSES = 50;
const arrays = Array.from({ length: N }, (_, i) => Object.freeze(Array.from({ length: LEN }, (_, j) => (i + j) & 0xffff)));
function sum(arrs) {
    let s = 0;
    for (let p = 0; p < PASSES; p++)
        for (const a of arrs)
            for (let j = 0; j < a.length; j++)
                s += a[j];
    return s;
}
noInline(sum);
const result = sum(arrays);
if (result <= 0)
    throw new Error("bad sum " + result);
