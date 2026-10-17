//@ memoryHog!

// One hex digit past maxLengthBits (1 << 30 bits, 2**28 hex digits).
try {
    eval('0x'+'f'.repeat(2**28 + 1)+'n');
} catch {}
