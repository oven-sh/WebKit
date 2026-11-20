function shouldBe(actual, expected) {
    if (!Object.is(actual, expected))
        throw new Error(`expected ${expected}, got ${actual}`);
}

const types = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float16Array, Float32Array, Float64Array];
const values = [0, 1, -1, 127, 128, 255, 256, -129, 32767, 65535, 2147483647, -2147483648, 4294967295, 0.5, 1.5, -0, 1e10, NaN, Infinity];

for (const From of types) {
    for (const To of types) {
        for (const length of [0, 1, 7, 8, 15, 16, 33, 100]) {
            const source = From.from({ length }, (_, i) => values[i % values.length]);
            const target = new To(length + 5).fill(9);
            target.set(source, 3);
            const expected = To.from([9, 9, 9, ...source, 9, 9]);
            target.forEach((value, i) => shouldBe(value, expected[i]));
        }

        // slice() copies from left to right, also into what it has yet to read.
        for (const shift of [0, 8, 16, 64]) {
            const buffer = new ArrayBuffer(1024), copy = new ArrayBuffer(1024);
            const fill = (buffer) => new Uint8Array(buffer).forEach((_, i, all) => all[i] = i % 61);
            fill(buffer);
            fill(copy);
            const source = new From(buffer, 32, 40);
            source.constructor = { [Symbol.species]: function () { return new To(buffer, shift, 40); } };
            source.slice();
            if (From === To)
                continue;
            const [from, to] = [new From(copy, 32, 40), new To(copy, shift, 40)];
            for (let i = 0; i < 40; ++i)
                to[i] = from[i];
            to.forEach((value, i) => shouldBe(new To(buffer, shift, 40)[i], value));
        }
    }
}
