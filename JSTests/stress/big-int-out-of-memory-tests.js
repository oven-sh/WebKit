//@ runDefault("--useDFGJIT=false")

function assert(a, message) {
    if (!a)
        throw new Error(message);
}

// maxLengthBits is 1 << 30; build an all-ones value of exactly that many bits.
let a = (1n << 1073741823n) - 1n;
a = (a << 1n) | 1n;

try {
    let b = a + 1n;
    assert(false, "Should throw OutOfMemoryError, but executed without exception");
} catch(e) {
    assert(e.message == "Out of memory: BigInt generated from this operation is too big", "Expected OutOfMemoryError, but got: " + e);
}

try {
    let b = a - (-1n);
    assert(false, "Should throw OutOfMemoryError, but executed without exception");
} catch(e) {
    assert(e.message == "Out of memory: BigInt generated from this operation is too big", "Expected OutOfMemoryError, but got: " + e);
}

try {
    let b = a * (-10n);
    assert(false, "Should throw OutOfMemoryError, but executed without exception");
} catch(e) {
    assert(e.message == "Out of memory: BigInt generated from this operation is too big", "Expected OutOfMemoryError, but got: " + e);
}

try {
    let b = a * 2n / a;
    assert(false, "Should throw OutOfMemoryError, but executed without exception");
} catch(e) {
    assert(e.message == "Out of memory: BigInt generated from this operation is too big", "Expected OutOfMemoryError, but got: " + e);
}

try {
    let b = a ^ -1n;
    assert(false, "Should throw OutOfMemoryError, but executed without exception");
} catch(e) {
    assert(e.message == "Out of memory: BigInt generated from this operation is too big", "Expected OutOfMemoryError, but got: " + e);
}

