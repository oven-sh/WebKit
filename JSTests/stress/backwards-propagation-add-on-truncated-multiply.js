function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": bad value " + actual + ", expected " + expected);
}

// An int32 times a value below 2^22 is below 2^53, so the double product is exact, and a multiply
// whose result is only truncated can be a wrapping 32-bit multiply. An addition on top of such a
// product can pass 2^53. There the double sum is rounded, and its low 32 bits are not those of
// the exact sum.
//
// 2147483646 * 4194303 is 9007197098868738. Adding 4294967295 gives 9007201393836033, which is
// above 2^53 and odd, so the double is 9007201393836032.

var optimized = false;

function scale(x, factor) { return x * factor; }

function makeTest(body, smallArguments, small, largeArguments, large) {
    let f = new Function("x", "y", "z", body);
    noInline(f);
    return { f, body, smallArguments, small, largeArguments, large };
}

function callWithSmallProduct(test) {
    let a = test.smallArguments;
    shouldBe(test.f(a[0], a[1], a[2]), test.small, test.body);
}

function callWithLargeProduct(test) {
    let a = test.largeArguments;
    shouldBe(test.f(a[0], a[1], a[2]), test.large, test.body);
}

const sums = [
    makeTest("return (x * 4194303 + 4294967295) | 0;", [5], 20971514, [2147483646], 2139095040),
    makeTest("return (4294967295 + x * 4194303) | 0;", [5], 20971514, [2147483646], 2139095040),
    makeTest("return (4194303 * x + 4294967295) | 0;", [5], 20971514, [2147483646], 2139095040),
    makeTest("return (x * -4194303 + 4294967295) | 0;", [-5], 20971514, [-2147483646], 2139095040),
    makeTest("return (x * 4194303 + -4294967295) | 0;", [-5], -20971514, [-2147483646], -2139095040),
    makeTest("return (x * 4194303 - 4294967295) | 0;", [-5], -20971514, [-2147483646], -2139095040),
    makeTest("return (x * 4194303 + 2147483648 + 2147483648) | 0;", [5], 20971515, [2147483647], 2143289344),
    makeTest("return (+(x * 4194303) + 4294967295) | 0;", [5], 20971514, [2147483646], 2139095040),
    makeTest("var product = x * 4194303; return (product | 0) ^ ((product + 4294967295) | 0);", [5], 1, [2147483646], 2),
    makeTest("return (scale(x, 4194303) + 4294967295) | 0;", [5], 20971514, [2147483646], 2139095040),
    makeTest("return (x * (y & 0x3fffff) + 4294967295) | 0;", [5, 3], 14, [2147483646, -1], 2139095040),
    makeTest("return ((x & 0x3fffff) * y + 4294967295) | 0;", [5, "3"], 14, [-1, "2147483646"], 2139095040),
    makeTest("return (x * 4194303 + (y | 0) + (z | 0)) | 0;", [5, 6, 7], 20971528, [-2147483648, -2147483648, -1], 0),
    makeTest("return (x * 4194303 + 4294967295) >>> 0;", [5], 20971514, [2147483646], 2139095040),
    makeTest("return ~(x * 4194303 + 4294967295);", [5], -20971515, [2147483646], -2139095041),

    // The profile of these has the large product from the first call on.
    makeTest("return (x * 4194303 + 4294967295) | 0;", [2147483646], 2139095040, [2147483646], 2139095040),
    makeTest("return (x * 4194303 + (y | 0) + (z | 0)) | 0;", [-2147483648, -2147483648, -1], 0, [-2147483648, -2147483648, -1], 0),

    // No sum of these passes 2^53.
    makeTest("return (x * 4194303) | 0;", [5], 20971515, [2147483646], 2139095042),
    makeTest("return (x * 2097151 + 4294967295 + 4294967295) | 0;", [5], 10485753, [2147483646], 2143289344),
];

// Which multiplies check for overflow. The additions on top of a truncated product add less than
// 2^48, so the product has to be at most 2^53 - 2^48. For an int32 times a constant, that is a
// constant of at most 2^22 - 2^17 = 4063232. A multiply that checks for overflow leaves the
// optimized code on the first product that is not an int32, and a wrapping multiply stays in it.

function makeProductTest(expression, small, large) {
    return makeTest("var result = " + expression + "; optimized = $vm.dfgTrue(); return result;", [5, 136072, 9], small, [2147483646, 2147483647, 9], large);
}

const wrapping = [
    makeProductTest("(x * 4194303) | 0", 20971515, 2139095042),
    makeProductTest("(x * 31 + 7) | 0", 162, 2147483593),
    makeProductTest("(x * 4063232 + 1) | 0", 20316161, -8126463),
    makeProductTest("(x * -4063232 + 1) | 0", -20316159, 8126465),
    makeProductTest("(4063232 * x + 1) | 0", 20316161, -8126463),
    makeProductTest("(x * (y & 0x1fffff) + 1) | 0", 680361, 2143289347),
    makeProductTest("((0x1fffff & y) * x + 1) | 0", 680361, 2143289347),
    makeProductTest("(x * (y >> 11) + 1) | 0", 331, 2145386499),
];

const checking = [
    makeProductTest("(x * 4063233 + 1) | 0", 20316166, 2139357183),
    makeProductTest("(x * -4063233 + 1) | 0", -20316164, -2139357181),
    makeProductTest("(x * (y & 0x3fffff) + 1) | 0", 680361, 2139095043),
    makeProductTest("(x * (y >> 9) + 4294967295) | 0", 1324, 2139095040),
    makeProductTest("(x * (y & -4) + 4294967295) | 0", 680359, 0),
    makeProductTest("(x * (y >> z) + 4294967295) | 0", 1324, 2139095040),
];

// Calls the function with a small product until that runs in optimized code, and then with the
// first large product. Returns undefined when it cannot tell: no call ran in optimized code, or
// the optimized code is gone after the call with the large product.
function largeProductLeavesOptimizedCode(test) {
    for (let i = 0; i < testLoopCount; ++i) {
        callWithSmallProduct(test);
        if (!optimized)
            continue;
        callWithLargeProduct(test);
        if (optimized)
            return false;
        callWithSmallProduct(test);
        return optimized ? true : undefined;
    }
}

for (let i = 0; i < testLoopCount; ++i) {
    sums.forEach(callWithSmallProduct);
    wrapping.forEach(callWithSmallProduct);
    checking.forEach(callWithSmallProduct);
}

for (let test of wrapping) {
    if (largeProductLeavesOptimizedCode(test) === true)
        throw new Error(test.body + ": the multiply checks for overflow");
}

for (let test of checking) {
    if (largeProductLeavesOptimizedCode(test) === false)
        throw new Error(test.body + ": the multiply does not check for overflow");
}

sums.forEach(callWithLargeProduct);
wrapping.forEach(callWithLargeProduct);
checking.forEach(callWithLargeProduct);
