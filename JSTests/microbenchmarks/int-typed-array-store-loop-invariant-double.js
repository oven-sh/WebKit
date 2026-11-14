function store(array, value, count)
{
    for (let i = 0; i < count; i++)
        array[i & 63] = value;
    return array[0];
}
noInline(store);

const array = new Int32Array(64);
let sum = 0;
for (let i = 0; i < 20000; i++) {
    sum += store(array, i + 0.5, 1000);
    sum += store(array, NaN, 1000);
}
if (sum !== 199990000)
    throw new Error(`bad sum: ${sum}`);
