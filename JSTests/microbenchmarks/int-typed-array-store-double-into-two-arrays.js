function store(a, b, source, count)
{
    for (let i = 0; i < count; i++) {
        const value = source[i & 63];
        a[i & 63] = value;
        b[i & 63] = value;
    }
    return a[1] + b[1];
}
noInline(store);

const a = new Int32Array(64);
const b = new Uint8Array(64);
const source = new Float64Array(64);
for (let i = 0; i < source.length; i++)
    source[i] = i * 300.5;

let sum = 0;
for (let i = 0; i < 20000; i++)
    sum += store(a, b, source, 1000);
if (sum !== 20000 * (300 + 44))
    throw new Error(`bad sum: ${sum}`);
