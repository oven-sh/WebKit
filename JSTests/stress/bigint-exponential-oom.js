function shouldThrow(func, errorMessage) {
    var errorThrown = false;
    var error = null;
    try {
        func();
    } catch (e) {
        errorThrown = true;
        error = e;
    }
    if (!errorThrown)
        throw new Error('not thrown');
    if (String(error) !== errorMessage)
        throw new Error(`bad error: ${String(error)}`);
}

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual);
}

shouldThrow(() => {
    2n ** 0xfffffffffffffffffn;
}, `RangeError: Out of memory: BigInt generated from this operation is too big`);
// Exponents below maxLengthBits (1 << 30) are representable.
shouldBe((2n ** 0xffffffn) >> 0xffffffn, 1n);
shouldBe((2n ** 0xfffffffn) >> 0xfffffffn, 1n);
// 2^(maxLengthBits - 1) has exactly maxLengthBits bits: the largest power of two.
shouldBe((2n ** 1073741823n) >> 1073741823n, 1n);
shouldThrow(() => {
    2n ** 1073741824n;
}, `RangeError: Out of memory: BigInt generated from this operation is too big`);
shouldThrow(() => {
    2n ** 0xffffffffn;
}, `RangeError: Out of memory: BigInt generated from this operation is too big`);
shouldThrow(() => {
    2n ** 0xfffffffffffffffn;
}, `RangeError: Out of memory: BigInt generated from this operation is too big`);
shouldThrow(() => {
    10n ** 1073741824n;
}, `RangeError: Out of memory: BigInt generated from this operation is too big`);
