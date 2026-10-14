function store(array, count)
{
    for (let i = 0; i < count; i++)
        array[i & 63] = 1.5;
    return array[0];
}
noInline(store);

const array = new Int32Array(64);
let sum = 0;
for (let i = 0; i < 40000; i++)
    sum += store(array, 1000);
if (sum !== 40000)
    throw new Error(`bad sum: ${sum}`);
